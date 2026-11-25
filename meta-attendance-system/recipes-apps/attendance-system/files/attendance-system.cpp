#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#ifndef STATE_DIR
#define STATE_DIR "/var/lib/attendance-system"
#endif
#ifndef RUN_DIR
#define RUN_DIR "/run/attendance-system"
#endif
#ifndef CAMERA_CONFIG
#define CAMERA_CONFIG "/etc/tapo-rtsp.conf"
#endif

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr char frame_file[] = RUN_DIR "/latest.ppm";
constexpr char database_file[] = STATE_DIR "/attendance.db";
constexpr char motion[] = "phat hien chuyen dong";
constexpr auto ai_timeout = 8s;
std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void stop(int)
{
    stopping.store(true, std::memory_order_relaxed);
}

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::system_error system_error(const char* operation, int code = errno)
{
    return std::system_error(code, std::generic_category(), operation);
}

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
    void reset(int fd = -1)
    {
        if (fd_ >= 0)
            close(fd_);
        fd_ = fd;
    }

private:
    int fd_;
};

class Child {
public:
    Child(const std::vector<std::string>& command, bool capture = false)
    {
        std::vector<char*> argv;
        for (const auto& arg : command)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        Fd reader, writer;
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        if (error)
            throw system_error("posix_spawn_file_actions_init", error);

        try {
            if (capture) {
                int pipe_fds[2];
                if (pipe2(pipe_fds, O_CLOEXEC) < 0)
                    throw system_error("pipe2");
                reader.reset(pipe_fds[0]);
                writer.reset(pipe_fds[1]);
                if (fcntl(reader.get(), F_SETFL, O_NONBLOCK) < 0)
                    throw system_error("fcntl pipe");

                error = posix_spawn_file_actions_adddup2(&actions, writer.get(), STDOUT_FILENO);
                if (error)
                    throw system_error("Redirect child stdout", error);
            }
            error = posix_spawn(&pid_, argv.front(), &actions, nullptr, argv.data(), environ);
            if (error)
                throw system_error(command.front().c_str(), error);
        } catch (...) {
            posix_spawn_file_actions_destroy(&actions);
            throw;
        }
        posix_spawn_file_actions_destroy(&actions);
        output_.reset(reader.release());
    }

    ~Child() { terminate(); }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    // Returns one complete stdout line. A short timeout lets the caller enforce its deadline.
    std::optional<std::string> line(int timeout_ms)
    {
        auto newline = buffered_.find('\n');
        if (newline == std::string::npos && output_.get() >= 0) {
            pollfd pfd{output_.get(), POLLIN, 0};
            int ready = poll(&pfd, 1, timeout_ms);
            if (ready < 0 && errno != EINTR)
                throw system_error("poll child stdout");
            if (ready > 0) {
                char data[4096];
                auto count = read(output_.get(), data, sizeof(data));
                if (count > 0)
                    buffered_.append(data, static_cast<size_t>(count));
                else if (count == 0)
                    output_.reset();
                else if (errno != EAGAIN && errno != EINTR)
                    throw system_error("read child stdout");
                newline = buffered_.find('\n');
            }
        }
        if (newline == std::string::npos) {
            if (buffered_.size() > 4096)
                throw std::runtime_error("Child emitted a line longer than 4096 bytes");
            return std::nullopt;
        }

        std::string result = buffered_.substr(0, newline);
        buffered_.erase(0, newline + 1);
        return result;
    }

    bool output_open() const { return output_.get() >= 0; }

    bool exited()
    {
        if (pid_ < 0)
            return true;
        const pid_t result = waitpid(pid_, &status_, WNOHANG);
        if (result == pid_)
            pid_ = -1;
        else if (result < 0 && errno != EINTR)
            throw system_error("waitpid");
        return pid_ < 0;
    }

    bool succeeded() const { return WIFEXITED(status_) && WEXITSTATUS(status_) == 0; }

    void terminate(bool graceful = false) noexcept
    {
        if (pid_ < 0)
            return;
        if (graceful) {
            kill(pid_, SIGTERM);
            const auto deadline = Clock::now() + 200ms;
            do {
                if (waitpid(pid_, &status_, WNOHANG) == pid_) {
                    pid_ = -1;
                    return;
                }
                std::this_thread::sleep_for(10ms);
            } while (Clock::now() < deadline);
        }
        kill(pid_, SIGKILL);
        while (waitpid(pid_, &status_, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }

private:
    pid_t pid_ = -1;
    int status_ = 0;
    Fd output_;
    std::string buffered_;
};

struct Result {
    std::string status = "timeout";
    std::string person;
    int64_t frame_us = 0;
    std::optional<double> cosine;
};

std::optional<Result> parse_result(const std::string& line)
{
    if (line.compare(0, 9, "frame_us=") != 0)
        return std::nullopt;

    Result result;
    result.status.clear();
    std::istringstream input(line);
    std::string field;
    while (input >> field) {
        const auto equals = field.find('=');
        if (equals == std::string::npos)
            continue;
        const auto name = field.substr(0, equals);
        const auto value = field.substr(equals + 1);
        if (name == "frame_us")
            result.frame_us = std::stoll(value);
        else if (name == "result")
            result.status = value;
        else if (name == "cosine")
            result.cosine = std::stod(value);
        else if (name == "id")
            result.person = value;
    }

    if ((result.status == "match" && !result.person.empty() && result.cosine) ||
        result.status == "unknown" || result.status == "no-reference" || result.status == "no-face")
        return result;
    return std::nullopt;
}

class Session {
public:
    Session() : started_ms(now_ms())
    {
        fs::remove(frame_file);
        camera_ = std::thread(&Session::camera, this);
        try {
            ai_ = std::thread(&Session::recognize, this);
        } catch (...) {
            finished_.store(true);
            camera_.join();
            throw;
        }
    }

    ~Session()
    {
        finished_.store(true);
        join();
        std::error_code ignored;
        fs::remove(frame_file, ignored);
    }

    bool ready() const { return finished_.load(); }

    Result finish()
    {
        join();
        if ((result_.status == "timeout" || result_.status == "no-face") && camera_failed_.load())
            result_.status = "camera-error";
        return result_;
    }

    const int64_t started_ms;

private:
    void join()
    {
        if (ai_.joinable())
            ai_.join();
        if (camera_.joinable())
            camera_.join();
    }

    void camera()
    {
        try {
            if (finished_.load() || stopping.load())
                return;
            Child camera({BIN_DIR "/tapo-rtsp", CAMERA_CONFIG, "--frame-file", frame_file});
            while (!finished_.load() && !stopping.load() && !camera.exited())
                std::this_thread::sleep_for(25ms);
            if (!finished_.load() && !stopping.load())
                camera_failed_.store(true);
            camera.terminate(true);
        } catch (const std::exception& error) {
            camera_failed_.store(true);
            std::cerr << "Camera: " << error.what() << '\n';
        }
    }

    void recognize()
    {
        try {
            if (!stopping.load() && !finished_.load()) {
                const auto launch = Clock::now();
                Child ai({BIN_DIR "/attendance-ai", "--frame-file", frame_file}, true);
                const auto deadline = launch + ai_timeout;
                while (!stopping.load() && !finished_.load()) {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - Clock::now());
                    if (remaining <= 0ms)
                        break;

                    auto line = ai.line(std::min(remaining, 50ms).count());
                    if (!line && !ai.output_open()) {
                        result_.status = "ai-error";
                        break;
                    }
                    if (!line)
                        continue;

                    auto candidate = parse_result(*line);
                    if (!candidate)
                        continue;
                    if (candidate->status == "match") {
                        result_ = std::move(*candidate);
                        break;
                    }

                    // A transient empty/unknown frame may be followed by a confident match.
                    if (result_.status == "timeout" ||
                        (result_.status == "no-face" && candidate->status != "no-face") ||
                        (candidate->cosine && (!result_.cosine || *candidate->cosine > *result_.cosine)))
                        result_ = std::move(*candidate);
                }
                ai.terminate();
            }
        } catch (const std::exception& error) {
            result_.status = "ai-error";
            std::cerr << "AI: " << error.what() << '\n';
        }
        if (stopping.load())
            result_.status = "cancelled";
        finished_.store(true);
    }

    Result result_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> camera_failed_{false};
    std::thread camera_;
    std::thread ai_;
};

class Database {
public:
    Database()
    {
        fs::create_directories(STATE_DIR);
        sqlite3* handle = nullptr;
        const int rc = sqlite3_open(database_file, &handle);
        db_.reset(handle);
        if (rc != SQLITE_OK)
            throw std::runtime_error(handle ? sqlite3_errmsg(handle) : "Cannot allocate SQLite connection");
        check(sqlite3_busy_timeout(db_.get(), 2000), SQLITE_OK);
        check(sqlite3_exec(db_.get(),
            "CREATE TABLE IF NOT EXISTS attendance_events ("
            "event_id INTEGER PRIMARY KEY, started_ms INTEGER NOT NULL, "
            "finished_ms INTEGER NOT NULL, status TEXT NOT NULL, "
            "person_id TEXT, frame_us INTEGER, cosine REAL);"
            "CREATE VIEW IF NOT EXISTS attendance AS "
            "SELECT * FROM attendance_events WHERE status = 'match';",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }

    void record(int64_t started, const Result& result)
    {
        auto stmt = prepare("INSERT INTO attendance_events "
                            "(started_ms,finished_ms,status,person_id,frame_us,cosine) "
                            "VALUES (?,?,?,?,?,?);");
        check(sqlite3_bind_int64(stmt.get(), 1, started), SQLITE_OK);
        check(sqlite3_bind_int64(stmt.get(), 2, now_ms()), SQLITE_OK);
        check(sqlite3_bind_text(stmt.get(), 3, result.status.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
        if (result.status == "match")
            check(sqlite3_bind_text(stmt.get(), 4, result.person.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
        if (result.frame_us != 0)
            check(sqlite3_bind_int64(stmt.get(), 5, result.frame_us), SQLITE_OK);
        if (result.cosine)
            check(sqlite3_bind_double(stmt.get(), 6, *result.cosine), SQLITE_OK);
        check(sqlite3_step(stmt.get()), SQLITE_DONE);
    }

    void history()
    {
        auto stmt = prepare("SELECT datetime(finished_ms / 1000, 'unixepoch'), "
                            "status, COALESCE(person_id, '-'), COALESCE(cosine, '-') "
                            "FROM attendance_events ORDER BY event_id DESC LIMIT 20;");
        std::cout << "UTC\tStatus\tID\tCosine\n";
        int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            for (int i = 0; i < 4; ++i)
                std::cout << sqlite3_column_text(stmt.get(), i) << (i == 3 ? '\n' : '\t');
        }
        check(rc, SQLITE_DONE);
    }

private:
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

    Statement prepare(const char* sql)
    {
        sqlite3_stmt* stmt = nullptr;
        check(sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr), SQLITE_OK);
        return Statement(stmt, sqlite3_finalize);
    }

    void check(int actual, int expected) const
    {
        if (actual != expected)
            throw std::runtime_error(sqlite3_errmsg(db_.get()));
    }

    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
};

void show(const std::string& title, const std::string& detail = {})
{
    try {
        Child tft({BIN_DIR "/tft-test", "--show", title, detail});
        const auto deadline = Clock::now() + 1s;
        while (!tft.exited()) {
            if (Clock::now() >= deadline || stopping.load())
                throw std::runtime_error("TFT command interrupted or timed out");
            std::this_thread::sleep_for(20ms);
        }
        if (!tft.succeeded())
            throw std::runtime_error("TFT command failed");
    } catch (const std::exception& error) {
        std::cerr << "TFT: " << error.what() << '\n';
    }
}

void publish(Database& database, Session& session)
{
    const auto result = session.finish();
    if (stopping.load() || result.status == "cancelled")
        return;

    try {
        database.record(session.started_ms, result);
    } catch (const std::exception& error) {
        show("LOI LUU DU LIEU");
        throw;
    }

    std::cout << "result=" << result.status;
    if (!result.person.empty())
        std::cout << " id=" << result.person;
    if (result.cosine)
        std::cout << " cosine=" << *result.cosine;
    std::cout << std::endl;

    if (result.status == "match")
        show("DIEM DANH OK", result.person);
    else if (result.status == "unknown")
        show("KHONG NHAN RA");
    else if (result.status == "no-reference")
        show("CHUA CO MAU");
    else if (result.status == "no-face")
        show("KHONG THAY MAT");
    else if (result.status == "camera-error")
        show("LOI CAMERA");
    else if (result.status == "ai-error")
        show("LOI NHAN DIEN");
    else
        show("HET THOI GIAN");
}

void run_pir(Database& database)
{
    Child pir({BIN_DIR "/pir-test"}, true);
    std::unique_ptr<Session> session;
    std::cout << "Waiting for /dev/m_device" << std::endl;

    while (!stopping.load()) {
        if (session && session->ready()) {
            // Consume any PIR transitions queued during this session before rearming.
            while (pir.line(0)) {}
            publish(database, *session);
            session.reset();
            while (pir.line(0)) {}
        }
        const auto event = pir.line(50);
        if (!event && !pir.output_open() && !stopping.load())
            throw std::runtime_error("PIR reader exited; check /dev/m_device");
        if (event && *event == motion && !session) {
            std::cout << "Motion detected, starting camera and AI" << std::endl;
            session = std::make_unique<Session>();
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::string option = argc == 2 ? argv[1] : "";
    if (option == "--help") {
        std::cout << "Usage: attendance-system [--once | history]\n";
        return 0;
    }
    if (argc != 1 && option != "--once" && option != "history") {
        std::cerr << "Usage: attendance-system [--once | history]\n";
        return 2;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    umask(0077);

    try {
        Database database;
        if (option == "history") {
            database.history();
            return 0;
        }

        fs::create_directories(RUN_DIR);
        Fd lock(open(RUN_DIR "/controller.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600));
        if (lock.get() < 0)
            throw system_error("Open controller lock");
        if (flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
            throw system_error("Lock controller; check if attendance-system is already running");

        if (option == "--once") {
            Session session;
            publish(database, session);
        } else {
            show("CHO CHUYEN DONG");
            run_pir(database);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "attendance-system: " << error.what() << '\n';
        return 1;
    }
}

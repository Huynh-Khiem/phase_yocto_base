#include <microhttpd.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <signal.h>

#ifndef DB_PATH
#define DB_PATH "/var/lib/attendance-system/attendance.db"
#endif
#ifndef PROC_DIR
#define PROC_DIR "/proc"
#endif

namespace {

namespace fs = std::filesystem;
using namespace std::string_view_literals;

constexpr auto process_names = std::array{
    "attendance-system"sv, "pir-test"sv, "tapo-rtsp"sv, "attendance-ai"sv, "tft-test"sv
};
constexpr const char* database_path = DB_PATH;

struct Assets {
    std::string html;
    std::string css;
    std::string js;
};

std::string load_asset(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::string("Cannot open ") + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void append_json_string(std::ostream& output, std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            output << '\\' << static_cast<char>(ch);
        } else if (ch < 0x20) {
            output << "\\u00" << hex[ch >> 4] << hex[ch & 0xf];
        } else {
            output << static_cast<char>(ch);
        }
    }
    output << '"';
}

std::string process_snapshot()
{
    constexpr size_t comm_length = 15;
    std::array<std::vector<unsigned>, process_names.size()> pids;
    for (const auto& entry : fs::directory_iterator(PROC_DIR)) {
        const auto pid = entry.path().filename().string();
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos)
            continue;

        std::ifstream stat(entry.path() / "stat");
        std::string line;
        if (!std::getline(stat, line))
            continue;
        // Linux truncates comm to 15 bytes and places it in parentheses in /proc/PID/stat.
        const auto left = line.find('(');
        const auto right = line.rfind(')');
        if (left == std::string::npos || right == std::string::npos ||
            right <= left || right + 2 >= line.size())
            continue;

        const char state = line[right + 2];
        if (state == 'Z' || state == 'X' || state == 'x')
            continue;

        const auto comm = std::string_view(line).substr(left + 1, right - left - 1);
        for (size_t i = 0; i < process_names.size(); ++i) {
            if (comm == process_names[i].substr(0, comm_length)) {
                pids[i].push_back(static_cast<unsigned>(std::stoul(pid)));
                break;
            }
        }
    }

    std::ostringstream json;
    json << '[';
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i != 0)
            json << ',';
        std::sort(pids[i].begin(), pids[i].end());
        json << "{\"name\":";
        append_json_string(json, process_names[i]);
        json << ",\"pids\":[";
        for (size_t j = 0; j < pids[i].size(); ++j) {
            if (j != 0)
                json << ',';
            json << pids[i][j];
        }
        json << "]}";
    }
    json << ']';
    return json.str();
}

void append_column(std::ostream& output, sqlite3_stmt* statement, int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        output << "null";
        return;
    }
    const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    if (!value)
        throw std::runtime_error("SQLite text conversion failed");
    append_json_string(output, std::string_view(value, sqlite3_column_bytes(statement, column)));
}

std::string event_snapshot()
{
    if (!fs::exists(database_path))
        return "[]";

    sqlite3* handle = nullptr;
    const int opened = sqlite3_open_v2(database_path, &handle, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(handle, sqlite3_close);
    if (opened != SQLITE_OK || sqlite3_busy_timeout(handle, 100) != SQLITE_OK)
        throw std::runtime_error("Cannot read the attendance database");

    sqlite3_stmt* query = nullptr;
    const int prepared = sqlite3_prepare_v2(handle,
        "SELECT event_id, finished_ms, status, person_id, cosine "
        "FROM attendance_results ORDER BY event_id DESC LIMIT 30", -1, &query, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(query, sqlite3_finalize);
    if (prepared != SQLITE_OK)
        throw std::runtime_error("Cannot query attendance results");

    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << '[';
    bool first = true;
    int result;
    while ((result = sqlite3_step(query)) == SQLITE_ROW) {
        if (!first)
            json << ',';
        first = false;
        json << "{\"id\":" << sqlite3_column_int64(query, 0)
             << ",\"finished_ms\":" << sqlite3_column_int64(query, 1)
             << ",\"status\":";
        append_column(json, query, 2);
        json << ",\"person_id\":";
        append_column(json, query, 3);
        json << ",\"cosine\":";
        const auto score = sqlite3_column_double(query, 4);
        if (sqlite3_column_type(query, 4) == SQLITE_NULL || !std::isfinite(score))
            json << "null";
        else
            json << score;
        json << '}';
    }
    if (result != SQLITE_DONE)
        throw std::runtime_error("Attendance results are temporarily unavailable");

    json << ']';
    return json.str();
}

MHD_Result reply(MHD_Connection* connection, unsigned status, const char* type, std::string_view payload)
{
    auto* response = MHD_create_response_from_buffer(
        payload.size(), const_cast<char*>(payload.data()), MHD_RESPMEM_MUST_COPY);
    if (!response)
        return MHD_NO;
    MHD_add_response_header(response, "Content-Type", type);
    MHD_add_response_header(response, "Cache-Control", "no-store");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "Content-Security-Policy",
        "default-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
    if (status == MHD_HTTP_METHOD_NOT_ALLOWED)
        MHD_add_response_header(response, "Allow", "GET, HEAD");
    const auto queued = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return queued;
}

MHD_Result handle(void* context, MHD_Connection* connection, const char* url,
                  const char* method, const char*, const char*, size_t*, void**)
{
    constexpr char json[] = "application/json; charset=utf-8";
    const auto& assets = *static_cast<const Assets*>(context);
    const std::string_view path(url);
    const std::string_view verb(method);
    if (verb != "GET" && verb != "HEAD")
        return reply(connection, MHD_HTTP_METHOD_NOT_ALLOWED, json, R"({"error":"method-not-allowed"})");

    try {
        if (path == "/" || path == "/index.html")
            return reply(connection, MHD_HTTP_OK, "text/html; charset=utf-8", assets.html);
        if (path == "/dashboard.css")
            return reply(connection, MHD_HTTP_OK, "text/css; charset=utf-8", assets.css);
        if (path == "/dashboard.js")
            return reply(connection, MHD_HTTP_OK, "application/javascript; charset=utf-8", assets.js);
        if (path == "/api/status")
            return reply(connection, MHD_HTTP_OK, json, process_snapshot());
        if (path == "/api/results")
            return reply(connection, MHD_HTTP_OK, json, event_snapshot());
        return reply(connection, MHD_HTTP_NOT_FOUND, json, R"({"error":"not-found"})");
    } catch (const std::exception&) {
        return reply(connection, MHD_HTTP_SERVICE_UNAVAILABLE, json, R"({"error":"temporarily-unavailable"})");
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc > 3)
            throw std::runtime_error("Usage: attendance-web [IPv4-address] [port]");
        const char* address = argc > 1 ? argv[1] : "127.0.0.1";
        const std::string port_text = argc > 2 ? argv[2] : "8080";
        if (port_text.empty() || port_text.size() > 5 ||
            port_text.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Invalid port");
        const auto port = std::stoul(port_text);
        if (port == 0 || port > 65535)
            throw std::runtime_error("Invalid port");

        sockaddr_in bind_address{};
        bind_address.sin_family = AF_INET;
        bind_address.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, address, &bind_address.sin_addr) != 1)
            throw std::runtime_error("Invalid IPv4 address");

        Assets assets{load_asset(WEB_DIR "/index.html"), load_asset(WEB_DIR "/dashboard.css"),
                      load_asset(WEB_DIR "/dashboard.js")};

        // Block before starting HTTP threads so SIGTERM is handled in the main thread.
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGTERM);
        sigaddset(&signals, SIGINT);
        if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0)
            throw std::runtime_error("Cannot block termination signals");

        using Daemon = std::unique_ptr<MHD_Daemon, decltype(&MHD_stop_daemon)>;
        Daemon daemon(MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD,
            static_cast<uint16_t>(port), nullptr, nullptr, handle, &assets,
            MHD_OPTION_SOCK_ADDR, reinterpret_cast<const sockaddr*>(&bind_address),
            MHD_OPTION_CONNECTION_LIMIT, 16u,
            MHD_OPTION_CONNECTION_TIMEOUT, 8u,
            MHD_OPTION_END), MHD_stop_daemon);
        if (!daemon)
            throw std::runtime_error("Cannot start the HTTP server");
        std::cout << "Listening on http://" << address << ':' << port << std::endl;
        int signal;
        return sigwait(&signals, &signal) == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "attendance-web: " << error.what() << '\n';
        return 1;
    }
}

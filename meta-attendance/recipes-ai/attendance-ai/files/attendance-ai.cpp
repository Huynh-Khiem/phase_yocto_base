#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr char live_frame[] = "/run/tapo-rtsp/latest.ppm";
constexpr int det_size = 640;
constexpr int rec_size = 112;
constexpr float detection_threshold = 0.70f;
constexpr float nms_threshold = 0.40f;
constexpr float default_match_threshold = 0.60f;
constexpr float match_margin = 0.10f;
constexpr char reference_header[] = "attendance-ai:sface-2021dec:v1";
using Embedding = std::array<float, 128>;

volatile std::sig_atomic_t stopping = 0;

void stop(int)
{
    stopping = 1;
}

struct Frame {
    int width = 0;
    int height = 0;
    int64_t received_us = 0;
    std::vector<unsigned char> rgb;
};

struct Detection {
    float x1, y1, x2, y2, score;
    std::array<float, 10> landmarks;
};

struct Face {
    Detection detection;
    Embedding embedding;
};

struct Reference {
    std::string id;
    Embedding embedding;
};

std::string ppm_token(std::istream& in, Frame& frame)
{
    while (in >> std::ws && in.peek() == '#') {
        std::string comment;
        std::getline(in, comment);
        const std::string prefix = "# received_unix_us=";
        if (comment.compare(0, prefix.size(), prefix) == 0)
            frame.received_us = std::stoll(comment.substr(prefix.size()));
    }
    std::string token;
    if (!(in >> token))
        throw std::runtime_error("Incomplete PPM header");
    return token;
}

Frame read_ppm(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open " + path.string());

    Frame frame;
    if (ppm_token(in, frame) != "P6")
        throw std::runtime_error("Expected a binary RGB PPM (P6)");

    frame.width = std::stoi(ppm_token(in, frame));
    frame.height = std::stoi(ppm_token(in, frame));
    if (frame.width < 1 || frame.width > 4096 ||
        frame.height < 1 || frame.height > 4096 ||
        ppm_token(in, frame) != "255")
        throw std::runtime_error("Invalid RGB PPM dimensions or depth");

    const int separator = in.get();
    if (separator != ' ' && separator != '\n' && separator != '\t' && separator != '\r')
        throw std::runtime_error("Missing PPM pixel separator");
    if (separator == '\r' && in.peek() == '\n')
        in.get();

    frame.rgb.resize(static_cast<size_t>(frame.width) * frame.height * 3);
    if (!in.read(reinterpret_cast<char*>(frame.rgb.data()), frame.rgb.size()))
        throw std::runtime_error("Incomplete PPM pixels");
    return frame;
}

bool fresh(const Frame& frame)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return frame.received_us > now_us - 5000000 && frame.received_us <= now_us;
}

float sample_rgb(const Frame& frame, float x, float y, int channel)
{
    if (x <= -1 || y <= -1 || x >= frame.width || y >= frame.height)
        return 0;

    const int left = static_cast<int>(std::floor(x));
    const int top = static_cast<int>(std::floor(y));
    const float dx = x - left;
    const float dy = y - top;
    const auto pixel = [&](int column, int row) {
        if (column < 0 || column >= frame.width || row < 0 || row >= frame.height)
            return 0.0f;
        return static_cast<float>(frame.rgb[(static_cast<size_t>(row) * frame.width + column) * 3 +
                                            channel]);
    };
    const float upper = (1 - dx) * pixel(left, top) + dx * pixel(left + 1, top);
    const float lower = (1 - dx) * pixel(left, top + 1) + dx * pixel(left + 1, top + 1);
    return (1 - dy) * upper + dy * lower;
}

float intersection_over_union(const Detection& a, const Detection& b)
{
    const float w = std::max(0.0f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
    const float h = std::max(0.0f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    const float overlap = w * h;
    return overlap / ((a.x2 - a.x1) * (a.y2 - a.y1) +
                      (b.x2 - b.x1) * (b.y2 - b.y1) - overlap);
}

void normalize(Embedding& values)
{
    const float norm = std::sqrt(std::inner_product(values.begin(), values.end(), values.begin(), 0.0f));
    if (!std::isfinite(norm) || norm < 1e-12f)
        throw std::runtime_error("Invalid SFace embedding");
    for (float& value : values)
        value /= norm;
}

class FaceEngine {
public:
    FaceEngine()
        : env_(ORT_LOGGING_LEVEL_WARNING, "attendance-ai")
    {
        Ort::SessionOptions options;
        options.SetExecutionMode(ORT_SEQUENTIAL);
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
        detector_ = Ort::Session(env_, MODEL_DIR "/face_detection_yunet_2023mar.onnx", options);
        recognizer_ = Ort::Session(env_, MODEL_DIR "/face_recognition_sface_2021dec.onnx", options);
    }

    std::vector<Face> process(const Frame& frame)
    {
        std::vector<Face> faces;
        for (const auto& detection : detect(frame))
            faces.push_back({detection, recognize(frame, detection.landmarks)});
        return faces;
    }

private:
    std::vector<Detection> detect(const Frame& frame)
    {
        const float scale = static_cast<float>(det_size) / std::max(frame.width, frame.height);
        const float padding_x = (det_size - frame.width * scale) / 2;
        const float padding_y = (det_size - frame.height * scale) / 2;
        std::vector<float> pixels(3 * det_size * det_size);
        for (int y = 0; y < det_size; y++) {
            for (int x = 0; x < det_size; x++) {
                const float sx = (x + 0.5f - padding_x) / scale - 0.5f;
                const float sy = (y + 0.5f - padding_y) / scale - 0.5f;
                for (int c = 0; c < 3; c++)
                    pixels[(c * det_size + y) * det_size + x] = sample_rgb(frame, sx, sy, 2 - c);
            }
        }

        const std::array<int64_t, 4> shape{1, 3, det_size, det_size};
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(),
                                                     shape.data(), shape.size());
        const char* input[] = {"input"};
        const char* output[] = {"cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32",
                                "bbox_8", "bbox_16", "bbox_32", "kps_8", "kps_16", "kps_32"};
        const auto values = detector_.Run(Ort::RunOptions{nullptr}, input, &tensor, 1, output, 12);

        std::vector<Detection> candidates;
        for (int level = 0; level < 3; ++level) {
            const int stride = 8 << level;
            const int grid = det_size / stride;
            const int count = grid * grid;
            const float* cls = tensor_data(values[level], count);
            const float* obj = tensor_data(values[3 + level], count);
            const float* bbox = tensor_data(values[6 + level], 4 * count);
            const float* kps = tensor_data(values[9 + level], 10 * count);
            for (int i = 0; i < count; ++i) {
                const float score = std::sqrt(std::clamp(cls[i], 0.0f, 1.0f) *
                                              std::clamp(obj[i], 0.0f, 1.0f));
                if (!(score >= detection_threshold))
                    continue;

                const int gx = i % grid;
                const int gy = i / grid;
                const float cx = (gx + bbox[4 * i]) * stride;
                const float cy = (gy + bbox[4 * i + 1]) * stride;
                const float w = std::exp(bbox[4 * i + 2]) * stride;
                const float h = std::exp(bbox[4 * i + 3]) * stride;
                Detection detection{
                    std::clamp((cx - w / 2 - padding_x) / scale, 0.0f, float(frame.width)),
                    std::clamp((cy - h / 2 - padding_y) / scale, 0.0f, float(frame.height)),
                    std::clamp((cx + w / 2 - padding_x) / scale, 0.0f, float(frame.width)),
                    std::clamp((cy + h / 2 - padding_y) / scale, 0.0f, float(frame.height)),
                    score, {}
                };
                if (!(detection.x2 > detection.x1 && detection.y2 > detection.y1))
                    continue;
                for (int p = 0; p < 5; p++) {
                    detection.landmarks[2 * p] = ((gx + kps[10 * i + 2 * p]) * stride - padding_x) / scale;
                    detection.landmarks[2 * p + 1] =
                        ((gy + kps[10 * i + 2 * p + 1]) * stride - padding_y) / scale;
                }
                candidates.push_back(detection);
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Detection& a, const Detection& b) { return a.score > b.score; });
        std::vector<Detection> result;
        for (const auto& candidate : candidates) {
            const auto overlaps = [&](const Detection& kept) {
                return intersection_over_union(candidate, kept) > nms_threshold;
            };
            if (std::none_of(result.begin(), result.end(), overlaps))
                result.push_back(candidate);
        }
        return result;
    }

    Embedding recognize(const Frame& frame, const std::array<float, 10>& landmarks)
    {
        constexpr std::array<float, 10> target{
            38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f, 71.7366f,
            41.5493f, 92.3655f, 70.7299f, 92.2041f
        };
        float sx = 0, sy = 0, tx = 0, ty = 0;
        for (size_t i = 0; i < target.size(); i += 2) {
            sx += landmarks[i] / 5;
            sy += landmarks[i + 1] / 5;
            tx += target[i] / 5;
            ty += target[i + 1] / 5;
        }

        float a_num = 0, b_num = 0, denominator = 0;
        for (size_t i = 0; i < target.size(); i += 2) {
            const float x = landmarks[i] - sx;
            const float y = landmarks[i + 1] - sy;
            const float u = target[i] - tx;
            const float v = target[i + 1] - ty;
            a_num += x * u + y * v;
            b_num += x * v - y * u;
            denominator += x * x + y * y;
        }
        if (!(denominator > 1e-6f))
            throw std::runtime_error("Degenerate face landmarks");

        // Invert the similarity transform that maps the landmarks to the SFace template.
        const float a = a_num / denominator;
        const float b = b_num / denominator;
        const float determinant = a * a + b * b;
        if (!std::isfinite(determinant) || determinant < 1e-12f)
            throw std::runtime_error("Invalid face alignment");

        std::vector<float> pixels(3 * rec_size * rec_size);
        for (int y = 0; y < rec_size; y++) {
            for (int x = 0; x < rec_size; x++) {
                const float dx = x - tx;
                const float dy = y - ty;
                const float source_x = sx + (a * dx + b * dy) / determinant;
                const float source_y = sy + (-b * dx + a * dy) / determinant;
                for (int c = 0; c < 3; c++)
                    pixels[(c * rec_size + y) * rec_size + x] = sample_rgb(frame, source_x, source_y, c);
            }
        }

        // The supplied SFace graph contains its own normalization.
        const std::array<int64_t, 4> shape{1, 3, rec_size, rec_size};
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(),
                                                     shape.data(), shape.size());
        const char* input[] = {"data"};
        const char* output[] = {"fc1"};
        const auto values = recognizer_.Run(Ort::RunOptions{nullptr}, input, &tensor, 1, output, 1);
        Embedding embedding;
        std::copy_n(tensor_data(values.front(), embedding.size()), embedding.size(), embedding.begin());
        normalize(embedding);
        return embedding;
    }

    static const float* tensor_data(const Ort::Value& tensor, size_t count)
    {
        const auto info = tensor.GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            info.GetElementCount() != count)
            throw std::runtime_error("Unexpected ONNX output shape or type");
        return tensor.GetTensorData<float>();
    }

    Ort::Env env_;
    Ort::Session detector_{nullptr};
    Ort::Session recognizer_{nullptr};
};

bool valid_id(const std::string& id)
{
    return !id.empty() && id.size() <= 64 &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
               std::string::npos;
}

std::vector<Reference> load_references(const fs::path& db)
{
    std::vector<Reference> references;
    if (!fs::exists(db))
        return references;
    for (const auto& entry : fs::directory_iterator(db)) {
        if (entry.path().extension() != ".embedding")
            continue;

        Reference reference{entry.path().stem().string(), {}};
        if (!valid_id(reference.id))
            throw std::runtime_error("Invalid reference filename: " + entry.path().string());
        std::ifstream in(entry.path());
        std::string header;
        if (!std::getline(in, header))
            throw std::runtime_error("Cannot read reference: " + entry.path().string());
        if (header != reference_header) {
            std::cerr << "Ignoring incompatible reference " << entry.path()
                      << "; re-enroll with --enroll " << reference.id << '\n';
            continue;
        }
        for (float& value : reference.embedding)
            if (!(in >> value))
                throw std::runtime_error("Invalid reference: " + entry.path().string());
        normalize(reference.embedding);
        references.push_back(reference);
    }
    std::sort(references.begin(), references.end(),
              [](const Reference& a, const Reference& b) { return a.id < b.id; });
    return references;
}

void save_reference(const fs::path& db, const std::string& id, const Embedding& embedding)
{
    fs::create_directories(db);
    const fs::path destination = db / (id + ".embedding");
    const fs::path temporary = db / (id + "." + std::to_string(getpid()) + ".tmp");
    try {
        std::ofstream out(temporary);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out << reference_header << '\n' << std::setprecision(std::numeric_limits<float>::max_digits10);
        for (float value : embedding)
            out << value << '\n';
        out.close();
        fs::rename(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

std::string evaluate(FaceEngine& engine, const Frame& frame,
                     const std::vector<Reference>& references, float threshold)
{
    const auto faces = engine.process(frame);
    std::ostringstream result;
    result << std::fixed << std::setprecision(3);
    if (faces.empty())
        result << "frame_us=" << frame.received_us << " result=no-face\n";
    for (const auto& face : faces) {
        const Reference* best = nullptr;
        const Reference* runner_up = nullptr;
        float best_score = -std::numeric_limits<float>::infinity();
        float runner_up_score = -std::numeric_limits<float>::infinity();
        for (const auto& reference : references) {
            const float score = std::inner_product(face.embedding.begin(), face.embedding.end(),
                                                   reference.embedding.begin(), 0.0f);
            if (score > best_score) {
                runner_up = best;
                runner_up_score = best_score;
                best = &reference;
                best_score = score;
            } else if (score > runner_up_score) {
                runner_up = &reference;
                runner_up_score = score;
            }
        }
        const auto& box = face.detection;
        result << "frame_us=" << frame.received_us << " detection=" << box.score
               << " box=" << box.x1 << ',' << box.y1 << ',' << box.x2 << ',' << box.y2;
        if (!best) {
            result << " result=no-reference";
        } else {
            result << " cosine=" << std::clamp(best_score, -1.0f, 1.0f);
            if (best_score >= threshold && (!runner_up || best_score - runner_up_score >= match_margin))
                result << " result=match id=" << best->id;
            else
                result << " result=unknown";
        }
        result << '\n';
    }
    return result.str();
}

class FrameWorkers {
public:
    FrameWorkers(FaceEngine& engine, const std::vector<Reference>& references, float threshold)
        : engine_(engine), references_(references), threshold_(threshold)
    {
        try {
            for (auto& worker : workers_)
                worker = std::thread(&FrameWorkers::work, this);
        } catch (...) {
            close();
            throw;
        }
    }

    ~FrameWorkers()
    {
        close();
    }

    void submit(Frame frame)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = std::move(frame);
        }
        ready_.notify_one();
    }

    bool failed()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }

private:
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable())
                worker.join();
    }

    void work()
    {
        for (;;) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return closed_ || pending_.has_value(); });
                if (closed_)
                    return;
                frame = std::move(*pending_);
                pending_.reset();
            }

            try {
                const auto result = evaluate(engine_, frame, references_, threshold_);
                std::lock_guard<std::mutex> lock(output_mutex_);
                std::cout << result << std::flush;
            } catch (const std::exception& error) {
                {
                    std::lock_guard<std::mutex> lock(output_mutex_);
                    std::cerr << "Inference failed: " << error.what() << '\n';
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    failed_ = true;
                    closed_ = true;
                }
                ready_.notify_all();
                return;
            }
        }
    }

    FaceEngine& engine_;
    const std::vector<Reference>& references_;
    const float threshold_;
    std::mutex mutex_;
    std::mutex output_mutex_;
    std::condition_variable ready_;
    std::optional<Frame> pending_;
    bool closed_ = false;
    bool failed_ = false;
    std::array<std::thread, 2> workers_;
};

int run_live(FaceEngine& engine, const std::vector<Reference>& references, float threshold)
{
    FrameWorkers workers(engine, references, threshold);
    std::cerr << "Waiting for " << live_frame << " (2 inference workers, " << references.size()
              << " references loaded)\n";
    timespec last_write{};
    int64_t previous_frame = 0;
    while (!stopping && !workers.failed()) {
        struct stat state {};
        if (stat(live_frame, &state) == 0 &&
            (state.st_mtim.tv_sec != last_write.tv_sec || state.st_mtim.tv_nsec != last_write.tv_nsec)) {
            last_write = state.st_mtim;
            Frame frame;
            try {
                frame = read_ppm(live_frame);
            } catch (const std::exception& error) {
                // The camera may remove the frame while its service restarts.
                if (fs::exists(live_frame))
                    std::cerr << error.what() << '\n';
            }
            if (frame.received_us != previous_frame && fresh(frame)) {
                previous_frame = frame.received_us;
                workers.submit(std::move(frame));
            }
        }
        std::this_thread::sleep_for(15ms);
    }
    return workers.failed() ? 1 : 0;
}

void usage()
{
    std::cout << "Usage: attendance-ai [--image P6.ppm] [--db DIRECTORY] [--threshold COSINE]\n"
                 "       Default cosine threshold: 0.60. Minimum lead over second match: 0.10.\n"
                 "       attendance-ai --enroll ID [--image P6.ppm] [--db DIRECTORY]\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    umask(0077);

    try {
        fs::path db = "/var/lib/attendance-ai";
        fs::path image;
        std::string enroll;
        float threshold = default_match_threshold;
        for (int i = 1; i < argc; i++) {
            const std::string option = argv[i];
            if (option == "--help") {
                usage();
                return 0;
            }
            if (option != "--db" && option != "--image" && option != "--enroll" &&
                option != "--threshold")
                throw std::runtime_error("Unknown option: " + option);
            if (++i == argc || !*argv[i])
                throw std::runtime_error("Missing value for " + option);
            const std::string value = argv[i];
            if (option == "--db")
                db = value;
            else if (option == "--image")
                image = value;
            else if (option == "--enroll")
                enroll = value;
            else {
                size_t parsed = 0;
                threshold = std::stof(value, &parsed);
                if (parsed != value.size() || !std::isfinite(threshold) || threshold < -1 || threshold > 1)
                    throw std::runtime_error("Expected a cosine threshold between -1 and 1");
            }
        }

        if (!enroll.empty() && !valid_id(enroll))
            throw std::runtime_error("ID must contain 1-64 letters, digits, underscores or hyphens");

        FaceEngine engine;
        if (!enroll.empty()) {
            const auto frame = read_ppm(image.empty() ? fs::path(live_frame) : image);
            if (image.empty() && !fresh(frame))
                throw std::runtime_error("Camera frame is older than five seconds or its timestamp is invalid");
            const auto faces = engine.process(frame);
            if (faces.size() != 1)
                throw std::runtime_error("Enrollment needs exactly one face; detected " +
                                         std::to_string(faces.size()));
            save_reference(db, enroll, faces.front().embedding);
            std::cout << "enrolled id=" << enroll << '\n';
            return 0;
        }

        const auto references = load_references(db);
        if (!image.empty()) {
            std::cout << evaluate(engine, read_ppm(image), references, threshold);
            return 0;
        }
        return run_live(engine, references, threshold);
    } catch (const std::exception& error) {
        std::cerr << "attendance-ai: " << error.what() << '\n';
        return 1;
    }
}

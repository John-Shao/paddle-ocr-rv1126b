#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <string>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "opencv2/opencv.hpp"

#include "image_utils.h"
#include "ppocr_system.h"

namespace {

constexpr float kDetThreshold = 0.3f;
constexpr float kBoxThreshold = 0.6f;
constexpr bool kUseDilation = false;
constexpr const char* kDbScoreMode = "slow";
constexpr const char* kDbBoxType = "poly";
constexpr float kDbUnclipRatio = 1.5f;
constexpr const char* kNoTextPrompt = "没有识别到文字信息";

struct Options {
    std::string det_model = "models/ppocrv4_det_i8.rknn";
    std::string rec_model = "models/ppocrv4_rec_fp16.rknn";
    std::string camera = "/dev/video-camera0";
    std::string snapshot_url = "http://127.0.0.1:8080/api/v1/snapshot.jpg";
    std::string image;
    bool snapshot_url_set = false;
    int width = 1280;
    int height = 720;
    int warmup = 3;
    std::string daemon_socket = "/run/ppocrd.sock";
    std::string tts_socket = "/run/melottsd.sock";
    int tts_priority = 4;
    bool daemon = false;
    bool speak = false;
    bool verbose = false;
};

volatile sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

const char* arg_value(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return nullptr;
}

bool has_flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) return true;
    }
    return false;
}

void usage(const char* prog) {
    std::printf(
        "Usage:\n"
        "  %s [--snapshot-url http://127.0.0.1:8080/api/v1/snapshot.jpg]\n"
        "  %s --daemon [--daemon-socket /run/ppocrd.sock]\n"
        "  %s --service [--speak]\n"
        "  %s --camera /dev/video-camera0 [--width 1280] [--height 720]\n"
        "  %s --image frame.jpg\n"
        "\n"
        "Options:\n"
        "  --det MODEL      det RKNN path, default models/ppocrv4_det_i8.rknn\n"
        "  --rec MODEL      rec RKNN path, default models/ppocrv4_rec_fp16.rknn\n"
        "  --snapshot-url URL  camera_core_d JPEG endpoint, default http://127.0.0.1:8080/api/v1/snapshot.jpg\n"
        "  --camera DEV     direct V4L2 camera device, disabled unless this option is set\n"
        "  --image FILE     read still image instead of camera capture\n"
        "  --width N        camera width, default 1280\n"
        "  --height N       camera height, default 720\n"
        "  --warmup N       frames to discard before capture, default 3\n"
        "  --daemon          run as ppocrd service, models stay loaded\n"
        "  --daemon-socket PATH  ppocrd socket, default /run/ppocrd.sock\n"
        "  --service         send one OCR request to ppocrd instead of running locally\n"
        "  --speak           speak OCR text through melottsd\n"
        "  --tts-socket PATH  melottsd socket, default /run/melottsd.sock\n"
        "  --tts-priority N   melottsd priority, default 4\n"
        "  --no-tts           print text only, do not speak\n",
        prog, prog, prog, prog, prog);
}

Options parse_options(int argc, char** argv) {
    Options opt;
    if (const char* v = arg_value(argc, argv, "--det")) opt.det_model = v;
    if (const char* v = arg_value(argc, argv, "--rec")) opt.rec_model = v;
    if (const char* v = arg_value(argc, argv, "--camera")) opt.camera = v;
    if (const char* v = arg_value(argc, argv, "--snapshot-url")) {
        opt.snapshot_url = v;
        opt.snapshot_url_set = true;
    }
    if (const char* v = arg_value(argc, argv, "--image")) opt.image = v;
    if (const char* v = arg_value(argc, argv, "--width")) opt.width = std::atoi(v);
    if (const char* v = arg_value(argc, argv, "--height")) opt.height = std::atoi(v);
    if (const char* v = arg_value(argc, argv, "--warmup")) opt.warmup = std::atoi(v);
    if (const char* v = arg_value(argc, argv, "--daemon-socket")) opt.daemon_socket = v;
    if (const char* v = arg_value(argc, argv, "--tts-socket")) opt.tts_socket = v;
    if (const char* v = arg_value(argc, argv, "--tts-priority")) opt.tts_priority = std::atoi(v);
    opt.daemon = has_flag(argc, argv, "--daemon");
    opt.speak = has_flag(argc, argv, "--speak") && !has_flag(argc, argv, "--no-tts");
    opt.verbose = has_flag(argc, argv, "--verbose");
    std::string prog = argv[0] ? argv[0] : "";
    if (prog.find("ppocrd") != std::string::npos) opt.daemon = true;
    return opt;
}

class StdoutSilencer {
public:
    explicit StdoutSilencer(bool enabled) : enabled_(enabled) {
        if (!enabled_) return;
        std::fflush(stdout);
        saved_fd_ = dup(STDOUT_FILENO);
        null_fd_ = open("/dev/null", O_WRONLY);
        if (saved_fd_ >= 0 && null_fd_ >= 0) {
            dup2(null_fd_, STDOUT_FILENO);
        }
    }
    ~StdoutSilencer() {
        if (!enabled_) return;
        std::fflush(stdout);
        if (saved_fd_ >= 0) {
            dup2(saved_fd_, STDOUT_FILENO);
            close(saved_fd_);
        }
        if (null_fd_ >= 0) close(null_fd_);
    }

private:
    bool enabled_;
    int saved_fd_ = -1;
    int null_fd_ = -1;
};

bool http_get(const std::string& url, std::vector<unsigned char>* body) {
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        std::fprintf(stderr, "only http:// snapshot URLs are supported: %s\n", url.c_str());
        return false;
    }
    std::string rest = url.substr(prefix.size());
    std::string host_port;
    std::string path = "/";
    size_t slash = rest.find('/');
    if (slash == std::string::npos) {
        host_port = rest;
    } else {
        host_port = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    std::string host = host_port;
    std::string port = "80";
    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        port = host_port.substr(colon + 1);
    }

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        std::fprintf(stderr, "getaddrinfo failed for %s:%s\n", host.c_str(), port.c_str());
        return false;
    }

    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        std::fprintf(stderr, "connect snapshot URL failed: %s\n", url.c_str());
        return false;
    }

    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host_port + "\r\nConnection: close\r\n\r\n";
    if (write(fd, req.data(), req.size()) != static_cast<ssize_t>(req.size())) {
        std::fprintf(stderr, "write HTTP request failed\n");
        close(fd);
        return false;
    }

    std::vector<unsigned char> response;
    unsigned char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        response.insert(response.end(), buf, buf + n);
    }
    close(fd);

    if (response.size() > 2 && response[0] == 0xff && response[1] == 0xd8) {
        body->swap(response);
        return true;
    }

    const char marker[] = "\r\n\r\n";
    auto it = std::search(response.begin(), response.end(), marker, marker + 4);
    int header_skip = 4;
    if (it == response.end()) {
        const char lf_marker[] = "\n\n";
        it = std::search(response.begin(), response.end(), lf_marker, lf_marker + 2);
        header_skip = 2;
    }
    if (it == response.end()) {
        std::fprintf(stderr, "bad HTTP response from snapshot URL\n");
        return false;
    }
    std::string header(response.begin(), it);
    if (header.find(" 200 ") == std::string::npos) {
        std::fprintf(stderr, "snapshot HTTP error: %s\n", header.c_str());
        return false;
    }
    body->assign(it + header_skip, response.end());
    return !body->empty();
}

bool capture_snapshot(const Options& opt, image_buffer_t* out) {
    std::vector<unsigned char> jpeg;
    if (!http_get(opt.snapshot_url, &jpeg)) return false;
    const char* tmp = "/tmp/ppocr_text_snapshot.jpg";
    FILE* f = std::fopen(tmp, "wb");
    if (!f) {
        std::fprintf(stderr, "open %s failed: %s\n", tmp, std::strerror(errno));
        return false;
    }
    size_t written = std::fwrite(jpeg.data(), 1, jpeg.size(), f);
    std::fclose(f);
    if (written != jpeg.size()) {
        std::fprintf(stderr, "write snapshot failed\n");
        return false;
    }
    StdoutSilencer quiet(!opt.verbose);
    return read_image(tmp, out) == 0;
}

bool mat_to_rgb_image(const cv::Mat& input, image_buffer_t* out) {
    cv::Mat rgb;
    if (input.empty()) return false;
    if (input.channels() == 3) {
        cv::cvtColor(input, rgb, cv::COLOR_BGR2RGB);
    } else if (input.channels() == 4) {
        cv::cvtColor(input, rgb, cv::COLOR_BGRA2RGB);
    } else if (input.channels() == 1) {
        cv::cvtColor(input, rgb, cv::COLOR_GRAY2RGB);
    } else {
        std::fprintf(stderr, "unsupported camera channels: %d\n", input.channels());
        return false;
    }

    std::memset(out, 0, sizeof(*out));
    out->width = rgb.cols;
    out->height = rgb.rows;
    out->format = IMAGE_FORMAT_RGB888;
    out->size = out->width * out->height * 3;
    out->virt_addr = static_cast<unsigned char*>(std::malloc(out->size));
    if (!out->virt_addr) return false;
    std::memcpy(out->virt_addr, rgb.data, out->size);
    return true;
}

int xioctl(int fd, unsigned long request, void* arg) {
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

bool capture_camera(const Options& opt, image_buffer_t* out) {
    int fd = open(opt.camera.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        std::fprintf(stderr, "failed to open camera: %s\n", opt.camera.c_str());
        return false;
    }

    v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = opt.width;
    fmt.fmt.pix_mp.height = opt.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", std::strerror(errno));
        close(fd);
        return false;
    }

    const int width = fmt.fmt.pix_mp.width;
    const int height = fmt.fmt.pix_mp.height;
    const int plane_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        std::fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", std::strerror(errno));
        close(fd);
        return false;
    }

    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };
    std::vector<Buffer> buffers(req.count);

    for (unsigned int i = 0; i < req.count; ++i) {
        v4l2_buffer buf;
        v4l2_plane planes[VIDEO_MAX_PLANES];
        std::memset(&buf, 0, sizeof(buf));
        std::memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            std::fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n", std::strerror(errno));
            close(fd);
            return false;
        }
        buffers[i].length = buf.m.planes[0].length;
        buffers[i].start = mmap(NULL, buffers[i].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                fd, buf.m.planes[0].m.mem_offset);
        if (buffers[i].start == MAP_FAILED) {
            std::fprintf(stderr, "mmap failed: %s\n", std::strerror(errno));
            close(fd);
            return false;
        }
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            std::fprintf(stderr, "VIDIOC_QBUF failed: %s\n", std::strerror(errno));
            close(fd);
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        std::fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", std::strerror(errno));
        close(fd);
        return false;
    }

    v4l2_buffer got;
    v4l2_plane got_planes[VIDEO_MAX_PLANES];
    int frames_to_skip = opt.warmup < 0 ? 0 : opt.warmup;
    bool ok = false;
    for (int frame_idx = 0; frame_idx <= frames_to_skip; ++frame_idx) {
        for (;;) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval tv;
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            int ret = select(fd + 1, &fds, NULL, NULL, &tv);
            if (ret < 0 && errno == EINTR) continue;
            if (ret <= 0) {
                std::fprintf(stderr, "camera capture timeout\n");
                goto cleanup;
            }

            std::memset(&got, 0, sizeof(got));
            std::memset(got_planes, 0, sizeof(got_planes));
            got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            got.memory = V4L2_MEMORY_MMAP;
            got.length = 1;
            got.m.planes = got_planes;
            if (xioctl(fd, VIDIOC_DQBUF, &got) < 0) {
                if (errno == EAGAIN) continue;
                std::fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", std::strerror(errno));
                goto cleanup;
            }
            break;
        }

        if (frame_idx < frames_to_skip) {
            if (xioctl(fd, VIDIOC_QBUF, &got) < 0) {
                std::fprintf(stderr, "VIDIOC_QBUF requeue failed: %s\n", std::strerror(errno));
                goto cleanup;
            }
        }
    }

    {
        size_t bytes = got.m.planes[0].bytesused > 0 ? got.m.planes[0].bytesused : plane_size;
        if (bytes < static_cast<size_t>(width * height * 3 / 2)) {
            std::fprintf(stderr, "captured NV12 buffer too small: %zu\n", bytes);
            goto cleanup;
        }
        cv::Mat nv12(height * 3 / 2, width, CV_8UC1, buffers[got.index].start);
        cv::Mat rgb;
        cv::cvtColor(nv12, rgb, cv::COLOR_YUV2RGB_NV12);

        std::memset(out, 0, sizeof(*out));
        out->width = rgb.cols;
        out->height = rgb.rows;
        out->format = IMAGE_FORMAT_RGB888;
        out->size = out->width * out->height * 3;
        out->virt_addr = static_cast<unsigned char*>(std::malloc(out->size));
        if (!out->virt_addr) goto cleanup;
        std::memcpy(out->virt_addr, rgb.data, out->size);
        ok = true;
    }

cleanup:
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (auto& b : buffers) {
        if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
    }
    close(fd);
    return ok;
}

std::string join_text(const ppocr_text_recog_array_result_t& results) {
    std::string text;
    for (int i = 0; i < results.count; ++i) {
        const char* s = results.text_result[i].text.str;
        if (!s || !s[0]) continue;
        if (!text.empty()) text += "\n";
        text += s;
    }
    return text;
}

std::string text_for_speech(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool last_was_sep = false;
    for (char ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            if (!last_was_sep && !out.empty()) {
                out += "，";
                last_was_sep = true;
            }
            continue;
        }
        out.push_back(ch);
        last_was_sep = false;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out.empty() ? kNoTextPrompt : out;
}

int connect_unix_socket(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool speak_text(const Options& opt, const std::string& text) {
    int fd = connect_unix_socket(opt.tts_socket);
    if (fd < 0) {
        std::fprintf(stderr, "TTS unavailable: cannot connect %s: %s\n",
                     opt.tts_socket.c_str(), std::strerror(errno));
        return false;
    }

    std::string req = std::to_string(opt.tts_priority) + "\t" + text + "\n";
    const char* p = req.data();
    size_t left = req.size();
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            std::fprintf(stderr, "TTS write failed: %s\n", std::strerror(errno));
            close(fd);
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }

    char ack[64];
    ssize_t r;
    do {
        r = read(fd, ack, sizeof(ack) - 1);
    } while (r < 0 && errno == EINTR);
    close(fd);
    if (r <= 0) {
        std::fprintf(stderr, "TTS ack failed\n");
        return false;
    }
    ack[r] = 0;
    return std::strncmp(ack, "done", 4) == 0 || std::strncmp(ack, "preempted", 9) == 0;
}

bool write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool write_string(int fd, const std::string& text) {
    return write_all(fd, text.data(), text.size());
}

std::string read_line(int fd, size_t max_len) {
    std::string line;
    while (line.size() < max_len) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (ch == '\n') break;
        if (ch != '\r') line.push_back(ch);
    }
    return line;
}

std::string request_value(const std::string& req, const std::string& key) {
    std::string prefix = key + "=";
    size_t pos = req.find(prefix);
    if (pos == std::string::npos) return "";
    pos += prefix.size();
    size_t end = req.find_first_of(" \t", pos);
    return req.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

bool request_flag(const std::string& req, const std::string& key) {
    std::string v = request_value(req, key);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool recognize_once(const Options& opt, ppocr_system_app_context& app, std::string* out_text) {
    image_buffer_t image;
    std::memset(&image, 0, sizeof(image));
    int ret = 0;

    if (!opt.image.empty()) {
        {
            StdoutSilencer quiet(!opt.verbose);
            ret = read_image(opt.image.c_str(), &image);
        }
        if (ret != 0) {
            std::fprintf(stderr, "read image failed: %s ret=%d\n", opt.image.c_str(), ret);
            return false;
        }
    } else if (!opt.camera.empty() && opt.snapshot_url.empty()) {
        if (!capture_camera(opt, &image)) return false;
    } else {
        if (!capture_snapshot(opt, &image)) return false;
    }

    ppocr_det_postprocess_params params;
    params.threshold = kDetThreshold;
    params.box_threshold = kBoxThreshold;
    params.use_dilate = kUseDilation;
    params.db_score_mode = const_cast<char*>(kDbScoreMode);
    params.db_box_type = const_cast<char*>(kDbBoxType);
    params.db_unclip_ratio = kDbUnclipRatio;

    ppocr_text_recog_array_result_t results;
    std::memset(&results, 0, sizeof(results));
    {
        StdoutSilencer quiet(!opt.verbose);
        ret = inference_ppocr_system_model(&app, &image, &params, &results);
    }

    if (image.virt_addr) std::free(image.virt_addr);
    if (ret != 0) {
        std::fprintf(stderr, "OCR inference failed: ret=%d\n", ret);
        return false;
    }
    *out_text = join_text(results);
    return true;
}

bool handle_client(int fd, const Options& base_opt, ppocr_system_app_context& app) {
    std::string req = read_line(fd, 2048);
    Options opt = base_opt;
    opt.speak = request_flag(req, "speak");
    opt.verbose = request_flag(req, "verbose");
    if (std::string v = request_value(req, "image"); !v.empty()) opt.image = v;
    if (std::string v = request_value(req, "snapshot_url"); !v.empty()) {
        opt.snapshot_url = v;
        opt.image.clear();
    }
    if (std::string v = request_value(req, "tts_priority"); !v.empty()) opt.tts_priority = std::atoi(v.c_str());

    std::string text;
    if (!recognize_once(opt, app, &text)) {
        write_string(fd, "ERR OCR failed\n");
        return false;
    }
    std::string response = text.empty() ? std::string("[NO_TEXT]\n") : text + "\n";
    write_string(fd, response);
    if (opt.speak) speak_text(opt, text_for_speech(text));
    return true;
}

int run_daemon(const Options& opt, ppocr_system_app_context& app) {
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        std::fprintf(stderr, "socket failed: %s\n", std::strerror(errno));
        return 1;
    }

    unlink(opt.daemon_socket.c_str());
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, opt.daemon_socket.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "bind %s failed: %s\n", opt.daemon_socket.c_str(), std::strerror(errno));
        close(server);
        return 1;
    }
    chmod(opt.daemon_socket.c_str(), 0666);
    if (listen(server, 8) < 0) {
        std::fprintf(stderr, "listen failed: %s\n", std::strerror(errno));
        close(server);
        return 1;
    }

    std::printf("ppocrd listening on %s\n", opt.daemon_socket.c_str());
    std::fflush(stdout);
    while (!g_stop) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "accept failed: %s\n", std::strerror(errno));
            break;
        }
        handle_client(client, opt, app);
        close(client);
    }
    close(server);
    unlink(opt.daemon_socket.c_str());
    return 0;
}

int run_service_client(const Options& opt) {
    int fd = connect_unix_socket(opt.daemon_socket);
    if (fd < 0) {
        std::fprintf(stderr, "cannot connect ppocrd %s: %s\n", opt.daemon_socket.c_str(), std::strerror(errno));
        return 1;
    }
    std::string req = "ocr";
    if (opt.speak) req += " speak=1";
    if (opt.verbose) req += " verbose=1";
    if (!opt.image.empty()) req += " image=" + opt.image;
    if (opt.snapshot_url_set) req += " snapshot_url=" + opt.snapshot_url;
    req += "\n";
    if (!write_string(fd, req)) {
        std::fprintf(stderr, "write ppocrd request failed\n");
        close(fd);
        return 1;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        write_all(STDOUT_FILENO, buf, static_cast<size_t>(n));
    }
    close(fd);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        usage(argv[0]);
        return 0;
    }

    Options opt = parse_options(argc, argv);
    if (has_flag(argc, argv, "--service")) {
        return run_service_client(opt);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    ppocr_system_app_context app;
    std::memset(&app, 0, sizeof(app));

    int ret;
    {
        StdoutSilencer quiet(!opt.verbose);
        ret = init_ppocr_model(opt.det_model.c_str(), &app.det_context);
        if (ret == 0) ret = init_ppocr_model(opt.rec_model.c_str(), &app.rec_context);
    }
    if (ret != 0) {
        std::fprintf(stderr, "init OCR model failed: det=%s rec=%s ret=%d\n",
                     opt.det_model.c_str(), opt.rec_model.c_str(), ret);
        release_ppocr_model(&app.rec_context);
        release_ppocr_model(&app.det_context);
        return 1;
    }

    if (opt.daemon) {
        int rc = run_daemon(opt, app);
        release_ppocr_model(&app.rec_context);
        release_ppocr_model(&app.det_context);
        return rc;
    }

    if (arg_value(argc, argv, "--camera")) opt.snapshot_url.clear();
    std::string text;
    bool ok = recognize_once(opt, app, &text);
    if (ok) {
        if (text.empty()) {
            std::printf("[NO_TEXT]\n");
        } else {
            std::printf("%s\n", text.c_str());
        }
        if (opt.speak) {
            speak_text(opt, text_for_speech(text));
        }
    }

    release_ppocr_model(&app.rec_context);
    release_ppocr_model(&app.det_context);
    return ok ? 0 : 1;
}

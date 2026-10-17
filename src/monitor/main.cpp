#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <poll.h>

#include "oal/codec.hpp"
#include "oal/clock.hpp"
#include "oal/config.hpp"
#include "oal/log.hpp"
#include "oal/protocol.hpp"
#include "oal/signal.hpp"
#include "oal/stats.hpp"
#include "oal/wfb_link.hpp"

using namespace oal;

namespace {

constexpr const char* kTag = "monitor";

struct MonitorOptions {
    std::string bind = "0.0.0.0";
    std::uint16_t telemetry_port = 5622;
    std::uint16_t debug_port = 5623;
    std::uint16_t control_port = 5621;
    double duration_s = 0.0;
    double stats_interval_s = 1.0;
    bool measure_latency = true;
    bool frame_log = true;
    std::string log_file;
};

void print_usage() {
    std::printf(
        "oal_monitor - passive WFB-NG link monitor and logger\n"
        "Usage: oal_monitor [--config FILE] [options]\n\n"
        "The monitor is receive-only and never transmits on the radio link.\n\n"
        "Options:\n"
        "  --bind ADDR              bind address for rx sockets (default 0.0.0.0)\n"
        "  --telemetry-port PORT    WFB-RX local port for telemetry (default 5622)\n"
        "  --debug-port PORT        WFB-RX local port for debug (default 5623)\n"
        "  --control-port PORT      WFB-RX local port for control (default 5621)\n"
        "  --log-file PATH          JSONL frame log destination (default monitor-<time>.jsonl)\n"
        "  --no-frame-log           disable per-frame logging\n"
        "  --duration SEC           stop after SEC seconds, 0 = forever\n"
        "  --stats-interval SEC     statistics reporting interval (default 1)\n"
        "  --log-level LEVEL        trace|debug|info|warn|error (default info)\n"
        "  --no-measure-latency     do not compute one-way latency\n"
        "  --help                   show this help\n");
}

bool load_options(const Config& cfg, MonitorOptions& opt) {
    const OptionResolver r(cfg, "monitor");
    opt.bind = r.string("bind", opt.bind);
    opt.telemetry_port = static_cast<std::uint16_t>(r.integer("telemetry_port", opt.telemetry_port));
    opt.debug_port = static_cast<std::uint16_t>(r.integer("debug_port", opt.debug_port));
    opt.control_port = static_cast<std::uint16_t>(r.integer("control_port", opt.control_port));
    opt.duration_s = r.number("duration", opt.duration_s);
    opt.stats_interval_s = r.number("stats_interval", opt.stats_interval_s);
    opt.measure_latency = r.boolean("measure_latency", opt.measure_latency);
    opt.frame_log = r.boolean("frame_log", opt.frame_log);
    opt.log_file = r.string("log_file", opt.log_file);
    return true;
}

std::string default_log_path() {
    char buffer[128];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(buffer, sizeof(buffer), "monitor-%Y%m%d-%H%M%S.jsonl", &tm);
    return buffer;
}

void write_log_line(std::FILE* file, const Frame& frame, const char* direction,
                    double latency_ms, bool has_latency) {
    if (file == nullptr) {
        return;
    }
    std::fprintf(file,
                 "{\"recv_ns\":%llu,\"direction\":\"%s\",\"stream\":\"%s\",\"seq\":%u,"
                 "\"flags\":%u,\"sender_ns\":%llu,\"size\":%zu,\"latency_ms\":%.3f}\n",
                 static_cast<unsigned long long>(epoch_ns()), direction,
                 stream_name(frame.header.stream).c_str(), frame.header.seq,
                 static_cast<unsigned>(frame.header.flags),
                 static_cast<unsigned long long>(frame.header.timestamp_ns), frame.size(),
                 has_latency ? latency_ms : -1.0);
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    cfg.apply_command_line(argc, argv);
    if (cfg.get_bool("help", false)) {
        print_usage();
        return 0;
    }

    const std::string config_path = cfg.get_string("config", "");
    if (!config_path.empty()) {
        std::string error;
        if (!cfg.load_file(config_path, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        cfg.apply_command_line(argc, argv);
    }

    const std::string level = cfg.get_string("log_level", cfg.get_string("common.log_level", "info"));
    if (level == "trace") {
        set_log_level(LogLevel::Trace);
    } else if (level == "debug") {
        set_log_level(LogLevel::Debug);
    } else if (level == "warn") {
        set_log_level(LogLevel::Warn);
    } else if (level == "error") {
        set_log_level(LogLevel::Error);
    } else {
        set_log_level(LogLevel::Info);
    }
    set_log_use_color(cfg.get_bool("color", true));

    MonitorOptions options;
    load_options(cfg, options);

    install_signal_handlers();

    WfbRxChannel telemetry_rx;
    WfbRxChannel debug_rx;
    WfbRxChannel control_rx;

    StreamStats telemetry_stats("telemetry");
    StreamStats debug_stats("debug");
    StreamStats control_stats("control");

    std::string error;
    const std::uint16_t ports[3] = {options.telemetry_port, options.debug_port,
                                    options.control_port};
    WfbRxChannel* channels[3] = {&telemetry_rx, &debug_rx, &control_rx};
    StreamStats* stats[3] = {&telemetry_stats, &debug_stats, &control_stats};
    const char* directions[3] = {"robot->controller", "robot->controller", "controller->robot"};

    for (int i = 0; i < 3; ++i) {
        if (!channels[i]->open(ports[i], options.bind, &error)) {
            OAL_ERROR(kTag) << "open rx port " << ports[i] << " failed: " << error;
            return 1;
        }
    }

    OAL_INFO(kTag) << "listening passive on " << options.bind << " ports t="
                   << options.telemetry_port << " d=" << options.debug_port
                   << " c=" << options.control_port;

    std::string log_path = options.log_file;
    std::FILE* log_file = nullptr;
    if (options.frame_log) {
        if (log_path.empty()) {
            log_path = default_log_path();
        }
        log_file = std::fopen(log_path.c_str(), "w");
        if (log_file == nullptr) {
            OAL_ERROR(kTag) << "cannot open log file: " << log_path;
            return 1;
        }
        OAL_INFO(kTag) << "logging frames to " << log_path;
    }

    const std::uint64_t start_ns = now_ns();
    std::uint64_t last_stats_ns = start_ns;

    std::uint64_t seen_telemetry = 0;
    std::uint64_t seen_control = 0;
    std::uint64_t crc_errors = 0;

    while (!stop_requested()) {
        const double elapsed = seconds_since(start_ns);
        if (options.duration_s > 0.0 && elapsed >= options.duration_s) {
            break;
        }

        pollfd fds[3]{};
        for (int i = 0; i < 3; ++i) {
            fds[i].fd = channels[i]->fd();
            fds[i].events = POLLIN;
        }

        const int ready = ::poll(fds, 3, 50);
        if (ready < 0) {
            continue;
        }

        for (int i = 0; i < 3; ++i) {
            if ((fds[i].revents & POLLIN) == 0) {
                continue;
            }
            Frame frame;
            int guard = 0;
            while (guard++ < 4096) {
                bool crc_error = false;
                const int result = channels[i]->recv(frame, 0, &crc_error);
                if (result <= 0) {
                    if (result < 0 && crc_error) {
                        ++crc_errors;
                    }
                    break;
                }

                const std::uint64_t wall = epoch_ns();
                bool has_latency = false;
                double latency_ms = 0.0;
                if (options.measure_latency && wall > frame.header.timestamp_ns) {
                    latency_ms = static_cast<double>(wall - frame.header.timestamp_ns) / 1e6;
                    has_latency = true;
                    stats[i]->on_rx_latency(latency_ms / 1e3);
                }
                stats[i]->on_rx(frame.header.seq, frame.size());
                write_log_line(log_file, frame, directions[i], latency_ms, has_latency);

                if (i == 0) {
                    ++seen_telemetry;
                } else if (i == 2) {
                    ++seen_control;
                }
            }
        }

        const std::uint64_t now = now_ns();
        if (now - last_stats_ns >= static_cast<std::uint64_t>(options.stats_interval_s * 1e9)) {
            last_stats_ns = now;
            OAL_INFO(kTag) << format_report(telemetry_stats.report(now));
            OAL_INFO(kTag) << format_report(control_stats.report(now));
            if (debug_stats.report(now).rx_bytes > 0) {
                OAL_INFO(kTag) << format_report(debug_stats.report(now));
            }
            const bool bidirectional = seen_telemetry > 0 && seen_control > 0;
            OAL_INFO(kTag) << "crc_errors=" << crc_errors
                           << " bidirectional=" << (bidirectional ? "yes" : "no");
        }
    }

    if (log_file != nullptr) {
        std::fflush(log_file);
        std::fclose(log_file);
    }

    const std::uint64_t now = now_ns();
    OAL_INFO(kTag) << "final telemetry: " << format_report(telemetry_stats.report(now, true));
    OAL_INFO(kTag) << "final control:   " << format_report(control_stats.report(now, true));
    OAL_INFO(kTag) << "log file: " << (log_path.empty() ? "(disabled)" : log_path);
    return 0;
}

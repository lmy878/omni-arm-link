#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

constexpr const char* kTag = "robot";

struct RobotOptions {
    double rate = 200.0;
    Endpoint telemetry_tx{"127.0.0.1", 5602};
    Endpoint debug_tx{"127.0.0.1", 5603};
    std::string control_bind = "0.0.0.0";
    std::uint16_t control_port = 5601;
    double debug_rate = 5.0;
    int control_timeout_ms = 500;
    double duration_s = 0.0;
    double stats_interval_s = 1.0;
    double print_rate = 0.0;
    bool measure_latency = true;
};

void print_usage() {
    std::printf(
        "oal_robot - competition robot telemetry transmitter\n"
        "Usage: oal_robot [--config FILE] [options]\n\n"
        "Options:\n"
        "  --rate HZ                telemetry rate (default 200)\n"
        "  --telemetry-tx HOST:PORT WFB-TX endpoint for telemetry stream (default "
        "127.0.0.1:5602)\n"
        "  --debug-tx HOST:PORT     WFB-TX endpoint for debug stream (default 127.0.0.1:5603)\n"
        "  --control-bind ADDR      bind address for incoming control (default 0.0.0.0)\n"
        "  --control-port PORT      WFB-RX local port for control stream (default 5601)\n"
        "  --debug-rate HZ          debug stream rate (default 5)\n"
        "  --control-timeout-ms MS  estop if no control within this window (default 500)\n"
        "  --duration SEC           stop after SEC seconds, 0 = forever\n"
        "  --stats-interval SEC     statistics reporting interval (default 1)\n"
        "  --print-rate HZ          print decoded received frames (0 = off)\n"
        "  --log-level LEVEL        trace|debug|info|warn|error (default info)\n"
        "  --no-measure-latency     do not compute one-way latency (requires synced clocks)\n"
        "  --help                   show this help\n");
}

bool load_options(const Config& cfg, RobotOptions& opt) {
    const OptionResolver r(cfg, "robot");
    opt.rate = r.number("rate", opt.rate);
    opt.debug_rate = r.number("debug_rate", opt.debug_rate);
    opt.control_timeout_ms = static_cast<int>(r.integer("control_timeout_ms", opt.control_timeout_ms));
    opt.duration_s = r.number("duration", opt.duration_s);
    opt.stats_interval_s = r.number("stats_interval", opt.stats_interval_s);
    opt.print_rate = r.number("print_rate", opt.print_rate);
    opt.measure_latency = r.boolean("measure_latency", opt.measure_latency);
    opt.control_bind = r.string("control_bind", opt.control_bind);
    opt.control_port = static_cast<std::uint16_t>(r.integer("control_port", opt.control_port));

    const std::string telemetry_tx = r.string("telemetry_tx", "");
    const std::string debug_tx = r.string("debug_tx", "");
    if (!telemetry_tx.empty() && !parse_endpoint(telemetry_tx, opt.telemetry_tx)) {
        OAL_ERROR(kTag) << "invalid telemetry_tx endpoint: " << telemetry_tx;
        return false;
    }
    if (!debug_tx.empty() && !parse_endpoint(debug_tx, opt.debug_tx)) {
        OAL_ERROR(kTag) << "invalid debug_tx endpoint: " << debug_tx;
        return false;
    }
    if (opt.rate <= 0.0 || opt.rate > 2000.0) {
        OAL_ERROR(kTag) << "rate out of range: " << opt.rate;
        return false;
    }
    return true;
}

void fill_telemetry(TelemetryPayload& telemetry, double seconds) {
    for (std::size_t i = 0; i < kQuaternionCount; ++i) {
        const double angle = seconds * (0.5 + 0.1 * static_cast<double>(i)) + static_cast<double>(i);
        const double half = angle * 0.5;
        telemetry.quaternions[i][0] = static_cast<float>(std::cos(half));
        telemetry.quaternions[i][1] = static_cast<float>(std::sin(half));
        telemetry.quaternions[i][2] = static_cast<float>(std::sin(half * 0.5));
        telemetry.quaternions[i][3] = static_cast<float>(std::sin(half * 0.25));
    }
    for (std::size_t i = 0; i < kLaserCount; ++i) {
        const double phase = seconds * 2.0 + static_cast<double>(i) * 0.4;
        telemetry.lasers[i] = static_cast<float>(1.5 + 0.25 * std::sin(phase) +
                                                 0.02 * static_cast<double>(i));
    }
    for (std::size_t i = 0; i < kStateCount; ++i) {
        const double phase = seconds + static_cast<double>(i) * 0.13;
        telemetry.state[i] = static_cast<float>(std::sin(phase) * (1.0 + 0.01 * static_cast<double>(i)));
    }
}

struct ControlState {
    bool valid = false;
    bool estop = false;
    std::array<float, kAxisCount> axes{};
    std::uint16_t buttons = 0;
    std::uint8_t mode = 0;
    std::array<float, kParamCount> params{};
    std::uint64_t last_rx_ns = 0;
};

bool process_control_frame(const Frame& frame, const ControlState& current, ControlState& out,
                           StreamStats& stats, bool measure_latency) {
    ControlPayload control;
    if (!decode_control(frame.payload.data(), frame.payload.size(), control)) {
        return false;
    }

    out = current;
    out.valid = true;
    out.axes = control.axes;
    out.buttons = control.buttons;
    out.mode = control.mode;
    out.params = control.params;
    out.estop = (frame.header.flags & kFlagEstop) != 0 || (control.buttons & kButtonEstop) != 0;
    out.last_rx_ns = now_ns();

    stats.on_rx(frame.header.seq, frame.size());
    if (measure_latency) {
        const std::uint64_t now = epoch_ns();
        if (now > frame.header.timestamp_ns) {
            stats.on_rx_latency(static_cast<double>(now - frame.header.timestamp_ns) / 1e9);
        }
    }
    return true;
}

void send_debug(WfbTxChannel& channel, std::uint32_t& seq, DebugLevel level,
                const std::string& text, StreamStats& stats) {
    DebugPayload debug;
    debug.level = level;
    const std::size_t length = text.size() < kDebugMaxBytes ? text.size() : kDebugMaxBytes;
    debug.length = static_cast<std::uint8_t>(length);
    std::memcpy(debug.text.data(), text.data(), length);

    std::vector<std::uint8_t> payload;
    encode_debug(debug, payload);
    if (channel.send(stream_id(Stream::Debug), kFlagNone, seq++, epoch_ns(), payload)) {
        stats.on_tx(payload.size() + kHeaderSize);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    cfg.apply_command_line(argc, argv);
    for (const auto& arg : cfg.positional()) {
        if (arg == "help" || arg == "-h") {
            print_usage();
            return 0;
        }
    }
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

    RobotOptions options;
    if (!load_options(cfg, options)) {
        return 2;
    }

    install_signal_handlers();

    WfbTxChannel telemetry_tx;
    WfbTxChannel debug_tx;
    WfbRxChannel control_rx;

    std::string error;
    if (!telemetry_tx.open(options.telemetry_tx, &error)) {
        OAL_ERROR(kTag) << "open telemetry tx failed: " << error;
        return 1;
    }
    if (!debug_tx.open(options.debug_tx, &error)) {
        OAL_ERROR(kTag) << "open debug tx failed: " << error;
        return 1;
    }
    if (!control_rx.open(options.control_port, options.control_bind, &error)) {
        OAL_ERROR(kTag) << "open control rx failed: " << error;
        return 1;
    }

    OAL_INFO(kTag) << "telemetry -> " << options.telemetry_tx.host << ":"
                   << options.telemetry_tx.port << " at " << options.rate << " Hz";
    OAL_INFO(kTag) << "control   <- " << options.control_bind << ":" << options.control_port;
    OAL_INFO(kTag) << "control timeout " << options.control_timeout_ms << " ms";

    StreamStats telemetry_stats("telemetry");
    StreamStats debug_stats("debug");
    StreamStats control_stats("control");

    RateScheduler scheduler(options.rate);
    const std::uint64_t start_ns = now_ns();
    std::uint64_t last_stats_ns = start_ns;
    std::uint64_t last_debug_ns = start_ns;
    std::uint64_t last_print_ns = start_ns;

    std::uint32_t telemetry_seq = 0;
    std::uint32_t debug_seq = 0;
    ControlState control;
    bool safe_state = true;
    const std::uint64_t control_timeout_ns =
        static_cast<std::uint64_t>(options.control_timeout_ms) * 1000000U;
    const std::uint64_t debug_period_ns =
        options.debug_rate > 0.0 ? static_cast<std::uint64_t>(1e9 / options.debug_rate) : 0;
    const std::uint64_t print_period_ns =
        options.print_rate > 0.0 ? static_cast<std::uint64_t>(1e9 / options.print_rate) : 0;

    std::vector<std::uint8_t> payload;
    payload.reserve(kTelemetryPayloadBytes);

    while (!stop_requested()) {
        scheduler.sleep_next();

        const double elapsed = seconds_since(start_ns);
        if (options.duration_s > 0.0 && elapsed >= options.duration_s) {
            break;
        }

        const std::uint64_t now = now_ns();

        Frame frame;
        int guard = 0;
        bool crc_error = false;
        while (guard++ < 4096) {
            const int result = control_rx.recv(frame, 0, &crc_error);
            if (result <= 0) {
                if (result < 0 && !crc_error) {
                    OAL_WARN(kTag) << "control rx error: " << control_rx.last_error();
                }
                break;
            }
            if (process_control_frame(frame, control, control, control_stats,
                                      options.measure_latency) &&
                print_period_ns != 0 && (now - last_print_ns) >= print_period_ns) {
                last_print_ns = now;
                OAL_INFO(kTag) << "rx control seq=" << frame.header.seq
                               << " mode=" << static_cast<unsigned>(control.mode)
                               << " buttons=" << control.buttons
                               << " estop=" << (control.estop ? 1 : 0)
                               << " axes=[" << control.axes[0] << " " << control.axes[1] << " "
                               << control.axes[2] << " " << control.axes[3] << "]";
            }
        }

        const std::uint64_t check_now = now_ns();
        const bool link_lost =
            !control.valid || (check_now - control.last_rx_ns) > control_timeout_ns;
        const bool new_safe_state = control.estop || link_lost;
        if (new_safe_state != safe_state) {
            if (new_safe_state) {
                OAL_WARN(kTag) << "entering SAFE state"
                               << (control.estop ? " (estop)" : " (control link lost)");
            } else {
                OAL_INFO(kTag) << "resuming normal operation";
            }
            safe_state = new_safe_state;
        }

        TelemetryPayload telemetry;
        fill_telemetry(telemetry, elapsed);
        encode_telemetry(telemetry, payload);

        std::uint16_t flags = kFlagNone;
        if (safe_state) {
            flags |= kFlagEstop;
        }
        if (telemetry_tx.send(stream_id(Stream::Telemetry), flags, telemetry_seq++, epoch_ns(),
                              payload)) {
            telemetry_stats.on_tx(payload.size() + kHeaderSize);
        } else {
            OAL_WARN(kTag) << "telemetry send failed: " << telemetry_tx.last_error();
        }

        if (debug_period_ns != 0 && (now - last_debug_ns) >= debug_period_ns) {
            last_debug_ns = now;
            char text[128];
            std::snprintf(text, sizeof(text),
                          "t=%.3f mode=%u estop=%d axes=[%.2f %.2f %.2f %.2f]", elapsed,
                          static_cast<unsigned>(control.mode), safe_state ? 1 : 0,
                          static_cast<double>(control.axes[0]), static_cast<double>(control.axes[1]),
                          static_cast<double>(control.axes[2]), static_cast<double>(control.axes[3]));
            send_debug(debug_tx, debug_seq, DebugLevel::Info, text, debug_stats);
        }

        if (now - last_stats_ns >= static_cast<std::uint64_t>(options.stats_interval_s * 1e9)) {
            last_stats_ns = now;
            OAL_INFO(kTag) << format_report(telemetry_stats.report(now));
            OAL_INFO(kTag) << format_report(control_stats.report(now));
            OAL_INFO(kTag) << "safe_state=" << (safe_state ? "yes" : "no")
                           << " scheduler_missed=" << scheduler.missed();
        }
    }

    OAL_INFO(kTag) << "shutting down";
    return 0;
}

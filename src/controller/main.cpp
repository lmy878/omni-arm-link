#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

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

constexpr const char* kTag = "controller";

struct ControllerOptions {
    double rate = 200.0;
    std::string telemetry_bind = "0.0.0.0";
    std::uint16_t telemetry_port = 5612;
    std::uint16_t debug_port = 5613;
    Endpoint control_tx{"127.0.0.1", 5611};
    double duration_s = 0.0;
    double stats_interval_s = 1.0;
    bool measure_latency = true;
    bool auto_motion = true;
    bool interactive = false;
    double print_rate = 0.0;
    int telemetry_timeout_ms = 500;
};

void print_usage() {
    std::printf(
        "oal_controller - competition robot ground controller\n"
        "Usage: oal_controller [--config FILE] [options]\n\n"
        "Options:\n"
        "  --rate HZ                control rate (default 200)\n"
        "  --telemetry-bind ADDR    bind address for telemetry rx (default 0.0.0.0)\n"
        "  --telemetry-port PORT    WFB-RX local port for telemetry (default 5612)\n"
        "  --debug-port PORT        WFB-RX local port for debug (default 5613)\n"
        "  --control-tx HOST:PORT   WFB-TX endpoint for control stream (default 127.0.0.1:5611)\n"
        "  --duration SEC           stop after SEC seconds, 0 = forever\n"
        "  --stats-interval SEC     statistics reporting interval (default 1)\n"
        "  --print-rate HZ          print decoded received frames (0 = off)\n"
        "  --log-level LEVEL        trace|debug|info|warn|error (default info)\n"
        "  --telemetry-timeout-ms MS warn if telemetry absent (default 500)\n"
        "  --interactive            read arm/estop/mode commands from stdin\n"
        "  --no-auto-motion         do not synthesize joystick movement\n"
        "  --no-measure-latency     do not compute one-way latency\n"
        "  --help                   show this help\n\n"
        "Interactive keys: a=arm d=disarm e=estop-toggle m=mode c=calibrate q=quit\n");
}

bool load_options(const Config& cfg, ControllerOptions& opt) {
    const OptionResolver r(cfg, "controller");
    opt.rate = r.number("rate", opt.rate);
    opt.duration_s = r.number("duration", opt.duration_s);
    opt.stats_interval_s = r.number("stats_interval", opt.stats_interval_s);
    opt.measure_latency = r.boolean("measure_latency", opt.measure_latency);
    opt.auto_motion = r.boolean("auto_motion", opt.auto_motion);
    opt.interactive = r.boolean("interactive", opt.interactive);
    opt.print_rate = r.number("print_rate", opt.print_rate);
    opt.telemetry_timeout_ms =
        static_cast<int>(r.integer("telemetry_timeout_ms", opt.telemetry_timeout_ms));
    opt.telemetry_bind = r.string("telemetry_bind", opt.telemetry_bind);
    opt.telemetry_port = static_cast<std::uint16_t>(r.integer("telemetry_port", opt.telemetry_port));
    opt.debug_port = static_cast<std::uint16_t>(r.integer("debug_port", opt.debug_port));

    const std::string control_tx = r.string("control_tx", "");
    if (!control_tx.empty() && !parse_endpoint(control_tx, opt.control_tx)) {
        OAL_ERROR(kTag) << "invalid control_tx endpoint: " << control_tx;
        return false;
    }
    if (opt.rate <= 0.0 || opt.rate > 2000.0) {
        OAL_ERROR(kTag) << "rate out of range: " << opt.rate;
        return false;
    }
    return true;
}

class StdinCommands {
public:
    void enable() {
        const int flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
            enabled_ = true;
        }
    }

    int poll_command() {
        if (!enabled_) {
            return -1;
        }
        pollfd pfd{};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 0) <= 0) {
            return -1;
        }
        char buffer[32];
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n <= 0) {
            return -1;
        }
        last_ = buffer[n - 1];
        return static_cast<unsigned char>(last_);
    }

private:
    bool enabled_ = false;
    char last_ = 0;
};

void handle_command(int command, std::uint16_t& buttons, std::uint8_t& mode, bool& estop,
                    bool& quit) {
    switch (command) {
        case 'a':
            buttons |= kButtonArm;
            buttons &= static_cast<std::uint16_t>(~kButtonDisarm);
            OAL_INFO(kTag) << "command: ARM";
            break;
        case 'd':
            buttons |= kButtonDisarm;
            buttons &= static_cast<std::uint16_t>(~kButtonArm);
            OAL_INFO(kTag) << "command: DISARM";
            break;
        case 'e':
            estop = !estop;
            OAL_WARN(kTag) << "command: ESTOP " << (estop ? "ON" : "OFF");
            break;
        case 'm':
            mode = static_cast<std::uint8_t>(
                (static_cast<unsigned>(mode) + 1U) % 4U);
            OAL_INFO(kTag) << "command: mode -> " << static_cast<unsigned>(mode);
            break;
        case 'c':
            buttons |= kButtonCalibrate;
            OAL_INFO(kTag) << "command: CALIBRATE";
            break;
        case 'q':
            quit = true;
            break;
        default:
            break;
    }
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

    ControllerOptions options;
    if (!load_options(cfg, options)) {
        return 2;
    }

    install_signal_handlers();

    WfbTxChannel control_tx;
    WfbRxChannel telemetry_rx;
    WfbRxChannel debug_rx;

    std::string error;
    if (!control_tx.open(options.control_tx, &error)) {
        OAL_ERROR(kTag) << "open control tx failed: " << error;
        return 1;
    }
    if (!telemetry_rx.open(options.telemetry_port, options.telemetry_bind, &error)) {
        OAL_ERROR(kTag) << "open telemetry rx failed: " << error;
        return 1;
    }
    if (!debug_rx.open(options.debug_port, options.telemetry_bind, &error)) {
        OAL_ERROR(kTag) << "open debug rx failed: " << error;
        return 1;
    }

    OAL_INFO(kTag) << "telemetry <- " << options.telemetry_bind << ":" << options.telemetry_port;
    OAL_INFO(kTag) << "debug     <- " << options.telemetry_bind << ":" << options.debug_port;
    OAL_INFO(kTag) << "control   -> " << options.control_tx.host << ":" << options.control_tx.port;

    StreamStats telemetry_stats("telemetry");
    StreamStats debug_stats("debug");
    StreamStats control_stats("control");

    StdinCommands stdin_commands;
    if (options.interactive) {
        stdin_commands.enable();
        OAL_INFO(kTag) << "interactive mode: a=arm d=disarm e=estop m=mode c=calibrate q=quit";
    }

    RateScheduler scheduler(options.rate);
    const std::uint64_t start_ns = now_ns();
    std::uint64_t last_stats_ns = start_ns;
    std::uint64_t last_telemetry_ns = start_ns;
    std::uint64_t last_print_ns = start_ns;
    const std::uint64_t print_period_ns =
        options.print_rate > 0.0 ? static_cast<std::uint64_t>(1e9 / options.print_rate) : 0;

    std::uint32_t control_seq = 0;
    std::uint16_t buttons = 0;
    std::uint8_t mode = static_cast<std::uint8_t>(ControlMode::Manual);
    bool estop = false;

    std::vector<std::uint8_t> payload;
    payload.reserve(kControlPayloadBytes);

    while (!stop_requested()) {
        scheduler.sleep_next();

        const double elapsed = seconds_since(start_ns);
        if (options.duration_s > 0.0 && elapsed >= options.duration_s) {
            break;
        }

        const std::uint64_t now = now_ns();

        Frame frame;
        int guard = 0;
        while (guard++ < 4096) {
            const int result = telemetry_rx.recv(frame, 0);
            if (result <= 0) {
                break;
            }
            telemetry_stats.on_rx(frame.header.seq, frame.size());
            last_telemetry_ns = now;
            if (options.measure_latency) {
                const std::uint64_t wall = epoch_ns();
                if (wall > frame.header.timestamp_ns) {
                    telemetry_stats.on_rx_latency(
                        static_cast<double>(wall - frame.header.timestamp_ns) / 1e9);
                }
            }
            if (print_period_ns != 0 && (now - last_print_ns) >= print_period_ns) {
                TelemetryPayload telemetry;
                if (decode_telemetry(frame.payload.data(), frame.payload.size(), telemetry)) {
                    last_print_ns = now;
                    OAL_INFO(kTag) << "rx telemetry seq=" << frame.header.seq;
                    for (std::size_t i = 0; i < kQuaternionCount; ++i) {
                        OAL_INFO(kTag) << "  q[" << i << "]=[" << telemetry.quaternions[i][0]
                                       << " " << telemetry.quaternions[i][1] << " "
                                       << telemetry.quaternions[i][2] << " "
                                       << telemetry.quaternions[i][3] << "]";
                    }
                    std::ostringstream lasers;
                    lasers << "  lasers=[";
                    for (std::size_t i = 0; i < kLaserCount; ++i) {
                        lasers << (i ? " " : "") << telemetry.lasers[i];
                    }
                    lasers << "]";
                    OAL_INFO(kTag) << lasers.str();
                    std::ostringstream states;
                    states << "  state=[";
                    for (std::size_t i = 0; i < kStateCount; ++i) {
                        states << (i ? " " : "") << telemetry.state[i];
                    }
                    states << "]";
                    OAL_INFO(kTag) << states.str();
                }
            }
        }

        guard = 0;
        while (guard++ < 4096) {
            const int result = debug_rx.recv(frame, 0);
            if (result <= 0) {
                break;
            }
            debug_stats.on_rx(frame.header.seq, frame.size());
            DebugPayload debug;
            if (decode_debug(frame.payload.data(), frame.payload.size(), debug)) {
                if (options.print_rate > 0.0) {
                    OAL_INFO(kTag) << "rx debug: "
                                   << std::string(debug.text.data(), debug.length);
                } else {
                    OAL_DEBUG("robot") << std::string(debug.text.data(), debug.length);
                }
            }
        }

        if (options.interactive) {
            int command = 0;
            while ((command = stdin_commands.poll_command()) >= 0) {
                bool quit = false;
                handle_command(command, buttons, mode, estop, quit);
                if (quit) {
                    request_stop();
                }
            }
        }

        ControlPayload control;
        control.mode = mode;
        control.buttons = buttons;
        control.flags = estop ? static_cast<std::uint8_t>(kFlagEstop) : 0U;
        if (options.auto_motion && !estop) {
            control.axes[0] = static_cast<float>(0.6 * std::sin(elapsed * 1.3));
            control.axes[1] = static_cast<float>(0.6 * std::cos(elapsed * 1.1));
            control.axes[2] = static_cast<float>(0.3 * std::sin(elapsed * 0.7));
            control.axes[3] = 0.5f;
        }
        control.params[0] = 1.0f;

        encode_control(control, payload);
        std::uint16_t flags = estop ? kFlagEstop : kFlagNone;
        if (control_tx.send(stream_id(Stream::Control), flags, control_seq++, epoch_ns(), payload)) {
            control_stats.on_tx(payload.size() + kHeaderSize);
        } else {
            OAL_WARN(kTag) << "control send failed: " << control_tx.last_error();
        }

        const bool telemetry_lost =
            (now - last_telemetry_ns) >
            static_cast<std::uint64_t>(options.telemetry_timeout_ms) * 1000000U;
        if (telemetry_lost && (now - last_stats_ns) >= 1000000000ULL) {
            OAL_WARN(kTag) << "telemetry link idle for >" << options.telemetry_timeout_ms << " ms";
        }

        if (now - last_stats_ns >= static_cast<std::uint64_t>(options.stats_interval_s * 1e9)) {
            last_stats_ns = now;
            OAL_INFO(kTag) << format_report(telemetry_stats.report(now));
            OAL_INFO(kTag) << format_report(control_stats.report(now));
            OAL_INFO(kTag) << "mode=" << static_cast<unsigned>(mode)
                           << " estop=" << (estop ? "yes" : "no")
                           << " scheduler_missed=" << scheduler.missed();
        }
    }

    OAL_INFO(kTag) << "shutting down";
    return 0;
}

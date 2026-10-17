#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "oal/codec.hpp"
#include "oal/clock.hpp"
#include "oal/config.hpp"
#include "oal/log.hpp"
#include "oal/protocol.hpp"
#include "oal/signal.hpp"
#include "oal/stats.hpp"
#include "oal/udp_socket.hpp"
#include "oal/wfb_link.hpp"

using namespace oal;

namespace {

constexpr const char* kTag = "sim";

struct Impairments {
    double loss = 0.0;
    double duplicate = 0.0;
    double reorder = 0.0;
    double jitter_ms = 0.0;
    double delay_ms = 0.0;
};

class PacketSink {
public:
    explicit PacketSink(Impairments imp, std::uint32_t seed = 0xC0FFEE)
        : impairments_(imp), rng_(seed) {}

    bool add_target(const Endpoint& endpoint, std::string* error) {
        Target target;
        target.endpoint = endpoint;
        if (!target.socket.connect(endpoint.host, endpoint.port, error)) {
            return false;
        }
        target.socket.set_send_buffer(1 << 20);
        targets_.push_back(std::move(target));
        return true;
    }

    void deliver(const std::vector<std::uint8_t>& data) {
        for (auto& target : targets_) {
            if (impairments_.loss > 0.0 && chance(impairments_.loss)) {
                ++dropped_;
                continue;
            }
            const std::int64_t copies = chance(impairments_.duplicate) ? 2 : 1;
            for (std::int64_t i = 0; i < copies; ++i) {
                enqueue(target, data);
            }
        }
    }

    void flush() {
        const std::uint64_t now = now_ns();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->due_ns <= now) {
                it->target->socket.send(it->data.data(), it->data.size());
                ++sent_;
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::uint64_t sent() const { return sent_; }
    std::uint64_t dropped() const { return dropped_; }
    std::size_t queued() const { return pending_.size(); }

private:
    struct Target {
        Endpoint endpoint;
        UdpSocket socket;
    };

    struct Pending {
        std::uint64_t due_ns;
        Target* target;
        std::vector<std::uint8_t> data;
    };

    bool chance(double probability) {
        if (probability <= 0.0) {
            return false;
        }
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(rng_) < probability;
    }

    void enqueue(Target& target, const std::vector<std::uint8_t>& data) {
        double delay = impairments_.delay_ms;
        if (impairments_.jitter_ms > 0.0) {
            std::uniform_real_distribution<double> dist(0.0, impairments_.jitter_ms);
            delay += dist(rng_);
        }
        if (impairments_.reorder > 0.0 && chance(impairments_.reorder)) {
            delay += 20.0;
        }
        const std::uint64_t due = now_ns() + static_cast<std::uint64_t>(delay * 1e6);
        pending_.push_back(Pending{due, &target, data});
    }

    Impairments impairments_;
    std::mt19937 rng_;
    std::vector<Target> targets_;
    std::vector<Pending> pending_;
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
};

void print_usage() {
    std::printf(
        "oal_sim - application-level robot/controller simulator (no radio required)\n"
        "Usage: oal_sim <robot|controller> [options]\n\n"
        "Common options:\n"
        "  --dest HOST:PORT         destination (repeatable)\n"
        "  --rate HZ                packet rate (default 200)\n"
        "  --duration SEC           stop after SEC seconds, 0 = forever\n"
        "  --stats-interval SEC     reporting interval (default 1)\n"
        "  --loss P                 drop probability\n"
        "  --duplicate P            duplicate probability\n"
        "  --reorder P              reorder probability\n"
        "  --jitter-ms MS           random extra delay\n"
        "  --delay-ms MS            fixed link delay\n"
        "  --log-level LEVEL        trace|debug|info|warn|error\n"
        "  --help                   show this help\n\n"
        "robot options:\n"
        "  --debug-rate HZ          debug stream rate (default 5)\n\n"
        "controller options:\n"
        "  --estop                  send estop from the start\n"
        "  --mode N                 control mode value (default 1)\n");
}

void fill_telemetry(TelemetryPayload& telemetry, double seconds) {
    for (std::size_t i = 0; i < kQuaternionCount; ++i) {
        const double half = (seconds + static_cast<double>(i)) * 0.5;
        telemetry.quaternions[i][0] = static_cast<float>(std::cos(half));
        telemetry.quaternions[i][1] = static_cast<float>(std::sin(half));
    }
    for (std::size_t i = 0; i < kLaserCount; ++i) {
        telemetry.lasers[i] =
            static_cast<float>(2.0 + std::sin(seconds * 2.0 + static_cast<double>(i)));
    }
    for (std::size_t i = 0; i < kStateCount; ++i) {
        telemetry.state[i] = static_cast<float>(std::sin(seconds + static_cast<double>(i) * 0.1));
    }
}

bool parse_destinations(const Config& cfg, std::vector<Endpoint>& out) {
    for (const auto& [key, value] : cfg.entries()) {
        if (key == "dest") {
            Endpoint endpoint;
            if (!parse_endpoint(value, endpoint)) {
                OAL_ERROR(kTag) << "invalid --dest: " << value;
                return false;
            }
            out.push_back(endpoint);
        }
    }
    return true;
}

Impairments load_impairments(const Config& cfg) {
    Impairments imp;
    imp.loss = cfg.get_double("loss", 0.0);
    imp.duplicate = cfg.get_double("duplicate", 0.0);
    imp.reorder = cfg.get_double("reorder", 0.0);
    imp.jitter_ms = cfg.get_double("jitter_ms", 0.0);
    imp.delay_ms = cfg.get_double("delay_ms", 0.0);
    return imp;
}

void configure_logging(const Config& cfg) {
    const std::string level = cfg.get_string("log_level", "info");
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
}

int run_robot(const Config& cfg) {
    std::vector<Endpoint> destinations;
    if (!parse_destinations(cfg, destinations) || destinations.empty()) {
        OAL_ERROR(kTag) << "robot mode requires at least one --dest HOST:PORT";
        return 2;
    }

    const double rate = cfg.get_double("rate", 200.0);
    const double debug_rate = cfg.get_double("debug_rate", 5.0);
    const double duration_s = cfg.get_double("duration", 0.0);
    const double stats_interval_s = cfg.get_double("stats_interval", 1.0);

    PacketSink sink(load_impairments(cfg));

    std::string error;
    for (const auto& dest : destinations) {
        if (!sink.add_target(dest, &error)) {
            OAL_ERROR(kTag) << "cannot open target " << dest.host << ":" << dest.port << ": "
                            << error;
            return 1;
        }
        OAL_INFO(kTag) << "telemetry -> " << dest.host << ":" << dest.port;
    }

    StreamStats stats("telemetry-tx");
    RateScheduler scheduler(rate);
    const std::uint64_t start_ns = now_ns();
    std::uint64_t last_stats_ns = start_ns;
    std::uint64_t last_debug_ns = start_ns;
    std::uint32_t seq = 0;
    std::uint32_t debug_seq = 0;
    std::vector<std::uint8_t> payload;
    const std::uint64_t debug_period_ns =
        debug_rate > 0.0 ? static_cast<std::uint64_t>(1e9 / debug_rate) : 0;

    while (!stop_requested()) {
        scheduler.sleep_next();
        const double elapsed = seconds_since(start_ns);
        if (duration_s > 0.0 && elapsed >= duration_s) {
            break;
        }

        TelemetryPayload telemetry;
        fill_telemetry(telemetry, elapsed);
        encode_telemetry(telemetry, payload);

        Frame frame;
        frame.header.stream = stream_id(Stream::Telemetry);
        frame.header.seq = seq++;
        frame.header.timestamp_ns = epoch_ns();
        frame.payload = payload;
        std::vector<std::uint8_t> wire;
        encode_frame(frame, wire);
        sink.deliver(wire);
        stats.on_tx(wire.size());

        const std::uint64_t now = now_ns();
        if (debug_period_ns != 0 && now - last_debug_ns >= debug_period_ns) {
            last_debug_ns = now;
            DebugPayload debug;
            char text[96];
            std::snprintf(text, sizeof(text), "sim robot t=%.3f", elapsed);
            debug.length = static_cast<std::uint8_t>(std::strlen(text));
            std::memcpy(debug.text.data(), text, debug.length);
            std::vector<std::uint8_t> debug_payload;
            encode_debug(debug, debug_payload);
            Frame debug_frame;
            debug_frame.header.stream = stream_id(Stream::Debug);
            debug_frame.header.seq = debug_seq++;
            debug_frame.header.timestamp_ns = epoch_ns();
            debug_frame.payload = debug_payload;
            std::vector<std::uint8_t> debug_wire;
            encode_frame(debug_frame, debug_wire);
            sink.deliver(debug_wire);
        }

        sink.flush();

        if (now - last_stats_ns >= static_cast<std::uint64_t>(stats_interval_s * 1e9)) {
            last_stats_ns = now;
            OAL_INFO(kTag) << format_report(stats.report(now));
            OAL_INFO(kTag) << "sent=" << sink.sent() << " dropped=" << sink.dropped()
                           << " scheduler_missed=" << scheduler.missed();
        }
    }

    OAL_INFO(kTag) << "shutting down";
    return 0;
}

int run_controller(const Config& cfg) {
    std::vector<Endpoint> destinations;
    if (!parse_destinations(cfg, destinations) || destinations.empty()) {
        OAL_ERROR(kTag) << "controller mode requires at least one --dest HOST:PORT";
        return 2;
    }

    const double rate = cfg.get_double("rate", 200.0);
    const double duration_s = cfg.get_double("duration", 0.0);
    const double stats_interval_s = cfg.get_double("stats_interval", 1.0);
    const bool estop = cfg.get_bool("estop", false);
    const std::uint8_t mode = static_cast<std::uint8_t>(cfg.get_int("mode", 1));

    PacketSink sink(load_impairments(cfg));
    std::string error;
    for (const auto& dest : destinations) {
        if (!sink.add_target(dest, &error)) {
            OAL_ERROR(kTag) << "cannot open target " << dest.host << ":" << dest.port << ": "
                            << error;
            return 1;
        }
        OAL_INFO(kTag) << "control -> " << dest.host << ":" << dest.port;
    }

    StreamStats stats("control-tx");
    RateScheduler scheduler(rate);
    const std::uint64_t start_ns = now_ns();
    std::uint64_t last_stats_ns = start_ns;
    std::uint32_t seq = 0;

    while (!stop_requested()) {
        scheduler.sleep_next();
        const double elapsed = seconds_since(start_ns);
        if (duration_s > 0.0 && elapsed >= duration_s) {
            break;
        }

        ControlPayload control;
        control.mode = mode;
        control.flags = estop ? static_cast<std::uint8_t>(kFlagEstop) : 0U;
        control.buttons = estop ? static_cast<std::uint16_t>(kButtonEstop) : 0U;
        if (!estop) {
            control.axes[0] = static_cast<float>(0.5 * std::sin(elapsed));
            control.axes[1] = static_cast<float>(0.5 * std::cos(elapsed));
        }

        std::vector<std::uint8_t> payload;
        encode_control(control, payload);
        Frame frame;
        frame.header.stream = stream_id(Stream::Control);
        frame.header.seq = seq++;
        frame.header.timestamp_ns = epoch_ns();
        frame.header.flags = estop ? kFlagEstop : kFlagNone;
        frame.payload = payload;
        std::vector<std::uint8_t> wire;
        encode_frame(frame, wire);
        sink.deliver(wire);
        stats.on_tx(wire.size());
        sink.flush();

        const std::uint64_t now = now_ns();
        if (now - last_stats_ns >= static_cast<std::uint64_t>(stats_interval_s * 1e9)) {
            last_stats_ns = now;
            OAL_INFO(kTag) << format_report(stats.report(now));
            OAL_INFO(kTag) << "sent=" << sink.sent() << " dropped=" << sink.dropped();
        }
    }

    OAL_INFO(kTag) << "shutting down";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    cfg.apply_command_line(argc, argv);

    if (cfg.get_bool("help", false)) {
        print_usage();
        return 0;
    }
    if (cfg.positional().empty()) {
        print_usage();
        return 2;
    }

    const std::string mode = cfg.positional().front();
    if (mode != "robot" && mode != "controller") {
        OAL_ERROR(kTag) << "unknown mode: " << mode;
        print_usage();
        return 2;
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

    configure_logging(cfg);
    install_signal_handlers();

    return mode == "robot" ? run_robot(cfg) : run_controller(cfg);
}

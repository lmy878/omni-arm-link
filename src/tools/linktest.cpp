#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include "oal/codec.hpp"
#include "oal/clock.hpp"
#include "oal/config.hpp"
#include "oal/crc.hpp"
#include "oal/log.hpp"
#include "oal/protocol.hpp"
#include "oal/signal.hpp"
#include "oal/stats.hpp"
#include "oal/wfb_link.hpp"

using namespace oal;

namespace {

constexpr const char* kTag = "linktest";
constexpr std::uint8_t kPing = 0;
constexpr std::uint8_t kPong = 1;

struct Options {
    enum class Role { Controller, Robot, None };
    Role role = Role::None;
    std::uint8_t tx_stream = 0;
    std::uint8_t rx_stream = 0;
    Endpoint tx_endpoint{"127.0.0.1", 5631};
    std::uint16_t rx_port = 5632;
    std::string rx_bind = "0.0.0.0";
    double rate = 200.0;
    std::size_t payload_size = 1;
    double duration_s = 0.0;
    double stats_interval_s = 1.0;
    std::string report_path;
    bool self_test = false;
};

void print_usage() {
    std::printf(
        "oal_linktest - WFB-NG bidirectional link latency/loss test\n"
        "Usage: oal_linktest [--config FILE] [options]\n\n"
        "Roles (set stream ids and defaults):\n"
        "  --role controller        initiator, tx=0xFE, rx=0x7E, tx=127.0.0.1:5631, rx=5632\n"
        "  --role robot             responder, tx=0x7E, rx=0xFE, tx=127.0.0.1:5632, rx=5631\n\n"
        "Options:\n"
        "  --tx-stream N            transmit stream id (decimal or 0x..)\n"
        "  --rx-stream N            receive stream id\n"
        "  --tx-endpoint HOST:PORT  WFB-TX listen endpoint to send pings/pongs to\n"
        "  --rx-port PORT           local port where WFB-RX delivers frames\n"
        "  --rx-bind ADDR           bind address for rx socket (role default: 127.0.0.2)\n"
        "  --rate HZ                probe rate (default 200)\n"
        "  --size BYTES             probe payload size, padded (default 1)\n"
        "  --duration SEC           stop after SEC seconds, 0 = forever\n"
        "  --stats-interval SEC     reporting interval (default 1)\n"
        "  --report PATH            write a final machine-readable report to PATH\n"
        "  --self-test              run offline codec/CRC round-trip test and exit\n"
        "  --help                   show this help\n");
}

std::uint8_t parse_stream(const std::string& value, std::uint8_t fallback) {
    try {
        return static_cast<std::uint8_t>(std::stoul(value, nullptr, 0));
    } catch (...) {
        return fallback;
    }
}

bool load_options(Config& cfg, Options& opt) {
    const std::string role = cfg.get_string("role", "");
    if (role == "controller") {
        opt.role = Options::Role::Controller;
        opt.tx_stream = stream_id(Stream::TestUp);
        opt.rx_stream = stream_id(Stream::TestDown);
        opt.tx_endpoint = Endpoint{"127.0.0.1", 5631};
        opt.rx_port = 5632;
        opt.rx_bind = "127.0.0.2";
    } else if (role == "robot") {
        opt.role = Options::Role::Robot;
        opt.tx_stream = stream_id(Stream::TestDown);
        opt.rx_stream = stream_id(Stream::TestUp);
        opt.tx_endpoint = Endpoint{"127.0.0.1", 5632};
        opt.rx_port = 5631;
        opt.rx_bind = "127.0.0.2";
    }

    opt.tx_stream = parse_stream(cfg.get_string("tx_stream", ""), opt.tx_stream);
    opt.rx_stream = parse_stream(cfg.get_string("rx_stream", ""), opt.rx_stream);
    opt.rate = cfg.get_double("rate", opt.rate);
    opt.duration_s = cfg.get_double("duration", opt.duration_s);
    opt.stats_interval_s = cfg.get_double("stats_interval", opt.stats_interval_s);
    opt.report_path = cfg.get_string("report", opt.report_path);
    opt.rx_bind = cfg.get_string("rx_bind", opt.rx_bind);
    opt.self_test = cfg.get_bool("self_test", opt.self_test);
    opt.rx_port = static_cast<std::uint16_t>(cfg.get_int("rx_port", opt.rx_port));
    opt.payload_size = static_cast<std::size_t>(cfg.get_int("size", 1));
    if (opt.payload_size < 1) {
        opt.payload_size = 1;
    }
    if (opt.payload_size > kWfbMaxPacketBytes) {
        opt.payload_size = kWfbMaxPacketBytes;
    }

    const std::string tx_endpoint = cfg.get_string("tx_endpoint", "");
    if (!tx_endpoint.empty() && !parse_endpoint(tx_endpoint, opt.tx_endpoint)) {
        OAL_ERROR(kTag) << "invalid tx-endpoint: " << tx_endpoint;
        return false;
    }

    return true;
}

bool write_report(const std::string& path, const Options& options, double duration_s,
                  std::uint64_t sent, std::uint64_t received, std::uint64_t tx_bytes,
                  const StreamReport& rx, std::uint64_t scheduler_missed) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }

    const bool initiator = options.role != Options::Role::Robot;
    out << std::fixed << std::setprecision(3);
    out << "role=" << (initiator ? "controller" : "robot") << '\n';
    out << "initiator=" << (initiator ? 1 : 0) << '\n';
    out << "duration_s=" << duration_s << '\n';
    out << "sent=" << sent << '\n';
    out << "received=" << received << '\n';
    out << "tx_bytes=" << tx_bytes << '\n';
    out << "rx_bytes=" << rx.rx_bytes << '\n';
    out << "lost=" << rx.seq.lost << '\n';
    out << "duplicates=" << rx.seq.duplicates << '\n';
    out << "reordered=" << rx.seq.reordered << '\n';
    out << "loss_rate=" << rx.seq.loss_rate << '\n';
    out << "rtt_available=" << (rx.has_latency ? 1 : 0) << '\n';
    out << "rtt_samples=" << rx.latency.samples << '\n';
    out << "rtt_min_ms=" << rx.latency.min_s * 1e3 << '\n';
    out << "rtt_mean_ms=" << rx.latency.mean_s * 1e3 << '\n';
    out << "rtt_p50_ms=" << rx.latency.p50_s * 1e3 << '\n';
    out << "rtt_p95_ms=" << rx.latency.p95_s * 1e3 << '\n';
    out << "rtt_p99_ms=" << rx.latency.p99_s * 1e3 << '\n';
    out << "rtt_max_ms=" << rx.latency.max_s * 1e3 << '\n';
    out << "rtt_jitter_ms=" << rx.latency.jitter_s * 1e3 << '\n';
    out << "rate_hz=" << options.rate << '\n';
    out << "size_bytes=" << options.payload_size << '\n';
    out << "scheduler_missed=" << scheduler_missed << '\n';
    return static_cast<bool>(out);
}

int run_self_test() {
    int failures = 0;

    ControlPayload control;
    control.axes = {1.0f, -2.5f, 0.25f, 0.0f};
    control.buttons = 0x0111;
    control.mode = 2;
    control.params = {1.1f, 2.2f, 3.3f, 4.4f};
    std::vector<std::uint8_t> buffer;
    encode_control(control, buffer);
    ControlPayload control_out;
    if (!decode_control(buffer.data(), buffer.size(), control_out) ||
        control_out.buttons != control.buttons || control_out.mode != control.mode) {
        std::fprintf(stderr, "control round-trip failed\n");
        ++failures;
    }

    TelemetryPayload telemetry;
    telemetry.lasers[7] = 3.75f;
    telemetry.state[49] = -1.5f;
    encode_telemetry(telemetry, buffer);
    if (buffer.size() != kTelemetryPayloadBytes) {
        std::fprintf(stderr, "telemetry size mismatch: %zu\n", buffer.size());
        ++failures;
    }
    TelemetryPayload telemetry_out;
    if (!decode_telemetry(buffer.data(), buffer.size(), telemetry_out) ||
        telemetry_out.lasers[7] != 3.75f || telemetry_out.state[49] != -1.5f) {
        std::fprintf(stderr, "telemetry round-trip failed\n");
        ++failures;
    }

    DebugPayload debug;
    debug.level = DebugLevel::Warn;
    debug.length = 5;
    std::memcpy(debug.text.data(), "hello", 5);
    encode_debug(debug, buffer);
    DebugPayload debug_out;
    if (!decode_debug(buffer.data(), buffer.size(), debug_out) || debug_out.length != 5 ||
        std::memcmp(debug_out.text.data(), "hello", 5) != 0) {
        std::fprintf(stderr, "debug round-trip failed\n");
        ++failures;
    }

    Frame frame;
    frame.header.stream = stream_id(Stream::Telemetry);
    frame.header.seq = 42;
    frame.header.timestamp_ns = 123456789ULL;
    encode_telemetry(telemetry, buffer);
    frame.payload = buffer;
    std::vector<std::uint8_t> wire;
    encode_frame(frame, wire);

    Frame decoded;
    std::string error;
    if (!decode_frame(wire.data(), wire.size(), decoded, &error) ||
        decoded.header.seq != 42 || decoded.header.timestamp_ns != 123456789ULL) {
        std::fprintf(stderr, "frame round-trip failed: %s\n", error.c_str());
        ++failures;
    }

    std::vector<std::uint8_t> corrupted = wire;
    corrupted.back() ^= 0xFFU;
    if (decode_frame(corrupted.data(), corrupted.size(), decoded, &error)) {
        std::fprintf(stderr, "corrupted frame was accepted\n");
        ++failures;
    }

    if (crc32("123456789", 9) != 0xCBF43926U) {
        std::fprintf(stderr, "crc32 known-answer failed\n");
        ++failures;
    }

    SeqTracker tracker;
    for (std::uint32_t seq = 0; seq < 100; ++seq) {
        if (seq % 10 != 3) {
            tracker.update(seq);
        }
    }
    if (tracker.snapshot().lost != 10) {
        std::fprintf(stderr, "seq tracker loss count mismatch: %llu\n",
                     static_cast<unsigned long long>(tracker.snapshot().lost));
        ++failures;
    }

    if (failures == 0) {
        std::printf("self-test passed\n");
    }
    return failures == 0 ? 0 : 1;
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

    Options options;
    if (!load_options(cfg, options)) {
        return 2;
    }

    if (options.self_test) {
        return run_self_test();
    }

    const std::string level = cfg.get_string("log_level", "info");
    if (level == "debug") {
        set_log_level(LogLevel::Debug);
    } else if (level == "trace") {
        set_log_level(LogLevel::Trace);
    } else if (level == "warn") {
        set_log_level(LogLevel::Warn);
    } else {
        set_log_level(LogLevel::Info);
    }
    set_log_use_color(cfg.get_bool("color", true));

    install_signal_handlers();

    const bool initiator = options.role != Options::Role::Robot;

    WfbTxChannel tx;
    WfbRxChannel rx;
    std::string error;
    if (!tx.open(options.tx_endpoint, &error)) {
        OAL_ERROR(kTag) << "open tx failed: " << error;
        return 1;
    }
    rx.set_reuse_address(true);
    if (!rx.open(options.rx_port, options.rx_bind, &error)) {
        OAL_ERROR(kTag) << "open rx failed: " << error;
        return 1;
    }

    OAL_INFO(kTag) << (initiator ? "initiator" : "responder") << " tx_stream="
                   << static_cast<unsigned>(options.tx_stream) << " -> "
                   << options.tx_endpoint.host << ":" << options.tx_endpoint.port
                   << ", rx_stream=" << static_cast<unsigned>(options.rx_stream) << " on port "
                   << options.rx_port << ", rate=" << options.rate << " Hz, size="
                   << options.payload_size;

    StreamStats tx_stats("tx");
    StreamStats reply_window("reply");
    StreamStats reply_total("reply");
    StreamStats request_window("request");
    StreamStats request_total("request");

    std::vector<std::uint8_t> ping(options.payload_size, 0);
    ping[0] = kPing;
    std::vector<std::uint8_t> pong(options.payload_size, 0);
    pong[0] = kPong;

    RateScheduler scheduler(options.rate);
    const std::uint64_t start_ns = now_ns();
    std::uint64_t last_stats_ns = start_ns;
    std::uint32_t seq = 0;
    std::uint64_t received_pongs = 0;
    std::uint64_t sent_pings = 0;
    std::uint64_t echoed_pongs = 0;

    while (!stop_requested()) {
        scheduler.sleep_next();

        const double elapsed = seconds_since(start_ns);
        if (options.duration_s > 0.0 && elapsed >= options.duration_s) {
            break;
        }

        Frame frame;
        int guard = 0;
        while (guard++ < 8192) {
            const int result = rx.recv(frame, 0);
            if (result <= 0) {
                break;
            }
            if (frame.payload.empty()) {
                continue;
            }
            const std::uint8_t kind = frame.payload[0];
            if (kind == kPing) {
                request_window.on_rx(frame.header.seq, frame.size());
                request_total.on_rx(frame.header.seq, frame.size());
                std::vector<std::uint8_t> echo = pong;
                if (tx.send(options.tx_stream, kFlagNone, frame.header.seq,
                            frame.header.timestamp_ns, echo)) {
                    tx_stats.on_tx(echo.size() + kHeaderSize);
                    ++echoed_pongs;
                }
            } else if (kind == kPong) {
                ++received_pongs;
                reply_window.on_rx(frame.header.seq, frame.size());
                reply_total.on_rx(frame.header.seq, frame.size());
                const std::uint64_t now = epoch_ns();
                if (now > frame.header.timestamp_ns) {
                    const double rtt = static_cast<double>(now - frame.header.timestamp_ns) / 1e9;
                    reply_window.on_rx_latency(rtt);
                    reply_total.on_rx_latency(rtt);
                }
            }
        }

        if (initiator) {
            if (tx.send(options.tx_stream, kFlagNone, seq++, epoch_ns(), ping)) {
                tx_stats.on_tx(ping.size() + kHeaderSize);
                ++sent_pings;
            }
        }

        const std::uint64_t now = now_ns();
        if (now - last_stats_ns >= static_cast<std::uint64_t>(options.stats_interval_s * 1e9)) {
            last_stats_ns = now;
            const StreamReport periodic = initiator ? reply_window.report(now, true)
                                                    : request_window.report(now, true);
            OAL_INFO(kTag) << format_report(periodic);
            OAL_INFO(kTag) << (initiator ? "pings_sent" : "pongs_echoed") << "=" << tx.packets()
                           << " pongs=" << received_pongs << " rx_total=" << rx.packets()
                           << " scheduler_missed=" << scheduler.missed();
        }
    }

    const std::uint64_t end_ns = now_ns();
    const double duration_s = seconds_since(start_ns);
    const StreamReport final_rx = initiator ? reply_total.report(end_ns, false)
                                            : request_total.report(end_ns, false);
    const std::uint64_t sent = initiator ? sent_pings : echoed_pongs;
    OAL_INFO(kTag) << "final " << format_report(final_rx);
    OAL_INFO(kTag) << "sent=" << sent << " received=" << final_rx.seq.received
                   << " duration=" << duration_s << "s scheduler_missed=" << scheduler.missed();

    if (!options.report_path.empty()) {
        if (write_report(options.report_path, options, duration_s, sent, final_rx.seq.received,
                         tx.bytes(), final_rx, scheduler.missed())) {
            OAL_INFO(kTag) << "report written to " << options.report_path;
        } else {
            OAL_WARN(kTag) << "failed to write report: " << options.report_path;
        }
    }

    OAL_INFO(kTag) << "shutting down";
    return 0;
}

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace oal {

class RateMeter {
public:
    void add(std::uint64_t count = 1) {
        total_ += count;
        window_ += count;
    }

    std::uint64_t total() const { return total_; }

    double sample_rate(std::uint64_t now_ns) {
        const double elapsed = window_start_ns_ == 0
                                   ? 0.0
                                   : static_cast<double>(now_ns - window_start_ns_) / 1e9;
        window_start_ns_ = now_ns;
        const double rate = elapsed > 0.0 ? static_cast<double>(window_) / elapsed : 0.0;
        window_ = 0;
        return rate;
    }

private:
    std::uint64_t total_ = 0;
    std::uint64_t window_ = 0;
    std::uint64_t window_start_ns_ = 0;
};

struct SeqSnapshot {
    std::uint64_t received = 0;
    std::uint64_t lost = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t reordered = 0;
    std::uint64_t late = 0;
    std::uint64_t resyncs = 0;
    double loss_rate = 0.0;
};

class SeqTracker {
public:
    void update(std::uint32_t seq);
    SeqSnapshot snapshot() const { return snapshot_; }
    void reset() { *this = SeqTracker(); }

private:
    SeqSnapshot snapshot_;
    bool initialized_ = false;
    std::uint32_t highest_ = 0;
    static constexpr std::int64_t kResyncThreshold = 1000;
};

struct LatencySnapshot {
    std::uint64_t samples = 0;
    double min_s = 0.0;
    double max_s = 0.0;
    double mean_s = 0.0;
    double p50_s = 0.0;
    double p95_s = 0.0;
    double p99_s = 0.0;
    double jitter_s = 0.0;
};

class LatencyWindow {
public:
    explicit LatencyWindow(std::size_t capacity = 8192) : capacity_(capacity) {}

    void add(double seconds);
    void clear() { samples_.clear(); }
    std::size_t count() const { return samples_.size(); }

    LatencySnapshot snapshot() const;

private:
    std::size_t capacity_;
    std::deque<double> samples_;
};

struct StreamReport {
    std::string name;
    double window_seconds = 0.0;
    double rx_pps = 0.0;
    double rx_kbps = 0.0;
    double tx_pps = 0.0;
    double tx_kbps = 0.0;
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_bytes = 0;
    SeqSnapshot seq;
    LatencySnapshot latency;
    bool has_latency = false;
};

class StreamStats {
public:
    explicit StreamStats(std::string name, std::size_t latency_capacity = 8192);

    void on_rx(std::uint32_t seq, std::size_t bytes);
    void on_rx_latency(double seconds);
    void on_tx(std::size_t bytes);

    const std::string& name() const { return name_; }
    StreamReport report(std::uint64_t now_ns, bool reset_window = false);
    void reset();

private:
    std::string name_;
    SeqTracker seq_;
    LatencyWindow latency_;
    RateMeter rx_rate_;
    RateMeter tx_rate_;
    RateMeter rx_byte_rate_;
    RateMeter tx_byte_rate_;
    std::uint64_t rx_bytes_ = 0;
    std::uint64_t tx_bytes_ = 0;
    std::uint64_t last_report_ns_ = 0;
};

std::string format_report(const StreamReport& report);

}  // namespace oal

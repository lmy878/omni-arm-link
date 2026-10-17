#include "oal/stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oal {
namespace {

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double index = fraction * static_cast<double>(sorted.size() - 1);
    const auto low = static_cast<std::size_t>(index);
    const auto high = std::min(low + 1, sorted.size() - 1);
    const double frac = index - static_cast<double>(low);
    return sorted[low] * (1.0 - frac) + sorted[high] * frac;
}

double to_ms(double seconds) { return seconds * 1e3; }

}  // namespace

void SeqTracker::update(std::uint32_t seq) {
    if (!initialized_) {
        initialized_ = true;
        highest_ = seq;
        snapshot_.received += 1;
        return;
    }

    const std::int32_t delta = static_cast<std::int32_t>(seq - highest_);
    if (delta > 0) {
        const std::int64_t gap = static_cast<std::int64_t>(delta) - 1;
        if (gap > kResyncThreshold) {
            snapshot_.resyncs += 1;
        } else {
            snapshot_.lost += static_cast<std::uint64_t>(gap);
        }
        highest_ = seq;
        snapshot_.received += 1;
    } else if (delta == 0) {
        snapshot_.duplicates += 1;
    } else {
        if (static_cast<std::int64_t>(-delta) > 64) {
            snapshot_.late += 1;
        } else {
            snapshot_.reordered += 1;
        }
        snapshot_.received += 1;
    }

    const std::uint64_t total = snapshot_.received + snapshot_.lost;
    snapshot_.loss_rate = total > 0
                              ? static_cast<double>(snapshot_.lost) / static_cast<double>(total)
                              : 0.0;
}

void LatencyWindow::add(double seconds) {
    if (samples_.size() >= capacity_) {
        samples_.pop_front();
    }
    samples_.push_back(seconds);
}

LatencySnapshot LatencyWindow::snapshot() const {
    LatencySnapshot out;
    if (samples_.empty()) {
        return out;
    }

    std::vector<double> sorted(samples_.begin(), samples_.end());
    std::sort(sorted.begin(), sorted.end());

    double sum = 0.0;
    for (double v : sorted) {
        sum += v;
    }

    double jitter_sum = 0.0;
    std::size_t jitter_count = 0;
    double previous = samples_.front();
    for (double v : samples_) {
        jitter_sum += std::fabs(v - previous);
        previous = v;
        ++jitter_count;
    }

    out.samples = sorted.size();
    out.min_s = sorted.front();
    out.max_s = sorted.back();
    out.mean_s = sum / static_cast<double>(sorted.size());
    out.p50_s = percentile(sorted, 0.50);
    out.p95_s = percentile(sorted, 0.95);
    out.p99_s = percentile(sorted, 0.99);
    out.jitter_s = jitter_count > 1 ? jitter_sum / static_cast<double>(jitter_count - 1) : 0.0;
    return out;
}

StreamStats::StreamStats(std::string name, std::size_t latency_capacity)
    : name_(std::move(name)), latency_(latency_capacity) {}

void StreamStats::on_rx(std::uint32_t seq, std::size_t bytes) {
    seq_.update(seq);
    rx_rate_.add(1);
    rx_byte_rate_.add(bytes);
    rx_bytes_ += bytes;
}

void StreamStats::on_rx_latency(double seconds) { latency_.add(seconds); }

void StreamStats::on_tx(std::size_t bytes) {
    tx_rate_.add(1);
    tx_byte_rate_.add(bytes);
    tx_bytes_ += bytes;
}

StreamReport StreamStats::report(std::uint64_t now_ns, bool reset_window) {
    StreamReport out;
    out.name = name_;
    if (last_report_ns_ != 0 && now_ns > last_report_ns_) {
        out.window_seconds = static_cast<double>(now_ns - last_report_ns_) / 1e9;
    }
    last_report_ns_ = now_ns;

    const double rx_pps = rx_rate_.sample_rate(now_ns);
    const double tx_pps = tx_rate_.sample_rate(now_ns);
    out.rx_pps = rx_pps;
    out.tx_pps = tx_pps;
    out.rx_kbps = rx_byte_rate_.sample_rate(now_ns) * 8.0 / 1000.0;
    out.tx_kbps = tx_byte_rate_.sample_rate(now_ns) * 8.0 / 1000.0;
    out.rx_bytes = rx_bytes_;
    out.tx_bytes = tx_bytes_;
    out.seq = seq_.snapshot();
    out.latency = latency_.snapshot();
    out.has_latency = latency_.count() > 0;

    if (reset_window) {
        reset();
    }
    return out;
}

void StreamStats::reset() {
    seq_.reset();
    latency_.clear();
    rx_bytes_ = 0;
    tx_bytes_ = 0;
}

std::string format_report(const StreamReport& report) {
    char buffer[640];
    std::snprintf(buffer, sizeof(buffer),
                  "%-9s rx=%6.0fpps %7.0fkbps tx=%6.0fpps %7.0fkbps loss=%5.2f%% "
                  "lost=%llu dup=%llu ro=%llu "
                  "p50=%6.2fms p95=%6.2fms p99=%6.2fms max=%6.2fms jit=%5.2fms",
                  report.name.c_str(), report.rx_pps, report.rx_kbps, report.tx_pps, report.tx_kbps,
                  report.seq.loss_rate * 100.0,
                  static_cast<unsigned long long>(report.seq.lost),
                  static_cast<unsigned long long>(report.seq.duplicates),
                  static_cast<unsigned long long>(report.seq.reordered),
                  to_ms(report.latency.p50_s), to_ms(report.latency.p95_s),
                  to_ms(report.latency.p99_s), to_ms(report.latency.max_s),
                  to_ms(report.latency.jitter_s));
    return buffer;
}

}  // namespace oal

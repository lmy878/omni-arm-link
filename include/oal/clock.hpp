#pragma once

#include <chrono>
#include <cstdint>

namespace oal {

using SteadyClock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;

std::uint64_t now_ns();
std::uint64_t epoch_ns();

double seconds_since(std::uint64_t start_ns);

class RateScheduler {
public:
    explicit RateScheduler(double hz);

    double hz() const { return hz_; }
    std::chrono::nanoseconds period() const { return period_; }

    void reset();
    std::uint64_t ticks() const { return ticks_; }
    std::uint64_t missed() const { return missed_; }

    void sleep_next();
    bool due() const;

private:
    double hz_;
    std::chrono::nanoseconds period_;
    SteadyClock::time_point next_;
    std::uint64_t ticks_ = 0;
    std::uint64_t missed_ = 0;
};

}  // namespace oal

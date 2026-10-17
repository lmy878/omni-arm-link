#include "oal/clock.hpp"

#include <thread>

namespace oal {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch())
            .count());
}

std::uint64_t epoch_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(WallClock::now().time_since_epoch())
            .count());
}

double seconds_since(std::uint64_t start_ns) {
    return static_cast<double>(now_ns() - start_ns) / 1e9;
}

RateScheduler::RateScheduler(double hz)
    : hz_(hz),
      period_(std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / hz))),
      next_(SteadyClock::now()) {}

void RateScheduler::reset() {
    next_ = SteadyClock::now();
    ticks_ = 0;
    missed_ = 0;
}

bool RateScheduler::due() const {
    return SteadyClock::now() >= next_;
}

void RateScheduler::sleep_next() {
    next_ += period_;
    ++ticks_;

    auto now = SteadyClock::now();
    if (now < next_) {
        while (SteadyClock::now() < next_) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(50000));
        }
        return;
    }

    const auto behind = now - next_;
    const auto periods = static_cast<std::int64_t>(behind / period_) + 1;
    next_ += period_ * periods;
    missed_ += static_cast<std::uint64_t>(periods);
}

}  // namespace oal

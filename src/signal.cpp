#include "oal/signal.hpp"

#include <csignal>
#include <atomic>

namespace oal {
namespace {

std::atomic<bool> g_stop{false};

void handler(int) { g_stop.store(true); }

}  // namespace

void install_signal_handlers() {
    std::signal(SIGINT, handler);
    std::signal(SIGTERM, handler);
    std::signal(SIGPIPE, SIG_IGN);
}

bool stop_requested() { return g_stop.load(); }

void request_stop() { g_stop.store(true); }

}  // namespace oal

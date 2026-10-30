#pragma once
#include "service_policy.hpp"
#include <atomic>

namespace ya::power_wake {
// Shared by the board, power coordinator and UI, with hardware notification
// and UI callbacks supplied by their owning tasks.
class Handoff {
  std::atomic<bool> requested{false};

public:
  template <class Notify> void request(Notify notify) {
    requested = true; // Publish before notification can schedule the power task.
    notify();
  }
  bool pending() const { return requested.load(); }
  bool publish(std::atomic<bool> &screen_sleeping, std::atomic<bool> &wake_ui) {
    if (!requested.exchange(false))
      return false;
    wake_ui = true;
    screen_sleeping = false;
    return true;
  }
};

template <class Request>
uint32_t route_keys(uint32_t events, bool screen_sleeping, Request request) {
  if ((events & service_policy::KEY_WAKE) || (events && screen_sleeping)) {
    request();
    // Only a shutdown hold can also act during a wake gesture.
    return events & 8;
  }
  return events;
}

template <class Wake> void consume_ui(std::atomic<bool> &wake_ui, Wake wake) {
  if (wake_ui.exchange(false))
    wake();
}

template <class Wake, class ReadEvents>
uint32_t read_input(std::atomic<bool> &wake_ui, Wake wake, ReadEvents read_events) {
  consume_ui(wake_ui, wake);
  return read_events();
}
} // namespace ya::power_wake

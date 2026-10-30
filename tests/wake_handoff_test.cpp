#include "../main/power_policy.hpp"
#include "../main/wake_handoff.hpp"
#include <cassert>
#include <initializer_list>

using ya::power_wake::Handoff;
using ya::power_wake::read_input;
using ya::power_wake::route_keys;
using ya::service_policy::KEY_WAKE;

int main() {
  Handoff handoff;
  std::atomic<bool> sleeping{true}, wake_ui{false};
  unsigned notifications = 0, ui_wakes = 0, actions = 0;
  // These are the same request/publish operations used by power_request_wake()
  // and power_tick(), including publication before a notification can run.
  auto request = [&] {
    handoff.request([&] {
      assert(handoff.pending());
      ++notifications;
    });
  };
  assert(!handoff.publish(sleeping, wake_ui));
  auto events = route_keys(KEY_WAKE, sleeping, request);
  assert(events == 0 && notifications == 1 && handoff.pending());
  assert(sleeping && !wake_ui); // Only the power coordinator changes UI state.
  assert(route_keys(0, sleeping, request) == 0); // released before power runs
  assert(handoff.pending()); // a complete short press cannot be lost
  assert(handoff.publish(sleeping, wake_ui));
  assert(!sleeping && wake_ui && !handoff.pending());

  // Exercise the production UI handoff, with an ordinary key already queued:
  // the wake callback must run before the UI reads or handles that key.
  bool ui_asleep = true;
  events = read_input(wake_ui, [&] {
    ++ui_wakes;
    ui_asleep = false;
    assert(!wake_ui);
  }, [&] {
    assert(!ui_asleep && ui_wakes == 1 && !wake_ui);
    return 2u;
  });
  assert(events == 2); // a new Home press is not consumed as another wake
  ++actions;
  assert(read_input(wake_ui, [&] { ++ui_wakes; }, [] { return 0u; }) == 0);
  assert(!handoff.publish(sleeping, wake_ui));
  assert(ui_wakes == 1 && actions == 1);

  // Every ordinary action is suppressed if screen-off wins the press race.
  for (unsigned action = 1; action <= 4; action *= 2) {
    sleeping = true;
    assert(route_keys(action, sleeping, request) == 0);
    assert(handoff.publish(sleeping, wake_ui));
    ya::power_wake::consume_ui(wake_ui, [] {});
    assert(route_keys(action, sleeping, request) == action);
  }
  sleeping = true;
  assert(route_keys(8, sleeping, request) == 8); // retain orderly shutdown hold
  assert(handoff.publish(sleeping, wake_ui));
  ya::power_wake::consume_ui(wake_ui, [] {});
  assert(route_keys(KEY_WAKE, false, request) == 0); // queued wake stays valid
  assert(handoff.publish(sleeping, wake_ui));
  ya::power_wake::consume_ui(wake_ui, [] {});

  // The same scope guard used in deep() restores only storage that this sleep
  // attempt unmounted. It runs after a later mutation lock is released.
  unsigned mounts = 0;
  for (bool originally_mounted : {false, true}) {
    bool mounted = originally_mounted, mutation_locked = false;
    auto abort_sleep = [&] {
      if (mounted)
        mounted = false;
      ya::power_policy::RestoreStorageOnExit restore(originally_mounted, [&] {
        assert(!mutation_locked);
        mounted = true;
        ++mounts;
      });
      struct Mutation {
        bool &locked;
        explicit Mutation(bool &value) : locked(value) { locked = true; }
        ~Mutation() { locked = false; }
      } mutation(mutation_locked);
      sleeping = true;
      assert(route_keys(KEY_WAKE, sleeping, request) == 0);
      assert(handoff.pending());
      return; // A wake during the final preparation delay aborts deep sleep.
    };
    abort_sleep();
    assert(mounted == originally_mounted && !mutation_locked);
    assert(handoff.publish(sleeping, wake_ui));
    ya::power_wake::consume_ui(wake_ui, [] {});
  }
  assert(mounts == 1); // absent/unmounted cards are never mounted as a side effect
}

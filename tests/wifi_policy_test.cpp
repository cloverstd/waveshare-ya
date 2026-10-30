#include "../main/wifi_policy.hpp"
#include <cassert>
using namespace ya::wifi_policy;
Network net(const char *s, Security sec, int rssi) {
  Network n;
  std::snprintf(n.ssid, sizeof n.ssid, "%s", s);
  n.security = sec;
  n.rssi = rssi;
  return n;
}
int main() {
  assert(bars(-55) == 4 && bars(-56) == 3 && bars(-67) == 3 && bars(-68) == 2 && bars(-75) == 2 &&
         bars(-76) == 1 && bars(0) == 0 && bars(-127) == 0);
  Network rows[3];
  int count = 0;
  insert(rows, count, 3, net("Home", Security::PERSONAL, -80));
  insert(rows, count, 3, net("Home", Security::PERSONAL, -40));
  assert(count == 1 && rows[0].rssi == -40);
  insert(rows, count, 3, net("Home", Security::OPEN, -20));
  insert(rows, count, 3, net("Office", Security::PERSONAL, -60));
  insert(rows, count, 3, net("Extra", Security::PERSONAL, -50));
  sort(rows, count);
  assert(count == 3 && rows[0].security == Security::OPEN && !strcmp(rows[2].ssid, "Extra"));
  // Identity survives reordering, and open APs cannot match protected profiles.
  Network home = net("Home", Security::PERSONAL, -40);
  assert(same(rows[1], home));
  assert(!same(rows[0], home));
  Record r;
  auto &a = r.profiles[0];
  a.id = 1;
  a.order = 3;
  strcpy(a.ssid, "Home");
  strcpy(a.password, "test-password");
  a.security = Security::PERSONAL;
  r.next_id = 3;
  r.count = 2;
  r.mirror_id = 1;
  auto &b = r.profiles[1];
  b = a;
  b.id = 2;
  b.order = 2;
  strcpy(b.ssid, "Extra");
  assert(valid_record(r));
  assert(choose(r, rows, 3, 0) == 0);
  assert(choose(r, rows, 3, 1) == 1);
  assert(choose(r, rows, 3, 3) == -1);
  a.automatic = 0;
  assert(choose(r, rows, 3, 0) == 1);
  a.automatic = 1;
  Network only_open[] = {net("Home", Security::OPEN, -20)};
  assert(choose(r, only_open, 1, 0) == -1);
  a.hidden = 1;
  assert(choose(r, only_open, 1, 0) == 0);
  a.hidden = 0;
  assert(!roam_due(9999, 0, -60000) && roam_due(10000, 0, -60000) &&
         !roam_due(60001, 50000, 10000) && roam_due(70000, 50000, 10000));
  // Journal recovery at both sides of the v1 mirror write, including legacy forget.
  r.previous = digest("old", "old-password");
  r.mirror = digest("Home", "test-password");
  r.pending = 1;
  assert(recovery(r.previous, r) == Recovery::COMPLETE &&
         recovery(r.mirror, r) == Recovery::COMPLETE);
  assert(recovery(digest("", ""), r) == Recovery::IMPORT);
  r.pending = 0;
  assert(recovery(r.mirror, r) == Recovery::USE && recovery(r.previous, r) == Recovery::IMPORT);
  Profile changed = a;
  strcpy(changed.password, "different-password");
  assert(!same_credentials(a, changed));
  assert(same_credentials(a, a));
  Record bad = r;
  bad.profiles[1].id = 1;
  assert(!valid_record(bad));
  bad = r;
  memset(bad.profiles[0].ssid, 'x', 33);
  assert(!valid_record(bad));
  bad = r;
  bad.count = 8;
  assert(!valid_record(bad));
  uint8_t report[] = {1, 52, 13, 2, 1, 2, 3, 4, 5, 1, 0, 0, 0, 81, 6, 7};
  char out[160];
  assert(neighbors(report, sizeof report, out, sizeof out));
  assert(strstr(out, "02:01:02:03:04:05,0x00000001,81,6,7"));
  for (size_t n = 0; n < sizeof report; ++n)
    assert(!neighbors(report, n, out, sizeof out));
  assert(!neighbors(report, sizeof report, out, 8));
  report[2] = 12;
  assert(!neighbors(report, sizeof report, out, sizeof out));
}

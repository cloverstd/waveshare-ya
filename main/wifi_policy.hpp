#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
namespace ya::wifi_policy {
enum class Security : uint8_t { OPEN, PERSONAL, WPA3, UNSUPPORTED };
struct Network {
  char ssid[33]{};
  Security security = Security::OPEN;
  int8_t rssi = -127;
};
struct Profile {
  uint32_t id = 0, order = 0;
  char ssid[33]{}, password[65]{};
  Security security = Security::OPEN;
  uint8_t automatic = 1, hidden = 0;
};
struct Record {
  uint32_t schema = 1, next_id = 1, order = 0, mirror_id = 0;
  uint64_t mirror = 0, previous = 0;
  uint8_t pending = 0, count = 0;
  Profile profiles[8]{};
};
inline uint64_t digest(const char *s, const char *p) {
  uint64_t h = 1469598103934665603ULL;
  for (const char *v : {s, p}) {
    while (*v) {
      h ^= (uint8_t)*v++;
      h *= 1099511628211ULL;
    }
    h ^= 255;
    h *= 1099511628211ULL;
  }
  return h;
}
inline bool same_credentials(const Profile &a, const Profile &b) {
  return a.security == b.security && !strcmp(a.ssid, b.ssid) && !strcmp(a.password, b.password);
}
enum class Recovery { USE, COMPLETE, IMPORT };
inline Recovery recovery(uint64_t legacy, const Record &r) {
  if (r.pending && (legacy == r.previous || legacy == r.mirror))
    return Recovery::COMPLETE;
  return legacy == r.mirror ? Recovery::USE : Recovery::IMPORT;
}
inline bool valid_record(const Record &r) {
  if (r.schema != 1 || r.count > 8 || r.pending > 1 || !r.next_id)
    return false;
  int count = 0;
  bool mirror = !r.mirror_id;
  for (auto &p : r.profiles) {
    if (!p.id)
      continue;
    ++count;
    if (p.id == r.mirror_id)
      mirror = true;
    if (!p.ssid[0] || !memchr(p.ssid, 0, 33) || !memchr(p.password, 0, 65) ||
        p.security >= Security::UNSUPPORTED || p.automatic > 1 || p.hidden > 1 || p.id >= r.next_id)
      return false;
    for (auto &q : r.profiles)
      if (&q != &p && q.id == p.id)
        return false;
  }
  return count == r.count && mirror;
}
inline bool compatible(Security profile, Security ap) {
  return profile == ap || (profile == Security::PERSONAL && ap == Security::WPA3);
}
inline int choose(const Record &r, const Network *networks, int count, uint32_t excluded) {
  int best = -1, best_rssi = -128;
  uint32_t newest = 0;
  for (auto &p : r.profiles)
    if (p.id && p.automatic)
      newest = std::max(newest, p.order);
  for (int i = 0; i < 8; ++i) {
    auto &p = r.profiles[i];
    if (!p.id || !p.automatic || (excluded & (1u << i)))
      continue;
    int rssi = -128;
    for (int j = 0; j < count; ++j)
      if (compatible(p.security, networks[j].security) && !strcmp(p.ssid, networks[j].ssid))
        rssi = networks[j].rssi;
    if (p.hidden)
      rssi = std::max(rssi, -100);
    if (rssi == -128)
      continue;
    if (best < 0 || p.order == newest || (r.profiles[best].order != newest && rssi > best_rssi)) {
      best = i;
      best_rssi = rssi;
    }
  }
  return best;
}
inline int bars(int rssi) {
  return rssi >= 0 || rssi < -100 ? 0 : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
}
inline bool same(const Network &a, const Network &b) {
  return a.security == b.security && !strcmp(a.ssid, b.ssid);
}
inline void insert(Network *out, int &count, int capacity, const Network &n) {
  if (!n.ssid[0])
    return;
  for (int i = 0; i < count; ++i)
    if (same(out[i], n)) {
      if (n.rssi > out[i].rssi)
        out[i] = n;
      return;
    }
  if (count < capacity)
    out[count++] = n;
  else {
    auto *weak =
        std::min_element(out, out + count, [](auto &a, auto &b) { return a.rssi < b.rssi; });
    if (n.rssi > weak->rssi)
      *weak = n;
  }
}
inline void sort(Network *out, int count) {
  std::sort(out, out + count, [](auto &a, auto &b) {
    return a.rssi != b.rssi ? a.rssi > b.rssi : strcmp(a.ssid, b.ssid) < 0;
  });
}
inline bool roam_due(int64_t now, int64_t weak_since, int64_t last) {
  return weak_since >= 0 && now - weak_since >= 10000 && now - last >= 60000;
}
// The SDK report begins with a dialog token, then length-delimited neighbor IEs.
inline bool neighbors(const uint8_t *data, size_t len, char *out, size_t cap) {
  if (!cap)
    return false;
  out[0] = 0;
  if (!data || len < 1)
    return false;
  size_t used = 0;
  ++data;
  --len;
  while (len) {
    if (len < 2 || size_t(data[1]) + 2 > len) {
      out[0] = 0;
      return false;
    }
    size_t n = data[1];
    if (data[0] == 52) {
      if (n < 13) {
        out[0] = 0;
        return false;
      }
      const auto *p = data + 2;
      uint32_t info =
          uint32_t(p[6]) | uint32_t(p[7]) << 8 | uint32_t(p[8]) << 16 | uint32_t(p[9]) << 24;
      int wrote = snprintf(out + used, cap - used,
                           " neighbor=%02x:%02x:%02x:%02x:%02x:%02x,0x%08lx,%u,%u,%u", p[0], p[1],
                           p[2], p[3], p[4], p[5], (unsigned long)info, p[10], p[11], p[12]);
      if (wrote < 0 || size_t(wrote) >= cap - used) {
        out[0] = 0;
        return false;
      }
      used += wrote;
    }
    data += n + 2;
    len -= n + 2;
  }
  return used > 0;
}
} // namespace ya::wifi_policy

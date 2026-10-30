#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace ya::gallery {
constexpr size_t MAX_ALBUMS = 8, MAX_LOCAL = 9, MAX_ONLINE = 90, MAX_FILE = 1024 * 1024,
                 MAX_JPEG = 128 * 1024, MAX_CATALOG = 128 * 1024;
struct Photo {
  std::string id, url, thumbnail_url, file;
};
struct Album {
  std::string id, name, source, manifest;
  std::vector<Photo> photos;
};
struct Catalog {
  std::string error;
  uint32_t revision = 0;
  std::vector<Album> albums;
};
// Bound parser recursion and allocation before handing a remote document to cJSON.
inline bool bounded_document(std::string_view s) {
  if (s.empty() || s.size() > MAX_CATALOG || s.find('\0') != std::string_view::npos ||
      s.find("\\u0000") != std::string_view::npos)
    return false;
  char stack[16];
  unsigned depth = 0, nodes = 0;
  bool quoted = false, escape = false;
  for (unsigned char c : s) {
    if (quoted) {
      if (c < 32)
        return false;
      if (escape) {
        escape = false;
        continue;
      }
      if (c == '\\')
        escape = true;
      else if (c == '"')
        quoted = false;
      continue;
    }
    if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (depth == 16 || ++nodes > 2048)
        return false;
      stack[depth++] = c;
    } else if (c == '}' || c == ']') {
      if (!depth || stack[--depth] != (c == '}' ? '{' : '['))
        return false;
    } else if (c == ',' && ++nodes > 2048)
      return false;
  }
  return !quoted && !escape && !depth;
}
inline bool id_valid(const std::string &s) {
  return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
  });
}
inline bool name_valid(const std::string &s) {
  if (s.empty() || s.size() > 96)
    return false;
  unsigned count = 0;
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = (unsigned char)s[i++], minimum = 0;
    unsigned more = 0;
    if (cp < 32 || cp == 127)
      return false;
    if (cp >= 0xc2 && cp <= 0xdf) {
      more = 1;
      cp &= 31;
      minimum = 128;
    } else if (cp >= 0xe0 && cp <= 0xef) {
      more = 2;
      cp &= 15;
      minimum = 2048;
    } else if (cp >= 0xf0 && cp <= 0xf4) {
      more = 3;
      cp &= 7;
      minimum = 65536;
    } else if (cp >= 128)
      return false;
    while (more--) {
      if (i == s.size() || ((unsigned char)s[i] & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | ((unsigned char)s[i++] & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || ++count > 32)
      return false;
  }
  return true;
}
inline bool digest_valid(const std::string &s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
inline bool url_valid(const std::string &s) {
  size_t p = s.rfind("https://", 0) == 0 ? 8 : s.rfind("http://", 0) == 0 ? 7 : 0;
  return p && s.size() > p && s.size() <= 512 && s[p] != '/' && s[p] != '?' &&
         std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; }) &&
         s.find('@') == std::string::npos && s.find_first_of(" \r\n\t\\#") == std::string::npos;
}
inline bool path_valid(const std::string &s) {
  return s.rfind("photos/", 0) == 0 && s.size() < 240 && s.find("..") == std::string::npos &&
         s.find('\\') == std::string::npos;
}
inline int wrap(int index, int count) { return count > 0 ? ((index % count) + count) % count : 0; }
inline size_t pages(size_t n) { return (n + 8) / 9; }
inline bool room(uint64_t total, uint64_t used, uint64_t extra) {
  uint64_t reserve = std::max<uint64_t>(512 * 1024, total / 4);
  return used <= total && reserve <= total - used && extra <= total - used - reserve;
}
inline bool valid(const Catalog &c) {
  if (c.albums.size() > MAX_ALBUMS)
    return false;
  size_t online = 0;
  for (size_t i = 0; i < c.albums.size(); ++i) {
    const auto &a = c.albums[i];
    if (!id_valid(a.id) || !name_valid(a.name))
      return false;
    for (size_t j = 0; j < i; ++j)
      if (c.albums[j].id == a.id)
        return false;
    if (a.source != "local" && a.source != "urls" && a.source != "manifest")
      return false;
    if (a.source == "manifest" && !url_valid(a.manifest))
      return false;
    if (a.source == "local") {
      if (a.photos.size() > MAX_LOCAL)
        return false;
    } else
      online += a.photos.size();
    for (size_t p = 0; p < a.photos.size(); ++p) {
      const auto &v = a.photos[p];
      if (!id_valid(v.id))
        return false;
      for (size_t j = 0; j < p; ++j)
        if (a.photos[j].id == v.id)
          return false;
      if (a.source == "local") {
        if (!digest_valid(v.file))
          return false;
      } else if (!url_valid(v.url) || (!v.thumbnail_url.empty() && !url_valid(v.thumbnail_url)))
        return false;
    }
  }
  return online <= MAX_ONLINE;
}
} // namespace ya::gallery

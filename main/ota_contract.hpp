#pragma once
#include "identity.hpp"
#include "cJSON.h"
#include <cmath>
#include <cstring>

namespace ya::ota_contract {
struct Release {
  char version[32]{}, url[384]{}, sha[65]{}, notes[256]{};
  size_t size = 0;
};
inline bool https_url(const char *url) {
  if (std::strncmp(url, "https://", 8) || !url[8] || url[8] == '/' || url[8] == '?' ||
      url[8] == '#')
    return false;
  const char *host_end = std::strpbrk(url + 8, "/?");
  if (!host_end)
    host_end = url + std::strlen(url);
  for (const char *p = url; *p; ++p)
    if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '#' ||
        (*p == '@' && p < host_end))
      return false;
  return true;
}
inline bool field(cJSON *j, const char *name, char *out, size_t n) {
  auto *x = cJSON_GetObjectItemCaseSensitive(j, name);
  if (!cJSON_IsString(x) || std::strlen(x->valuestring) >= n)
    return false;
  std::memcpy(out, x->valuestring, std::strlen(x->valuestring) + 1);
  return true;
}
inline bool alnum(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}
inline bool fields(cJSON *j, Release &next, const char *product_id, const char *board_id,
                   const char *layout_id, size_t max_size) {
  if (!cJSON_IsObject(j))
    return false;
  // Duplicate keys must not make the publisher and firmware interpret different metadata.
  for (auto *a = j->child; a; a = a->next)
    for (auto *b = a->next; b; b = b->next)
      if (!std::strcmp(a->string, b->string))
        return false;
  char product[64], board[64], layout[32];
  auto *size = cJSON_GetObjectItemCaseSensitive(j, "size");
  bool ok = field(j, "product", product, sizeof(product)) && !std::strcmp(product, product_id) &&
            field(j, "board", board, sizeof(board)) && !std::strcmp(board, board_id) &&
            field(j, "layout", layout, sizeof(layout)) && !std::strcmp(layout, layout_id) &&
            field(j, "version", next.version, sizeof(next.version)) && alnum(next.version[0]) &&
            field(j, "url", next.url, sizeof(next.url)) && https_url(next.url) &&
            field(j, "sha256", next.sha, sizeof(next.sha)) && std::strlen(next.sha) == 64 &&
            cJSON_IsNumber(size) && std::isfinite(size->valuedouble) && size->valuedouble >= 512 &&
            size->valuedouble <= max_size && std::floor(size->valuedouble) == size->valuedouble;
  if (!ok)
    return false;
  next.size = (size_t)size->valuedouble;
  if (cJSON_GetObjectItemCaseSensitive(j, "notes") &&
      !field(j, "notes", next.notes, sizeof(next.notes)))
    return false;
  for (const char *p = next.sha; *p; ++p)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
      return false;
  for (const char *p = next.version; *p; ++p)
    if (!(alnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == '+'))
      return false;
  return true;
}
inline bool json_without_nul(const char *body, size_t size) {
  if (std::memchr(body, 0, size))
    return false;
  // cJSON stores C strings, so an escaped NUL could hide a suffix from validation.
  for (size_t i = 0; i + 1 < size; ++i)
    if (body[i] == '\\') {
      ++i;
      if (body[i] == 'u' && i + 4 < size && !std::memcmp(body + i + 1, "0000", 4))
        return false;
    }
  return true;
}
inline bool parse(const char *body, size_t size, Release &out, const char *product_id,
                  const char *board_id, const char *layout_id, size_t max_size) {
  if (!size || size >= 4096 || !json_without_nul(body, size) || !flat_json_object(body, size))
    return false;
  auto *j = cJSON_ParseWithLength(body, size);
  if (!j)
    return false;
  Release next;
  bool ok = fields(j, next, product_id, board_id, layout_id, max_size);
  cJSON_Delete(j);
  if (ok)
    out = next;
  return ok;
}
} // namespace ya::ota_contract

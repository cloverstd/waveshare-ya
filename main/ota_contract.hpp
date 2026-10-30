#pragma once
#include "identity.hpp"
#include "cJSON.h"
#include <cmath>
#include <cstring>

namespace ya::ota_contract {
struct Release {
  char version[32]{}, url[384]{}, sha[65]{}, notes[256]{};
  size_t size = 0;
  char delta_index_url[384]{}, delta_index_sha[65]{};
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
inline bool digest(const char *s) {
  if (std::strlen(s) != 64) return false;
  for (; *s; ++s) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return false;
  return true;
}
inline bool unique(cJSON *j) {
  for (auto *a = j->child; a; a = a->next)
    for (auto *b = a->next; b; b = b->next)
      if (!std::strcmp(a->string, b->string)) return false;
  return true;
}
inline bool stable(const char *v) {
  for (int i = 0; i < 3; ++i) {
    const char *start = v;
    while (*v >= '0' && *v <= '9') ++v;
    if (v == start || (v-start > 1 && *start == '0')) return false;
    if (i < 2 ? *v++ != '.' : *v != 0) return false;
  }
  return true;
}
inline bool newer(const char *target, const char *current) {
  if (!stable(target) || !stable(current)) return false;
  for (int i = 0; i < 3; ++i) {
    const char *a = target, *b = current;
    while (*target >= '0' && *target <= '9') ++target;
    while (*current >= '0' && *current <= '9') ++current;
    auto na = target-a, nb = current-b;
    if (na != nb) return na > nb;
    int c = std::strncmp(a,b,na);
    if (c) return c > 0;
    if (i < 2) { ++target; ++current; }
  }
  return false;
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
  bool has_url = cJSON_GetObjectItemCaseSensitive(j, "delta_index_url");
  bool has_sha = cJSON_GetObjectItemCaseSensitive(j, "delta_index_sha256");
  if (has_url != has_sha || (has_url &&
      (!field(j, "delta_index_url", next.delta_index_url, sizeof(next.delta_index_url)) ||
       !https_url(next.delta_index_url) ||
       !field(j, "delta_index_sha256", next.delta_index_sha, sizeof(next.delta_index_sha)) ||
       !digest(next.delta_index_sha)))) return false;
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
struct Delta {
  char base_version[32]{}, base_sha[65]{}, url[384]{}, sha[65]{};
  size_t base_size = 0, size = 0;
};
inline bool integer(cJSON *j, const char *key, size_t min, size_t max, size_t &out) {
  auto *n = cJSON_GetObjectItemCaseSensitive(j,key);
  if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble) || n->valuedouble < min ||
      n->valuedouble > max || std::floor(n->valuedouble) != n->valuedouble) return false;
  out = (size_t)n->valuedouble; return true;
}
// Parse bounded, explicitly shaped nested metadata; v1 full manifests remain flat.
inline bool parse_delta(const char *body, size_t size, const Release &target,
                        const char *base_version, size_t base_size, const char *base_sha,
                        Delta &out) {
  if (!size || size >= 4096 || !json_without_nul(body,size)) return false;
  // Bound recursion before cJSON touches an untrusted nested document.
  unsigned depth=0; bool quoted=false, escaped=false;
  for (size_t i=0;i<size;++i) {
    char c=body[i];
    if (quoted) {
      if(escaped) escaped=false; else if(c=='\\') escaped=true; else if(c=='"') quoted=false;
    } else if(c=='"') quoted=true;
    else if(c=='{' || c=='[') { if(++depth>3) return false; }
    else if(c=='}' || c==']') { if(!depth) return false; --depth; }
  }
  if(depth || quoted) return false;
  const char *end = nullptr;
  auto *j = cJSON_ParseWithLengthOpts(body,size,&end,false);
  if (!j) return false;
  while (end < body+size && (*end==' ' || *end=='\r' || *end=='\n' || *end=='\t')) ++end;
  bool ok = end == body+size && cJSON_IsObject(j) && unique(j);
  char text[65]; size_t n = 0;
  auto match = [&](const char *key, const char *value) {
    return field(j,key,text,sizeof(text)) && !std::strcmp(text,value);
  };
  ok = ok && integer(j,"schema",1,1,n) && match("product","waveshare-ya") &&
    match("board","esp32-s3-touch-amoled-2.16") && match("layout","ab6-v1") &&
    match("version",target.version) && match("sha256",target.sha) &&
    integer(j,"size",target.size,target.size,n);
  auto *patches = cJSON_GetObjectItemCaseSensitive(j,"patches");
  ok = ok && cJSON_IsArray(patches) && cJSON_GetArraySize(patches) <= 3;
  if (ok) for(auto *f=j->child; f; f=f->next)
    if (std::strcmp(f->string,"patches") && (cJSON_IsObject(f) || cJSON_IsArray(f))) ok=false;
  Delta found; bool matched = false;
  if (ok) for (auto *item = patches->child; item; item = item->next) {
    Delta d; char format[40];
    bool valid = cJSON_IsObject(item) && unique(item) &&
      field(item,"format",format,sizeof(format)) && !std::strcmp(format,"detools-heatshrink-v1") &&
      field(item,"base_version",d.base_version,sizeof(d.base_version)) && stable(d.base_version) &&
      field(item,"base_sha256",d.base_sha,sizeof(d.base_sha)) && digest(d.base_sha) &&
      field(item,"url",d.url,sizeof(d.url)) && https_url(d.url) &&
      field(item,"sha256",d.sha,sizeof(d.sha)) && digest(d.sha) &&
      integer(item,"base_size",512,0x600000,d.base_size) &&
      integer(item,"size",1,target.size-1,d.size);
    for (auto *f = item->child; f; f=f->next)
      if (cJSON_IsObject(f) || cJSON_IsArray(f)) valid = false;
    for (auto *previous=patches->child; previous!=item; previous=previous->next) {
      char version[32];
      if (field(previous,"base_version",version,sizeof(version)) && !std::strcmp(version,d.base_version)) valid=false;
    }
    if (!valid) { ok=false; break; }
    if (!std::strcmp(d.base_version,base_version) && d.base_size==base_size &&
        !std::strcmp(d.base_sha,base_sha)) {
      if (matched) { ok=false; break; }
      found=d; matched=true;
    }
  }
  cJSON_Delete(j);
  if (ok && matched) out=found;
  return ok && matched;
}
} // namespace ya::ota_contract

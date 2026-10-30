#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace ya::storage_policy {
enum class Contents { ERASED, EXISTING, READ_ERROR };
template <class Read> Contents inspect(size_t size, Read read) {
  if (!size) return Contents::READ_ERROR;
  uint8_t block[512];
  for (size_t offset = 0; offset < size; offset += sizeof block) {
    auto n = std::min(sizeof block, size - offset);
    if (!read(offset, block, n)) return Contents::READ_ERROR;
    for (size_t i = 0; i < n; ++i) if (block[i] != 0xff) return Contents::EXISTING;
  }
  return Contents::ERASED;
}
}

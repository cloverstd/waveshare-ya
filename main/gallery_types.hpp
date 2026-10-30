#pragma once
#include "gallery_model.hpp"
#include <memory>
namespace ya::gallery {
struct Image {
  unsigned width = 0, height = 0;
  std::shared_ptr<uint8_t> pixels;
  size_t size() const { return width * height * 2; }
};
struct View {
  uint32_t generation = 0, revision = 0;
  std::vector<std::shared_ptr<const Image>> images;
  std::vector<std::string> errors;
};
} // namespace ya::gallery

#pragma once
#include "ya.hpp"
#include "gallery_types.hpp"
#include "cJSON.h"
#include <memory>
#include <vector>
namespace ya::gallery {
void init();
cJSON *request(int connection, const cJSON *json);
void disconnect(int connection);
bool busy();
std::shared_ptr<const Catalog> snapshot();
std::shared_ptr<const View> view_snapshot();
uint32_t view(const std::string &album, int page, int index,
              bool refresh = false); // index=-1: grid
void cancel_view();
std::shared_ptr<Image> decode(const uint8_t *bytes, size_t size, unsigned width, unsigned height,
                              std::string &error);
std::shared_ptr<Image> thumbnail(const Image &image);
std::vector<uint8_t> encode(const Image &image);
extern const App app;
} // namespace ya::gallery

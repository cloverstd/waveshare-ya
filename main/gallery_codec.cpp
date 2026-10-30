#include "gallery.hpp"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "png.h"
#include <cmath>
#include <cstring>
namespace ya::gallery {
namespace {
std::shared_ptr<uint8_t> memory(size_t n) {
  return {
      static_cast<uint8_t *>(heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
      heap_caps_free};
}
// Read only the bounded TIFF orientation tag in JPEG APP1. No external offsets.
unsigned orientation(const uint8_t *b, size_t n) {
  size_t p = 2;
  while (p + 4 < n && b[p] == 255) {
    uint8_t marker = b[p + 1];
    if (marker == 0xda || marker == 0xd9)
      break;
    size_t len = (b[p + 2] << 8) | b[p + 3];
    if (len < 2 || p + 2 + len > n)
      break;
    if (marker == 0xe1 && len >= 16 && !memcmp(b + p + 4, "Exif\0\0", 6)) {
      const uint8_t *t = b + p + 10;
      size_t z = len - 8;
      bool le = t[0] == 'I' && t[1] == 'I';
      if (!le && !(t[0] == 'M' && t[1] == 'M'))
        return 1;
      auto u16 = [&](size_t x) -> uint32_t {
        if (x + 2 > z)
          return 0;
        return le ? uint32_t(t[x]) | (uint32_t(t[x + 1]) << 8) : (uint32_t(t[x]) << 8) | t[x + 1];
      };
      auto u32 = [&](size_t x) -> uint32_t {
        if (x + 4 > z)
          return 0;
        return le ? u16(x) | (u16(x + 2) << 16) : (u16(x) << 16) | u16(x + 2);
      };
      size_t d = u32(4);
      if (d > z || z - d < 2)
        return 1;
      unsigned count = std::min<uint32_t>(u16(d), 256);
      for (unsigned i = 0; i < count; ++i) {
        size_t q = d + 2 + 12 * i;
        if (q > z || z - q < 12)
          break;
        if (u16(q) == 274 && u16(q + 2) == 3 && u32(q + 4) == 1) {
          unsigned v = u16(q + 8);
          return v >= 1 && v <= 8 ? v : 1;
        }
      }
      return 1;
    }
    p += len + 2;
  }
  return 1;
}
std::shared_ptr<Image> resize(const uint16_t *src, unsigned sw, unsigned sh, unsigned maxw,
                              unsigned maxh, unsigned o = 1) {
  bool swap = o >= 5;
  unsigned ow = swap ? sh : sw, oh = swap ? sw : sh;
  double scale = std::min({1.0, double(maxw) / ow, double(maxh) / oh});
  auto out = std::make_shared<Image>();
  out->width = std::max(1u, unsigned(ow * scale));
  out->height = std::max(1u, unsigned(oh * scale));
  out->pixels = memory(out->size());
  if (!out->pixels)
    return {};
  auto *dst = reinterpret_cast<uint16_t *>(out->pixels.get());
  for (unsigned y = 0; y < out->height; ++y)
    for (unsigned x = 0; x < out->width; ++x) {
      unsigned u = std::min(ow - 1, unsigned(x / scale)), v = std::min(oh - 1, unsigned(y / scale)),
               sx = u, sy = v;
      switch (o) {
      case 2:
        sx = sw - 1 - u;
        break;
      case 3:
        sx = sw - 1 - u;
        sy = sh - 1 - v;
        break;
      case 4:
        sy = sh - 1 - v;
        break;
      case 5:
        sx = v;
        sy = u;
        break;
      case 6:
        sx = v;
        sy = sh - 1 - u;
        break;
      case 7:
        sx = sw - 1 - v;
        sy = sh - 1 - u;
        break;
      case 8:
        sx = sw - 1 - v;
        sy = u;
        break;
      default:
        break;
      }
      dst[y * out->width + x] = src[sy * sw + sx];
    }
  return out;
}
struct Png {
  const uint8_t *bytes;
  size_t length, offset;
  uint8_t *row = nullptr, *pixels = nullptr;
};
void read_png(png_structp png, png_bytep out, png_size_t size) {
  auto *p = static_cast<Png *>(png_get_io_ptr(png));
  if (size > p->length - p->offset)
    png_error(png, "truncated");
  memcpy(out, p->bytes + p->offset, size);
  p->offset += size;
}
} // namespace
std::shared_ptr<Image> decode(const uint8_t *bytes, size_t n, unsigned maxw, unsigned maxh,
                              std::string &error) {
  error = "unsupported_image";
  if (!n || n > MAX_FILE)
    return {};
  if (n > 2 && bytes[0] == 255 && bytes[1] == 216) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK)
      return {};
    jpeg_dec_io_t io{};
    io.inbuf = const_cast<uint8_t *>(bytes);
    io.inbuf_len = n;
    jpeg_dec_header_info_t h{};
    std::shared_ptr<Image> image;
    if (jpeg_dec_parse_header(decoder, &io, &h) == JPEG_ERR_OK && h.width && h.height &&
        h.width <= 2048 && h.height <= 2048) {
      // Library scaling is configured at open; reopen after learning dimensions.
      jpeg_dec_close(decoder);
      decoder = nullptr;
      double s = std::min(1.0, std::max(1.0 / 8, std::min(432.0 / h.width, 432.0 / h.height)));
      if (s < 1 && h.width >= 8 && h.height >= 8) {
        config.scale.width = std::max(8u, (unsigned(h.width * s) + 7) / 8 * 8);
        config.scale.height = std::max(8u, (unsigned(h.height * s) + 7) / 8 * 8);
      }
      if (jpeg_dec_open(&config, &decoder) == JPEG_ERR_OK) {
        io = {};
        io.inbuf = const_cast<uint8_t *>(bytes);
        io.inbuf_len = n;
        int length = 0;
        if (jpeg_dec_parse_header(decoder, &io, &h) == JPEG_ERR_OK &&
            jpeg_dec_get_outbuf_len(decoder, &length) == JPEG_ERR_OK && length > 0 &&
            length <= 1024 * 1024) {
          auto pixels = memory(length);
          io.outbuf = pixels.get();
          if (pixels && jpeg_dec_process(decoder, &io) == JPEG_ERR_OK)
            image = resize(reinterpret_cast<const uint16_t *>(pixels.get()),
                           config.scale.width ? config.scale.width : h.width,
                           config.scale.height ? config.scale.height : h.height, maxw, maxh,
                           orientation(bytes, n));
        }
      }
    }
    if (decoder)
      jpeg_dec_close(decoder);
    if (image)
      error.clear();
    return image;
  }
  if (n < 8 || png_sig_cmp(bytes, 0, 8))
    return {};
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png)
    return {};
  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    return {};
  }
  // Heap-owned cleanup state remains valid across libpng's longjmp.
  auto state = std::make_unique<Png>();
  state->bytes = bytes;
  state->length = n;
  state->offset = 0;
  if (setjmp(png_jmpbuf(png))) {
    heap_caps_free(state->row);
    heap_caps_free(state->pixels);
    png_destroy_read_struct(&png, &info, nullptr);
    return {};
  }
  png_set_read_fn(png, state.get(), read_png);
  png_set_user_limits(png, 2048, 2048);
  png_set_chunk_malloc_max(png, 64 * 1024);
  png_set_chunk_cache_max(png, 16);
  png_read_info(png, info);
  unsigned w = png_get_image_width(png, info), h = png_get_image_height(png, info);
  if (!w || !h || png_get_interlace_type(png, info) != PNG_INTERLACE_NONE)
    png_error(png, "unsupported_dimensions");
  int color = png_get_color_type(png, info), depth = png_get_bit_depth(png, info);
  if (depth == 16)
    png_set_strip_16(png);
  if (color == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  if (!(color & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
  png_read_update_info(png, info);
  size_t stride = png_get_rowbytes(png, info);
  if (stride != w * 4)
    png_error(png, "unsupported_stride");
  double s = std::min({1.0, double(maxw) / w, double(maxh) / h});
  unsigned dw = std::max(1u, unsigned(w * s)), dh = std::max(1u, unsigned(h * s));
  state->row =
      static_cast<uint8_t *>(heap_caps_malloc(stride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  state->pixels = static_cast<uint8_t *>(
      heap_caps_aligned_alloc(16, dw * dh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!state->row || !state->pixels)
    png_error(png, "no_memory");
  unsigned dy = 0;
  auto *out = reinterpret_cast<uint16_t *>(state->pixels);
  for (unsigned y = 0; y < h; ++y) {
    png_read_row(png, state->row, nullptr);
    while (dy < dh && std::min(h - 1, unsigned(dy / s)) == y) {
      for (unsigned x = 0; x < dw; ++x) {
        auto *p = state->row + std::min(w - 1, unsigned(x / s)) * 4;
        unsigned r = p[0] * p[3] / 255, g = p[1] * p[3] / 255, b = p[2] * p[3] / 255;
        out[dy * dw + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
      }
      ++dy;
    }
  }
  png_read_end(png, info);
  png_destroy_read_struct(&png, &info, nullptr);
  heap_caps_free(state->row);
  state->row = nullptr;
  auto image = std::make_shared<Image>();
  image->width = dw;
  image->height = dh;
  image->pixels = {state->pixels, heap_caps_free};
  state->pixels = nullptr;
  error.clear();
  return image;
}
std::shared_ptr<Image> thumbnail(const Image &image) {
  auto out = std::make_shared<Image>();
  out->width = 136;
  out->height = 104;
  out->pixels = memory(out->size());
  if (!out->pixels)
    return {};
  double s = std::max(136.0 / image.width, 104.0 / image.height);
  double ox = (image.width - 136 / s) / 2, oy = (image.height - 104 / s) / 2;
  auto *src = reinterpret_cast<const uint16_t *>(image.pixels.get());
  auto *dst = reinterpret_cast<uint16_t *>(out->pixels.get());
  for (unsigned y = 0; y < 104; ++y)
    for (unsigned x = 0; x < 136; ++x)
      dst[y * 136 + x] = src[std::min(image.height - 1, unsigned(oy + y / s)) * image.width +
                             std::min(image.width - 1, unsigned(ox + x / s))];
  return out;
}
std::vector<uint8_t> encode(const Image &image) {
  jpeg_enc_config_t c{};
  c.width = image.width;
  c.height = image.height;
  c.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  c.subsampling = JPEG_SUBSAMPLE_420;
  c.quality = 85;
  c.task_enable = false;
  jpeg_enc_handle_t encoder = nullptr;
  std::vector<uint8_t> bytes(MAX_JPEG);
  int n = 0;
  if (jpeg_enc_open(&c, &encoder) != JPEG_ERR_OK)
    return {};
  bool ok = false;
  for (int q = 85; q >= 40; q -= 15) {
    jpeg_enc_set_quality(encoder, q);
    if (jpeg_enc_process(encoder, image.pixels.get(), image.size(), bytes.data(), bytes.size(),
                         &n) == JPEG_ERR_OK &&
        n > 0) {
      ok = true;
      break;
    }
  }
  jpeg_enc_close(encoder);
  if (!ok)
    return {};
  bytes.resize(n);
  return bytes;
}
} // namespace ya::gallery

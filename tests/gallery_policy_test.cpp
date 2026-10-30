#include "../main/gallery_model.hpp"
#include <cassert>
using namespace ya::gallery;
int main() {
  assert(bounded_document("{\"schema\":1,\"photos\":[]}"));
  assert(!bounded_document(std::string(17, '[') + std::string(17, ']')));
  assert(!bounded_document("[}"));
  assert(!bounded_document("\"\\u0000\""));
  for (int n : {1, 9, 10, 90}) {
    assert(wrap(-1, n) == n - 1);
    assert(wrap(n, n) == 0);
  }
  assert(wrap(3, 0) == 0);
  assert(pages(0) == 0 && pages(9) == 1 && pages(10) == 2 && pages(90) == 10);
  assert(name_valid("旅行🙂"));
  assert(!name_valid(std::string(33, 'x')));
  assert(!name_valid(std::string("x\0y", 3)));
  assert(!name_valid("\xc0\x80"));
  assert(!name_valid("a\nb"));
  assert(url_valid("http://photos.example.test/photo.jpg"));
  assert(url_valid("https://host/a?token=secret"));
  assert(!url_valid("file:///test"));
  assert(!url_valid("https://?"));
  assert(!url_valid("https://user:pass@host/a"));
  assert(!url_valid(std::string("https://host/a\0b", 16)));
  assert(path_valid("photos/trip/a.jpg"));
  assert(!path_valid("photos/../secret"));
  assert(!path_valid("/photos/a.jpg"));
  assert(!path_valid("photos/a\\b.jpg"));
  assert(room(3 * 1024 * 1024, 1024 * 1024, 128 * 1024));
  assert(!room(3 * 1024 * 1024, 3 * 1024 * 1024, 1));
  assert(!room(100, 0, 0));
  assert(!room(1024, UINT64_MAX, UINT64_MAX));
  Catalog c;
  for (int i = 0; i < 8; ++i) {
    Album a{std::to_string(i), "本地", "local", "", {}};
    for (int j = 0; j < 9; ++j)
      a.photos.push_back({std::to_string(j), "", "", std::string(64, 'a')});
    c.albums.push_back(a);
  }
  assert(valid(c));
  c.albums[0].photos.push_back({"ten", "", "", std::string(64, 'a')});
  assert(!valid(c));
  c.albums[0].photos.pop_back();
  c.albums.push_back({"nine", "相册", "local", "", {}});
  assert(!valid(c));
  c = {};
  Album a{"online", "在线", "urls", "", {}};
  for (int i = 0; i < 90; ++i)
    a.photos.push_back({std::to_string(i), "https://host/a.png", "", ""});
  c.albums.push_back(a);
  assert(valid(c));
  c.albums.push_back({"other", "其他", "urls", "", {{"extra", "https://host/a.png", "", ""}}});
  assert(!valid(c));
  c.albums.pop_back();
  c.albums[0].photos[1].id = "0";
  assert(!valid(c));
}

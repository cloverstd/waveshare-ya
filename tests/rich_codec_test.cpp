#include "../main/rich_limits.hpp"
#include <cassert>
#include <string>
#include <vector>
int main() {
  const std::string text = "𠮷👩‍💻\n中文";
  std::vector<uint8_t> raw(text.begin(),text.end()); raw.resize(raw.size()+864,0xff);
  std::vector<uint8_t> packed(compressBound(raw.size()));uLongf n=packed.size();
  assert(compress(packed.data(),&n,raw.data(),raw.size())==Z_OK);packed.resize(n);
  std::vector<uint8_t> out(raw.size());
  assert(ya::rich::unpack(packed.data(),packed.size(),1,text.size(),out.data(),out.size()));assert(out==raw);
  assert(!ya::rich::unpack(packed.data(),packed.size(),2,text.size(),out.data(),out.size()));
  assert(!ya::rich::unpack(packed.data(),packed.size()-1,1,text.size(),out.data(),out.size()));
  packed.back()^=1;assert(!ya::rich::unpack(packed.data(),packed.size(),1,text.size(),out.data(),out.size()));
  // A compressed stream that inflates past declared dimensions must be rejected.
  raw.resize(100000,0);packed.resize(compressBound(raw.size()));n=packed.size();
  assert(compress(packed.data(),&n,raw.data(),raw.size())==Z_OK);
  assert(!ya::rich::unpack(packed.data(),n,1,text.size(),out.data(),out.size()));
}

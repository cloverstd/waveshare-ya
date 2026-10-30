#include "../main/log_assembler.hpp"
#include "../main/storage_policy.hpp"
#include "../main/rich_limits.hpp"
#include <cassert>
#include <string>
#include <vector>
using namespace ya;
int main() {
  LogAssembler assembler; LogBuffer log;
  assembler.feed(1,"E (504297) wifi:",0,2,log); assert(log.count==0);
  assembler.feed(2,"W (3) other: concurrent\n",1,2,log);
  assembler.feed(1,"connection failed",2,2,log); assembler.feed(1,"\n",3,2,log);
  assert(log.count==2); assert(std::string(log.at(1).text)=="E (504297) wifi:connection failed");
  assert(log.at(1).sequence==2);
  assembler.feed(3,"E(6)wifi: no-space\n",4,2,log); assert(log.count==3);
  assembler.feed(1,"W (7) long: ",5,2,log);
  assembler.feed(1,std::string(500,'x').c_str(),6,2,log); assembler.feed(1,"\n",7,2,log);
  assert(strlen(log.at(3).text)==255);
  assembler.feed(1,"I (8) filtered: ",8,2,log); assembler.feed(1,"body\n",9,2,log);assert(log.count==4);
  assembler.feed(2,"W (9) unterminated",10,2,log);assembler.expire(1010,log);assert(log.count==5);
  auto next=log.next_sequence;log.clear();assembler.clear();assembler.feed(1,"W (10) next\n",1011,2,log);assert(log.at(0).sequence==next);
  std::vector<uint8_t> flash(1030,255);
  auto inspect=[&]{return storage_policy::inspect(flash.size(),[&](size_t off,uint8_t *out,size_t n){memcpy(out,flash.data()+off,n);return true;});};
  assert(inspect()==storage_policy::Contents::ERASED);flash.back()=0;assert(inspect()==storage_policy::Contents::EXISTING);
  assert(storage_policy::inspect(1030,[](size_t,uint8_t*,size_t){return false;})==storage_policy::Contents::READ_ERROR);
  assert(storage_policy::inspect(0,[](size_t,uint8_t*,size_t){return true;})==storage_policy::Contents::READ_ERROR);
  assert(rich::dimensions(2048,4000,131072)); assert(!rich::dimensions(2049,1,1));assert(!rich::dimensions(1,4001,1));
  for (auto text : {std::string("生僻字𠮷👩‍💻\n test"),std::string(1000,'a')}) assert(rich::text_valid((const uint8_t*)text.data(),text.size()));
  for (auto text : {std::string(1001,'a'),std::string("a\0b",3),std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80")}) assert(!rich::text_valid((const uint8_t*)text.data(),text.size()));
}

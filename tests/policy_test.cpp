#include "../main/identity.hpp"
#include <cassert>
int main(){
 const char* valid=" {\"v\":1,\"password\":\"[literal] {text} \\\"\"} \n";
 assert(ya::flat_json_object(valid,std::strlen(valid)));
 const char* nested="{\"value\":{\"deep\":1}}";
 assert(!ya::flat_json_object(nested,std::strlen(nested)));
 const char* array="{\"value\":[1]}";
 assert(!ya::flat_json_object(array,std::strlen(array)));
 const char* trailing="{}{}";
 assert(!ya::flat_json_object(trailing,std::strlen(trailing)));
 ya::FirmwareIdentity good{ya::IDENTITY_MAGIC,"esp32-s3-touch-amoled-2.16","ab6-v1"};
 assert(ya::identity_valid(good));auto wrong=good;wrong.magic=0;assert(!ya::identity_valid(wrong));
 wrong=good;std::strcpy(wrong.board,"esp32-s3-touch-amoled-1.8");assert(!ya::identity_valid(wrong));
 wrong=good;std::strcpy(wrong.layout,"ab3-v0");assert(!ya::identity_valid(wrong));
 assert(!ya::update_power_allowed(true,true,false,true,29));
 assert(ya::update_power_allowed(true,true,false,true,30));
 assert(!ya::update_power_allowed(true,true,false,false,100));
 assert(!ya::update_power_allowed(true,true,false,true,-1));
 assert(ya::update_power_allowed(true,true,true,false,-1));
 assert(!ya::update_power_allowed(false,true,true,true,100));
 assert(!ya::update_power_allowed(true,false,true,true,100));
}

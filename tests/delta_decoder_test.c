// Exercises the exact pinned ESP decoder on a host, with bounded partition callbacks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_delta_ota.h"
typedef struct { unsigned char *base, *out; size_t base_size, capacity, written; } Context;
static esp_err_t read_base(uint8_t *buf,size_t n,int offset,void *arg) {
  Context *c=arg;
  if(offset<0 || (size_t)offset>c->base_size || n>c->base_size-(size_t)offset) return ESP_FAIL;
  memcpy(buf,c->base+offset,n);return ESP_OK;
}
static esp_err_t write_image(const uint8_t *buf,size_t n,void *arg) {
  Context *c=arg;
  if(n>c->capacity-c->written) return ESP_FAIL;
  memcpy(c->out+c->written,buf,n);c->written+=n;return ESP_OK;
}
static unsigned char *load(const char *name,size_t *size) {
  FILE *f=fopen(name,"rb");if(!f) exit(2);
  fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);
  unsigned char *buf=malloc(*size);if(!buf) exit(2);
  if(fread(buf,1,*size,f)!=*size) exit(2);fclose(f);return buf;
}
int main(int argc,char **argv) {
  if(argc!=5) return 2;
  Context ctx={0};size_t patch_size,expected_size;
  ctx.base=load(argv[1],&ctx.base_size);
  unsigned char *patch=load(argv[2],&patch_size),*expected=load(argv[3],&expected_size);
  ctx.capacity=expected_size;ctx.out=malloc(expected_size);
  esp_delta_ota_cfg_t cfg={.user_data=&ctx,.read_cb_with_user_data=read_base,.write_cb_with_user_data=write_image};
  esp_delta_ota_handle_t decoder=esp_delta_ota_init(&cfg);if(!decoder) return 2;
  size_t step=(size_t)atoi(argv[4]);int error=0;
  for(size_t off=0;off<patch_size;off+=step) {
    size_t n=patch_size-off;if(n>step)n=step;
    if(esp_delta_ota_feed_patch(decoder,patch+off,n)!=ESP_OK) {error=1;break;}
  }
  if(esp_delta_ota_finalize(decoder)!=ESP_OK) error=1;
  if(ctx.written!=expected_size || memcmp(ctx.out,expected,expected_size)) error=1;
  esp_delta_ota_deinit(decoder);
  free(ctx.base);free(ctx.out);free(patch);free(expected);return error;
}

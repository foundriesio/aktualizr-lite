#include <cstdint>
#include <sys/statvfs.h>
#include <dlfcn.h>

#ifndef __USE_FILE_OFFSET64
static __fsblkcnt_t free_blocks_numb;
#else
static __fsblkcnt64_t free_blocks_numb;
#endif
static bool override_blocks_numb{false};

void SetFreeBlockNumb(uint64_t free_blocks_numb_in) {
  free_blocks_numb = free_blocks_numb_in;
  override_blocks_numb = true;
}

void UnsetFreeBlockNumb() {
  override_blocks_numb = false;
}

int fstatvfs (int fd, struct statvfs* buf) {
  int (*original_fstatvfs)(int, struct statvfs*);
  original_fstatvfs = (int (*)(int, struct statvfs*))dlsym(RTLD_NEXT, "fstatvfs");
  int res = (*original_fstatvfs)(fd, buf);
  if (override_blocks_numb) {
    buf->f_bfree = free_blocks_numb;
  }
  return res;
}

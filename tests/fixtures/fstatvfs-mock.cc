#include <cstdint>
#include <sys/statvfs.h>
#include <dlfcn.h>

#ifndef __USE_FILE_OFFSET64
static __fsblkcnt_t free_blocks_numb;
static __fsblkcnt_t blocks_numb;
#else
static __fsblkcnt64_t free_blocks_numb;
static __fsblkcnt64_t blocks_numb;
#endif
static bool override_blocks_numb{false};

void SetFreeBlockNumb(uint64_t free_blocks_numb_in, uint64_t blocks_numb_in) {
  free_blocks_numb = free_blocks_numb_in;
  blocks_numb = blocks_numb_in;
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
    // we assume that a user is `root` in the tests so f_bavail (for users) == f_bfree (for root)
    buf->f_bavail = buf->f_bfree = free_blocks_numb;
    if (blocks_numb > 0) {
      // override the total number of blocks if set, otherwise the underlying system storage block number is used
      buf->f_blocks = blocks_numb;
    }
  }
  return res;
}

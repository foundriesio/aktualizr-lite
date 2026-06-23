#include "aktualizr-lite/storage/stat.h"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace storage {

struct Stat {
  uint64_t blockSize;
  uint64_t freeBlockNumber;
  uint64_t blockNumb;
};

static std::string getStat(const std::string& path, Stat& stat) {
  int fd{-1};
  if (-1 == (fd = open(path.c_str(), O_DIRECTORY | O_RDONLY))) {
    return std::string("Failed to open a file/directory; path: ") + path + ", err: " + std::strerror(errno);
  }
  struct statvfs fs_stat{};
  const auto stat_res = fstatvfs(fd, &fs_stat);
  close(fd);
  if (-1 == stat_res) {
    return std::string("Failed to obtain statistic about the path volume; path: ") + path +
           ", err: " + std::strerror(errno);
  }
  stat.blockNumb = fs_stat.f_blocks;
  stat.freeBlockNumber = (getuid() != 0 ? fs_stat.f_bavail : fs_stat.f_bfree);
  stat.blockSize = fs_stat.f_bsize;  // f_frsize == f_bsize on the linux-based systems
  return "";
}

Volume::UsageInfo Volume::getUsageInfo(const std::string& path, uint64_t reserved, const std::string& reserved_by,
                                       bool reserved_in_bytes) {
  Stat stat{};
  if (std::string err = getStat(path, stat); !err.empty()) {
    return UsageInfo{.err = err};
  }

  const uint64_t size_bytes{stat.blockSize * stat.blockNumb};
  UsageInfo::Type free{stat.blockSize * stat.freeBlockNumber,
                       (static_cast<double>(stat.freeBlockNumber) / stat.blockNumb) * 100};
  UsageInfo::Type reserved_usage{};
  if (reserved_in_bytes) {
    const uint64_t reserved_bytes{std::min(reserved, size_bytes)};
    reserved_usage = {reserved_bytes, size_bytes > 0 ? (static_cast<double>(reserved_bytes) / size_bytes) * 100 : 0};
  } else {
    reserved_usage = {stat.blockSize * static_cast<uint64_t>(std::ceil(stat.blockNumb * (reserved / 100.0))),
                      static_cast<float>(reserved)};
  }
  UsageInfo::Type available{(free.first > reserved_usage.first) ? (free.first - reserved_usage.first) : 0,
                            (free.second > reserved_usage.second) ? (free.second - reserved_usage.second) : 0};
  return {
      .path = path,
      .size = {size_bytes, 100},
      .free = free,
      .reserved = reserved_usage,
      .reserved_by = reserved_by,
      .available = available,
      .required = {},
      .err = "",
  };
}

Volume::UsageInfo& Volume::UsageInfo::withRequired(const uint64_t& val) {
  if (isOk() && size.first > 0) {
    required = {val, (static_cast<double>(val) / size.first) * 100};
  } else {
    required = {val, 0};
  }
  return *this;
}

std::string Volume::UsageInfo::str() const {
  std::stringstream ss;
  ss << *this;
  return ss.str();
}

}  // namespace storage

// NOLINTNEXTLINE(cert-dcl58-cpp,-warnings-as-errors)
namespace std {

ostream& operator<<(ostream& os, const storage::Volume::UsageInfo::Type& t) {
  os << t.first << "B " << t.second << "%";
  return os;
}

ostream& operator<<(ostream& os, const storage::Volume::UsageInfo& t) {
  if (!t.isOk()) {
    if (t.required.first > 0) {
      os << "required: " << t.required.first << "B unknown%";
    }
    os << "; fstatvfs err: " << t.err;
    return os;
  }

  if (t.required.first > 0) {
    os << "required: " << t.required << ", ";
  }
  os << "available: " << t.available << " at " << t.path << ", size: " << t.size << ", free: " << t.free
     << ", reserved: " << t.reserved << "(by `" << t.reserved_by << "`)";
  return os;
}

}  // namespace std

#ifndef AKTUALIZR_LITE_STORAGE_STAT_H_
#define AKTUALIZR_LITE_STORAGE_STAT_H_

#include <cstdint>
#include <string>

namespace storage {

struct Volume {
  struct UsageInfo {
    // <bytes, percentage of overall volume capacity>
    using Type = std::pair<uint64_t, float>;

    std::string path;
    Type size;
    Type free;
    Type reserved;
    std::string reserved_by;
    Type available;

    Type required;

    std::string err;
    bool isOk() const { return err.empty(); }

    UsageInfo& withRequired(const uint64_t& val);
    std::string str() const;
  };

  // Reports filesystem usage for `path` against a `reserved` watermark that bounds how much storage
  // may be consumed. By default `reserved` is a percentage of the volume capacity; when
  // `reserved_in_bytes` is true it is an absolute amount of free space to keep reserved (in bytes).
  static UsageInfo getUsageInfo(const std::string& path, uint64_t reserved, const std::string& reserved_by = "",
                                bool reserved_in_bytes = false);
};

}  // namespace storage

// NOLINTNEXTLINE(cert-dcl58-cpp,-warnings-as-errors)
namespace std {

ostream& operator<<(ostream& os, const storage::Volume::UsageInfo& t);
ostream& operator<<(ostream& os, const storage::Volume::UsageInfo::Type& t);

}  // namespace std

#endif  // AKTUALIZR_LITE_STORAGE_STAT_H_

#ifndef AKTUALIZR_LITE_DOWNLOADER_H_
#define AKTUALIZR_LITE_DOWNLOADER_H_

#include "aktualizr-lite/api.h"
#include "storage/stat.h"

class DownloadResultWithStat : public DownloadResult {
 public:
  storage::Volume::UsageInfo stat;
};

class Downloader {
 public:
  virtual DownloadResultWithStat Download(const TufTarget& target) = 0;

  virtual ~Downloader() = default;
  Downloader(const Downloader&) = delete;
  Downloader(const Downloader&&) = delete;
  Downloader& operator=(const Downloader&) = delete;
  Downloader& operator=(const Downloader&&) = delete;

 protected:
  explicit Downloader() = default;
};

#endif  // AKTUALIZR_LITE_DOWNLOADER_H_

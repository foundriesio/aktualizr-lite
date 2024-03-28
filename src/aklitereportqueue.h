#ifndef AKTUALIZR_LITE_REPORT_QUEUE_H_
#define AKTUALIZR_LITE_REPORT_QUEUE_H_

#include "primary/reportqueue.h"

class AkLiteReportQueue : public ReportQueue {
 public:
  AkLiteReportQueue(const Config& config_in, std::shared_ptr<HttpInterface> http_client,
                    std::shared_ptr<INvStorage> storage_in, int run_pause_s = 10, int event_number_limit = -1)
      : ReportQueue(config_in, std::move(http_client), std::move(storage_in), run_pause_s, event_number_limit) {}

  ~AkLiteReportQueue() override = default;
  AkLiteReportQueue(const AkLiteReportQueue&) = delete;
  AkLiteReportQueue(AkLiteReportQueue&&) = delete;
  AkLiteReportQueue& operator=(const AkLiteReportQueue&) = delete;
  AkLiteReportQueue& operator=(AkLiteReportQueue&&) = delete;

 private:
  bool checkConnectivity(const std::string& server) const override;
};

#endif

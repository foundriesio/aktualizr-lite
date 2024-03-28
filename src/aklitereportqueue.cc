#include "curl/curl.h"

#include "aklitereportqueue.h"

bool AkLiteReportQueue::checkConnectivity(const std::string& server) const {
  // Check if the device has Internet access in a fast way, without establishing a full TLS connection
  bool ret = true;
  CURL* curl = curl_easy_init();
  if (curl != nullptr) {
    curl_easy_setopt(curl, CURLOPT_URL, (server).c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    auto curl_ret = curl_easy_perform(curl);
    // Only assume the device is offline if the CURLE_COULDNT_RESOLVE_HOST is returned.
    // If the device is online, CURLE_PEER_FAILED_VERIFICATION is typically returned.
    if (curl_ret == CURLE_COULDNT_RESOLVE_HOST) {
      ret = false;
    }
    curl_easy_cleanup(curl);
  }
  return ret;
}

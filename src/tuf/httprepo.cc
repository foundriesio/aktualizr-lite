#include "httprepo.h"
#include <curl/curl.h>
#include "logging/logging.h"
#include "target.h"

#include "tuf/localreposource.h"
#include "uptane/exceptions.h"

#include <curl/curl.h>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>

#include <boost/algorithm/string.hpp>

namespace aklite::tuf {

HttpRepo::HttpRepo(const boost::filesystem::path& storage_path) { (void)storage_path; }

HttpRepo::HttpRepo(const Config& config) { (void)config; }

size_t writeFunction(void* ptr, size_t size, size_t nmemb, std::string* data) {
  data->append(static_cast<char*>(ptr), size * nmemb);
  return size * nmemb;
}

std::string curl_request(const std::string& path, bool post) {
  auto* curl = curl_easy_init();
  if (curl == nullptr) {
    LOG_ERROR << "Failed to instantiate curl handler";
    return "";
  }
  // TODO: Make server address configurable
  const std::string server = "http://127.0.0.1/";
  const std::string endpoint = server + path;
  curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  // curl_easy_setopt(curl, CURLOPT_USERPWD, "user:pass");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "aklite/1.0.0");
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  std::string response_string;
  if (post) {
    /* size of the POST data */
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);

    // /* pass in a pointer to the data - libcurl will not copy */
    std::array<unsigned char, 0> b = {};
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, b);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
  }

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunction);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
  curl_easy_setopt(curl, CURLOPT_PORT, 9080);

  std::string header_string;
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_string);

  char* url;
  int64_t response_code;
  double elapsed;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &elapsed);
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);

  auto curl_code = curl_easy_perform(curl);
  if (curl_code != CURLE_OK) {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    LOG_WARNING << (post ? "POST" : "GET") << " request to " << endpoint
                << " failed: " << curl_easy_strerror(curl_code);
    if (http_code != 0) {
      LOG_WARNING << "HTTP code: " << http_code << "\n";
    }
  }

  curl_easy_cleanup(curl);
  curl = nullptr;

  return response_string;
}

static std::vector<TufTarget> parseTargets(const std::string& targets_raw) {
  // targets_raw contains the "signed" portion of the targets.json file
  auto targets_json = Utils::parseJSON(targets_raw);
  if (!targets_json.isObject()) {
    throw Uptane::InvalidMetadata("", "targets", "invalid targets.json");
  }

  std::vector<TufTarget> targets;
  for (auto t_it = targets_json.begin(); t_it != targets_json.end(); t_it++) {
    const auto& content = *t_it;
    TufTarget t(t_it.key().asString(), content["hashes"]["sha256"].asString(),
                std::stoi(content["custom"]["version"].asString()), content["custom"]);
    targets.push_back(t);
  }
  return targets;
}

std::vector<TufTarget> HttpRepo::GetTargets() {
  const auto* url = "targets";
  auto targets_raw = curl_request(url, false);
  if (targets_raw.empty()) {
    return {};
  } else {
    return parseTargets(targets_raw);
  }
}

std::string HttpRepo::GetRoot(int version) {
  if (version != -1) {
    LOG_WARNING << "Fetching specific Root version is not supported. Retrieving the last one.";
  }

  const auto* url = "root";
  auto root_raw = curl_request(url, false);
  return root_raw;
}

void HttpRepo::UpdateMeta(std::shared_ptr<RepoSource> repo_src) {
  auto local_repo_src = std::dynamic_pointer_cast<aklite::tuf::LocalRepoSource>(repo_src);

  std::string url;
  if (local_repo_src != nullptr) {
    url = "targets/update/?localTufRepo=" + local_repo_src->SourceDir();
  } else {
    url = "targets/update/";
  }
  curl_request(url, true);
}

void HttpRepo::CheckMeta() { LOG_WARNING << "Skipping CheckMeta"; }

}  // namespace aklite::tuf

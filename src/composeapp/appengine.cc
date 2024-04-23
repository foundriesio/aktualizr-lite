#include "appengine.h"

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "exec.h"
#include "storage/stat.h"

namespace composeapp {
enum class ExitCode { ExitCodeInsufficientSpace = 100 };

AppEngine::Result AppEngine::fetch(const App& app) {
  Result res{false};
  try {
    if (local_source_path_.empty()) {
      exec(boost::format{"%s --store %s pull -p %s --storage-usage-watermark %d"} % composectl_cmd_ % storeRoot() %
               app.uri % storage_watermark_,
           "failed to pull compose app");
    } else {
      exec(boost::format{"%s --store %s pull -p %s -l %s --storage-usage-watermark %d"} % composectl_cmd_ %
               storeRoot() % app.uri % local_source_path_ % storage_watermark_,
           "failed to pull compose app");
    }
    res = true;
  } catch (const ExecError& exc) {
    if (exc.ExitCode == static_cast<int>(ExitCode::ExitCodeInsufficientSpace)) {
      const auto usage_stat{Utils::parseJSON(exc.StdErr)};
      auto usage_info{storageSpaceFunc()(usage_stat["path"].asString())};
      res = {Result::ID::InsufficientSpace, exc.what(), usage_info.withRequired(usage_stat["required"].asUInt64())};
    } else {
      res = {false, exc.what()};
    }
  } catch (const std::exception& exc) {
    res = {false, exc.what()};
  }
  return res;
}

bool AppEngine::isAppFetched(const App& app) const {
  bool res{false};
  try {
    std::future<std::string> output;
    exec(boost::format{"%s --store %s check %s --local --format json"} % composectl_cmd_ % storeRoot() % app.uri, "",
         boost::process::std_out > output);
    const std::string output_str{output.get()};
    const auto app_fetch_status{Utils::parseJSON(output_str)};
    if (app_fetch_status.isMember("fetch_check") && app_fetch_status["fetch_check"].isMember("missing_blobs") &&
        app_fetch_status["fetch_check"]["missing_blobs"].empty()) {
      res = true;
    }
  } catch (const ExecError& exc) {
    LOG_DEBUG << "app is not fully fetched; app: " << app.name << ", status: " << exc.what();
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to verify whether app is fetched; app: " << app.name << ", err: " << exc.what();
    throw;
  }
  return res;
}

void AppEngine::installAppAndImages(const App& app) {
  exec(boost::format{"%s --store %s --compose %s --host %s install %s"} % composectl_cmd_ % storeRoot() %
           installRoot() % dockerHost() % app.uri,
       "failed to installl compose app");
}

}  // namespace composeapp

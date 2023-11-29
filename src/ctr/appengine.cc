#include "appengine.h"

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "exec.h"

namespace ctr {
  AppEngine::Result AppEngine::fetch(const App& app) {
    exec(boost::format{"%s --store %s pull %s --storage-usage-watermark %d"} % composectl_cmd_ % storeRoot() % app.uri % storage_watermark_, "failed to pull compose app");
    return true;
  }
  void AppEngine::installAppAndImages(const App& app) {
    exec(boost::format{"%s --store %s install --compose-dir %s --docker-host %s %s"} % composectl_cmd_ % storeRoot() % installRoot() % dockerHost() % app.uri, "failed to installl compose app");
  }

} // namespace ctr

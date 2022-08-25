#include "engine.h"
#include "exec.h"

namespace containerd {

void Engine::pullImages(const App& app) {
  LOG_INFO << "Pulling containers";
  runComposeCmd(app, "pull", "failed to pull App images");
}
void Engine::installApp(const App& app) {
  LOG_INFO << "Installing App";
  runComposeCmd(app, "up --remove-orphans -d", "failed to install App");
}
void Engine::runComposeCmd(const App& app, const std::string& cmd, const std::string& err_msg) const {
  exec(compose_ + "--project-directory " + appRoot(app).string() + " " + cmd, err_msg);
}

}  // namespace containerd

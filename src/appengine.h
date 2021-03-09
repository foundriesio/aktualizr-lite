#ifndef AKTUALIZR_LITE_APP_ENGINE_H_
#define AKTUALIZR_LITE_APP_ENGINE_H_

#include <string>

class AppEngine {
 public:
  struct App {
    std::string name;
    std::string uri;
  };

 public:
  virtual bool fetch(const App& app) = 0;
  virtual bool install(const App& app) = 0;
  virtual bool run(const App& app) = 0;
  virtual void remove(const App& app) = 0;
  virtual bool isRunning(const App& app) const = 0;

 public:
  virtual ~AppEngine() = default;
  AppEngine(const AppEngine&&) = delete;
  AppEngine(const AppEngine&) = delete;
  AppEngine& operator=(const AppEngine&) = delete;

 protected:
  AppEngine() = default;
};

#endif  // AKTUALIZR_LITE_APP_ENGINE_H_

#ifndef AKTUALIZR_LITE_APP_ENGINE_H_
#define AKTUALIZR_LITE_APP_ENGINE_H_

#include <memory>
#include <set>
#include <string>

#include "json/json.h"

class AppEngine {
 public:
  struct App {
    std::string name;
    std::string uri;
  };

  struct Result {
    enum class ID {
      OK = 0,
      Failed,
      InsufficientSpace,
    };
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
    Result(bool var, std::string errMsg = "") : status{var ? ID::OK : ID::Failed}, err{std::move(errMsg)} {}
    Result(Result::ID id, std::string errMsg) : status{id}, err{std::move(errMsg)} {}
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
    operator bool() const { return status == ID::OK; }
    bool noSpace() const { return status == ID::InsufficientSpace; }

    ID status;
    std::string err;
  };

  using Apps = std::vector<App>;
  using Ptr = std::shared_ptr<AppEngine>;

  virtual Result fetch(const App& app) = 0;
  virtual Result verify(const App& app) = 0;
  virtual Result install(const App& app) = 0;
  virtual Result run(const App& app) = 0;
  virtual void remove(const App& app) = 0;
  virtual bool isFetched(const App& app) const = 0;
  virtual bool isRunning(const App& app) const = 0;
  virtual Json::Value getRunningAppsInfo() const = 0;
  virtual void prune(const Apps& app_shortlist) = 0;

  virtual ~AppEngine() = default;
  AppEngine(const AppEngine&&) = delete;
  AppEngine(const AppEngine&) = delete;
  AppEngine& operator=(const AppEngine&) = delete;
  AppEngine& operator=(const AppEngine&&) = delete;

 protected:
  AppEngine() = default;
};

#endif  // AKTUALIZR_LITE_APP_ENGINE_H_

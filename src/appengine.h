#ifndef AKTUALIZR_LITE_APP_ENGINE_H_
#define AKTUALIZR_LITE_APP_ENGINE_H_

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "json/json.h"

class AppEngine {
 public:
  class Client {
   public:
    using Ptr = std::shared_ptr<Client>;

    virtual void getContainers(Json::Value& root) = 0;
    virtual std::tuple<bool, std::string> getContainerState(const Json::Value& root, const std::string& app,
                                                            const std::string& service,
                                                            const std::string& hash) const = 0;
    virtual std::string getContainerLogs(const std::string& id, int tail) = 0;
    virtual const Json::Value& engineInfo() const = 0;
    virtual const std::string& arch() const = 0;
    virtual Json::Value getRunningApps(const std::function<void(const std::string&, Json::Value&)>& ext_func) = 0;

    virtual ~Client() = default;
    Client(const Client&&) = delete;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client& operator=(const Client&&) = delete;

   protected:
    Client() = default;
  };

  struct App {
    std::string name;
    std::string uri;
  };

  struct Result {
    enum class ID {
      OK = 0,
      Failed,
      InsufficientSpace,
      ImagePullFailure,
    };
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
    Result(bool var, std::string errMsg = "") : status{var ? ID::OK : ID::Failed}, err{std::move(errMsg)} {}
    Result(Result::ID id, std::string errMsg) : status{id}, err{std::move(errMsg)} {}
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
    operator bool() const { return status == ID::OK; }
    bool noSpace() const { return status == ID::InsufficientSpace; }
    bool imagePullFailure() const { return status == ID::ImagePullFailure; }

    ID status;
    std::string err;
  };

  using Apps = std::vector<App>;
  using Ptr = std::shared_ptr<AppEngine>;

  virtual Result fetch(const App& app) = 0;
  virtual Result verify(const App& app) = 0;
  virtual Result install(const App& app) = 0;
  virtual Result run(const App& app) = 0;
  virtual void stop(const App& app) = 0;
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

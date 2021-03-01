#ifndef AKTUALIZR_LITE_TARGET_H_
#define AKTUALIZR_LITE_TARGET_H_

#include <boost/optional.hpp>
#include "logging/logging.h"

#include "uptane/tuf.h"

class Target {
 public:
  static constexpr const char* const TagField{"tags"};
  static constexpr const char* const ComposeAppField{"docker_compose_apps"};
  static constexpr const char* const ComposeAppOstreeUri{"compose-apps-uri"};

  struct Version {
    std::string raw_ver;
    Version(std::string version) : raw_ver(std::move(version)) {}

    bool operator<(const Version& other) { return strverscmp(raw_ver.c_str(), other.raw_ver.c_str()) < 0; }
  };

 public:
  static bool hasTag(const Uptane::Target& target, const std::vector<std::string>& tags);
  static void setCorrelationID(Uptane::Target& target);
  static std::string ostreeURI(const Uptane::Target& target);
  static Json::Value appsJson(const Uptane::Target& target);
  static void setAppsJson(Uptane::Target& target, const Json::Value& apps_json);

  static void log(const std::string& prefix, const Uptane::Target& target,
                  boost::optional<std::set<std::string>> shortlist);

  class Apps {
   public:
    struct AppDesc {
      AppDesc(const std::string& app_name, const Json::Value& app_json) : name{app_name} {
        if (app_json.isNull() || !app_json.isObject() || !app_json.isMember("uri")) {
          throw std::runtime_error("Invalid format of App in Target json: " + app_json.toStyledString());
        }
        uri = app_json["uri"].asString();
      }
      AppDesc() = delete;
      bool operator==(const AppDesc& app_desc) const { return name == app_desc.name && uri == app_desc.uri; }

      std::string name;
      std::string uri;
    };

   public:
    Apps(const Uptane::Target& target) : target_apps_json_{Target::appsJson(target)} {}

    class Iterator {
     public:
      bool operator!=(const Iterator& rhs) const { return json_iter_ != rhs.json_iter_; }
      AppDesc operator*() const { return {json_iter_.key().asString(), *json_iter_}; }
      Iterator operator++() {
        ++json_iter_;
        return *this;
      }

     private:
      friend class Target::Apps;
      Iterator() = delete;
      Iterator& operator=(const Iterator&) = delete;
      Iterator(Json::ValueConstIterator&& json_iter) : json_iter_{json_iter} {}

     private:
      Json::ValueConstIterator json_iter_;
    };

    Iterator begin() const { return target_apps_json_.begin(); }
    Iterator end() const { return target_apps_json_.end(); }

    bool exists(const AppDesc& app) const {
      if (!target_apps_json_.isMember(app.name)) {
        return false;
      }

      return app == AppDesc{app.name, target_apps_json_[app.name]};
    }

    void remove(const AppDesc& app) { target_apps_json_.removeMember(app.name); }

    Uptane::Target createTarget(const Uptane::Target& target) {
      auto result{target};
      Target::setAppsJson(result, target_apps_json_);
      return result;
    }

    bool empty() const { return target_apps_json_.empty(); }

   private:
    Json::Value target_apps_json_;
  };  // Apps
};    // Target

#endif  // AKTUALIZR_LITE_TARGET_H_

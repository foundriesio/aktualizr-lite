#ifndef AKTUALIZR_LITE_TARGET_H_
#define AKTUALIZR_LITE_TARGET_H_

#include <boost/optional.hpp>
#include "logging/logging.h"

#include "uptane/tuf.h"

// This is the prototype of Target class of the future aklite's UpdateAgent that wraps the libaktualizr pack manager
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

  static void log(const std::string& prefix, const Uptane::Target& target,
                  boost::optional<std::set<std::string>> shortlist);

  static Uptane::Target subtractCurrentApps(const Uptane::Target& target, const Uptane::Target& current,
                                            boost::optional<std::set<std::string>> shortlist);
};

namespace aklite {

class Target {
 public:
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

      std::string name;
      std::string uri;
    };

   public:
    Apps(const Uptane::Target& target) {
      target_apps_json_ = Json::nullValue;
      if (!target.custom_data().isNull()) {
        target_apps_json_ = target.custom_data().get(::Target::ComposeAppField, Json::Value(Json::nullValue));
      }
    }

    class Iterator {
     public:
      bool operator!=(const Iterator& rhs) const { return json_iter_ != rhs.json_iter_; }
      AppDesc operator*() const { return {json_iter_.key().asString(), *json_iter_}; }
      Iterator operator++() {
        ++json_iter_;
        return *this;
      }

     private:
      friend class aklite::Target::Apps;
      Iterator() = delete;
      Iterator& operator=(const Iterator&) = delete;
      Iterator(Json::ValueConstIterator&& json_iter) : json_iter_{json_iter} {}

     private:
      Json::ValueConstIterator json_iter_;
    };

    Iterator begin() const { return target_apps_json_.begin(); }
    Iterator end() const { return target_apps_json_.end(); }

   private:
    Json::Value target_apps_json_;
  };  // Apps

};  // Target

}  // namespace aklite

#endif  // AKTUALIZR_LITE_TARGET_H_

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "httplib.h"
#include "libaktualizr/config.h"
#include "logging/logging.h"
#include "utilities/utils.h"

#include "aktualizr-lite/api.h"

namespace bpo = boost::program_options;

static void json_resp(httplib::Response& res, int code, const Json::Value& data) {
  res.status = code;
  Json::StreamWriterBuilder builder;
  auto content = Json::writeString(builder, data);
  res.set_content(content, "application/json");
}

class ApiException : public std::exception {
 public:
  ApiException(int status, Json::Value resp) : status(status), resp(std::move(resp)) {
    what_ = "HTTP_" + std::to_string(status);
  }

  const char* what() const noexcept override { return what_.c_str(); }

  int status;
  Json::Value resp;

 private:
  std::string what_;
};

struct CurrentInstaller {
  int id;
  std::unique_ptr<InstallContext> installer;
};

static void check_in(const AkliteClient& client, const httplib::Request& req, httplib::Response& res) {
  (void)req;
  LOG_DEBUG << "check_in called";
  auto result = client.CheckIn();
  int code = 200;
  Json::Value targets;
  if (result.status != CheckInResult::Status::Ok && result.status != CheckInResult::Status::OkCached) {
    code = 500;
  } else {
    for (const auto& t : result.Targets()) {
      Json::Value target;
      target["name"] = t.Name();
      target["version"] = t.Version();
      target["ostree-sha256"] = t.Sha256Hash();
      targets.append(target);
    }
  }
  Json::Value data;
  data["targets"] = targets;
  json_resp(res, code, data);
}

static void create_installer(const AkliteClient& client, const httplib::Request& req, httplib::Response& res,
                             CurrentInstaller* cur_installer) {
  (void)req;
  LOG_DEBUG << "create_installer called";

  Json::Value data;
  Json::Value input = Utils::parseJSON(req.body);

  auto target_name = input["target-name"];
  if (!target_name.isString()) {
    data["error"] = "Missing required item: target-name";
    throw ApiException(400, data);
  }

  std::string reason = "Update to " + target_name.asString();
  auto reason_val = input["reason"];
  if (reason_val.isString()) {
    reason = reason_val.asString();
  }

  TufTarget t(target_name.asString(), "", 0, Json::Value());
  cur_installer->id++;
  cur_installer->installer = client.Installer(t, reason);

  data["installer-id"] = cur_installer->id;
  json_resp(res, 201, data);
}

static void installer_download(const httplib::Request& req, httplib::Response& res, CurrentInstaller* cur_installer) {
  auto id_match = req.matches[1];
  LOG_DEBUG << "installer_download(" << id_match << ") called";

  Json::Value data;
  int id = -1;
  try {
    id = std::stoi(id_match.str(), nullptr, 0);
  } catch (const std::invalid_argument& exc) {
    // This should be "impossible". The regex given to httplib requires an int
    data["error"] = "Invalid format for installer-id";
    throw ApiException(400, data);
  }

  if (id != cur_installer->id) {
    data["error"] = "Invalid installer-id";
    throw ApiException(404, data);
  }

  auto result = cur_installer->installer->Download();
  int code = 500;
  if (result.status == DownloadResult::Status::Ok) {
    data["status"] = "Ok";
    code = 200;
  } else if (result.status == DownloadResult::Status::DownloadFailed) {
    data["status"] = "DownloadFailed";
  } else if (result.status == DownloadResult::Status::VerificationFailed) {
    data["status"] = "VerificationFailed";
  } else {
    data["status"] = "Unknown Error";
  }

  if (!result.description.empty()) {
    data["description"] = result.description;
  }
  json_resp(res, code, data);
}

static void installer_install(const httplib::Request& req, httplib::Response& res, CurrentInstaller* cur_installer) {
  auto id_match = req.matches[1];
  LOG_DEBUG << "installer_install(" << id_match << ") called";

  Json::Value data;
  int id = -1;
  try {
    id = std::stoi(id_match.str(), nullptr, 0);
  } catch (const std::invalid_argument& exc) {
    // This should be "impossible". The regex given to httplib requires an int
    data["error"] = "Invalid format for installer-id";
    throw ApiException(400, data);
  }

  if (id != cur_installer->id) {
    data["error"] = "Invalid installer-id";
    throw ApiException(404, data);
  }

  auto result = cur_installer->installer->Install();
  int code = 500;
  if (result.status == InstallResult::Status::Ok) {
    data["status"] = "Ok";
    code = 200;
  } else if (result.status == InstallResult::Status::NeedsCompletion) {
    data["status"] = "NeedsCompletion";
    code = 202;
  } else if (result.status == InstallResult::Status::Failed) {
    data["status"] = "Failed";
  } else {
    data["status"] = "Unknown Error";
  }

  if (!result.description.empty()) {
    data["description"] = result.description;
  }
  json_resp(res, code, data);
}

static void get_config(const AkliteClient& client, const httplib::Request& req, httplib::Response& res) {
  (void)req;
  auto pt = client.GetConfig();
  std::stringstream jsonss;
  boost::property_tree::json_parser::write_json(jsonss, pt);

  res.set_content(jsonss.str(), "application/json");
}

static void get_current_target(const AkliteClient& client, const httplib::Request& req, httplib::Response& res) {
  (void)req;
  LOG_DEBUG << "get_current_target called";
  auto current = client.GetCurrent();
  Json::Value target;
  target["name"] = current.Name();
  target["version"] = current.Version();
  target["ostree-sha256"] = current.Sha256Hash();
  json_resp(res, 200, target);
}

static void get_rollback_target(const AkliteClient& client, const httplib::Request& req, httplib::Response& res) {
  auto target_name = req.matches[1];
  LOG_ERROR << "get_rollback_target(" << target_name << ") called";
  TufTarget t(target_name, "", 0, Json::Value());
  int code = 404;
  if (client.IsRollback(t)) {
    code = 200;
  }
  json_resp(res, code, Json::Value());
}

bpo::variables_map parse_options(int argc, char** argv) {
  bpo::options_description description("aktualizr-lited command line options");

  // clang-format off
  description.add_options()
      ("help,h", "print usage")
      ("loglevel", bpo::value<int>(), "set log level 0-5 (trace, debug, info, warning, error, fatal)")
      ("config,c", bpo::value<std::vector<boost::filesystem::path> >()->composing(), "configuration file or directory")
      ("socket-path", bpo::value<boost::filesystem::path>(), "The unix domain socket path to bind to. Default=/var/run/aklite.sock");
  // clang-format on

  bpo::positional_options_description pos;

  bpo::variables_map vm;
  std::vector<std::string> unregistered_options;
  try {
    bpo::basic_parsed_options<char> parsed_options = bpo::command_line_parser(argc, argv).options(description).run();
    bpo::store(parsed_options, vm);
    bpo::notify(vm);
    if (vm.count("help") != 0) {
      std::cout << description << "\n";
      exit(EXIT_FAILURE);
    }
  } catch (const bpo::required_option& ex) {
    // print the error and append the default commandline option description
    std::cout << ex.what() << std::endl << description;
    exit(EXIT_FAILURE);
  } catch (const bpo::error& ex) {
    LOG_ERROR << "boost command line option error: " << ex.what();
    exit(EXIT_FAILURE);
  }

  return vm;
}

int main(int argc, char** argv) {
  logger_init(isatty(1) == 1);
  logger_set_threshold(boost::log::trivial::info);

  bpo::variables_map cli_map = parse_options(argc, argv);

  if (geteuid() != 0) {
    LOG_ERROR << "Running as non-root!\n";
  }

  try {
    auto dirs = AkliteClient::CONFIG_DIRS;
    if (cli_map.count("config") > 0) {
      dirs = cli_map["config"].as<std::vector<boost::filesystem::path>>();
    }
    AkliteClient client(dirs);

    httplib::Server svr;

    svr.set_logger([](const httplib::Request& req, const httplib::Response& resp) {
      LOG_INFO << req.method << " " << req.path << " HTTP_" << resp.status << " " << resp.reason;
    });
    svr.set_exception_handler([](const auto& req, auto& res, std::exception& e) {
      (void)req;
      if (auto* apie = dynamic_cast<ApiException*>(&e)) {
        json_resp(res, apie->status, apie->resp);
      } else {
        Json::Value data;
        data["error"] = e.what();
        json_resp(res, 500, data);
      }
    });

    std::mutex client_mutex;
    CurrentInstaller installer{0, nullptr};

    svr.Get("/check_in", [&client, &client_mutex](const httplib::Request& req, httplib::Response& res) {
      std::lock_guard<std::mutex> guard(client_mutex);
      check_in(client, req, res);
    });
    svr.Get("/config", [&client, &client_mutex](const httplib::Request& req, httplib::Response& res) {
      std::lock_guard<std::mutex> guard(client_mutex);
      get_config(client, req, res);
    });
    svr.Get("/targets/current", [&client, &client_mutex](const httplib::Request& req, httplib::Response& res) {
      std::lock_guard<std::mutex> guard(client_mutex);
      get_current_target(client, req, res);
    });
    svr.Get("/targets/rollback/(\\S+)", [&client, &client_mutex](const httplib::Request& req, httplib::Response& res) {
      std::lock_guard<std::mutex> guard(client_mutex);
      get_rollback_target(client, req, res);
    });
    svr.Post("/targets/installer",
             [&client, &client_mutex, &installer](const httplib::Request& req, httplib::Response& res) {
               std::lock_guard<std::mutex> guard(client_mutex);
               create_installer(client, req, res, &installer);
             });
    svr.Post("/targets/installer/(\\d+)/download",
             [&client, &client_mutex, &installer](const httplib::Request& req, httplib::Response& res) {
               std::lock_guard<std::mutex> guard(client_mutex);
               installer_download(req, res, &installer);
             });
    svr.Post("/targets/installer/(\\d+)/install",
             [&client, &client_mutex, &installer](const httplib::Request& req, httplib::Response& res) {
               std::lock_guard<std::mutex> guard(client_mutex);
               installer_install(req, res, &installer);
             });

    boost::filesystem::path socket_path("/var/run/aklite.sock");
    if (cli_map.count("socket-path") == 1) {
      socket_path = cli_map["socket-path"].as<boost::filesystem::path>();
    }
    LOG_INFO << "Server started on " << socket_path;
    if (svr.listen_unix_domain(socket_path.string().c_str())) {
      return 0;
    }
    return 1;
  } catch (const std::exception& ex) {
    LOG_ERROR << ex.what();
  }
  return EXIT_FAILURE;
}

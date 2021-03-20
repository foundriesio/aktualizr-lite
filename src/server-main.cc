#include <boost/container/flat_map.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "httplib.h"
#include "libaktualizr/config.h"
#include "logging/logging.h"

#include "helpers.h"

namespace bpo = boost::program_options;

static void json_resp(httplib::Response& res, int code, const Json::Value& data) {
  res.status = code;
  Json::StreamWriterBuilder builder;
  auto content = Json::writeString(builder, data);
  res.set_content(content.c_str(), "application/json");
}

class ApiException : public std::exception {
 public:
  ApiException(int status, Json::Value resp) : status(status), resp(std::move(resp)) {
    what_ = "HTTP_" + std::to_string(status) + " : " + resp.asString();
  }

  const char* what() const throw() { return what_.c_str(); }

  int status;
  Json::Value resp;

 private:
  std::string what_;
};

static void get_config(LiteClient& client, const httplib::Request& req, httplib::Response& res) {
  auto cmd = client.config.bootloader.reboot_command;

  std::stringstream ss;
  ss << client.config;

  boost::property_tree::ptree pt;
  boost::property_tree::ini_parser::read_ini(ss, pt);

  std::stringstream jsonss;
  boost::property_tree::json_parser::write_json(jsonss, pt);

  res.set_content(jsonss.str(), "application/json");
}

static void send_telemetry(LiteClient& client, const httplib::Request& req, httplib::Response& res) {
  client.reportNetworkInfo();
  client.reportHwInfo();
  Json::Value resp;
  json_resp(res, 200, resp);
}

static void get_current_target(LiteClient& client, const httplib::Request& req, httplib::Response& res) {
  auto current = client.getCurrent();
  Json::Value target;
  target["name"] = current.filename();
  target["version"] = current.custom_version();
  target["ostree-sha256"] = current.sha256Hash();
  target["docker_compose_apps"] = current.custom_data()["docker_compose_apps"];
  json_resp(res, 200, target);
}

static void get_targets(LiteClient& client, const httplib::Request& req, httplib::Response& res) {
  Json::Value data;

  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);

  LOG_INFO << "Refreshing Targets metadata";
  const auto rc = client.updateImageMeta();
  if (!std::get<0>(rc)) {
    LOG_WARNING << "Unable to update latest metadata, using local copy: " << std::get<1>(rc);
    if (!client.checkImageMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      data["warning"] = "Unable to update latest metadata and local copy is out-of-date";
      throw ApiException(500, data);
    }
  }

  boost::container::flat_map<int, Uptane::Target> sorted_targets;
  for (const auto& t : client.allTargets()) {
    int ver = 0;
    try {
      ver = std::stoi(t.custom_version(), nullptr, 0);
    } catch (const std::invalid_argument& exc) {
      LOG_ERROR << "Invalid version number format: " << t.custom_version();
      ver = -1;
    }
    if (!target_has_tags(t, client.tags)) {
      continue;
    }
    for (const auto& it : t.hardwareIds()) {
      if (it == hwid) {
        sorted_targets.emplace(ver, t);
        break;
      }
    }
  }

  Json::Value targets;
  for (auto& pair : sorted_targets) {
    Json::Value target;
    target["name"] = pair.second.filename();
    target["version"] = pair.first;
    target["ostree-sha256"] = pair.second.sha256Hash();
    target["docker_compose_apps"] = pair.second.custom_data()["docker_compose_apps"];
    targets.append(target);
  }
  data["targets"] = targets;
  json_resp(res, 200, data);
}

static void validate_target(LiteClient& client, const Uptane::Target& t) {
  Json::Value data;
  if (!t.IsValid()) {
    data["error"] = "Target isn't valid";
    throw ApiException(400, data);
  }

  if (!t.IsOstree()) {
    data["error"] = "Target isn't Ostree";
    throw ApiException(400, data);
  }

  if (!target_has_tags(t, client.tags)) {
    data["error"] = "Target does not have corect tag";
    throw ApiException(400, data);
  }

  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);
  for (auto const& it : t.hardwareIds()) {
    if (it == hwid) {
      return;
    }
  }
  data["error"] = "Target not a match for this hwid";
  throw ApiException(400, data);
}

static std::unique_ptr<Uptane::Target> find_target(LiteClient& client, const std::string name) {
  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);
  for (const auto& t : client.allTargets()) {
    if (t.filename() == name) {
      validate_target(client, t);
      return std_::make_unique<Uptane::Target>(t);
    }
  }
  throw ApiException(404, Json::Value{});
}

static void download_target(LiteClient& client, const httplib::Request& req, httplib::Response& res) {
  Json::Value data;
  Json::Value input = Utils::parseJSON(req.body);

  auto target_name = input["target-name"];
  if (!target_name.isString()) {
    data["error"] = "Missing required item: target-name";
    throw ApiException(400, data);
  }
  auto correlation_id = input["correlation-id"];
  if (!correlation_id.isString()) {
    data["error"] = "Missing required item: correlation-id";
    throw ApiException(400, data);
  }

  std::string reason = "Update to " + target_name.asString();
  auto reason_val = input["reason"];
  if (reason_val.isString()) {
    reason = reason_val.asString();
  }

  auto target = find_target(client, target_name.asString());
  client.logTarget("Downloading: ", *target);
  target->setCorrelationId(correlation_id.asString());

  // TODO - there's almost no chance this is going to work for huge
  // downloads. Do we make this a non-blocking call and have the client
  // poll on the correlation id? Or maybe do a thread with some stupid
  // server side events type thing?
  data::ResultCode::Numeric rc = client.download(*target, reason);
  if (rc != data::ResultCode::Numeric::kOk) {
    data["error"] = "Unable to download target";
    data["rc"] = (int)rc;
    json_resp(res, 500, data);
    return;
  }

  if (client.VerifyTarget(*target) != TargetStatus::kGood) {
    data::InstallationResult ires{data::ResultCode::Numeric::kVerificationFailed, "Downloaded target is invalid"};
    client.notifyInstallFinished(*target, ires);
    LOG_ERROR << "Downloaded target is invalid";
    data["error"] = "Downloaded target is invalid";
    data["rc"] = (int)ires.result_code.num_code;
    json_resp(res, 500, data);
    return;
  }

  json_resp(res, 201, data);
}

static void install_target(LiteClient& client, const httplib::Request& req, httplib::Response& res) {
  Json::Value data;
  Json::Value input = Utils::parseJSON(req.body);

  auto target_name = input["target-name"];
  if (!target_name.isString()) {
    data["error"] = "Missing required item: target-name";
    throw ApiException(400, data);
  }
  auto correlation_id = input["correlation-id"];
  if (!correlation_id.isString()) {
    data["error"] = "Missing required item: correlation-id";
    throw ApiException(400, data);
  }

  auto target = find_target(client, target_name.asString());
  client.logTarget("Installing: ", *target);
  target->setCorrelationId(correlation_id.asString());

  // Check for rollback
  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(client, known_but_not_installed_versions);
  if (known_local_target(client, *target, known_but_not_installed_versions)) {
    data["error"] = "Target has caused a prior rollback. Aborting installing";
    throw ApiException(400, data);
  }

  int status = 500;
  data::ResultCode::Numeric rc = client.install(*target);
  data["rc"] = (int)rc;
  if (rc == data::ResultCode::Numeric::kNeedCompletion || rc == data::ResultCode::Numeric::kOk) {
    data["needs-reboot"] = true;
    if (rc != data::ResultCode::Numeric::kNeedCompletion) {
      data["needs-reboot"] = false;
      client.http_client->updateHeader("x-ats-target", target->filename());
    }
    status = 201;
  }
  json_resp(res, status, data);
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
    return 1;
  }

  try {
    Config config(cli_map);
    config.telemetry.report_network = !config.tls.server.empty();
    config.telemetry.report_config = !config.tls.server.empty();

    LiteClient client(config);
    client.finalizeInstall();
    client.reportAktualizrConfiguration();

    // There's an old bug in libaktualizr that causes a core-dump if this
    // isn't run before you try to download/install a Target. Being defensive
    // here so that a client doesn't mistakenly try the API this way.
    client.updateImageMeta();

    httplib::Server svr;

    svr.set_logger([](const httplib::Request& req, const httplib::Response& resp) {
      LOG_INFO << req.method << " " << req.path << " HTTP_" << resp.status << " " << resp.reason;
    });
    svr.set_exception_handler([](const auto& req, auto& res, std::exception& e) {
      if (auto* apie = dynamic_cast<ApiException*>(&e)) {
        json_resp(res, apie->status, apie->resp);
      } else {
        Json::Value data;
        data["error"] = e.what();
        json_resp(res, 500, data);
      }
    });

    svr.Get("/config",
            [&client](const httplib::Request& req, httplib::Response& res) { get_config(client, req, res); });
    svr.Put("/telemetry",
            [&client](const httplib::Request& req, httplib::Response& res) { send_telemetry(client, req, res); });
    svr.Get("/targets",
            [&client](const httplib::Request& req, httplib::Response& res) { get_targets(client, req, res); });
    svr.Get("/targets/current",
            [&client](const httplib::Request& req, httplib::Response& res) { get_current_target(client, req, res); });
    svr.Post("/targets/download",
             [&client](const httplib::Request& req, httplib::Response& res) { download_target(client, req, res); });
    svr.Post("/targets/install",
             [&client](const httplib::Request& req, httplib::Response& res) { install_target(client, req, res); });

    boost::filesystem::path socket_path("/var/run/aklite.sock");
    if (cli_map.count("socket-path") == 1) {
      socket_path = cli_map["socket-path"].as<boost::filesystem::path>();
    }
    LOG_INFO << "Server started on " << socket_path;
    return !svr.listen_unix_domain(socket_path.string().c_str());
  } catch (const std::exception& ex) {
    LOG_ERROR << ex.what();
  }
  return EXIT_FAILURE;
}

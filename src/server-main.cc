#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "httplib.h"
#include "libaktualizr/config.h"
#include "logging/logging.h"

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
    what_ = "HTTP_" + std::to_string(status) + " : " + this->resp.asString();
  }

  const char* what() const noexcept override { return what_.c_str(); }

  int status;
  Json::Value resp;

 private:
  std::string what_;
};

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

    svr.Get("/config",
            [&client](const httplib::Request& req, httplib::Response& res) { get_config(client, req, res); });
    svr.Get("/targets/current",
            [&client](const httplib::Request& req, httplib::Response& res) { get_current_target(client, req, res); });

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

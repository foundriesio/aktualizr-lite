#include <iostream>
#include <vector>

#include <boost/program_options.hpp>

#include "logging/logging.h"

#include "cmds.h"

namespace po = boost::program_options;

static std::vector<apps::aklite_offline::Cmd::Ptr> cmds{
    std::make_shared<apps::aklite_offline::CheckCmd>(),
    std::make_shared<apps::aklite_offline::InstallCmd>(),
    std::make_shared<apps::aklite_offline::RunCmd>(),
    std::make_shared<apps::aklite_offline::CurrentCmd>(),
};

static void print_usage() {
  std::cout << "Usage:\n  aklite-offline <cmd> [flags]\n\nSupported commands: ";
  for (const auto& cmd : cmds) {
    auto name = cmd->name();
    std::cout << "\n  " << name.append(12 - name.length(), ' ') << " " << cmd->description();
  }
  std::cout << "\n\n";
}

static void print_usage_cmd(const apps::aklite_offline::Cmd& cmd) {
  std::cout << cmd.description() << "\n\nUsage:\n  aklite-offline " << cmd.name() << " [flags]\n\nFlags: \n";
  std::cout << cmd.options();
  std::cout << "\n\n";
}

int main(int argc, char** argv) {
  if (argc == 1) {
    std::cerr << "Missing command\n\n";
    print_usage();
    exit(EXIT_FAILURE);
  }

  const std::string cmd_name{argv[1]};
  const auto find_it{std::find_if(cmds.begin(), cmds.end(), [&cmd_name](const apps::aklite_offline::Cmd::Ptr& cmd) {
    return cmd->name() == cmd_name;
  })};

  if (cmds.end() == find_it) {
    if (cmd_name != "--help") {
      LOG_ERROR << "Unsupported command: " << cmd_name << "\n";
    }
    print_usage();
    exit(EXIT_FAILURE);
  }

  const apps::aklite_offline::Cmd& cmd{**find_it};
  po::options_description cmd_opts{cmd.options()};
  po::variables_map vm;
  po::positional_options_description run_pos;
  run_pos.add("cmd", -1);
  po::options_description arg_opts;
  arg_opts.add_options()("cmd", po::value<std::string>());
  arg_opts.add(cmd_opts);

  try {
    po::store(po::command_line_parser(argc, argv).options(arg_opts).positional(run_pos).run(), vm);
    po::notify(vm);
  } catch (const std::exception& exc) {
    if (vm.count("help") < 1) {
      LOG_ERROR << exc.what() << "\n";
    }
    print_usage_cmd(cmd);
    exit(EXIT_FAILURE);
  }

  if (vm.count("help") == 1) {
    print_usage_cmd(cmd);
    exit(EXIT_SUCCESS);
  }

  logger_init(isatty(1) == 1);
  logger_set_threshold(static_cast<boost::log::trivial::severity_level>(vm["log-level"].as<int>()));
  // libaktualizr assumes that the log level is set using a "loglevel" cmd line option, so we need
  // to set that option as well, otherwise libaktualizr overrides the log level with the default value
  vm.insert(std::pair<std::string, boost::program_options::variable_value>("loglevel", vm["log-level"]));

  return cmd(vm);
}

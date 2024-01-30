#include <iostream>
#include <vector>

#include <boost/program_options.hpp>

#include "cmds.h"

namespace po = boost::program_options;

static std::vector<Cmd::Ptr> cmds{
    std::make_shared<CheckCmd>(),
    std::make_shared<InstallCmd>(),
    std::make_shared<RunCmd>(),
    std::make_shared<PullCmd>(),
    std::make_shared<DaemonCmd>(),
};

static void print_usage() {
  std::cout << "Usage:\n\t custom-sota-client [cmd] [options]\nSupported commands: ";
  for (const auto& cmd : cmds) {
    std::cout << cmd->name() << " ";
  }
  std::cout << std::endl;
  std::cout << "Default command is \"daemon\"" << std::endl;
}

int main(int argc, char** argv) {
  std::string cmd_name = "daemon";

  if (argc > 1) {
    cmd_name = {argv[1]};
    if (cmd_name == "--help" || cmd_name == "-h") {
      print_usage();
      exit(EXIT_SUCCESS);
    }
  }

  const auto find_it{std::find_if(cmds.begin(), cmds.end(), [&cmd_name](const Cmd::Ptr& cmd) {
    return cmd->name() == cmd_name;
  })};

  if (cmds.end() == find_it) {
    LOG_ERROR << "Unsupported command: " << cmd_name << "\n";
    print_usage();
    exit(EXIT_FAILURE);
  }

  LOG_INFO << "Command: " << cmd_name;

  const Cmd& cmd{**find_it};
  po::options_description cmd_opts{cmd.options()};
  po::variables_map vm;
  po::positional_options_description run_pos;
  run_pos.add("cmd", 1);
  po::options_description arg_opts;
  arg_opts.add_options()("cmd", po::value<std::string>());
  arg_opts.add(cmd_opts);
  auto pos_opts = cmd.pos_options();
  for (const auto& option : pos_opts) {
    run_pos.add(option.c_str(), 1);
  }
  auto print_usage = [](const std::string& cmd, const std::vector<std::string>& pos_opts,
                        const po::options_description& opts) {
    std::cout << "custom-sota-client " << cmd;
    for (const auto& option : pos_opts) {
      std::cout << " [" + option + "]";
    }
    std::cout << " [options]\n" << opts;
  };

  try {
    po::store(po::command_line_parser(argc, argv).options(arg_opts).positional(run_pos).run(), vm);
    po::notify(vm);
  } catch (const std::exception& exc) {
    LOG_ERROR << exc.what() << "\n";
    print_usage(cmd_name, pos_opts, cmd_opts);
    exit(EXIT_FAILURE);
  }

  if (vm.count("help") == 1) {
    print_usage(vm["cmd"].as<std::string>(), pos_opts, cmd_opts);
    exit(EXIT_SUCCESS);
  }
  boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);

  return cmd(vm);
}

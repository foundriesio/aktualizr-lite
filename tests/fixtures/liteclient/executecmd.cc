static std::string executeCmd(const std::string& cmd, const std::vector<std::string>& args, const std::string& desc) {
  auto res = Process::spawn(cmd, args);
  if (std::get<0>(res) != 0) throw std::runtime_error("Failed to " + desc + ": " + std::get<2>(res));

  auto std_out = std::get<1>(res);
  boost::trim_right_if(std_out, boost::is_any_of(" \t\r\n"));
  return std_out;
}


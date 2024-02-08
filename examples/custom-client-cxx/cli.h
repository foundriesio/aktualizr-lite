#ifndef AKLITE_AKLITE_CUSTOM_CLI_H
#define AKLITE_AKLITE_CUSTOM_CLI_H

#include <string>

int cmd_pull(std::string target_name, std::string local_repo_path);
int cmd_install(std::string target_name, std::string local_repo_path);
int cmd_check(std::string local_repo_path);
int cmd_daemon(std::string local_repo_path);
int cmd_run();  

#endif

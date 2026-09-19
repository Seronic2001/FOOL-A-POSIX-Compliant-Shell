#pragma once

#include <string>
#include <vector>

// Declarations for all built-in command functions. Each returns an exit
// status (0 = success), so pipelines and scripts observe correct codes.
// `cd` is stateful and handled by the executor directly (void).

void execute_cd(const std::vector<std::string>& args, std::ostream& out);
int execute_pwd(const std::vector<std::string>& args, std::ostream& out);
int execute_ls(const std::vector<std::string>& args, std::ostream& out);
int execute_pinfo(const std::vector<std::string>& args, std::ostream& out);
int execute_history(const std::vector<std::string>& args, std::ostream& out);
int execute_clear(const std::vector<std::string>& args, std::ostream& out);
int execute_jobs(const std::vector<std::string>& args, std::ostream& out);
void execute_exit(const std::vector<std::string>& args, std::ostream& out);

int execute_echo(const std::vector<std::string>& args,
                 std::istream& in,
                 std::ostream& out);

int execute_search(const std::vector<std::string>& args,
                   std::istream& in,
                   std::ostream& out);

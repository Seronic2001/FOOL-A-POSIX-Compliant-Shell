#pragma once

#include <string>
#include <vector>

// Declarations for all built-in command functions
void execute_cd(const std::vector<std::string>& args, std::ostream& out);
void execute_pwd(const std::vector<std::string>& args, std::ostream& out);
void execute_ls(const std::vector<std::string>& args, std::ostream& out);
void execute_pinfo(const std::vector<std::string>& args, std::ostream& out);
void execute_history(const std::vector<std::string>& args, std::ostream& out);
void execute_clear(const std::vector<std::string>& args, std::ostream& out);
void execute_jobs(const std::vector<std::string>& args, std::ostream& out);
void execute_exit(const std::vector<std::string>& args, std::ostream& out);

void execute_echo(const std::vector<std::string>& args,
                  std::istream& in,
                  std::ostream& out);

void execute_search(const std::vector<std::string>& args,
                    std::istream& in,
                    std::ostream& out);
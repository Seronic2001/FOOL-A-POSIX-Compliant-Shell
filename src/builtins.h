#pragma once

#include <iosfwd>
#include <string>
#include <vector>

// Declarations for the built-in commands. Each returns an exit status
// (0 = success). `history` lives in history.h; `exit`, `fg` and `bg` are
// handled by the executor.

// Names of every built-in, for dispatch and tab completion.
const std::vector<std::string>& builtin_names();

int execute_cd(const std::vector<std::string>& args, std::ostream& out);
int execute_pwd(const std::vector<std::string>& args, std::ostream& out);
int execute_ls(const std::vector<std::string>& args, std::ostream& out);
int execute_pinfo(const std::vector<std::string>& args, std::ostream& out);
int execute_clear(const std::vector<std::string>& args, std::ostream& out);
int execute_jobs(const std::vector<std::string>& args, std::ostream& out);

// True if the built-in `ls` implements every option in `args`; otherwise
// the executor runs the system `ls` instead.
bool ls_supports(const std::vector<std::string>& args);

int execute_echo(const std::vector<std::string>& args,
                 std::istream& in,
                 std::ostream& out);

int execute_search(const std::vector<std::string>& args,
                   std::istream& in,
                   std::ostream& out);

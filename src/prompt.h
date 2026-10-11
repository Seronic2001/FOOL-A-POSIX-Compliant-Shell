#pragma once
#include <string>

// The directory the shell started in, treated as its "home" for the
// prompt's ~ substitution. Set once by main() at startup.
extern std::string shell_home_dir;

std::string generate_prompt_string();

#pragma once
#include <string>

// The directory the shell started in, treated as its "home" for the
// prompt's ~ substitution. Set once by main() at startup.
extern std::string shell_home_dir;

void display_prompt(void);
std::string generate_prompt_string();
size_t calculate_visible_length(const std::string& s);
#pragma once
#include <string>

void display_prompt(void);
std::string generate_prompt_string();
size_t calculate_visible_length(const std::string& s);
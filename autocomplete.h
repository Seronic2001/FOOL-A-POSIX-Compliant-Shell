#pragma once
#include <string>
#include <vector>

std::vector<std::string> handle_autocomplete(std::string& line,
                                             size_t& cursor_pos);
void initialize_command_cache();
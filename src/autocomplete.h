#pragma once
#include <string>
#include <vector>

// Completes the word under the cursor in place and returns every candidate.
std::vector<std::string> handle_autocomplete(std::string& line,
                                             size_t& cursor_pos);

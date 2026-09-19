#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Loads history from the history file at shell startup.
void initialize_history();

void save_history_on_exit();

void add_to_history(const std::string& command);

int execute_history(const std::vector<std::string>& args, std::ostream& out);

const std::vector<std::string>& get_history();

// Number of entries currently in memory.
size_t history_size();

// Sequence number of the first entry in memory (stable across trimming, so
// displayed numbering is monotonic like bash's).
long long history_first_sequence_number();

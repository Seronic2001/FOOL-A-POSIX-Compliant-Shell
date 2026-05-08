#pragma once
#include <string>
#include <vector>

// Loads history from the history file at shell startup.
void initialize_history();

void save_history_on_exit();

void add_to_history(const std::string& command);

void execute_history(const std::vector<std::string>& args, std::ostream& out);

const std::vector<std::string>& get_history();
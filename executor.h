#pragma once

#include <string>  // Crazy ??????????
#include <vector>

#include "parser.h"

// A struct to hold information about a background job
struct Job {
  pid_t pid;
  std::string command_name;
  bool is_stopped;
};

extern pid_t foreground_pid;
// The global list of all background jobs
extern std::vector<Job> background_jobs;

void execute(const std::vector<Command>& commands);
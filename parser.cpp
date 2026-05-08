#include <cctype>

#include "parser.h"

// processes a list of tokens for a single pipeline to handle I/O redirection
// and backgrounding.
void process_redirection_and_backgrounding(
    Command& command,
    const std::vector<std::string>& tokens) {
  // iterate through the tokens to find redirection symbols
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i] == "<") {
      if (i + 1 < tokens.size()) {
        command.stdin_file = tokens[i + 1];
        i++;  // Skip the filename token in the next iteration
      }
    } else if (tokens[i] == ">") {
      if (i + 1 < tokens.size()) {
        command.stdout_file = tokens[i + 1];
        command.append_out = false;
        i++;
      }
    } else if (tokens[i] == ">>") {
      if (i + 1 < tokens.size()) {
        command.stdout_file = tokens[i + 1];
        command.append_out = true;
        i++;
      }
    } else if (tokens[i] == "&") {
      command.background = true;
    } else {
      // This is a regular argument
      command.args.push_back(tokens[i]);
    }
  }
}

std::vector<Command> parse_input(const std::string& input_line) {
  std::vector<Command> final_commands;
  std::vector<std::string> raw_tokens;
  std::string current_token;
  char in_quote = 0;  // 0, '\'', or '"'

  // Perform a single tokenization pass over the entire line, respecting quotes.
  for (char c : input_line) {
    if (in_quote) {  // If we are inside a quote
      if (c == in_quote) {
        in_quote = 0;  // Closing quote
      } else {
        current_token += c;
      }
    } else {  // Not in a quote
      if (c == '\'' || c == '"') {
        if (!current_token.empty()) {
          raw_tokens.push_back(current_token);
          current_token.clear();
        }
        in_quote = c;
      } else if (isspace(c)) {
        if (!current_token.empty()) {
          raw_tokens.push_back(current_token);
          current_token.clear();
        }
      } else if (c == ';' || c == '|') {
        if (!current_token.empty()) {
          raw_tokens.push_back(current_token);
          current_token.clear();
        }
        raw_tokens.push_back(
            std::string(1, c));  // Add separator as its own token
      } else {
        current_token += c;
      }
    }
  }

  if (!current_token.empty()) {
    raw_tokens.push_back(current_token);
  }

  // Process the raw tokens into Command structs, grouped by separators.
  if (raw_tokens.empty())
    return final_commands;

  std::vector<std::vector<std::string>> semicolon_groups;
  semicolon_groups.emplace_back();

  for (const auto& token : raw_tokens) {
    if (token == ";") {
      if (!semicolon_groups.back().empty()) {
        semicolon_groups.emplace_back();
      }
    } else {
      semicolon_groups.back().push_back(token);
    }
  }

  for (const auto& group : semicolon_groups) {
    if (group.empty())
      continue;

    std::vector<Command> pipeline_commands;
    std::vector<std::string> pipe_segment_tokens;

    for (const auto& token : group) {
      if (token == "|") {
        Command cmd;
        process_redirection_and_backgrounding(cmd, pipe_segment_tokens);
        if (!cmd.args.empty())
          pipeline_commands.push_back(cmd);
        pipe_segment_tokens.clear();
      } else {
        pipe_segment_tokens.push_back(token);
      }
    }
    // Add the last command in the pipeline
    Command last_cmd;
    process_redirection_and_backgrounding(last_cmd, pipe_segment_tokens);

    // Only add the command if it has arguments(to handle cases like "ls; ;
    // cd")
    if (!last_cmd.args.empty())
      pipeline_commands.push_back(last_cmd);

    // After processing all parts of a pipeline, we add a special separator
    // command This tells the executor that this set of commands is done.
    if (!pipeline_commands.empty()) {
      final_commands.insert(final_commands.end(), pipeline_commands.begin(),
                            pipeline_commands.end());
      // Add a "break" command to signify the end of a semicolon-separated group
      Command break_command;
      break_command.args.push_back(";");  // Special marker
      final_commands.push_back(break_command);
    }
  }
  return final_commands;
}
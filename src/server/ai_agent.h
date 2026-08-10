#ifndef GITBOARD_SERVER_AI_AGENT_H_
#define GITBOARD_SERVER_AI_AGENT_H_

#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

#include "src/server/config.h"
#include "src/server/http.h"

namespace gitboard::server {

struct agent_tool_call {
  std::string id;
  std::string name;
  std::string arguments;
};

struct chat_message {
  std::string role;
  std::string content;
};

struct ai_http_request {
  std::string endpoint;
  std::vector<std::string> headers;
  std::string payload;
};

struct agent_tool_definition {
  std::string name;
  std::string description;
  std::string parameters_json;
};

struct parsed_agent_tool_calls {
  std::string assistant_message_json;
  std::vector<agent_tool_call> calls;
};

const std::vector<agent_tool_definition>& agent_tool_definitions();
std::string agent_initial_system_prompt(const ai_config& config,
                                        const std::string& repo_id,
                                        const std::string& tool_context,
                                        const std::string& history_summary);
std::string latest_request_instruction(const std::string& message);

void configure_ai_debug_log_file(const std::filesystem::path& requested_path);
const std::filesystem::path& ai_debug_log_path();

http_response handle_get_ai_config();
http_response handle_put_ai_config(const http_request& request);
http_response handle_ai_test();
http_response handle_agent_chat(const http_request& request,
                                const std::filesystem::path& gitboard_path);
http_response handle_agent_summarize(const http_request& request);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_AI_AGENT_H_

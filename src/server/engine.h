#ifndef GITBOARD_SERVER_ENGINE_H_
#define GITBOARD_SERVER_ENGINE_H_

#include <filesystem>
#include <string>

#include "src/core/json.h"
#include "src/server/ai_agent.h"
#include "src/server/config.h"
#include "src/server/http.h"

namespace gitboard::server {

gitboard::json_value require_object_body(const http_request& request);

std::string build_agent_tool_context(const std::filesystem::path& gitboard_path,
                                     const std::filesystem::path& repo_path,
                                     const std::string& repo_id,
                                     const repo_ai_config& repo_ai,
                                     const std::string& user_message);
std::string proposed_action_from_tool_call(const agent_tool_call& call,
                                           const gitboard::json_value& args,
                                           const std::filesystem::path& repo_path,
                                           std::size_t index);
std::string execute_agent_tool(const agent_tool_call& call,
                               const std::filesystem::path& gitboard_path,
                               const std::string& repo_id,
                               const std::filesystem::path& repo_path,
                               const repo_ai_config& repo_ai);

http_response handle_agent_apply_actions(
    const http_request& request,
    const std::filesystem::path& gitboard_path);
http_response handle_agent_file_allowed(const http_request& request);
http_response handle_agent_file_list(const http_request& request);
http_response handle_agent_file_read(const http_request& request);
http_response handle_agent_file_read_range(const http_request& request);
http_response handle_agent_file_search(const http_request& request);
http_response handle_agent_file_write(const http_request& request);
http_response handle_batch(const http_request& request,
                           const std::filesystem::path& gitboard_path);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_ENGINE_H_

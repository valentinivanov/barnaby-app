#ifndef GITBOARD_SERVER_AI_AGENT_ANTHROPIC_H_
#define GITBOARD_SERVER_AI_AGENT_ANTHROPIC_H_

#include <string>
#include <utility>
#include <vector>

#include "src/server/ai_agent.h"
#include "src/server/config.h"

namespace gitboard::server {

ai_http_request anthropic_chat_http_request(const ai_config& config,
                                            const std::string& api_key,
                                            std::string payload);
std::string anthropic_chat_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& tool_context, const std::string& history_summary,
    const std::vector<chat_message>& history, const std::string& message);
std::string anthropic_chat_followup_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& tool_context, const std::string& history_summary,
    const std::vector<chat_message>& history, const std::string& message,
    const std::vector<std::string>& provider_messages_json);
std::string anthropic_chat_repair_empty_content_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& message, const std::string& reasoning);
std::string anthropic_history_summary_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& history_summary,
    const std::vector<chat_message>& messages);
std::string anthropic_finish_reason(const std::string& output);
std::string anthropic_parse_chat_content(const std::string& output);
parsed_agent_tool_calls anthropic_parse_agent_tool_calls(const std::string& output);
void anthropic_append_tool_result_messages(
    const std::vector<std::pair<std::string, std::string>>& tool_results,
    std::vector<std::string>& provider_messages_json);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_AI_AGENT_ANTHROPIC_H_

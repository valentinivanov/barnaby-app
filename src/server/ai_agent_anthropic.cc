#include "src/server/ai_agent_anthropic.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "src/core/error.h"
#include "src/core/json.h"
#include "src/core/strings.h"

namespace gitboard::server {

namespace {

void append_anthropic_agent_tools_json(std::ostringstream& out) {
  out << "\"tools\":[";
  bool first = true;
  for (const auto& tool : agent_tool_definitions()) {
    if (!first) out << ",";
    first = false;
    out << "{\"name\":" << json_quote(tool.name)
        << ",\"description\":" << json_quote(tool.description)
        << ",\"input_schema\":" << tool.parameters_json << "}";
  }
  out << "]";
}

std::string anthropic_messages_endpoint_url(std::string base_url) {
  base_url = gitboard::trim(base_url);
  while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
  if (base_url.size() >= std::string("/v1/messages").size() &&
      base_url.rfind("/v1/messages") ==
          base_url.size() - std::string("/v1/messages").size()) {
    return base_url;
  }
  if (base_url.size() >= std::string("/v1").size() &&
      base_url.rfind("/v1") == base_url.size() - std::string("/v1").size()) {
    return base_url + "/messages";
  }
  return base_url + "/v1/messages";
}

std::string base_anthropic_payload(const ai_config& config,
                                   const std::string& system,
                                   const std::string& user) {
  std::ostringstream out;
  out << "{";
  out << "\"model\":" << json_quote(config.model) << ",";
  out << "\"stream\":false,";
  out << "\"max_tokens\":"
      << (config.max_output_tokens > 0 ? config.max_output_tokens : 4096) << ",";
  out << "\"system\":" << json_quote(system) << ",";
  out << "\"messages\":[{\"role\":\"user\",\"content\":"
      << json_quote(user) << "}]}";
  return out.str();
}

std::string excerpt_middle(std::string value, std::size_t max_size) {
  if (value.size() <= max_size) return value;
  if (max_size < 128) {
    value.resize(max_size);
    return value;
  }
  std::size_t head = max_size / 2;
  std::size_t tail = max_size - head;
  return value.substr(0, head) +
         "\n\n[...middle omitted from oversized provider output...]\n\n" +
         value.substr(value.size() - tail);
}

void append_text_fragments(const json_value& value, std::string& out) {
  if (value.type == json_value::k_string) {
    if (!out.empty()) out += "\n";
    out += value.string;
    return;
  }
  if (value.type == json_value::k_array) {
    for (const json_value& item : value.array) append_text_fragments(item, out);
    return;
  }
  if (value.type != json_value::k_object) return;
  for (const char* key : {"text", "content"}) {
    auto it = value.object.find(key);
    if (it != value.object.end()) append_text_fragments(it->second, out);
  }
}

}  // namespace

ai_http_request anthropic_chat_http_request(const ai_config& config,
                                            const std::string& api_key,
                                            std::string payload) {
  return ai_http_request{
      anthropic_messages_endpoint_url(config.base_url),
      {"Content-Type: application/json", "x-api-key: " + api_key,
       "anthropic-version: 2023-06-01"},
      std::move(payload),
  };
}

std::string anthropic_chat_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& tool_context, const std::string& history_summary,
    const std::vector<chat_message>& history, const std::string& message) {
  std::ostringstream out;
  out << "{";
  out << "\"model\":" << json_quote(config.model) << ",";
  out << "\"stream\":false,";
  out << "\"max_tokens\":"
      << (config.max_output_tokens > 0 ? config.max_output_tokens : 4096) << ",";
  out << "\"system\":"
      << json_quote(agent_initial_system_prompt(config, repo_id, tool_context,
                                                history_summary))
      << ",";
  out << "\"messages\":[";
  bool first = true;
  for (const auto& item : history) {
    if (!first) out << ",";
    first = false;
    out << "{\"role\":" << json_quote(item.role)
        << ",\"content\":" << json_quote(item.content) << "}";
  }
  if (!first) out << ",";
  out << "{\"role\":\"user\",\"content\":" << json_quote(message) << "}";
  out << "],";
  append_anthropic_agent_tools_json(out);
  out << ",\"tool_choice\":{\"type\":\"auto\"}}";
  return out.str();
}

std::string anthropic_chat_followup_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& tool_context, const std::string& history_summary,
    const std::vector<chat_message>& history, const std::string& message,
    const std::vector<std::string>& provider_messages_json) {
  std::ostringstream out;
  out << "{";
  out << "\"model\":" << json_quote(config.model) << ",";
  out << "\"stream\":false,";
  out << "\"max_tokens\":"
      << (config.max_output_tokens > 0 ? config.max_output_tokens : 4096) << ",";
  out << "\"system\":"
      << json_quote(agent_initial_system_prompt(config, repo_id, tool_context,
                                                history_summary))
      << ",";
  out << "\"messages\":[";
  bool first = true;
  for (const auto& item : history) {
    if (!first) out << ",";
    first = false;
    out << "{\"role\":" << json_quote(item.role)
        << ",\"content\":" << json_quote(item.content) << "}";
  }
  if (!first) out << ",";
  out << "{\"role\":\"user\",\"content\":" << json_quote(message) << "}";
  for (const auto& provider_message : provider_messages_json) {
    out << "," << provider_message;
  }
  out << ",{\"role\":\"user\",\"content\":"
      << json_quote(latest_request_instruction(message)) << "}";
  out << "],";
  append_anthropic_agent_tools_json(out);
  out << ",\"tool_choice\":{\"type\":\"auto\"}}";
  return out.str();
}

std::string anthropic_chat_repair_empty_content_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& message, const std::string& reasoning) {
  std::string system =
      "You repair an Agent Pip response for repoId " + repo_id +
      ". The previous assistant response was unusable because it was empty, "
      "cut off, or put user-facing material into a nonstandard reasoning "
      "field. Convert only the useful outcome into the final visible assistant "
      "answer. Do not include hidden chain-of-thought, scratch work, or "
      "analysis narration. The latest user request supersedes older history "
      "for the current intent. If the latest request asks to create a task, "
      "return a JSON object with content and drafts exactly in the normal "
      "Agent Pip draft format: "
      "{\"content\":\"short response\","
      "\"drafts\":[{\"title\":\"...\",\"body\":\"...\","
      "\"priority\":\"medium\",\"story_points\":100,\"status\":\"backlog\","
      "\"assignee\":\"\",\"tags\":[]}]}. Do not return "
      "{\"taskId\":\"...\",\"fields\":{...}} for new tasks; Barnaby assigns "
      "task ids only after user approval.";
  std::string user =
      "Latest user request:\n" + message +
      "\n\nPrevious unusable assistant material:\n" +
      excerpt_middle(reasoning, 16000);
  return base_anthropic_payload(config, system, user);
}

std::string anthropic_history_summary_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& history_summary,
    const std::vector<chat_message>& messages) {
  std::ostringstream material;
  if (!history_summary.empty()) {
    material << "Previous summary:\n" << history_summary << "\n\n";
  }
  material << "New overflow messages:\n";
  for (const auto& message : messages) {
    material << message.role << ": " << message.content << "\n";
  }
  std::string system =
      "You summarize Agent Pip conversation history for repoId " + repo_id +
      ". Produce a compact plain-text summary. Preserve user intent, "
      "decisions, unresolved questions, task ids, draft assumptions, and "
      "project-management advice already given. Do not invent repository facts. "
      "Do not say a task changed status, was applied, or was selected unless "
      "that fact appears explicitly in the supplied messages. Proposed actions "
      "are not applied changes.";
  return base_anthropic_payload(config, system, material.str());
}

std::string anthropic_finish_reason(const std::string& output) {
  json_value root = json_parser(output).parse();
  if (root.type != json_value::k_object) return "";
  return json_string_member(root, "stop_reason");
}

std::string anthropic_parse_chat_content(const std::string& output) {
  json_value root = json_parser(output).parse();
  if (root.type != json_value::k_object) throw error("AI response root is not an object");
  auto content_it = root.object.find("content");
  if (content_it != root.object.end()) {
    std::string fragments;
    append_text_fragments(content_it->second, fragments);
    if (!gitboard::trim(fragments).empty()) return fragments;
  }
  throw error("AI response did not include assistant text");
}

parsed_agent_tool_calls anthropic_parse_agent_tool_calls(const std::string& output) {
  parsed_agent_tool_calls parsed;
  json_value root = json_parser(output).parse();
  if (root.type != json_value::k_object) return parsed;
  auto content_it = root.object.find("content");
  if (content_it == root.object.end() ||
      content_it->second.type != json_value::k_array) {
    return parsed;
  }
  parsed.assistant_message_json =
      "{\"role\":\"assistant\",\"content\":" + stringify_json(content_it->second) + "}";
  int index = 0;
  for (const json_value& item : content_it->second.array) {
    if (item.type != json_value::k_object) continue;
    if (json_string_member(item, "type") != "tool_use") continue;
    agent_tool_call call;
    call.id = json_string_member(item, "id");
    if (call.id.empty()) call.id = "tool_use_" + std::to_string(++index);
    call.name = json_string_member(item, "name");
    auto input_it = item.object.find("input");
    call.arguments = input_it == item.object.end()
                         ? "{}"
                         : stringify_json(input_it->second);
    if (!call.name.empty()) parsed.calls.push_back(call);
  }
  if (parsed.calls.empty()) parsed.assistant_message_json.clear();
  return parsed;
}

void anthropic_append_tool_result_messages(
    const std::vector<std::pair<std::string, std::string>>& tool_results,
    std::vector<std::string>& provider_messages_json) {
  if (tool_results.empty()) return;
  std::ostringstream out;
  out << "{\"role\":\"user\",\"content\":[";
  bool first = true;
  for (const auto& [tool_call_id, content] : tool_results) {
    if (!first) out << ",";
    first = false;
    out << "{\"type\":\"tool_result\",\"tool_use_id\":"
        << json_quote(tool_call_id)
        << ",\"content\":" << json_quote(content) << "}";
  }
  out << "]}";
  provider_messages_json.push_back(out.str());
}

}  // namespace gitboard::server

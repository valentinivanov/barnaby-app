#include "src/server/ai_agent_openai.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "src/core/error.h"
#include "src/core/json.h"
#include "src/core/strings.h"

namespace gitboard::server {

namespace {

void append_openai_agent_tools_json(std::ostringstream& out) {
  out << "\"tools\":[";
  bool first = true;
  for (const auto& tool : agent_tool_definitions()) {
    if (!first) out << ",";
    first = false;
    out << "{\"type\":\"function\",\"function\":{\"name\":"
        << json_quote(tool.name) << ",\"description\":"
        << json_quote(tool.description)
        << ",\"parameters\":" << tool.parameters_json << "}}";
  }
  out << "]";
}

int openai_max_tokens(const ai_config& config) {
  return config.max_output_tokens > 0 ? config.max_output_tokens : 4096;
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

std::string openai_chat_endpoint_url(std::string base_url) {
  base_url = gitboard::trim(base_url);
  while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
  if (base_url.size() >= std::string("/chat/completions").size() &&
      base_url.rfind("/chat/completions") ==
          base_url.size() - std::string("/chat/completions").size()) {
    return base_url;
  }
  std::size_t scheme = base_url.find("://");
  std::size_t path_start = scheme == std::string::npos
                               ? base_url.find('/')
                               : base_url.find('/', scheme + 3);
  if (path_start == std::string::npos) {
    return base_url + "/v1/chat/completions";
  }
  return base_url + "/chat/completions";
}

std::string base_openai_payload(const ai_config& config,
                                const std::string& system,
                                const std::string& user) {
  std::ostringstream out;
  out << "{";
  out << "\"model\":" << json_quote(config.model) << ",";
  out << "\"stream\":false,";
  out << "\"max_tokens\":" << openai_max_tokens(config) << ",";
  out << "\"messages\":[";
  out << "{\"role\":\"system\",\"content\":" << json_quote(system) << "},";
  out << "{\"role\":\"user\",\"content\":" << json_quote(user) << "}";
  out << "]}";
  return out.str();
}

}  // namespace

ai_http_request openai_chat_http_request(const ai_config& config,
                                         const std::string& api_key,
                                         std::string payload) {
  return ai_http_request{
      openai_chat_endpoint_url(config.base_url),
      {"Content-Type: application/json", "Authorization: Bearer " + api_key},
      std::move(payload),
  };
}

std::string openai_chat_request_json(const ai_config& config,
                                     const std::string& repo_id,
                                     const std::string& tool_context,
                                     const std::string& history_summary,
                                     const std::vector<chat_message>& history,
                                     const std::string& message) {
  std::ostringstream out;
  out << "{";
  out << "\"model\":" << json_quote(config.model) << ",";
  out << "\"stream\":false,";
  out << "\"max_tokens\":" << openai_max_tokens(config) << ",";
  out << "\"messages\":[";
  std::string prompt =
      agent_initial_system_prompt(config, repo_id, tool_context, history_summary);
  out << "{\"role\":\"system\",\"content\":" << json_quote(prompt) << "},";
  for (const auto& item : history) {
    out << "{\"role\":" << json_quote(item.role)
        << ",\"content\":" << json_quote(item.content) << "},";
  }
  out << "{\"role\":\"user\",\"content\":" << json_quote(message) << "}";
  out << "],";
  append_openai_agent_tools_json(out);
  out << ",\"tool_choice\":\"auto\"}";
  return out.str();
}

std::string openai_chat_followup_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& tool_context, const std::string& history_summary,
    const std::vector<chat_message>& history, const std::string& message,
    const std::vector<std::string>& provider_messages_json) {
  std::ostringstream out;
  out << "{";
  out << "\"model\":" << json_quote(config.model) << ",";
  out << "\"stream\":false,";
  out << "\"max_tokens\":" << openai_max_tokens(config) << ",";
  out << "\"messages\":[";
  std::string prompt = config.system_prompt.empty()
                           ? default_ai_system_prompt()
                           : config.system_prompt;
  prompt += "\n\nCurrent repoId: " + repo_id;
  if (!history_summary.empty()) {
    prompt += "\n\nSummary of earlier conversation:\n" + history_summary;
  }
  prompt += "\n\nRead-only Barnaby task context:\n" + tool_context;
  prompt +=
      "\n\nUse the tool results below to continue the latest user request, "
      "not an older conversation turn. The latest user request supersedes "
      "older history for the current intent. Do not introduce yourself or ask "
      "what the user wants when the latest request is already present. If you "
      "already staged one part of a composite request, continue with any "
      "remaining requested parts. If the latest request asks for code review, "
      "provide review findings grounded in the file results; use "
      "read_project_file_range for additional focused chunks when needed. If "
      "the latest request asks to create a task, return the required "
      "draft-task JSON after forming the review suggestion: "
      "{\"content\":\"short response\",\"drafts\":[{\"title\":\"...\","
      "\"body\":\"...\",\"priority\":\"medium\",\"story_points\":100,\"status\":\"backlog\","
      "\"assignee\":\"\",\"tags\":[]}]}. Do not return "
      "{\"taskId\":\"...\",\"fields\":{...}} for new tasks; Barnaby assigns "
      "task ids only after user approval.";
  out << "{\"role\":\"system\",\"content\":" << json_quote(prompt) << "},";
  for (const auto& item : history) {
    out << "{\"role\":" << json_quote(item.role)
        << ",\"content\":" << json_quote(item.content) << "},";
  }
  out << "{\"role\":\"user\",\"content\":" << json_quote(message) << "}";
  for (const auto& provider_message : provider_messages_json) {
    out << "," << provider_message;
  }
  out << ",{\"role\":\"user\",\"content\":"
      << json_quote(latest_request_instruction(message)) << "}";
  out << "],";
  append_openai_agent_tools_json(out);
  out << ",\"tool_choice\":\"auto\"}";
  return out.str();
}

std::string openai_chat_repair_empty_content_request_json(
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
  return base_openai_payload(config, system, user);
}

std::string openai_history_summary_request_json(
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
  return base_openai_payload(config, system, material.str());
}

std::string openai_assistant_reasoning_text(const std::string& output) {
  json_value root = json_parser(output).parse();
  if (root.type != json_value::k_object) return "";
  auto choices_it = root.object.find("choices");
  if (choices_it == root.object.end() ||
      choices_it->second.type != json_value::k_array ||
      choices_it->second.array.empty()) {
    return "";
  }
  const json_value& first = choices_it->second.array.front();
  if (first.type != json_value::k_object) return "";
  auto message_it = first.object.find("message");
  if (message_it == first.object.end() ||
      message_it->second.type != json_value::k_object) {
    return "";
  }
  std::string reasoning = json_string_member(message_it->second, "reasoning");
  if (!reasoning.empty()) return reasoning;
  return json_string_member(message_it->second, "reasoning_content");
}

std::string openai_finish_reason(const std::string& output) {
  json_value root = json_parser(output).parse();
  if (root.type != json_value::k_object) return "";
  auto choices_it = root.object.find("choices");
  if (choices_it == root.object.end() ||
      choices_it->second.type != json_value::k_array ||
      choices_it->second.array.empty()) {
    return "";
  }
  const json_value& first = choices_it->second.array.front();
  if (first.type != json_value::k_object) return "";
  return json_string_member(first, "finish_reason");
}

std::string openai_parse_chat_content(const std::string& output) {
  json_value root = json_parser(output).parse();
  if (root.type != json_value::k_object) throw error("AI response root is not an object");
  auto choices_it = root.object.find("choices");
  if (choices_it != root.object.end() &&
      choices_it->second.type == json_value::k_array &&
      !choices_it->second.array.empty()) {
    const json_value& first = choices_it->second.array.front();
    if (first.type == json_value::k_object) {
      auto message_it = first.object.find("message");
      if (message_it != first.object.end() &&
          message_it->second.type == json_value::k_object) {
        std::string content = json_string_member(message_it->second, "content");
        if (!content.empty()) return content;
      }
      std::string text = json_string_member(first, "text");
      if (!text.empty()) return text;
    }
  }
  std::string output_text = json_string_member(root, "output_text");
  if (!output_text.empty()) return output_text;
  throw error("AI response did not include assistant text");
}

parsed_agent_tool_calls openai_parse_agent_tool_calls(const std::string& output) {
  parsed_agent_tool_calls parsed;
  json_value root = json_parser(output).parse();
  if (root.type != json_value::k_object) return parsed;
  auto choices_it = root.object.find("choices");
  if (choices_it == root.object.end() ||
      choices_it->second.type != json_value::k_array ||
      choices_it->second.array.empty()) {
    return parsed;
  }
  const json_value& first = choices_it->second.array.front();
  if (first.type != json_value::k_object) return parsed;
  auto message_it = first.object.find("message");
  if (message_it == first.object.end() ||
      message_it->second.type != json_value::k_object) {
    return parsed;
  }
  parsed.assistant_message_json = stringify_json(message_it->second);
  auto tool_calls_it = message_it->second.object.find("tool_calls");
  if (tool_calls_it == message_it->second.object.end() ||
      tool_calls_it->second.type != json_value::k_array) {
    return parsed;
  }
  int index = 0;
  for (const json_value& item : tool_calls_it->second.array) {
    if (item.type != json_value::k_object) continue;
    agent_tool_call call;
    call.id = json_string_member(item, "id");
    if (call.id.empty()) call.id = "tool_call_" + std::to_string(++index);
    auto function_it = item.object.find("function");
    if (function_it == item.object.end() ||
        function_it->second.type != json_value::k_object) {
      continue;
    }
    call.name = json_string_member(function_it->second, "name");
    call.arguments = json_string_member(function_it->second, "arguments");
    if (!call.name.empty()) parsed.calls.push_back(call);
  }
  return parsed;
}

void openai_append_tool_result_messages(
    const std::vector<std::pair<std::string, std::string>>& tool_results,
    std::vector<std::string>& provider_messages_json) {
  for (const auto& [tool_call_id, content] : tool_results) {
    provider_messages_json.push_back(
        "{\"role\":\"tool\",\"tool_call_id\":" + json_quote(tool_call_id) +
        ",\"content\":" + json_quote(content) + "}");
  }
}

}  // namespace gitboard::server

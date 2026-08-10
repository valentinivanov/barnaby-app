#include "src/server/ai_agent.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "src/core/error.h"
#include "src/core/filesystem.h"
#include "src/core/json.h"
#include "src/core/strings.h"
#include "src/server/ai_agent_anthropic.h"
#include "src/server/ai_http_client.h"
#include "src/server/ai_agent_openai.h"
#include "src/server/config.h"
#include "src/server/engine.h"
#include "src/server/http.h"

namespace fs = std::filesystem;

namespace gitboard::server {

constexpr std::size_t kMaxChatHistoryMessages = 10;
constexpr std::size_t kMaxChatMessageChars = 4000;
constexpr std::size_t kMaxChatSummaryChars = 12000;
constexpr int kMaxAgentFileRangeLines = 200;
constexpr int kMaxAgentToolRounds = 12;

enum class ai_provider_kind {
  k_openai_compatible,
  k_anthropic,
};

const std::vector<agent_tool_definition>& agent_tool_definitions() {
  static const std::vector<agent_tool_definition> tools = {
      {"list_allowed_paths",
       "List repository paths Agent Pip may access and their access mode.",
       "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
      {"list_project_files",
       "List files and directories under an allowed repository path.",
       "{\"type\":\"object\",\"required\":[\"path\"],\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Repository-relative directory path, such as doc/.\"}},\"additionalProperties\":false}"},
      {"read_project_file",
       "Read one small allowed repository file by repository-relative path. For code review or large files, use read_project_file_range instead.",
       "{\"type\":\"object\",\"required\":[\"path\"],\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Repository-relative file path, such as doc/statuses_config.md.\"}},\"additionalProperties\":false}"},
      {"read_project_file_range",
       "Read a bounded 1-based line range from one allowed repository file. Prefer this for code review and large files.",
       "{\"type\":\"object\",\"required\":[\"path\",\"startLine\",\"lineCount\"],\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Repository-relative file path, such as src/server/main.cc.\"},\"startLine\":{\"type\":\"integer\",\"minimum\":1},\"lineCount\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":" +
           std::to_string(kMaxAgentFileRangeLines) +
           "}},\"additionalProperties\":false}"},
      {"search_project_files",
       "Search allowed repository files under a path for exact text.",
       "{\"type\":\"object\",\"required\":[\"path\",\"query\"],\"properties\":{\"path\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"}},\"additionalProperties\":false}"},
      {"list_team",
       "List known Barnaby team members. Use team aliases as task assignee values.",
       "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
      {"add_team_member",
       "Add a known Barnaby team member so their alias can be used as a task assignee.",
       "{\"type\":\"object\",\"required\":[\"alias\"],\"properties\":{\"alias\":{\"type\":\"string\",\"description\":\"Short assignee alias.\"},\"email\":{\"type\":\"string\",\"description\":\"Optional email address.\"}},\"additionalProperties\":false}"},
      {"list_tasks",
       "List Barnaby tasks, optionally filtered by exact status, assignee, priority, tag, or text search.",
       "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"},\"assignee\":{\"type\":\"string\"},\"priority\":{\"type\":\"string\"},\"tag\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}},\"additionalProperties\":false}"},
      {"get_task",
       "Read one Barnaby task by task id.",
       "{\"type\":\"object\",\"required\":[\"taskId\"],\"properties\":{\"taskId\":{\"type\":\"string\",\"description\":\"Task id such as TASK-TWE8xG+sQ++7xv4ChVJvEg.\"}},\"additionalProperties\":false}"},
      {"update_task",
       "Propose field updates on an existing Barnaby task for user review. Use this for assignee, title, body, priority, story_points, tags, branches, prs, ci_status, and status changes.",
       "{\"type\":\"object\",\"required\":[\"taskId\",\"fields\"],\"properties\":{\"taskId\":{\"type\":\"string\"},\"fields\":{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"},\"assignee\":{\"type\":\"string\"},\"priority\":{\"type\":\"string\"},\"story_points\":{\"type\":\"integer\",\"enum\":[1,2,3,5,8,13,21,100]},\"status\":{\"type\":\"string\"},\"ci_status\":{\"type\":\"string\"},\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"branches\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"prs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"additionalProperties\":false}},\"additionalProperties\":false}"},
      {"move_task",
       "Propose moving an existing Barnaby task to another workflow status for user review.",
       "{\"type\":\"object\",\"required\":[\"taskId\",\"status\"],\"properties\":{\"taskId\":{\"type\":\"string\"},\"status\":{\"type\":\"string\"}},\"additionalProperties\":false}"},
      {"comment_task",
       "Propose adding a comment to an existing Barnaby task for user review.",
       "{\"type\":\"object\",\"required\":[\"taskId\",\"message\"],\"properties\":{\"taskId\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"}},\"additionalProperties\":false}"},
  };
  return tools;
}

#ifndef NDEBUG
std::mutex& ai_debug_log_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::ofstream& ai_debug_log_stream() {
  static std::ofstream stream;
  return stream;
}

fs::path& ai_debug_log_path_storage() {
  static fs::path path;
  return path;
}

std::string timestamp_for_log_name() {
  auto now = std::chrono::system_clock::now();
  std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y%m%d-%H%M%S");
  return out.str();
}

fs::path default_ai_debug_log_path() {
  return fs::temp_directory_path() /
         ("gitboard-ai-debug-" + timestamp_for_log_name() + "-" +
#ifdef _WIN32
          std::to_string(_getpid()) + ".log");
#else
          std::to_string(getpid()) + ".log");
#endif
}

void configure_ai_debug_log_file(const fs::path& requested_path) {
  fs::path path = requested_path.empty() ? default_ai_debug_log_path()
                                         : requested_path;
  if (path.has_parent_path()) fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::out | std::ios::app);
  if (!stream) throw error("failed to open AI debug log: " + path.string());
  ai_debug_log_stream() = std::move(stream);
  ai_debug_log_path_storage() = path;
}

const fs::path& ai_debug_log_path() {
  return ai_debug_log_path_storage();
}

void write_ai_debug_log(const std::string& label, const std::string& text) {
  std::lock_guard<std::mutex> lock(ai_debug_log_mutex());
  std::ofstream& stream = ai_debug_log_stream();
  if (!stream.is_open()) return;
  stream << "\n[gitboard-server ai] " << label << "\n" << text << "\n";
  stream.flush();
}

void debug_log_ai_json(const std::string& label, const std::string& json) {
  write_ai_debug_log(label, json);
}

void debug_log_ai_text(const std::string& label, const std::string& text) {
  write_ai_debug_log(label, text);
}
#else
void configure_ai_debug_log_file(const fs::path&) {}
const fs::path& ai_debug_log_path() {
  static fs::path path;
  return path;
}
void debug_log_ai_json(const std::string&, const std::string&) {}
void debug_log_ai_text(const std::string&, const std::string&) {}
#endif
ai_provider_kind ai_provider(const ai_config& config) {
  std::string provider = gitboard::lower_ascii(gitboard::trim(config.provider));
  if (provider == "anthropic-compatible" || provider == "anthropic" || provider == "claude" ||
      provider == "anthropic-messages") {
    return ai_provider_kind::k_anthropic;
  }
  return ai_provider_kind::k_openai_compatible;
}

std::string chat_request_json(const ai_config& config,
                              const std::string& repo_id,
                              const std::string& tool_context,
                              const std::string& history_summary,
                              const std::vector<chat_message>& history,
                              const std::string& message) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    return anthropic_chat_request_json(config, repo_id, tool_context,
                                       history_summary, history, message);
  }
  return openai_chat_request_json(config, repo_id, tool_context,
                                  history_summary, history, message);
}

std::string chat_followup_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& tool_context, const std::string& history_summary,
    const std::vector<chat_message>& history, const std::string& message,
    const std::vector<std::string>& provider_messages_json) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    return anthropic_chat_followup_request_json(
        config, repo_id, tool_context, history_summary, history, message,
        provider_messages_json);
  }
  return openai_chat_followup_request_json(config, repo_id, tool_context,
                                           history_summary, history, message,
                                           provider_messages_json);
}

std::string chat_repair_empty_content_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& message, const std::string& reasoning) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    return anthropic_chat_repair_empty_content_request_json(config, repo_id,
                                                            message, reasoning);
  }
  return openai_chat_repair_empty_content_request_json(config, repo_id, message,
                                                       reasoning);
}

std::string history_summary_request_json(
    const ai_config& config, const std::string& repo_id,
    const std::string& history_summary,
    const std::vector<chat_message>& messages) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    return anthropic_history_summary_request_json(config, repo_id,
                                                  history_summary, messages);
  }
  return openai_history_summary_request_json(config, repo_id, history_summary,
                                             messages);
}

std::string assistant_reasoning_text(const ai_config& config,
                                     const std::string& output) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) return "";
  return openai_assistant_reasoning_text(output);
}

std::string assistant_finish_reason(const ai_config& config,
                                    const std::string& output) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    return anthropic_finish_reason(output);
  }
  return openai_finish_reason(output);
}

std::string parse_chat_content(const ai_config& config, const std::string& output) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    return anthropic_parse_chat_content(output);
  }
  return openai_parse_chat_content(output);
}

parsed_agent_tool_calls parse_agent_tool_calls(const ai_config& config,
                                               const std::string& output) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    return anthropic_parse_agent_tool_calls(output);
  }
  return openai_parse_agent_tool_calls(output);
}

ai_http_request ai_chat_http_request(const ai_config& config,
                                     const std::string& api_key,
                                     std::string payload) {
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    return anthropic_chat_http_request(config, api_key, std::move(payload));
  }
  return openai_chat_http_request(config, api_key, std::move(payload));
}

void append_tool_result_messages(
    const ai_config& config,
    const std::vector<std::pair<std::string, std::string>>& tool_results,
    std::vector<std::string>& provider_messages_json) {
  if (tool_results.empty()) return;
  if (ai_provider(config) == ai_provider_kind::k_anthropic) {
    anthropic_append_tool_result_messages(tool_results, provider_messages_json);
    return;
  }
  openai_append_tool_result_messages(tool_results, provider_messages_json);
}

std::vector<chat_message> parse_chat_messages_field(const json_value& body,
                                                    const std::string& field,
                                                    std::size_t max_messages) {
  std::vector<chat_message> history;
  if (body.type != json_value::k_object) return history;
  auto it = body.object.find(field);
  if (it == body.object.end()) return history;
  if (it->second.type != json_value::k_array) {
    throw error(field + " must be an array");
  }
  if (it->second.array.size() > max_messages) {
    throw error(field + " has too many messages");
  }
  for (const json_value& item : it->second.array) {
    if (item.type != json_value::k_object) {
      throw error(field + " entries must be objects");
    }
    std::string role = json_string_member(item, "role");
    std::string content = json_string_member(item, "content");
    if (role != "user" && role != "assistant") {
      throw error(field + " roles must be user or assistant");
    }
    if (content.size() > kMaxChatMessageChars) {
      content.resize(kMaxChatMessageChars);
      content += "\n\n[Message truncated for context.]";
    }
    if (!gitboard::trim(content).empty()) {
      history.push_back(chat_message{role, content});
    }
  }
  return history;
}

std::vector<chat_message> parse_chat_history(const json_value& body) {
  return parse_chat_messages_field(body, "history", kMaxChatHistoryMessages);
}

std::string parse_history_summary(const json_value& body) {
  if (body.type != json_value::k_object) return "";
  auto it = body.object.find("historySummary");
  if (it == body.object.end()) return "";
  if (it->second.type != json_value::k_string) {
    throw error("historySummary must be a string");
  }
  if (it->second.string.size() > kMaxChatSummaryChars) {
    throw error("historySummary is too long");
  }
  return gitboard::trim(it->second.string);
}

std::string agent_initial_system_prompt(const ai_config& config,
                                        const std::string& repo_id,
                                        const std::string& tool_context,
                                        const std::string& history_summary) {
  std::string prompt = config.system_prompt.empty()
                           ? default_ai_system_prompt()
                           : config.system_prompt;
  prompt += "\n\nCurrent repoId: " + repo_id;
  if (!history_summary.empty()) {
    prompt += "\n\nSummary of earlier conversation:\n" + history_summary;
  }
  prompt += "\n\nRead-only Barnaby task context:\n" + tool_context;
  prompt +=
      "\n\nUse the provided project file tools when you need to inspect "
      "repository files. Do not pretend to read files from the tool list alone; "
      "call read_project_file_range, read_project_file, list_project_files, "
      "or search_project_files and then answer from the returned tool result. "
      "For code review or large files, prefer read_project_file_range so you "
      "can inspect focused chunks with line numbers instead of loading the "
      "entire file.";
  prompt +=
      "\n\nUse Barnaby task tools when the user asks to inspect or edit tasks. "
      "For existing task edits, call update_task, move_task, or comment_task "
      "to propose the change for user review; do not say you lack this "
      "ability and do not claim a proposed change was already applied. "
      "Story points must be one of 1, 2, 3, 5, 8, 13, 21, or 100; use 100 "
      "for unestimated tasks. "
      "Assignees must be exact aliases from "
      "team.team[].alias. Do not assign to an email address or invent an "
      "assignee. If the requested assignee is not in the team list, say you "
      "cannot find that user unless the user asks you to add them. When the "
      "user says \"me\" or \"myself\", use an exact matching team alias if one "
      "exists; otherwise say you cannot find the current user in the team list.";
  prompt +=
      "\n\nWhen the user asks you to create tasks, convert the request into "
      "drafts first. Do not claim the tasks were created. If the user asks "
      "to create a task, return a JSON object with this exact shape: "
      "{\"content\":\"short response\","
      "\"drafts\":[{\"title\":\"...\",\"body\":\"...\",\"priority\":\"medium\","
      "\"story_points\":100,\"status\":\"backlog\",\"assignee\":\"\",\"tags\":[]}]}. Do not return "
      "{\"taskId\":\"...\",\"fields\":{...}} for new tasks; Barnaby assigns "
      "task ids only after the user approves the draft. Keep draft titles "
      "concise. Use only known status, priority, assignee, and tag values "
      "when you are confident; otherwise omit the field or use the default.";
  return prompt;
}

std::string latest_request_instruction(const std::string& message) {
  return "Continue the latest user request now. Latest user request:\n" +
         message +
         "\n\nThe latest request supersedes older history for the current "
         "intent. Do not introduce yourself. Use the tool results above. If "
         "you already staged one part of a composite request, continue with "
         "any remaining requested parts. If the latest request asks for code "
         "review, provide concrete findings grounded in the file content and "
         "cite line numbers when available. If the latest request asks to "
         "create a task, return only draft-task JSON with this shape: "
         "{\"content\":\"short response\",\"drafts\":[{\"title\":\"...\","
         "\"body\":\"...\",\"priority\":\"medium\",\"story_points\":100,\"status\":\"backlog\","
         "\"assignee\":\"\",\"tags\":[]}]}. Do not return "
         "{\"taskId\":\"...\",\"fields\":{...}} for new tasks.";
}

std::string normalize_draft_priority(std::string value) {
  value = gitboard::trim(value);
  if (value == "high" || value == "medium" || value == "low") return value;
  return "medium";
}

int normalize_draft_story_points(const json_value& object) {
  auto it = object.object.find("story_points");
  if (it == object.object.end()) return 100;
  std::string raw;
  if (it->second.type == json_value::k_number) raw = it->second.number;
  else if (it->second.type == json_value::k_string) raw = it->second.string;
  else return 100;
  try {
    int value = std::stoi(raw);
    if (value == 1 || value == 2 || value == 3 || value == 5 ||
        value == 8 || value == 13 || value == 21 || value == 100) {
      return value;
    }
  } catch (const std::exception&) {
  }
  return 100;
}

std::string normalize_draft_status(std::string value) {
  value = gitboard::trim(value);
  return value.empty() ? "backlog" : value;
}

std::string truncate_text(std::string value, std::size_t max_size) {
  if (value.size() > max_size) value.resize(max_size);
  return value;
}

struct parsed_agent_chat {
  std::string content;
  std::string drafts_json = "[]";
};

void append_draft_json(const json_value& item, std::ostringstream& drafts,
                       bool& first, std::size_t& count) {
  if (item.type != json_value::k_object || count >= 12) return;
  std::string title = gitboard::trim(json_string_member(item, "title"));
  std::string body = json_string_member(item, "body");

  auto fields_it = item.object.find("fields");
  if (fields_it != item.object.end() &&
      fields_it->second.type == json_value::k_object) {
    if (title.empty()) title = gitboard::trim(json_string_member(fields_it->second, "title"));
    if (body.empty()) body = json_string_member(fields_it->second, "body");
  }

  auto nested_it = item.object.find("drafts");
  if (nested_it != item.object.end() &&
      nested_it->second.type == json_value::k_array) {
    if (title.empty()) {
      for (const json_value& nested : nested_it->second.array) {
        append_draft_json(nested, drafts, first, count);
      }
      return;
    }
    if (body.empty() && !nested_it->second.array.empty() &&
        nested_it->second.array.front().type == json_value::k_object) {
      body = json_string_member(nested_it->second.array.front(), "body");
    }
  }

  if (title.empty()) return;
  const json_value* field_source = &item;
  if (fields_it != item.object.end() &&
      fields_it->second.type == json_value::k_object) {
    field_source = &fields_it->second;
  }
  std::string priority =
      normalize_draft_priority(json_string_member(*field_source, "priority"));
  int story_points = normalize_draft_story_points(*field_source);
  std::string status =
      normalize_draft_status(json_string_member(*field_source, "status"));
  std::string assignee =
      gitboard::trim(json_string_member(*field_source, "assignee"));
  if (!first) drafts << ",";
  first = false;
  ++count;
  drafts << "{";
  drafts << "\"title\":" << json_quote(truncate_text(title, 200)) << ",";
  drafts << "\"body\":" << json_quote(truncate_text(body, 12000)) << ",";
  drafts << "\"priority\":" << json_quote(priority) << ",";
  drafts << "\"story_points\":" << story_points << ",";
  drafts << "\"status\":" << json_quote(truncate_text(status, 80)) << ",";
  drafts << "\"assignee\":" << json_quote(truncate_text(assignee, 80)) << ",";
  drafts << "\"tags\":" << json_array_string_member(*field_source, "tags");
  drafts << "}";
}

parsed_agent_chat parse_agent_chat_payload(const ai_config& config,
                                           const std::string& output) {
  std::string content = parse_chat_content(config, output);
  parsed_agent_chat parsed;
  parsed.content = content;

  auto envelope = extract_json_object_from_text(content);
  if (!envelope) return parsed;
  std::string envelope_content = json_string_member(*envelope, "content");
  if (!envelope_content.empty()) parsed.content = envelope_content;

  std::ostringstream drafts;
  drafts << "[";
  bool first = true;
  std::size_t count = 0;
  auto drafts_it = envelope->object.find("drafts");
  if (drafts_it != envelope->object.end() &&
      drafts_it->second.type == json_value::k_array) {
    for (const json_value& item : drafts_it->second.array) {
      append_draft_json(item, drafts, first, count);
    }
  } else {
    append_draft_json(*envelope, drafts, first, count);
  }
  drafts << "]";
  if (count == 0) return parsed;
  parsed.drafts_json = drafts.str();
  if (count > 0 && envelope_content.empty()) {
    parsed.content = "Please review these drafts before creating tasks.";
  }
  return parsed;
}

process_result post_ai_json(const ai_config& config,
                            const ai_http_request& request) {
  debug_log_ai_text("request endpoint", request.endpoint);
  debug_log_ai_json("request payload", request.payload);
  process_result result = post_ai_http_json(config, request);
  debug_log_ai_text("response exit_code", std::to_string(result.exit_code));
  debug_log_ai_json("response body", result.output);
  if (result.exit_code != 0) {
    std::string message = result.output.empty()
                              ? "AI endpoint request failed"
                              : result.output;
    throw error("AI endpoint request failed at " + request.endpoint + ": " + message);
  }
  return result;
}

process_result post_ai_chat_json(const ai_config& config,
                                 const std::string& api_key,
                                 const std::string& payload) {
  return post_ai_json(config, ai_chat_http_request(config, api_key, payload));
}
bool is_mutating_agent_tool(const std::string& name) {
  return name == "update_task" || name == "move_task" ||
         name == "comment_task" || name == "add_team_member";
}

std::string proposed_action_dedupe_key(const agent_tool_call& call,
                                       const json_value& args) {
  if (call.name == "update_task") {
    auto fields_it = args.object.find("fields");
    return call.name + ":" + json_string_member(args, "taskId") + ":" +
           (fields_it == args.object.end() ? std::string()
                                           : stringify_json(fields_it->second));
  }
  if (call.name == "move_task") {
    return call.name + ":" + json_string_member(args, "taskId") + ":" +
           gitboard::trim(json_string_member(args, "status"));
  }
  if (call.name == "comment_task") {
    return call.name + ":" + json_string_member(args, "taskId") + ":" +
           gitboard::trim(json_string_member(args, "message"));
  }
  if (call.name == "add_team_member") {
    return call.name + ":" + gitboard::trim(json_string_member(args, "alias")) +
           ":" + gitboard::trim(json_string_member(args, "email"));
  }
  return call.name + ":" + call.arguments;
}

bool model_hit_output_limit(const ai_config& config, const std::string& output) {
  try {
    std::string reason = assistant_finish_reason(config, output);
    return reason == "length" || reason == "max_tokens" ||
           reason == "model_length";
  } catch (const std::exception&) {
    return false;
  }
}

std::string repair_material_from_response(const ai_config& config,
                                          const std::string& output) {
  std::string material;
  try {
    material = parse_chat_content(config, output);
  } catch (const std::exception&) {
  }
  try {
    std::string reasoning = assistant_reasoning_text(config, output);
    if (!gitboard::trim(reasoning).empty()) {
      if (!material.empty()) material += "\n\n";
      material += reasoning;
    }
  } catch (const std::exception&) {
  }
  return material;
}

http_response handle_get_ai_config() {
  server_config config = load_config();
  return json_response(200, ai_config_response_json(config.ai));
}

http_response handle_put_ai_config(const http_request& request) {
  json_value body = require_object_body(request);
  server_config config = load_config();
  ai_config updated = parse_ai_config_update(body, config.ai);
  updated.api_key_ref = ai_api_key_ref(updated);

  auto api_key_it = body.object.find("apiKey");
  if (api_key_it != body.object.end()) {
    if (api_key_it->second.type != json_value::k_string) {
      return json_response(400, error_json("apiKey must be a string"));
    }
    std::string api_key = gitboard::trim(api_key_it->second.string);
    if (!api_key.empty()) {
      save_ai_api_key(api_key, updated);
      if (!config.ai.api_key_ref.empty() &&
          config.ai.api_key_ref != updated.api_key_ref) {
        clear_ai_api_key(config.ai);
      }
    }
  }
  auto clear_key_it = body.object.find("clearApiKey");
  if (clear_key_it != body.object.end()) {
    if (clear_key_it->second.type != json_value::k_bool) {
      return json_response(400, error_json("clearApiKey must be a boolean"));
    }
    if (clear_key_it->second.boolean) clear_ai_api_key(updated);
  }

  config.ai = updated;
  save_config(config);
  return json_response(200, ai_config_response_json(config.ai));
}

http_response handle_ai_test() {
  server_config config = load_config();
  if (!config.ai.enabled) {
    return json_response(400, error_json("AI is not enabled"));
  }
  if (config.ai.base_url.empty()) {
    return json_response(400, error_json("AI endpoint URL is required"));
  }
  if (config.ai.model.empty()) {
    return json_response(400, error_json("AI model is required"));
  }
  if (!ai_api_key_configured(config.ai)) {
    return json_response(400, error_json("AI API key is not configured"));
  }
  return json_response(200,
                       "{\n  \"ok\": true,\n"
                       "  \"message\": \"AI configuration is complete. "
                       "Endpoint calls are not enabled in this phase.\"\n}\n");
}

http_response handle_agent_chat(const http_request& request,
                                const fs::path& gitboard_path) {
  json_value body = require_object_body(request);
  auto repo_it = body.object.find("repoId");
  auto message_it = body.object.find("message");
  if (repo_it == body.object.end() || repo_it->second.type != json_value::k_string) {
    return json_response(400, error_json("repoId is required"));
  }
  if (message_it == body.object.end() ||
      message_it->second.type != json_value::k_string ||
      gitboard::trim(message_it->second.string).empty()) {
    return json_response(400, error_json("message is required"));
  }
  std::string repo_id = repo_it->second.string;
  if (!valid_repo_id(repo_id)) {
    return json_response(400, error_json("invalid repository id"));
  }
  server_config config = load_config();
  if (!config.ai.enabled) {
    return json_response(400, error_json("AI is not enabled"));
  }
  auto repo = config.repositories.find(repo_id);
  if (repo == config.repositories.end()) {
    return json_response(404, error_json("repository id not found"));
  }
  auto repo_ai = config.repository_ai.find(repo_id);
  repo_ai_config repo_ai_value =
      repo_ai == config.repository_ai.end() ? repo_ai_config{} : repo_ai->second;
  if (config.ai.base_url.empty()) {
    return json_response(400, error_json("AI endpoint URL is required"));
  }
  if (config.ai.model.empty()) {
    return json_response(400, error_json("AI model is required"));
  }
  if (!ai_api_key_configured(config.ai)) {
    return json_response(400, error_json("AI API key is not configured"));
  }
  std::string conversation_id = "phase1";
  auto conversation_it = body.object.find("conversationId");
  if (conversation_it != body.object.end() &&
      conversation_it->second.type == json_value::k_string &&
      !conversation_it->second.string.empty()) {
    conversation_id = conversation_it->second.string;
  }
  if (conversation_id == "phase1" || conversation_id == "phase2") {
    conversation_id = "phase3";
  }
  std::vector<chat_message> history;
  std::string history_summary;
  try {
    history = parse_chat_history(body);
    history_summary = parse_history_summary(body);
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }

  std::string api_key = load_ai_api_key(config.ai);
  std::string tool_context;
  try {
    tool_context = build_agent_tool_context(gitboard_path, repo->second, repo_id,
                                            repo_ai_value,
                                            message_it->second.string);
  } catch (const std::exception& ex) {
    return json_response(500, error_json(ex.what()));
  }
  std::string payload = chat_request_json(config.ai, repo_id, tool_context,
                                          history_summary, history,
                                          message_it->second.string);

  process_result result;
  try {
    result = post_ai_chat_json(config.ai, api_key, payload);
  } catch (const std::exception& ex) {
    return json_response(502, error_json(ex.what()));
  }

  bool tasks_changed = false;
  std::vector<std::string> proposed_actions;
  std::set<std::string> proposed_action_keys;
  std::vector<std::string> provider_messages_json;

  auto proposed_actions_json_string = [&proposed_actions]() {
    std::ostringstream proposed_actions_json;
    proposed_actions_json << "[";
    for (std::size_t i = 0; i < proposed_actions.size(); ++i) {
      if (i > 0) proposed_actions_json << ",";
      proposed_actions_json << proposed_actions[i];
    }
    proposed_actions_json << "]";
    return proposed_actions_json.str();
  };

  bool tool_calls_drained = false;
  for (int tool_round = 0; tool_round < kMaxAgentToolRounds; ++tool_round) {
    parsed_agent_tool_calls tool_calls;
    try {
      tool_calls = parse_agent_tool_calls(config.ai, result.output);
    } catch (const std::exception& ex) {
      return json_response(502, error_json(ex.what()));
    }
    if (tool_calls.calls.empty()) {
      tool_calls_drained = true;
      break;
    }
    debug_log_ai_text("tool round", std::to_string(tool_round + 1));
    if (!tool_calls.assistant_message_json.empty()) {
      provider_messages_json.push_back(tool_calls.assistant_message_json);
    }
    std::vector<std::pair<std::string, std::string>> tool_results;
    for (const auto& call : tool_calls.calls) {
      debug_log_ai_text("tool call", call.name + " " + call.arguments);
      if (is_mutating_agent_tool(call.name)) {
        try {
          json_value args;
          if (call.arguments.empty()) {
            args.type = json_value::k_object;
          } else {
            args = json_parser(call.arguments).parse();
            if (args.type != json_value::k_object) {
              throw error("tool arguments must be an object");
            }
          }
          const std::string dedupe_key = proposed_action_dedupe_key(call, args);
          if (proposed_action_keys.count(dedupe_key)) {
            debug_log_ai_text("staged action deduped", call.name + " " + call.arguments);
            tool_results.push_back(
                {call.id,
                 "{\"ok\":true,\"staged\":true,\"duplicate\":true,\"message\":\"Duplicate action already staged for user review and has not been applied.\"}"});
            continue;
          }
          std::string action = proposed_action_from_tool_call(
              call, args, repo->second, proposed_actions.size());
          proposed_actions.push_back(action);
          proposed_action_keys.insert(dedupe_key);
          debug_log_ai_json("staged action", action);
          tool_results.push_back(
              {call.id,
               "{\"ok\":true,\"staged\":true,\"message\":\"Action staged for user review and has not been applied.\"}"});
        } catch (const std::exception& ex) {
          debug_log_ai_text("staged action rejected",
                            call.name + " " + call.arguments + " -> " + ex.what());
          tool_results.push_back(
              {call.id, "{\"ok\":false,\"error\":" + json_quote(ex.what()) + "}"});
        }
        continue;
      }
      tool_results.push_back(
          {call.id, execute_agent_tool(call, gitboard_path, repo_id,
                                       repo->second, repo_ai_value)});
      debug_log_ai_json("tool result", tool_results.back().second);
    }
    append_tool_result_messages(config.ai, tool_results, provider_messages_json);
    std::string followup_payload = chat_followup_request_json(
        config.ai, repo_id, tool_context, history_summary, history,
        message_it->second.string, provider_messages_json);
    try {
      result = post_ai_chat_json(config.ai, api_key, followup_payload);
    } catch (const std::exception& ex) {
      return json_response(502, error_json(ex.what()));
    }
  }

  if (!tool_calls_drained) {
    parsed_agent_tool_calls pending_tool_calls;
    try {
      pending_tool_calls = parse_agent_tool_calls(config.ai, result.output);
    } catch (const std::exception& ex) {
      return json_response(502, error_json(ex.what()));
    }
    if (!pending_tool_calls.calls.empty()) {
      debug_log_ai_text("tool round limit reached",
                        std::to_string(pending_tool_calls.calls.size()) +
                            " pending tool calls after " +
                            std::to_string(kMaxAgentToolRounds) + " rounds");
      if (proposed_actions.empty()) {
        return json_response(
            502,
            error_json("AI response still requested more tool calls after the "
                       "tool-round safety limit"));
      }
      std::string content =
          "I staged " + std::to_string(proposed_actions.size()) +
          " proposed " +
          (proposed_actions.size() == 1 ? std::string("action")
                                        : std::string("actions")) +
          " for your review, but the AI response still requested more tool "
          "calls after Barnaby's safety limit. I stopped instead of silently "
          "dropping additional requested changes. Review these staged actions "
          "or ask me to continue.";
      std::string response =
          std::string("{\n"
                      "  \"ok\": true,\n"
                      "  \"conversationId\": ") +
          json_quote(conversation_id) + ",\n"
          "  \"messages\": [{\"role\": \"assistant\", \"content\": " +
          json_quote(content) +
          "}],\n"
          "  \"drafts\": [],\n"
          "  \"tasksChanged\": false,\n"
          "  \"proposedActions\": " + proposed_actions_json_string() + ",\n"
          "  \"ui\": {}\n"
          "}\n";
      return json_response(200, response);
    }
  }

  parsed_agent_chat parsed_chat;
  try {
    parsed_chat = parse_agent_chat_payload(config.ai, result.output);
    if (model_hit_output_limit(config.ai, result.output) &&
        proposed_actions.empty() && parsed_chat.drafts_json == "[]") {
      throw error("AI response was cut off by the output token limit");
    }
  } catch (const std::exception& ex) {
    if (!proposed_actions.empty()) {
      parsed_chat.content =
          "I staged " + std::to_string(proposed_actions.size()) +
          " proposed " + (proposed_actions.size() == 1 ? std::string("action")
                                                        : std::string("actions")) +
          " for your review. Select the ones you want to apply.";
    } else {
      std::string repair_material = repair_material_from_response(config.ai, result.output);
      if (gitboard::trim(repair_material).empty()) {
        return json_response(502, error_json(ex.what()));
      }
      debug_log_ai_text("repairing empty assistant content",
                        "provider output material present");
      try {
        result = post_ai_chat_json(
            config.ai, api_key,
            chat_repair_empty_content_request_json(
                config.ai, repo_id, message_it->second.string, repair_material));
        parsed_chat = parse_agent_chat_payload(config.ai, result.output);
        if (model_hit_output_limit(config.ai, result.output) &&
            parsed_chat.drafts_json == "[]") {
          throw error("AI repair response was cut off by the output token limit");
        }
      } catch (const std::exception& repair_ex) {
        debug_log_ai_text("repairing assistant content failed", repair_ex.what());
        parsed_chat.content =
            "The AI response was cut off before Barnaby could extract a usable "
            "answer or task draft. Please ask Pip to continue with a shorter "
            "response, or raise the AI max output tokens in settings.";
        parsed_chat.drafts_json = "[]";
      }
    }
  }
  std::string response =
      std::string("{\n"
                  "  \"ok\": true,\n"
                  "  \"conversationId\": ") +
      json_quote(conversation_id) + ",\n"
      "  \"messages\": [{\"role\": \"assistant\", \"content\": " +
      json_quote(parsed_chat.content) +
      "}],\n"
      "  \"drafts\": " + parsed_chat.drafts_json + ",\n"
      "  \"tasksChanged\": " + std::string(tasks_changed ? "true" : "false") + ",\n"
      "  \"proposedActions\": " + proposed_actions_json_string() + ",\n"
      "  \"ui\": {}\n"
      "}\n";
  return json_response(200, response);
}

http_response handle_agent_summarize(const http_request& request) {
  json_value body = require_object_body(request);
  auto repo_it = body.object.find("repoId");
  if (repo_it == body.object.end() || repo_it->second.type != json_value::k_string) {
    return json_response(400, error_json("repoId is required"));
  }
  std::string repo_id = repo_it->second.string;
  if (!valid_repo_id(repo_id)) {
    return json_response(400, error_json("invalid repository id"));
  }

  server_config config = load_config();
  if (!config.ai.enabled) {
    return json_response(400, error_json("AI is not enabled"));
  }
  if (config.repositories.find(repo_id) == config.repositories.end()) {
    return json_response(404, error_json("repository id not found"));
  }
  if (config.ai.base_url.empty()) {
    return json_response(400, error_json("AI endpoint URL is required"));
  }
  if (config.ai.model.empty()) {
    return json_response(400, error_json("AI model is required"));
  }
  if (!ai_api_key_configured(config.ai)) {
    return json_response(400, error_json("AI API key is not configured"));
  }

  std::vector<chat_message> messages;
  std::string history_summary;
  try {
    messages = parse_chat_messages_field(body, "messages", 3);
    history_summary = parse_history_summary(body);
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
  if (messages.empty()) {
    return json_response(400, error_json("messages are required"));
  }

  std::string api_key = load_ai_api_key(config.ai);
  std::string payload = history_summary_request_json(config.ai, repo_id,
                                                     history_summary, messages);
  process_result result;
  try {
    result = post_ai_chat_json(config.ai, api_key, payload);
  } catch (const std::exception& ex) {
    return json_response(502, error_json(ex.what()));
  }

  std::string summary;
  try {
    summary = parse_chat_content(config.ai, result.output);
  } catch (const std::exception& ex) {
    return json_response(502, error_json(ex.what()));
  }
  if (summary.size() > kMaxChatSummaryChars) {
    summary.resize(kMaxChatSummaryChars);
  }
  return json_response(200,
                       "{\n  \"ok\": true,\n  \"historySummary\": " +
                           json_quote(summary) + "\n}\n");
}

}  // namespace gitboard::server

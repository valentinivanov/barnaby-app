#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct command_result {
  int exit_code = 0;
  std::string output;
};

struct loaded_task {
  std::map<std::string, std::string> scalars;
  std::map<std::string, std::vector<std::string>> lists;
  std::string body;
};

struct expected_task {
  std::string id;
  std::string title;
  std::string assignee;
  std::string priority = "medium";
  int story_points = 100;
  std::vector<std::string> tags;
  std::string status = "todo";
  std::string created_at;
  std::string created_by;
  std::vector<std::string> branches;
  std::vector<std::string> prs;
  std::string ci_status = "unknown";
  std::string body;
  std::string comment;
};

std::string shell_quote(std::string_view s) {
#ifdef _WIN32
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else out += c;
  }
  out += "\"";
  return out;
#else
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
#endif
}

command_result run_capture(const std::string& command) {
  std::array<char, 256> buffer{};
  command_result result;
#ifdef _WIN32
  FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
  FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
  if (!pipe) throw failure("failed to run command: " + command);
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    result.output += buffer.data();
  }
#ifdef _WIN32
  result.exit_code = _pclose(pipe);
#else
  result.exit_code = pclose(pipe);
#endif
  return result;
}

std::string require_success(const std::string& command, const std::string& step) {
  command_result result = run_capture(command);
  if (result.exit_code != 0) {
    throw failure(step + " failed\ncommand: " + command +
                  "\noutput:\n" + result.output);
  }
  return result.output;
}

std::string base64_encode(const std::string& input) {
  static constexpr char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0;
  int valb = -6;
  for (unsigned char c : input) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string base64_decode(const std::string& input) {
  static const std::string chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::array<int, 256> table{};
  table.fill(-1);
  for (int i = 0; i < static_cast<int>(chars.size()); ++i) {
    table[static_cast<unsigned char>(chars[i])] = i;
  }
  std::string out;
  int val = 0;
  int valb = -8;
  for (unsigned char c : input) {
    if (std::isspace(c)) continue;
    if (c == '=') break;
    if (table[c] == -1) throw failure("invalid Base64 payload");
    val = (val << 6) + table[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, delim)) out.push_back(part);
  return out;
}

std::string unquote_yaml(std::string value) {
  value = trim(std::move(value));
  if (value.size() >= 2 &&
      ((value.front() == '\'' && value.back() == '\'') ||
       (value.front() == '"' && value.back() == '"'))) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

loaded_task parse_task_content(const std::string& content) {
  std::size_t yaml_start = 0;
  if (starts_with(content, "---\r\n")) {
    yaml_start = 5;
  } else if (starts_with(content, "---\n")) {
    yaml_start = 4;
  } else {
    throw failure("task content is missing front matter");
  }
  const auto end = content.find("\n---", yaml_start);
  if (end == std::string::npos) throw failure("unterminated front matter");
  std::string yaml = content.substr(yaml_start, end - yaml_start);
  std::string body = content.substr(end + 4);
  if (starts_with(body, "\r\n")) body.erase(0, 2);
  else if (starts_with(body, "\n")) body.erase(0, 1);
  if (starts_with(body, "\r\n")) body.erase(0, 2);
  else if (starts_with(body, "\n")) body.erase(0, 1);

  loaded_task task;
  task.body = body;
  std::string current_list_key;
  for (std::string line : split(yaml, '\n')) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim(line).empty()) continue;
    std::string trimmed = trim(line);
    if (starts_with(trimmed, "- ")) {
      if (current_list_key.empty()) {
        throw failure("YAML list item without list key");
      }
      task.lists[current_list_key].push_back(unquote_yaml(trimmed.substr(2)));
      continue;
    }
    current_list_key.clear();
    auto colon = line.find(':');
    if (colon == std::string::npos) throw failure("invalid YAML line: " + line);
    std::string key = trim(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));
    if (value.empty()) {
      current_list_key = key;
      task.lists[key] = {};
    } else if (value == "[]") {
      task.lists[key] = {};
    } else {
      task.scalars[key] = unquote_yaml(value);
    }
  }
  return task;
}

loaded_task parse_task_command_output(const std::string& output,
                                      const std::string& expected_id) {
  std::vector<std::string> lines = split(output, '\n');
  if (lines.size() < 2) throw failure("task output is missing lines");
  if (trim(lines[0]) != expected_id) {
    throw failure("task output id mismatch: expected " + expected_id +
                  ", got " + lines[0]);
  }
  return parse_task_content(base64_decode(trim(lines[1])));
}

std::pair<std::string, fs::path> parse_create_output(const std::string& output,
                                                     const fs::path& root) {
  static const std::regex re("^Created (TASK-[A-Za-z0-9+-]+) ([^\\n]+)\\n?$");
  std::smatch match;
  if (!std::regex_match(output, match, re)) {
    throw failure("unexpected create output: " + output);
  }
  return {match[1].str(), root / "tasks" / match[2].str()};
}

std::chrono::system_clock::time_point parse_timestamp_seconds(
    const std::string& value) {
  std::tm tm{};
  std::istringstream in(value.substr(0, 19));
  in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (!in) throw failure("failed to parse timestamp: " + value);
  std::time_t local = std::mktime(&tm);
  std::tm* as_utc = std::gmtime(&local);
  std::time_t utc_as_local = std::mktime(as_utc);
  std::time_t utc = local + (local - utc_as_local);
  return std::chrono::system_clock::from_time_t(utc);
}

void expect(bool condition, const std::string& message) {
  if (!condition) throw failure(message);
}

void expect_eq(const std::string& actual, const std::string& expected,
               const std::string& field) {
  if (actual != expected) {
    throw failure(field + " mismatch: expected '" + expected + "', got '" +
                  actual + "'");
  }
}

void expect_vec_eq(const std::vector<std::string>& actual,
                   const std::vector<std::string>& expected,
                   const std::string& field) {
  if (actual != expected) throw failure(field + " list mismatch");
}

std::string scalar(const loaded_task& task, const std::string& key) {
  auto it = task.scalars.find(key);
  if (it == task.scalars.end()) return "";
  return it->second;
}

std::vector<std::string> list_value(const loaded_task& task,
                                    const std::string& key) {
  auto it = task.lists.find(key);
  if (it == task.lists.end()) return {};
  return it->second;
}

void expect_recent_updated_at(const loaded_task& task) {
  const auto updated = parse_timestamp_seconds(scalar(task, "updated_at"));
  const auto now = std::chrono::system_clock::now();
  const auto diff =
      std::chrono::duration_cast<std::chrono::seconds>(now - updated).count();
  expect(diff >= -10 && diff <= 10,
         "updated_at is not within 10 seconds from current time");
}

void compare_loaded_to_expected(const loaded_task& actual,
                                const expected_task& expected) {
  expect_eq(scalar(actual, "id"), expected.id, "id");
  expect_eq(scalar(actual, "title"), expected.title, "title");
  expect_eq(scalar(actual, "assignee"), expected.assignee, "assignee");
  expect_eq(scalar(actual, "priority"), expected.priority, "priority");
  expect_eq(scalar(actual, "story_points"), std::to_string(expected.story_points),
            "story_points");
  expect_eq(scalar(actual, "status"), expected.status, "status");
  expect_eq(scalar(actual, "created_by"), expected.created_by, "created_by");
  expect_eq(scalar(actual, "ci_status"), expected.ci_status, "ci_status");
  expect_vec_eq(list_value(actual, "tags"), expected.tags, "tags");
  expect_vec_eq(list_value(actual, "branches"), expected.branches, "branches");
  expect_vec_eq(list_value(actual, "prs"), expected.prs, "prs");
  expect_eq(actual.body, expected.body, "body");
  if (!expected.comment.empty()) {
    expect(actual.body.find(expected.comment) != std::string::npos,
           "comment body does not contain expected comment");
  }
}

std::string gitboard_command(const fs::path& gitboard, const fs::path& root,
                             const std::vector<std::string>& args) {
  std::string command = shell_quote(gitboard.string()) + " --project-root " +
                        shell_quote(root.string());
  for (const auto& arg : args) command += " " + shell_quote(arg);
  return command;
}

loaded_task run_task_load(const fs::path& gitboard, const fs::path& root,
                          const std::string& task_id) {
  return parse_task_command_output(
      require_success(gitboard_command(gitboard, root, {"task", task_id}),
                      "load " + task_id),
      task_id);
}

void run_points_and_check(const fs::path& gitboard, const fs::path& root,
                          expected_task& expected, int value) {
  require_success(gitboard_command(gitboard, root,
                                   {"points", expected.id, std::to_string(value)}),
                  "points");
  expected.story_points = value;
  loaded_task loaded = run_task_load(gitboard, root, expected.id);
  expect_recent_updated_at(loaded);
  expect_eq(scalar(loaded, "story_points"), std::to_string(value),
            "story_points");
}

void run_update_and_check(const fs::path& gitboard, const fs::path& root,
                          expected_task& expected, const std::string& command,
                          const std::string& value) {
  require_success(gitboard_command(gitboard, root,
                                   {command, expected.id, base64_encode(value)}),
                  command);
  loaded_task loaded = run_task_load(gitboard, root, expected.id);
  expect_recent_updated_at(loaded);
  if (command == "body") {
    expected.body = value;
    if (!expected.body.empty() && expected.body.back() != '\n') {
      expected.body.push_back('\n');
    }
    expect_eq(loaded.body, expected.body, "body");
  } else if (command == "assignee") {
    expected.assignee = value;
    expect_eq(scalar(loaded, "assignee"), expected.assignee, "assignee");
  } else if (command == "branches") {
    expected.branches = split(value, ',');
    expect_vec_eq(list_value(loaded, "branches"), expected.branches, "branches");
  } else if (command == "ci_status") {
    expected.ci_status = value;
    expect_eq(scalar(loaded, "ci_status"), expected.ci_status, "ci_status");
  } else if (command == "priority") {
    expected.priority = value;
    expect_eq(scalar(loaded, "priority"), expected.priority, "priority");
  } else if (command == "prs") {
    expected.prs = split(value, ',');
    expect_vec_eq(list_value(loaded, "prs"), expected.prs, "prs");
  } else if (command == "tags") {
    expected.tags = split(value, ',');
    expect_vec_eq(list_value(loaded, "tags"), expected.tags, "tags");
  } else if (command == "title") {
    expected.title = value;
    expect_eq(scalar(loaded, "title"), expected.title, "title");
  }
}

std::string json_string_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string to_crlf(const std::string& input) {
  std::string out;
  for (char c : input) {
    if (c == '\n') out += "\r\n";
    else out.push_back(c);
  }
  return out;
}

void replace_first(std::string& value, const std::string& from,
                   const std::string& to) {
  auto pos = value.find(from);
  if (pos == std::string::npos) throw failure("missing text to replace: " + from);
  value.replace(pos, from.size(), to);
}

void write_batch_file(const fs::path& path, const std::string& id1,
                      const std::string& id2) {
  std::ofstream out(path);
  out << "{\n"
      << "  \"batch\": [\n"
      << "    {\"cmd\": \"list\", \"args\": []},\n"
      << "    {\"cmd\": \"task\", \"args\": [\"" << json_string_escape(id1)
      << "\"]},\n"
      << "    {\"cmd\": \"task\", \"args\": [\"" << json_string_escape(id2)
      << "\"]}\n"
      << "  ]\n"
      << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: positive_test PATH_TO_GITBOARD\n";
    return 2;
  }

  const fs::path gitboard = fs::absolute(argv[1]).lexically_normal();
  const fs::path root =
      fs::temp_directory_path() /
      ("gitboard-positive-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path remote_root = root.parent_path() / (root.filename().string() + "-remote.git");
  const fs::path remote_peer_root = root.parent_path() / (root.filename().string() + "-remote-peer");

  try {
    fs::create_directories(root);
    expected_task expected;

    auto [id1, file1] = parse_create_output(
        require_success(gitboard_command(gitboard, root,
                                         {"create", base64_encode("First task")}),
                        "create first task"),
        root);
    expect(std::regex_match(id1, std::regex("^TASK-[A-Za-z0-9+-]{22}$")),
           "first task id should be TASK- plus base64 GUID bytes: " + id1);
    expect(fs::exists(file1), "first task file was not created");
    const fs::path initialized_statuses = root / "tasks" / "statuses.json";
    expect(fs::exists(initialized_statuses),
           "first task database initialization should copy statuses.json");
    std::ifstream initialized_statuses_in(initialized_statuses);
    std::stringstream initialized_statuses_buffer;
    initialized_statuses_buffer << initialized_statuses_in.rdbuf();
    std::string initialized_statuses_content = initialized_statuses_buffer.str();
    expect(initialized_statuses_content.find("\"display\"") != std::string::npos,
           "initialized statuses.json should include bundled display metadata");
    expect(initialized_statuses_content.find("\"archived\"") != std::string::npos,
           "initialized statuses.json should include bundled archived status");
    const fs::path initialized_team = root / "tasks" / "team.json";
    expect(fs::exists(initialized_team),
           "first task database initialization should create team.json");
    std::ifstream initialized_team_in(initialized_team);
    std::stringstream initialized_team_buffer;
    initialized_team_buffer << initialized_team_in.rdbuf();
    expect(initialized_team_buffer.str() ==
               "{\"team\":[{\"alias\":\"agent_pip\",\"email\":\"pip@barnaby\"}]}\n",
           "initialized team.json should contain the default Agent Pip team");
    expected.id = id1;
    expected.title = "First task";
    expected.body =
        "# First task\n\n## Description\n\n\n## Checklist\n- [ ] \n\n## Comments\n";

    auto [id2, file2] = parse_create_output(
        require_success(gitboard_command(gitboard, root,
                                         {"create", base64_encode("Second task")}),
                        "create second task"),
        root);
    expect(fs::exists(file2), "second task file was not created");
    expect(id1 != id2, "created task ids must be different");
    const fs::path peer_root = root / "peer-repo";
    fs::create_directories(peer_root);
    auto [peer_id, peer_file] = parse_create_output(
        require_success(gitboard_command(gitboard, peer_root,
                                         {"create", base64_encode("First task")}),
                        "create peer task from same starting state"),
        peer_root);
    expect(fs::exists(peer_file), "peer task file was not created");
    expect(peer_id != id1,
           "task ids created from the same starting repo state must differ");
    fs::remove_all(peer_root);
    std::string default_team =
        require_success(gitboard_command(gitboard, root, {"team"}), "team default");
    expect(default_team.find("\"alias\":\"agent_pip\"") != std::string::npos,
           "initialized team file should return Agent Pip");
    {
      std::ofstream out(root / "tasks" / "team.json");
      out << "{\"team\":[{\"alias\":\"alice\",\"email\":\"alice@example.com\"}]}\n";
    }
    std::string team =
        require_success(gitboard_command(gitboard, root, {"team"}), "team");
    expect(team.find("\"alias\":\"alice\"") != std::string::npos,
           "team output is missing alice");
    {
      std::ofstream out(root / "tasks" / "statuses.json");
      out << "{\"todo\":{\"transitions\":[\"blocked\"]},"
             "\"blocked\":{\"transitions\":[]}}\n";
    }
    std::string local_statuses =
        require_success(gitboard_command(gitboard, root, {"statuses"}),
                        "local statuses");
    expect(local_statuses.find("\"blocked\"") != std::string::npos,
           "statuses should load tasks/statuses.json when present");
    fs::remove(root / "tasks" / "statuses.json");
    {
      std::ifstream in(file2);
      std::stringstream buffer;
      buffer << in.rdbuf();
      std::ofstream out(file2, std::ios::binary | std::ios::trunc);
      out << to_crlf(buffer.str());
    }

    loaded_task first_loaded = run_task_load(gitboard, root, id1);
    loaded_task second_loaded = run_task_load(gitboard, root, id2);
    expected.created_at = scalar(first_loaded, "created_at");
    expect(!expected.created_at.empty(), "first task created_at is empty");
    expect(!scalar(second_loaded, "created_at").empty(),
           "second task created_at is empty");

    run_update_and_check(gitboard, root, expected, "body",
                         "body-value-positive-test");
    run_update_and_check(gitboard, root, expected, "assignee", "assignee-user");
    run_update_and_check(gitboard, root, expected, "branches",
                         "branch-a,branch-b");
    run_update_and_check(gitboard, root, expected, "ci_status", "passed");
    run_update_and_check(gitboard, root, expected, "priority", "urgent");
    run_points_and_check(gitboard, root, expected, 13);
    run_update_and_check(gitboard, root, expected, "prs", "pr-1,pr-2");
    run_update_and_check(gitboard, root, expected, "tags", "tag-a,tag-b");
    run_update_and_check(gitboard, root, expected, "title",
                         "Updated positive title");

    expected.status = "in_progress";
    require_success(gitboard_command(gitboard, root,
                                     {"move", id1, base64_encode(expected.status)}),
                    "move");
    loaded_task moved = run_task_load(gitboard, root, id1);
    expect_recent_updated_at(moved);
    expect_eq(scalar(moved, "status"), expected.status, "status");

    command_result invalid_move =
        run_capture(gitboard_command(gitboard, root,
                                     {"move", id1,
                                      base64_encode("undefined_status")}));
    expect(invalid_move.exit_code != 0,
           "move to undefined destination status should fail");
    expect(invalid_move.output.find("destination status is not defined") !=
               std::string::npos,
           "undefined destination status error should be explicit");

    command_result invalid_points =
        run_capture(gitboard_command(gitboard, root, {"points", id1, "4"}));
    expect(invalid_points.exit_code != 0,
           "points with unsupported value should fail");
    expect(invalid_points.output.find("points value must be one of") !=
               std::string::npos,
           "invalid points error should be explicit");

    expected.comment = "positive comment";
    require_success(gitboard_command(gitboard, root,
                                     {"comment", id1, base64_encode(expected.comment)}),
                    "comment");
    loaded_task commented = run_task_load(gitboard, root, id1);
    expect_recent_updated_at(commented);
    expect(commented.body.find(expected.comment) != std::string::npos,
           "comment was not written to task body");
    expected.body = commented.body;

    loaded_task final_loaded = run_task_load(gitboard, root, id1);
    compare_loaded_to_expected(final_loaded, expected);

    std::string json_task =
        require_success(gitboard_command(gitboard, root,
                                         {"task", id1, "--json"}),
                        "task json");
    expect(json_task.find("\"id\": \"" + id1 + "\"") != std::string::npos,
           "json task output is missing id");
    expect(json_task.find("\"title\": \"Updated positive title\"") !=
               std::string::npos,
           "json task output is missing title");
    expect(json_task.find("\"tags\": [\"tag-a\", \"tag-b\"]") !=
               std::string::npos,
           "json task output is missing tags");
    expect(json_task.find("\"story_points\": 13") != std::string::npos,
           "json task output is missing story_points");
    expect(json_task.find("\"body\": ") != std::string::npos,
           "json task output is missing body");

    std::string query_output =
        require_success(gitboard_command(gitboard, root,
                                         {"query", "--text", "BODY-VALUE"}),
                        "query text");
    expect(query_output.find("\"" + id1 + "\"") != std::string::npos,
           "query text output is missing matching task");
    expect(query_output.find("\"" + id2 + "\"") == std::string::npos,
           "query text output should not include non-matching task");

    std::string filtered_output =
        require_success(gitboard_command(
                            gitboard, root,
                            {"query", "--status", "in_progress", "--assignee",
                             "assignee-user", "--priority", "urgent", "--tag",
                             "tag-a"}),
                        "query structured filters");
    expect(filtered_output.find("\"" + id1 + "\"") != std::string::npos,
           "query structured filters output is missing matching task");
    expect(filtered_output.find("\"" + id2 + "\"") == std::string::npos,
           "query structured filters should not include non-matching task");

    std::string title_filter_output =
        require_success(gitboard_command(gitboard, root,
                                         {"query", "--title", "positive"}),
                        "query title filter");
    expect(title_filter_output.find("\"" + id1 + "\"") != std::string::npos,
           "query title filter should use case-insensitive substring matching");

    std::string or_filter_output =
        require_success(gitboard_command(
                            gitboard, root,
                            {"query", "--status", "todo", "--status",
                             "in_progress"}),
                        "query repeated filter");
    expect(or_filter_output.find("\"" + id1 + "\"") != std::string::npos &&
               or_filter_output.find("\"" + id2 + "\"") != std::string::npos,
           "query repeated filters should OR values for the same field");

    fs::path batch_path = root / "batch.json";
    write_batch_file(batch_path, id1, id2);
    std::string batch_output =
        require_success(gitboard_command(gitboard, root,
                                         {"batch", batch_path.string()}),
                        "batch");
    expect(batch_output.find("\"ok\": true") != std::string::npos,
           "batch output does not contain successful entries");
    expect(batch_output.find("\"cmd\": \"list\"") != std::string::npos,
           "batch output is missing list result");
    expect(batch_output.find("\"cmd\": \"task\"") != std::string::npos,
           "batch output is missing task result");

    require_success("git -C " + shell_quote(root.string()) + " init", "git init");
    require_success("git -C " + shell_quote(root.string()) +
                        " config user.email positive@example.com",
                    "git config email");
    require_success("git -C " + shell_quote(root.string()) +
                        " config user.name Positive",
                    "git config name");
    require_success("git -C " + shell_quote(root.string()) + " add tasks",
                    "git add baseline");
    require_success("git -C " + shell_quote(root.string()) +
                        " commit -m baseline",
                    "git commit baseline");
    require_success("git init --bare " + shell_quote(remote_root.string()),
                    "git init bare remote");
    require_success("git -C " + shell_quote(root.string()) + " remote add origin " +
                        shell_quote(remote_root.string()),
                    "git remote add origin");
    require_success("git -C " + shell_quote(root.string()) + " push -u origin HEAD",
                    "git push baseline");
    require_success("git clone " + shell_quote(remote_root.string()) + " " +
                        shell_quote(remote_peer_root.string()),
                    "git clone remote peer");
    require_success("git -C " + shell_quote(remote_peer_root.string()) +
                        " config user.email positive@example.com",
                    "git config peer email");
    require_success("git -C " + shell_quote(remote_peer_root.string()) +
                        " config user.name Positive",
                    "git config peer name");
    fs::create_directories(remote_peer_root / "tasks");
    {
      std::ofstream out(remote_peer_root / "tasks" / "TASK-REMOTE_remote-task.md");
      out << "---\n"
          << "assignee: ''\n"
          << "branches: []\n"
          << "ci_status: unknown\n"
          << "created_at: 2026-06-30T00:00:00Z\n"
          << "created_by: ''\n"
          << "id: TASK-REMOTE\n"
          << "priority: medium\n"
          << "prs: []\n"
          << "status: backlog\n"
          << "story_points: 100\n"
          << "tags: []\n"
          << "title: Remote task\n"
          << "updated_at: 2026-06-30T00:00:00Z\n"
          << "---\n\n# Remote task\n";
    }
    require_success("git -C " + shell_quote(remote_peer_root.string()) +
                        " add tasks/TASK-REMOTE_remote-task.md",
                    "git add remote task");
    require_success("git -C " + shell_quote(remote_peer_root.string()) +
                        " commit -m 'remote task'",
                    "git commit remote task");
    require_success("git -C " + shell_quote(remote_peer_root.string()) + " push",
                    "git push remote task");
    std::string remote_status =
        require_success(gitboard_command(gitboard, root, {"remotestatus"}),
                        "remotestatus");
    expect(remote_status.find("\"available\": true") != std::string::npos,
           "remotestatus should report upstream availability:\n" + remote_status);
    expect(remote_status.find("TASK-REMOTE_remote-task.md") != std::string::npos,
           "remotestatus should report remote added task filename:\n" +
               remote_status);

    require_success(gitboard_command(gitboard, root,
                                     {"comment", id1,
                                      base64_encode("git user comment")}),
                    "comment with git user");
    loaded_task git_user_commented = run_task_load(gitboard, root, id1);
    expect(git_user_commented.body.find("Positive: git user comment") !=
               std::string::npos,
           "comment should use repository git user.name");

    auto [id3, file3] = parse_create_output(
        require_success(gitboard_command(gitboard, root,
                                         {"create", base64_encode("Third task")}),
                        "create third task"),
        root);
    (void)file3;

    {
      std::ifstream in(file1);
      std::stringstream buffer;
      buffer << in.rdbuf();
      std::string content = buffer.str();
      replace_first(content, "id: " + id1, "id: TASK-999");
      std::ofstream out(file1, std::ios::binary | std::ios::trunc);
      out << content;
    }
    {
      std::ofstream out(root / "tasks" / "team.json",
                        std::ios::binary | std::ios::trunc);
      out << "{\"team\":[{\"alias\":\"bob\",\"email\":\"bob@example.com\"}]}\n";
    }
    fs::remove(file2);

    std::string dbstatus =
        require_success(gitboard_command(gitboard, root, {"dbstatus"}),
                        "dbstatus");
    expect(dbstatus.find("\"added\": [\"" + id3 + "\"]") != std::string::npos,
           "dbstatus added array is missing created task:\n" + dbstatus);
    expect(dbstatus.find("\"modified\": [\"TASK-999\", \"team.json\"]") !=
               std::string::npos,
           "dbstatus modified array is missing front matter id or json file:\n" +
               dbstatus);
    expect(dbstatus.find("\"deleted\": [\"" + id2 + "\"]") != std::string::npos,
           "dbstatus deleted array is missing filename id:\n" + dbstatus);

    std::string publish =
        require_success(gitboard_command(gitboard, root, {"publish"}),
                        "publish");
    expect(publish.find("Published 4 task database files") != std::string::npos,
           "publish output is missing task database file count:\n" + publish);
    expect(publish.find(id1) != std::string::npos,
           "publish output is missing modified task filename id:\n" + publish);
    expect(publish.find(id2) != std::string::npos,
           "publish output is missing deleted task filename id:\n" + publish);
    expect(publish.find(id3) != std::string::npos,
           "publish output is missing added task filename id:\n" + publish);
    std::string post_publish_status =
        require_success("git -C " + shell_quote(root.string()) +
                            " status --porcelain -uall",
                        "git status after publish");
    expect(post_publish_status.find("tasks/") == std::string::npos,
           "publish should commit task json and markdown changes:\n" +
               post_publish_status);

    fs::remove_all(root);
    fs::remove_all(remote_root);
    fs::remove_all(remote_peer_root);
    std::cout << "positive test passed\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "positive test failed: " << ex.what() << "\n";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::remove_all(remote_root, ignored);
    fs::remove_all(remote_peer_root, ignored);
    return 1;
  }
}

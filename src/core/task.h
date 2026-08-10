#ifndef GITBOARD_CORE_TASK_H_
#define GITBOARD_CORE_TASK_H_

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace gitboard {

struct task {
  std::string id;
  std::string title;
  std::string assignee;
  std::string priority = "medium";
  int story_points = 100;
  std::vector<std::string> tags;
  std::string status = "todo";
  std::string created_at;
  std::string updated_at;
  std::string created_by;
  std::vector<std::string> branches;
  std::vector<std::string> prs;
  std::string ci_status = "unknown";
  std::string body;
  std::map<std::string, std::string> extra_scalars;
  std::map<std::string, std::vector<std::string>> extra_lists;
};

task task_from_content(const std::string& content);
std::string task_to_content(const task& current_task);
std::string default_body(const std::string& title);
std::string extract_filename_id(const std::filesystem::path& path);

}  // namespace gitboard

#endif  // GITBOARD_CORE_TASK_H_

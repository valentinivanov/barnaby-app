#ifndef GITBOARD_CORE_TASK_STORE_H_
#define GITBOARD_CORE_TASK_STORE_H_

#include <filesystem>
#include <string>
#include <vector>

#include "src/core/task.h"

namespace gitboard {

struct task_store {
  std::filesystem::path project_root;
  std::filesystem::path tasks_dir;

  explicit task_store(std::filesystem::path root);

  std::vector<std::filesystem::path> task_files() const;
  std::filesystem::path find_task_path(const std::string& id) const;
  task load(const std::string& id) const;
  void save(const std::filesystem::path& path, const task& current_task) const;
};

}  // namespace gitboard

#endif  // GITBOARD_CORE_TASK_STORE_H_

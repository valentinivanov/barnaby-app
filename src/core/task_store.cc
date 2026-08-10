#include "src/core/task_store.h"

#include <algorithm>

#include "src/core/error.h"
#include "src/core/filesystem.h"
#include "src/core/strings.h"

namespace gitboard {

task_store::task_store(std::filesystem::path root)
    : project_root(std::move(root)), tasks_dir(project_root / "tasks") {}

std::vector<std::filesystem::path> task_store::task_files() const {
  std::vector<std::filesystem::path> files;
  if (!std::filesystem::exists(tasks_dir)) return files;
  for (const auto& entry : std::filesystem::directory_iterator(tasks_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".md") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::filesystem::path task_store::find_task_path(const std::string& id) const {
  for (const auto& path : task_files()) {
    if (starts_with(path.filename().string(), id + "_")) return path;
  }
  throw error("task not found: " + id);
}

task task_store::load(const std::string& id) const {
  return task_from_content(read_file(find_task_path(id)));
}

void task_store::save(const std::filesystem::path& path,
                      const task& current_task) const {
  write_file_atomic(path, task_to_content(current_task));
}

}  // namespace gitboard

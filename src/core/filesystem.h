#ifndef GITBOARD_CORE_FILESYSTEM_H_
#define GITBOARD_CORE_FILESYSTEM_H_

#include <filesystem>
#include <string>

namespace gitboard {

std::string read_file(const std::filesystem::path& path);
void write_file_atomic(const std::filesystem::path& path,
                       const std::string& content);

}  // namespace gitboard

#endif  // GITBOARD_CORE_FILESYSTEM_H_

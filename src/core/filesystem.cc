#include "src/core/filesystem.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "src/core/error.h"

namespace gitboard {
namespace {

std::filesystem::path create_temp_path_for(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto parent = path.parent_path();
  const auto filename = path.filename().wstring();
  for (int i = 0; i < 100; ++i) {
    std::filesystem::path candidate =
        parent / (filename + L".tmp." + std::to_wstring(_getpid()) + L"." +
                  std::to_wstring(GetTickCount64()) + L"." +
                  std::to_wstring(i));
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw error("failed to create temp file for: " + path.string());
#else
  std::string pattern =
      (path.parent_path() / (path.filename().string() + ".tmp.XXXXXX")).string();
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  int fd = mkstemp(buffer.data());
  if (fd < 0) throw error("failed to create temp file for: " + path.string());
  close(fd);
  return buffer.data();
#endif
}

void replace_file(const std::filesystem::path& from,
                  const std::filesystem::path& to) {
#ifdef _WIN32
  if (!MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw error("failed to replace file: " + to.string());
  }
#else
  std::filesystem::rename(from, to);
#endif
}

}  // namespace

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw error("failed to read file: " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file_atomic(const std::filesystem::path& path,
                       const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  const auto tmp = create_temp_path_for(path);
  try {
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) throw error("failed to write file: " + tmp.string());
      out << content;
      out.flush();
      if (!out) throw error("failed while writing file: " + tmp.string());
    }
    replace_file(tmp, path);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    throw;
  }
}

}  // namespace gitboard

#ifndef GITBOARD_SERVER_PROCESS_H_
#define GITBOARD_SERVER_PROCESS_H_

#include <filesystem>
#include <string>
#include <vector>

namespace gitboard::server {

struct process_result {
  int exit_code = 0;
  std::string output;
};

process_result run_process(const std::filesystem::path& executable,
                           const std::vector<std::string>& args,
                           const std::filesystem::path& cwd = {});

void open_url_in_browser(const std::string& url);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_PROCESS_H_

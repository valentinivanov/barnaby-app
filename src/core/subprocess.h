#ifndef GITBOARD_CORE_SUBPROCESS_H_
#define GITBOARD_CORE_SUBPROCESS_H_

#include <string>
#include <vector>

namespace gitboard {

struct process_output {
  int exit_code = 0;
  std::string stdout_text;
  std::string stderr_text;
};

process_output run_capture(const std::vector<std::string>& argv);
void run_checked(const std::vector<std::string>& argv);

}  // namespace gitboard

#endif  // GITBOARD_CORE_SUBPROCESS_H_

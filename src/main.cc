#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "src/commands/commands.h"
#include "src/commands/context.h"
#include "src/core/error.h"
#include "src/core/strings.h"
#include "src/core/task_store.h"

namespace fs = std::filesystem;

namespace {

void usage(std::ostream& out) {
  out << "usage: gitboard [--project-root PATH] COMMAND [ARGS...]\n"
      << "Commands: team, list, query, task, create, body, assignee, branches,\n"
      << "          ci_status, priority, points, prs, tags, title, move, comment,\n"
      << "          publish, dbstatus, remotestatus, sync, statuses, batch\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path project_root = fs::current_path();
    for (auto it = args.begin(); it != args.end();) {
      if (*it == "--project-root") {
        if (std::next(it) == args.end()) {
          throw gitboard::error("--project-root requires PATH");
        }
        project_root = fs::absolute(*std::next(it)).lexically_normal();
        it = args.erase(it, std::next(it, 2));
      } else {
        ++it;
      }
    }
    if (args.empty()) {
      usage(std::cerr);
      return 2;
    }
    std::string cmd = args.front();
    args.erase(args.begin());
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
      usage(std::cout);
      return 0;
    }
    gitboard::context ctx{gitboard::task_store(project_root)};
    std::string output = gitboard::execute(ctx, cmd, args);
    std::cout << output;
    return 0;
  } catch (const gitboard::error& ex) {
    std::string msg = ex.what();
    if (gitboard::starts_with(msg, "{\n  \"batch\"")) {
      std::cout << msg;
    } else {
      std::cerr << "gitboard: " << msg << "\n";
    }
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "gitboard: " << ex.what() << "\n";
    return 1;
  }
}

#include <iostream>
#include <stdexcept>
#include <string>

#include "src/core/error.h"
#include "src/core/task.h"
#include "src/core/time.h"

namespace {

struct failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
  if (!condition) throw failure(message);
}

void expect_contains(const std::string& haystack, const std::string& needle,
                     const std::string& message) {
  if (haystack.find(needle) == std::string::npos) {
    throw failure(message + ": missing '" + needle + "'");
  }
}

void test_unknown_front_matter_round_trip() {
  gitboard::task task = gitboard::task_from_content(R"(---
id: TASK-001
title: Keep custom metadata
status: todo
estimate: 3h
reviewers:
- alice
- bob
---

Body text.
)");

  expect(task.extra_scalars.at("estimate") == "3h",
         "custom scalar should be preserved in parsed task");
  expect(task.extra_lists.at("reviewers").size() == 2,
         "custom list should be preserved in parsed task");

  task.title = "Updated title";
  std::string content = gitboard::task_to_content(task);
  expect_contains(content, "estimate: 3h\n",
                  "custom scalar should be emitted on save");
  expect_contains(content, "reviewers:\n- alice\n- bob\n",
                  "custom list should be emitted on save");
}

void test_yaml_apostrophe_round_trip() {
  gitboard::task task = gitboard::task_from_content(R"(---
id: TASK-002
title: 'Investigate Agent Pip''s task editing'
status: todo
---

Body text.
)");

  expect(task.title == "Investigate Agent Pip's task editing",
         "single-quoted YAML scalar should unescape apostrophes");

  gitboard::task saved_once = gitboard::task_from_content(gitboard::task_to_content(task));
  gitboard::task saved_twice =
      gitboard::task_from_content(gitboard::task_to_content(saved_once));
  expect(saved_once.title == "Investigate Agent Pip's task editing",
         "apostrophe should survive one save/load round trip");
  expect(saved_twice.title == "Investigate Agent Pip's task editing",
         "apostrophe should not be doubled by repeated saves");
}

void test_story_points_default_and_validation() {
  gitboard::task task = gitboard::task_from_content(R"(---
id: TASK-003
title: Missing story points
status: todo
---

Body text.
)");

  expect(task.story_points == 100,
         "missing story_points should default to 100");
  std::string content = gitboard::task_to_content(task);
  expect_contains(content, "story_points: 100\n",
                  "saving should write default story_points");

  gitboard::task estimated = gitboard::task_from_content(R"(---
id: TASK-004
title: Estimated
story_points: 8
status: todo
---

Body text.
)");
  expect(estimated.story_points == 8,
         "valid story_points should be parsed");

  try {
    (void)gitboard::task_from_content(R"(---
id: TASK-005
title: Bad estimate
story_points: 4
status: todo
---

Body text.
)");
    throw failure("invalid story_points should fail");
  } catch (const gitboard::error&) {
  }
}

void test_current_utc_iso_marks_utc() {
  std::string timestamp = gitboard::current_utc_iso();
  expect(!timestamp.empty() && timestamp.back() == 'Z',
         "UTC timestamp should end with Z");
}

}  // namespace

int main() {
  try {
    test_unknown_front_matter_round_trip();
    test_yaml_apostrophe_round_trip();
    test_story_points_default_and_validation();
    test_current_utc_iso_marks_utc();
  } catch (const std::exception& ex) {
    std::cerr << "core_phase2_test: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "core phase 2 tests passed\n";
  return 0;
}

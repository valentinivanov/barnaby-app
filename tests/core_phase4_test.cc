#include <iostream>
#include <stdexcept>
#include <string>

#include "src/core/base64.h"
#include "src/core/strings.h"

namespace {

struct failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void expect_eq(const std::string& actual, const std::string& expected,
               const std::string& message) {
  if (actual != expected) {
    throw failure(message + ": expected '" + expected + "', got '" + actual +
                  "'");
  }
}

void test_base64_round_trip() {
  const std::string input = "phase4 cleanup \x01\x02\x7f";
  expect_eq(gitboard::base64_decode(gitboard::base64_encode(input)), input,
            "base64 round trip");
}

void test_string_helpers() {
  expect_eq(gitboard::trim("  value\t\n"), "value", "trim string_view");
  expect_eq(gitboard::lower_ascii("AbC-123"), "abc-123", "lower ascii");
  expect_eq(gitboard::slugify("Hello, W\xc3\xb6rld!"), "hello-w-rld",
            "ASCII-only slugify");
}

}  // namespace

int main() {
  try {
    test_base64_round_trip();
    test_string_helpers();
  } catch (const std::exception& ex) {
    std::cerr << "core_phase4_test: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "core phase 4 tests passed\n";
  return 0;
}

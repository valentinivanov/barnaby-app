#include "src/core/strings.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace gitboard {

std::string ltrim(std::string_view s) {
  auto it = std::find_if(s.begin(), s.end(), [](unsigned char c) {
    return !std::isspace(c);
  });
  return std::string(it, s.end());
}

std::string rtrim(std::string_view s) {
  auto it = std::find_if(s.rbegin(), s.rend(), [](unsigned char c) {
    return !std::isspace(c);
  }).base();
  return std::string(s.begin(), it);
}

std::string trim(std::string_view s) { return rtrim(ltrim(s)); }

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, delim)) out.push_back(part);
  return out;
}

std::vector<std::string> split_list(const std::string& decoded) {
  std::vector<std::string> out;
  for (auto part : split(decoded, ',')) {
    part = trim(part);
    if (!part.empty()) out.push_back(part);
  }
  return out;
}

std::string json_escape(std::string_view s) {
  std::ostringstream out;
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(c) << std::dec;
        } else {
          out << static_cast<char>(c);
        }
    }
  }
  return out.str();
}

std::string shell_quote(std::string_view s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string slugify(const std::string& title) {
  std::string out;
  bool dash = false;
  for (unsigned char c : title) {
    // Slugs are intentionally ASCII-only; high bytes become separators.
    if (c < 128 && std::isalnum(c)) {
      if (dash && !out.empty()) out.push_back('-');
      out.push_back(static_cast<char>(std::tolower(c)));
      dash = false;
    } else {
      dash = true;
    }
  }
  if (out.empty()) return "task";
  return out;
}

}  // namespace gitboard

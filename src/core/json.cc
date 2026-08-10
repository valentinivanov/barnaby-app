#include "src/core/json.h"

#include <cctype>
#include <exception>
#include <sstream>

#include "src/core/error.h"
#include "src/core/strings.h"

namespace gitboard {
namespace {

constexpr int kMaxJsonDepth = 256;

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void append_utf8(std::string& out, unsigned int codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    throw error("invalid JSON unicode escape");
  }
}

bool high_surrogate(unsigned int codepoint) {
  return codepoint >= 0xD800 && codepoint <= 0xDBFF;
}

bool low_surrogate(unsigned int codepoint) {
  return codepoint >= 0xDC00 && codepoint <= 0xDFFF;
}

}  // namespace

json_parser::json_parser(std::string_view input) : input_(input) {}

json_value json_parser::parse() {
  json_value value = parse_value();
  skip_ws();
  if (pos_ != input_.size()) throw error("unexpected trailing JSON");
  return value;
}

json_value json_parser::parse_value(int depth) {
  if (depth > kMaxJsonDepth) throw error("JSON nesting is too deep");
  skip_ws();
  if (pos_ >= input_.size()) throw error("unexpected end of JSON");
  if (input_[pos_] == '"') {
    json_value v;
    v.type = json_value::k_string;
    v.string = parse_string();
    return v;
  }
  if (input_[pos_] == '[') return parse_array(depth + 1);
  if (input_[pos_] == '{') return parse_object(depth + 1);
  if (input_[pos_] == '-' ||
      std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
    return parse_number();
  }
  if (starts_with(input_.substr(pos_), "true")) {
    pos_ += 4;
    json_value v;
    v.type = json_value::k_bool;
    v.boolean = true;
    return v;
  }
  if (starts_with(input_.substr(pos_), "false")) {
    pos_ += 5;
    json_value v;
    v.type = json_value::k_bool;
    v.boolean = false;
    return v;
  }
  if (starts_with(input_.substr(pos_), "null")) {
    pos_ += 4;
    return json_value{};
  }
  throw error("unsupported JSON value");
}

json_value json_parser::parse_array(int depth) {
  expect('[');
  json_value v;
  v.type = json_value::k_array;
  skip_ws();
  if (consume(']')) return v;
  while (true) {
    v.array.push_back(parse_value(depth));
    skip_ws();
    if (consume(']')) return v;
    expect(',');
  }
}

json_value json_parser::parse_object(int depth) {
  expect('{');
  json_value v;
  v.type = json_value::k_object;
  skip_ws();
  if (consume('}')) return v;
  while (true) {
    skip_ws();
    std::string key = parse_string();
    skip_ws();
    expect(':');
    v.object[key] = parse_value(depth);
    skip_ws();
    if (consume('}')) return v;
    expect(',');
  }
}

json_value json_parser::parse_number() {
  std::size_t start = pos_;
  if (consume('-') && pos_ >= input_.size()) throw error("bad JSON number");
  if (pos_ >= input_.size()) throw error("bad JSON number");
  if (input_[pos_] == '0') {
    ++pos_;
  } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
    while (pos_ < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  } else {
    throw error("bad JSON number");
  }
  if (pos_ < input_.size() && input_[pos_] == '.') {
    ++pos_;
    if (pos_ >= input_.size() ||
        !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      throw error("bad JSON number");
    }
    while (pos_ < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  }
  if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
    ++pos_;
    if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
      ++pos_;
    }
    if (pos_ >= input_.size() ||
        !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      throw error("bad JSON number");
    }
    while (pos_ < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  }
  json_value v;
  v.type = json_value::k_number;
  v.number = std::string(input_.substr(start, pos_ - start));
  return v;
}

std::string json_parser::parse_string() {
  expect('"');
  std::string out;
  while (pos_ < input_.size()) {
    char c = input_[pos_++];
    if (c == '"') return out;
    if (c == '\\') {
      if (pos_ >= input_.size()) throw error("bad JSON escape");
      char e = input_[pos_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          unsigned int codepoint = 0;
          for (int i = 0; i < 4; ++i) {
            if (pos_ >= input_.size()) throw error("bad JSON unicode escape");
            int digit = hex_digit(input_[pos_++]);
            if (digit < 0) throw error("bad JSON unicode escape");
            codepoint = (codepoint << 4) | static_cast<unsigned int>(digit);
          }
          if (high_surrogate(codepoint)) {
            if (pos_ + 6 > input_.size() || input_[pos_] != '\\' ||
                input_[pos_ + 1] != 'u') {
              throw error("missing JSON low surrogate");
            }
            pos_ += 2;
            unsigned int low = 0;
            for (int i = 0; i < 4; ++i) {
              int digit = hex_digit(input_[pos_++]);
              if (digit < 0) throw error("bad JSON unicode escape");
              low = (low << 4) | static_cast<unsigned int>(digit);
            }
            if (!low_surrogate(low)) throw error("bad JSON low surrogate");
            codepoint =
                0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
          } else if (low_surrogate(codepoint)) {
            throw error("unexpected JSON low surrogate");
          }
          append_utf8(out, codepoint);
          break;
        }
        default:
          throw error("unsupported JSON escape");
      }
    } else {
      out.push_back(c);
    }
  }
  throw error("unterminated JSON string");
}

void json_parser::skip_ws() {
  while (pos_ < input_.size() &&
         std::isspace(static_cast<unsigned char>(input_[pos_]))) {
    ++pos_;
  }
}

void json_parser::expect(char c) {
  skip_ws();
  if (pos_ >= input_.size() || input_[pos_] != c) {
    throw error(std::string("expected JSON character: ") + c);
  }
  ++pos_;
}

bool json_parser::consume(char c) {
  skip_ws();
  if (pos_ < input_.size() && input_[pos_] == c) {
    ++pos_;
    return true;
  }
  return false;
}

std::string json_quote(std::string_view s) {
  return "\"" + json_escape(s) + "\"";
}

std::string stringify_json(const json_value& value) {
  switch (value.type) {
    case json_value::k_null:
      return "null";
    case json_value::k_bool:
      return value.boolean ? "true" : "false";
    case json_value::k_string:
      return json_quote(value.string);
    case json_value::k_number:
      return value.number;
    case json_value::k_array: {
      std::string out = "[";
      bool first = true;
      for (const auto& item : value.array) {
        if (!first) out += ",";
        first = false;
        out += stringify_json(item);
      }
      out += "]";
      return out;
    }
    case json_value::k_object: {
      std::string out = "{";
      bool first = true;
      for (const auto& [key, item] : value.object) {
        if (!first) out += ",";
        first = false;
        out += json_quote(key) + ":" + stringify_json(item);
      }
      out += "}";
      return out;
    }
    default:
      throw error("unsupported JSON value type");
  }
}

std::string json_string_member(const json_value& object,
                               const std::string& name) {
  if (object.type != json_value::k_object) return "";
  auto it = object.object.find(name);
  if (it == object.object.end() || it->second.type != json_value::k_string) {
    return "";
  }
  return it->second.string;
}

int json_int_member(const json_value& object, const std::string& name) {
  if (object.type != json_value::k_object) return 0;
  auto it = object.object.find(name);
  if (it == object.object.end()) return 0;
  if (it->second.type != json_value::k_number) {
    throw error(name + " must be a number");
  }
  std::size_t consumed = 0;
  int value = 0;
  try {
    value = std::stoi(it->second.number, &consumed);
  } catch (const std::exception&) {
    throw error(name + " must be an integer");
  }
  if (consumed != it->second.number.size()) {
    throw error(name + " must be an integer");
  }
  return value;
}

bool json_bool_member_or_default(const json_value& object,
                                 const std::string& name,
                                 bool fallback) {
  if (object.type != json_value::k_object) return fallback;
  auto it = object.object.find(name);
  if (it == object.object.end()) return fallback;
  if (it->second.type != json_value::k_bool) {
    throw error(name + " must be a boolean");
  }
  return it->second.boolean;
}

std::string json_array_string_member(const json_value& object,
                                     const std::string& name) {
  if (object.type != json_value::k_object) return "[]";
  auto it = object.object.find(name);
  if (it == object.object.end() || it->second.type != json_value::k_array) {
    return "[]";
  }
  std::ostringstream out;
  out << "[";
  bool first = true;
  for (const auto& item : it->second.array) {
    if (item.type != json_value::k_string) continue;
    std::string value = trim(item.string);
    if (value.empty() || value.size() > 80) continue;
    if (!first) out << ",";
    first = false;
    out << json_quote(value);
  }
  out << "]";
  return out.str();
}

std::optional<json_value> extract_json_object_from_text(std::string text) {
  text = trim(text);
  if (text.empty()) return std::nullopt;
  if (text.front() == '{') {
    try {
      json_value parsed = json_parser(text).parse();
      if (parsed.type == json_value::k_object) return parsed;
    } catch (const std::exception&) {
    }
  }

  std::size_t fence = text.find("```");
  while (fence != std::string::npos) {
    std::size_t start = text.find('\n', fence);
    if (start == std::string::npos) break;
    std::size_t end = text.find("```", start + 1);
    if (end == std::string::npos) break;
    std::string block = trim(text.substr(start + 1, end - start - 1));
    if (!block.empty() && block.front() == '{') {
      try {
        json_value parsed = json_parser(block).parse();
        if (parsed.type == json_value::k_object) return parsed;
      } catch (const std::exception&) {
      }
    }
    fence = text.find("```", end + 3);
  }
  return std::nullopt;
}

}  // namespace gitboard

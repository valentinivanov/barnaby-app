#include "src/core/base64.h"

#include <array>
#include <cctype>

#include "src/core/error.h"

namespace gitboard {
namespace {

constexpr std::array<int, 256> make_decode_table() {
  std::array<int, 256> table{};
  for (int& value : table) value = -1;
  constexpr char chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (int i = 0; i < 64; ++i) {
    table[static_cast<unsigned char>(chars[i])] = i;
  }
  return table;
}

}  // namespace

std::string base64_encode(const std::string& input) {
  static constexpr char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  unsigned int val = 0;
  int valb = -6;
  for (unsigned char c : input) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string base64_decode(const std::string& input) {
  static constexpr std::array<int, 256> table = make_decode_table();
  std::string out;
  unsigned int val = 0;
  int valb = -8;
  for (unsigned char c : input) {
    if (std::isspace(c)) continue;
    if (c == '=') break;
    if (table[c] == -1) throw error("invalid Base64 input");
    val = (val << 6) + table[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

}  // namespace gitboard

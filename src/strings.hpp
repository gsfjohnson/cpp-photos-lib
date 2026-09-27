// Internal: locale-independent text helpers.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace photos::detail {

inline bool isSpace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
inline bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

inline std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
  return s;
}

inline std::vector<std::string> splitWhitespace(std::string_view s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isSpace(s[i])) ++i;
    const std::size_t start = i;
    while (i < s.size() && !isSpace(s[i])) ++i;
    if (i > start) out.emplace_back(s.substr(start, i - start));
  }
  return out;
}

// A decimal (or 0x-prefixed hexadecimal) integer, the whole string.
inline std::optional<std::int64_t> parseInt(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  bool negative = false;
  if (s[0] == '-' || s[0] == '+') {
    negative = s[0] == '-';
    s.remove_prefix(1);
  }
  int base = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s.remove_prefix(2);
  }
  if (s.empty() || s.size() > 18) return std::nullopt;
  std::int64_t v = 0;
  for (char c : s) {
    int d;
    if (isDigit(c))
      d = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F')
      d = c - 'A' + 10;
    else
      return std::nullopt;
    v = v * base + d;
  }
  return negative ? -v : v;
}

inline std::optional<double> parseDouble(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  std::istringstream is{std::string(s)};
  is.imbue(std::locale::classic());
  double d;
  is >> d;
  if (is.fail()) return std::nullopt;
  is >> std::ws;
  if (!is.eof()) return std::nullopt;
  return d;
}

inline bool isValidUtf8(std::string_view s) noexcept {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t n;
    std::uint32_t cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xe0) == 0xc0) {
      n = 1;
      cp = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
      n = 2;
      cp = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
      n = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    for (std::size_t k = 1; k <= n; ++k) {
      if (i + k >= s.size()) return false;
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3f);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) {
      return false;
    }
    i += n + 1;
  }
  return true;
}

inline std::string latin1ToUtf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x80) {
      out += ch;
    } else {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3f));
    }
  }
  return out;
}

inline bool isAscii(std::string_view s) noexcept {
  for (char c : s) {
    if (static_cast<unsigned char>(c) >= 0x80) return false;
  }
  return true;
}

inline std::string hex4(unsigned v) {
  static const char digits[] = "0123456789abcdef";
  std::string s = "0x0000";
  for (int i = 0; i < 4; ++i) s[5 - i] = digits[(v >> (4 * i)) & 0xf];
  return s;
}

inline std::string hex2(unsigned v) {
  static const char digits[] = "0123456789abcdef";
  std::string s = "0x00";
  s[2] = digits[(v >> 4) & 0xf];
  s[3] = digits[v & 0xf];
  return s;
}

}  // namespace photos::detail

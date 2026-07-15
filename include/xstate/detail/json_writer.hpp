#pragma once

#include <string>
#include <string_view>

namespace xstate {
namespace detail {

inline void jsonEscapeInto(std::string& out, std::string_view s) {
  for (char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[(ch >> 4) & 0xF];
          out += hex[ch & 0xF];
        } else {
          out += ch;
        }
    }
  }
}

inline std::string jsonString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  jsonEscapeInto(out, s);
  out += '"';
  return out;
}

}  // namespace detail
}  // namespace xstate

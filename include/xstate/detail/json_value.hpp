#pragma once

#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../errors.hpp"
#include "json_writer.hpp"

namespace xstate {
namespace detail {

// Minimal self-contained JSON document model + recursive-descent parser.
// Only what machine configs and snapshots need; throws JsonError with the
// byte offset and line on malformed input.
struct JsonValue {
  enum class Type { Null, Bool, Number, String, Object, Array };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string str;
  std::vector<std::pair<std::string, JsonValue>> object;  // preserves order
  std::vector<JsonValue> array;

  bool isNull() const { return type == Type::Null; }
  bool isString() const { return type == Type::String; }
  bool isNumber() const { return type == Type::Number; }
  bool isObject() const { return type == Type::Object; }
  bool isArray() const { return type == Type::Array; }

  const JsonValue* find(std::string_view key) const {
    for (const auto& [k, v] : object)
      if (k == key) return &v;
    return nullptr;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view src) : src_(src) {}

  JsonValue parse() {
    JsonValue v = parseValue();
    skipWs();
    if (pos_ != src_.size()) fail("trailing characters after JSON value");
    return v;
  }

 private:
  std::string_view src_;
  std::size_t pos_ = 0;

  [[noreturn]] void fail(const std::string& what) const {
    std::size_t line = 1;
    for (std::size_t i = 0; i < pos_ && i < src_.size(); ++i)
      if (src_[i] == '\n') ++line;
    throw JsonError("JSON error at line " + std::to_string(line) + " (offset " +
                    std::to_string(pos_) + "): " + what);
  }

  void skipWs() {
    while (pos_ < src_.size() &&
           (src_[pos_] == ' ' || src_[pos_] == '\t' || src_[pos_] == '\n' || src_[pos_] == '\r'))
      ++pos_;
  }

  char peek() {
    if (pos_ >= src_.size()) fail("unexpected end of input");
    return src_[pos_];
  }

  void expect(char c) {
    if (peek() != c) fail(std::string("expected '") + c + "'");
    ++pos_;
  }

  bool consume(std::string_view word) {
    if (src_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  JsonValue parseValue() {
    skipWs();
    switch (peek()) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': {
        JsonValue v;
        v.type = JsonValue::Type::String;
        v.str = parseString();
        return v;
      }
      case 't':
        if (!consume("true")) fail("invalid literal");
        return makeBool(true);
      case 'f':
        if (!consume("false")) fail("invalid literal");
        return makeBool(false);
      case 'n':
        if (!consume("null")) fail("invalid literal");
        return JsonValue{};
      default: return parseNumber();
    }
  }

  static JsonValue makeBool(bool b) {
    JsonValue v;
    v.type = JsonValue::Type::Bool;
    v.boolean = b;
    return v;
  }

  JsonValue parseObject() {
    expect('{');
    JsonValue v;
    v.type = JsonValue::Type::Object;
    skipWs();
    if (peek() == '}') {
      ++pos_;
      return v;
    }
    while (true) {
      skipWs();
      std::string key = parseString();
      skipWs();
      expect(':');
      v.object.emplace_back(std::move(key), parseValue());
      skipWs();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      expect('}');
      return v;
    }
  }

  JsonValue parseArray() {
    expect('[');
    JsonValue v;
    v.type = JsonValue::Type::Array;
    skipWs();
    if (peek() == ']') {
      ++pos_;
      return v;
    }
    while (true) {
      v.array.push_back(parseValue());
      skipWs();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      expect(']');
      return v;
    }
  }

  std::string parseString() {
    expect('"');
    std::string out;
    while (true) {
      if (pos_ >= src_.size()) fail("unterminated string");
      char c = src_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= src_.size()) fail("unterminated escape");
      char esc = src_[pos_++];
      switch (esc) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (pos_ + 4 > src_.size()) fail("bad \\u escape");
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            char h = src_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else fail("bad \\u escape");
          }
          // UTF-8 encode (BMP only; surrogate pairs unsupported in v1 configs)
          if (code < 0x80) {
            out += static_cast<char>(code);
          } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          }
          break;
        }
        default: fail("unknown escape");
      }
    }
  }

  JsonValue parseNumber() {
    std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    while (pos_ < src_.size() && ((src_[pos_] >= '0' && src_[pos_] <= '9') || src_[pos_] == '.' ||
                                  src_[pos_] == 'e' || src_[pos_] == 'E' || src_[pos_] == '+' ||
                                  src_[pos_] == '-'))
      ++pos_;
    if (pos_ == start || (pos_ == start + 1 && src_[start] == '-')) fail("invalid number");
    JsonValue v;
    v.type = JsonValue::Type::Number;
    try {
      v.number = std::stod(std::string(src_.substr(start, pos_ - start)));
    } catch (...) {
      fail("invalid number");
    }
    return v;
  }
};

inline JsonValue parseJson(std::string_view src) { return JsonParser(src).parse(); }

inline std::string writeJson(const JsonValue& v) {
  switch (v.type) {
    case JsonValue::Type::Null: return "null";
    case JsonValue::Type::Bool: return v.boolean ? "true" : "false";
    case JsonValue::Type::Number: {
      if (v.number == std::floor(v.number) && std::abs(v.number) < 1e15) {
        return std::to_string(static_cast<long long>(v.number));
      }
      return std::to_string(v.number);
    }
    case JsonValue::Type::String: return jsonString(v.str);
    case JsonValue::Type::Object: {
      std::string out = "{";
      bool first = true;
      for (const auto& [k, val] : v.object) {
        if (!first) out += ",";
        first = false;
        out += jsonString(k) + ":" + writeJson(val);
      }
      return out + "}";
    }
    case JsonValue::Type::Array: {
      std::string out = "[";
      bool first = true;
      for (const auto& item : v.array) {
        if (!first) out += ",";
        first = false;
        out += writeJson(item);
      }
      return out + "]";
    }
  }
  return "null";
}

}  // namespace detail
}  // namespace xstate

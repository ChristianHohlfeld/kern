#include "kern/core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kern::json {

namespace {
const Value kNull;
const std::string kEmpty;
}  // namespace

const std::string& Value::as_string() const { return type_ == Type::string ? string_ : kEmpty; }

bool Value::truthy() const {
  switch (type_) {
    case Type::null: return false;
    case Type::boolean: return bool_;
    case Type::number: return number_ != 0 && !std::isnan(number_);
    case Type::string: return !string_.empty();
    default: return true;
  }
}

const Value& Value::operator[](std::size_t i) const { return type_ == Type::array && i < items_.size() ? items_[i] : kNull; }

void Value::push(Value v) {
  if (type_ != Type::array) { *this = array(); }
  items_.push_back(std::move(v));
}

const Value* Value::find(std::string_view key) const {
  if (type_ != Type::object) return nullptr;
  for (const Member& m : members_)
    if (m.first == key) return &m.second;
  return nullptr;
}

Value* Value::find(std::string_view key) {
  if (type_ != Type::object) return nullptr;
  for (Member& m : members_)
    if (m.first == key) return &m.second;
  return nullptr;
}

const Value& Value::operator[](std::string_view key) const {
  const Value* v = find(key);
  return v ? *v : kNull;
}

Value& Value::set(std::string_view key, Value v) {
  if (type_ != Type::object) *this = object();
  for (Member& m : members_)
    if (m.first == key) { m.second = std::move(v); return m.second; }
  members_.emplace_back(std::string(key), std::move(v));
  return members_.back().second;
}

bool Value::remove(std::string_view key) {
  for (std::size_t i = 0; i < members_.size(); ++i)
    if (members_[i].first == key) { members_.erase(members_.begin() + static_cast<std::ptrdiff_t>(i)); return true; }
  return false;
}

bool Value::operator==(const Value& o) const {
  if (type_ != o.type_) return false;
  switch (type_) {
    case Type::null: return true;
    case Type::boolean: return bool_ == o.bool_;
    case Type::number: return number_ == o.number_;
    case Type::string: return string_ == o.string_;
    case Type::array: return items_ == o.items_;
    case Type::object:
      if (members_.size() != o.members_.size()) return false;
      for (std::size_t i = 0; i < members_.size(); ++i)
        if (members_[i].first != o.members_[i].first || members_[i].second != o.members_[i].second) return false;
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// parser

namespace {

struct Parser {
  std::string_view s;
  std::size_t i = 0;
  const ParseOptions& opt;
  std::string error;

  bool fail_at(const char* what) {
    if (error.empty()) {
      char buf[160];
      std::snprintf(buf, sizeof buf, "%s at offset %zu", what, i);
      error = buf;
    }
    return false;
  }
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  static void utf8(std::string& out, u32 cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(u32& out) {
    if (i + 4 > s.size()) return fail_at("truncated \\u escape");
    out = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s[i++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= static_cast<u32>(c - '0');
      else if (c >= 'a' && c <= 'f') out |= static_cast<u32>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= static_cast<u32>(c - 'A' + 10);
      else return fail_at("invalid \\u escape");
    }
    return true;
  }
  bool string(std::string& out) {
    ++i;  // opening quote
    while (true) {
      if (i >= s.size()) return fail_at("unterminated string");
      const char c = s[i];
      if (c == '"') { ++i; return true; }
      if (static_cast<unsigned char>(c) < 0x20) return fail_at("control character in string");
      if (c != '\\') {
        // Copy a run of plain bytes at once.
        const std::size_t start = i;
        while (i < s.size() && s[i] != '"' && s[i] != '\\' && static_cast<unsigned char>(s[i]) >= 0x20) ++i;
        out.append(s.data() + start, i - start);
        continue;
      }
      ++i;
      if (i >= s.size()) return fail_at("unterminated escape");
      const char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          u32 cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            const std::size_t save = i;
            i += 2;
            u32 lo = 0;
            if (!hex4(lo)) return false;
            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else i = save;
          }
          if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // lone surrogate
          utf8(out, cp);
          break;
        }
        default: return fail_at("invalid escape");
      }
    }
  }
  bool number(f64& out) {
    const std::size_t start = i;
    if (s[i] == '-') ++i;
    if (i >= s.size()) return fail_at("invalid number");
    if (s[i] == '0') ++i;
    else if (s[i] >= '1' && s[i] <= '9') { while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; }
    else return fail_at("invalid number");
    if (i < s.size() && s[i] == '.') {
      ++i;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail_at("invalid fraction");
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      ++i;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail_at("invalid exponent");
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    char buf[64];
    const std::size_t n = i - start;
    if (n >= sizeof buf) {
      std::string tmp(s.substr(start, n));
      out = std::strtod(tmp.c_str(), nullptr);
    } else {
      std::memcpy(buf, s.data() + start, n);
      buf[n] = 0;
      out = std::strtod(buf, nullptr);
    }
    if (!std::isfinite(out)) return fail_at("non-finite number");
    return true;
  }
  bool literal(const char* word) {
    const std::size_t n = std::strlen(word);
    if (s.substr(i, n) != word) return fail_at("invalid literal");
    i += n;
    return true;
  }
  bool value(Value& out, u32 depth) {
    if (depth > opt.max_depth) return fail_at("nesting limit exceeded");
    ws();
    if (i >= s.size()) return fail_at("unexpected end of input");
    const char c = s[i];
    if (c == '{') {
      ++i;
      out = Value::object();
      ws();
      if (i < s.size() && s[i] == '}') { ++i; return true; }
      while (true) {
        ws();
        if (i >= s.size() || s[i] != '"') return fail_at("expected a key");
        std::string key;
        if (!string(key)) return false;
        if (opt.reject_unsafe_keys && (key == "__proto__" || key == "prototype" || key == "constructor")) return fail_at("unsafe property name");
        ws();
        if (i >= s.size() || s[i] != ':') return fail_at("expected ':'");
        ++i;
        Value v;
        if (!value(v, depth + 1)) return false;
        // Duplicate keys: the last one wins (as JSON.parse).
        out.set(key, std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        return fail_at("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i;
      out = Value::array();
      ws();
      if (i < s.size() && s[i] == ']') { ++i; return true; }
      while (true) {
        Value v;
        if (!value(v, depth + 1)) return false;
        out.items().push_back(std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        return fail_at("expected ',' or ']'");
      }
    }
    if (c == '"') {
      std::string str;
      if (!string(str)) return false;
      out = Value(std::move(str));
      return true;
    }
    if (c == 't') { if (!literal("true")) return false; out = Value(true); return true; }
    if (c == 'f') { if (!literal("false")) return false; out = Value(false); return true; }
    if (c == 'n') { if (!literal("null")) return false; out = Value(); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      f64 n = 0;
      if (!number(n)) return false;
      out = Value(n);
      return true;
    }
    return fail_at("unexpected character");
  }
};

}  // namespace

bool parse(std::string_view text, Value& out, std::string* error, const ParseOptions& options) {
  Parser p{text, 0, options, {}};
  // A UTF-8 byte order mark is tolerated.
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    p.i = 3;
  Value v;
  bool ok = p.value(v, 0);
  if (ok) {
    p.ws();
    if (p.i != text.size()) ok = p.fail_at("trailing characters");
  }
  if (!ok) {
    if (error) *error = p.error;
    return false;
  }
  out = std::move(v);
  return true;
}

// ---------------------------------------------------------------------------
// writer

void append_number(std::string& out, f64 x) {
  if (!std::isfinite(x)) { out += "null"; return; }
  if (x == 0) { out += '0'; return; }  // also -0, as JavaScript
  // Shortest digits that round-trip.
  char buf[40];
  int precision = 1;
  for (; precision <= 17; ++precision) {
    std::snprintf(buf, sizeof buf, "%.*e", precision - 1, x);
    if (std::strtod(buf, nullptr) == x) break;
  }
  // buf: [-]d.ddde[+-]xx
  const char* p = buf;
  bool negative = false;
  if (*p == '-') { negative = true; ++p; }
  std::string digits;
  for (; *p && *p != 'e'; ++p)
    if (*p != '.') digits += *p;
  const int exponent = std::atoi(p + 1);
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  const int k = static_cast<int>(digits.size());
  const int n = exponent + 1;  // decimal point position
  if (negative) out += '-';
  if (k <= n && n <= 21) {
    out += digits;
    out.append(static_cast<std::size_t>(n - k), '0');
  } else if (0 < n && n <= 21) {
    out.append(digits, 0, static_cast<std::size_t>(n));
    out += '.';
    out.append(digits, static_cast<std::size_t>(n), std::string::npos);
  } else if (-6 < n && n <= 0) {
    out += "0.";
    out.append(static_cast<std::size_t>(-n), '0');
    out += digits;
  } else {
    out += digits[0];
    if (k > 1) {
      out += '.';
      out.append(digits, 1, std::string::npos);
    }
    out += 'e';
    out += n - 1 >= 0 ? '+' : '-';
    out += std::to_string(std::abs(n - 1));
  }
}

void append_string(std::string& out, std::string_view text) {
  static const char* hex = "0123456789abcdef";
  out += '"';
  for (std::size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

namespace {

void write(std::string& out, const Value& v, i32 indent, i32 depth) {
  auto newline = [&](i32 d) {
    if (indent <= 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent * d), ' ');
  };
  switch (v.type()) {
    case Type::null: out += "null"; break;
    case Type::boolean: out += v.as_bool() ? "true" : "false"; break;
    case Type::number: append_number(out, v.as_number()); break;
    case Type::string: append_string(out, v.as_string()); break;
    case Type::array: {
      if (v.items().empty()) { out += "[]"; break; }
      out += '[';
      bool first = true;
      for (const Value& item : v.items()) {
        if (!first) out += ',';
        first = false;
        newline(depth + 1);
        write(out, item, indent, depth + 1);
      }
      newline(depth);
      out += ']';
      break;
    }
    case Type::object: {
      if (v.members().empty()) { out += "{}"; break; }
      out += '{';
      bool first = true;
      for (const Value::Member& m : v.members()) {
        if (!first) out += ',';
        first = false;
        newline(depth + 1);
        append_string(out, m.first);
        out += indent <= 0 ? ":" : ": ";
        write(out, m.second, indent, depth + 1);
      }
      newline(depth);
      out += '}';
      break;
    }
  }
}

}  // namespace

std::string stringify(const Value& value, i32 indent) {
  std::string out;
  write(out, value, indent > 10 ? 10 : indent, 0);
  return out;
}

}  // namespace kern::json

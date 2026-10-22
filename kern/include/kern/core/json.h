// JSON document model, parser and writer.
//
// - Objects keep their key order (documents round-trip byte for byte through
//   parse + stringify when they were written by this writer or by JavaScript's
//   JSON.stringify).
// - Numbers are IEEE doubles; stringify uses the ECMAScript Number-to-String
//   algorithm (shortest round-trip digits, same exponent rules), so output
//   matches JSON.stringify.
// - The parser rejects non-finite results, nesting deeper than max_depth and,
//   by default, the prototype-polluting keys "__proto__", "prototype" and
//   "constructor" (documents usually come from untrusted files).
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kern/core/base.h"

namespace kern::json {

enum class Type : u8 { null, boolean, number, string, array, object };

class Value {
 public:
  using Member = std::pair<std::string, Value>;

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : type_(Type::boolean), bool_(b) {}
  Value(f64 n) : type_(Type::number), number_(n) {}
  Value(i32 n) : type_(Type::number), number_(n) {}
  Value(u32 n) : type_(Type::number), number_(n) {}
  Value(i64 n) : type_(Type::number), number_(static_cast<f64>(n)) {}
  Value(const char* s) : type_(Type::string), string_(s ? s : "") {}
  Value(std::string s) : type_(Type::string), string_(std::move(s)) {}
  Value(std::string_view s) : type_(Type::string), string_(s) {}

  static Value array() { Value v; v.type_ = Type::array; return v; }
  static Value object() { Value v; v.type_ = Type::object; return v; }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::null; }
  bool is_bool() const { return type_ == Type::boolean; }
  bool is_number() const { return type_ == Type::number; }
  bool is_string() const { return type_ == Type::string; }
  bool is_array() const { return type_ == Type::array; }
  bool is_object() const { return type_ == Type::object; }

  bool as_bool(bool fallback = false) const { return type_ == Type::boolean ? bool_ : fallback; }
  f64 as_number(f64 fallback = 0) const { return type_ == Type::number ? number_ : fallback; }
  const std::string& as_string() const;
  /// JavaScript truthiness (false, 0, NaN, "", null are falsy).
  bool truthy() const;

  // arrays
  std::size_t size() const { return type_ == Type::array ? items_.size() : type_ == Type::object ? members_.size() : 0; }
  const Value& operator[](std::size_t i) const;
  Value& at(std::size_t i) { return items_[i]; }
  void push(Value v);
  const std::vector<Value>& items() const { return items_; }
  std::vector<Value>& items() { return items_; }

  // objects
  const Value* find(std::string_view key) const;
  Value* find(std::string_view key);
  bool has(std::string_view key) const { return find(key) != nullptr; }
  /// Member value or null.
  const Value& operator[](std::string_view key) const;
  /// Sets (replacing in place, keeping the key's position) or appends a member.
  Value& set(std::string_view key, Value v);
  bool remove(std::string_view key);
  const std::vector<Member>& members() const { return members_; }
  std::vector<Member>& members() { return members_; }

  bool operator==(const Value& o) const;
  bool operator!=(const Value& o) const { return !(*this == o); }

 private:
  Type type_ = Type::null;
  bool bool_ = false;
  f64 number_ = 0;
  std::string string_;
  std::vector<Value> items_;
  std::vector<Member> members_;
};

struct ParseOptions {
  u32 max_depth = 80;
  bool reject_unsafe_keys = true;
};

/// Parses `text`; on failure returns false and describes the problem (with offset) in `error`.
bool parse(std::string_view text, Value& out, std::string* error = nullptr, const ParseOptions& options = {});

/// Serialises like JSON.stringify(value, null, indent); indent <= 0 writes compact JSON.
std::string stringify(const Value& value, i32 indent = -1);

/// Appends an ECMAScript Number::toString rendering (non-finite numbers become "null", as in JSON).
void append_number(std::string& out, f64 value);
/// Appends a JSON string literal (quotes and escapes as JSON.stringify).
void append_string(std::string& out, std::string_view text);

}  // namespace kern::json

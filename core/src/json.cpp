#include "rommsync/json.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rommsync::json {
namespace {

const std::string& EmptyString() {
  static const std::string kEmpty;
  return kEmpty;
}

const std::vector<Value>& EmptyElements() {
  static const std::vector<Value> kEmpty;
  return kEmpty;
}

const std::vector<Member>& EmptyMembers() {
  static const std::vector<Member> kEmpty;
  return kEmpty;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

/// Append one code point as UTF-8. Only ever called with a scalar value, so the
/// surrogate range is already excluded by the caller.
void AppendUtf8(std::uint32_t code_point, std::string* out) {
  if (code_point < 0x80) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

}  // namespace

const char* ToString(Type type) {
  switch (type) {
    case Type::kNull:
      return "a null";
    case Type::kBool:
      return "a bool";
    case Type::kNumber:
      return "a number";
    case Type::kString:
      return "a string";
    case Type::kArray:
      return "an array";
    case Type::kObject:
      return "an object";
  }
  return "a value";
}

const std::string& Value::string() const {
  return type_ == Type::kString ? string_ : EmptyString();
}

const std::vector<Value>& Value::elements() const {
  return type_ == Type::kArray ? elements_ : EmptyElements();
}

const std::vector<Member>& Value::members() const {
  return type_ == Type::kObject ? members_ : EmptyMembers();
}

std::size_t Value::size() const {
  if (type_ == Type::kArray) {
    return elements_.size();
  }
  if (type_ == Type::kObject) {
    return members_.size();
  }
  return 0;
}

const Value* Value::Find(std::string_view key) const {
  if (type_ != Type::kObject) {
    return nullptr;
  }
  for (const Member& member : members_) {
    if (member.key == key) {
      return &member.value;
    }
  }
  return nullptr;
}

std::string Error::Describe() const {
  if (ok()) {
    return {};
  }
  if (!field.empty()) {
    return "field " + field + ": " + message;
  }
  return "at offset " + std::to_string(offset) + ": " + message;
}

/// Recursive descent over the whole text. Every failure path sets `error_` and
/// unwinds; nothing here reports a position it did not actually reach.
class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  ParseResult Run() {
    ParseResult result;
    SkipWhitespace();
    if (at_ >= text_.size()) {
      Fail("empty body");
      result.error = error_;
      return result;
    }
    Value root;
    if (!ParseValue(0, &root)) {
      result.error = error_;
      return result;
    }
    SkipWhitespace();
    if (at_ < text_.size()) {
      Fail("trailing content after the document");
      result.error = error_;
      return result;
    }
    result.value = std::move(root);
    return result;
  }

 private:
  bool Fail(std::string message) {
    if (error_.ok()) {
      error_.offset = at_;
      error_.message = std::move(message);
    }
    return false;
  }

  void SkipWhitespace() {
    while (at_ < text_.size()) {
      const char c = text_[at_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++at_;
      } else {
        break;
      }
    }
  }

  bool Literal(std::string_view word) {
    if (text_.compare(at_, word.size(), word) != 0) {
      return Fail("not a JSON value");
    }
    at_ += word.size();
    return true;
  }

  bool ParseValue(int depth, Value* out) {
    if (depth >= kMaxDepth) {
      return Fail("nested deeper than " + std::to_string(kMaxDepth) + " levels");
    }
    if (at_ >= text_.size()) {
      return Fail("value expected, body ended");
    }
    switch (text_[at_]) {
      case 'n':
        if (!Literal("null")) {
          return false;
        }
        out->type_ = Type::kNull;
        return true;
      case 't':
        if (!Literal("true")) {
          return false;
        }
        out->type_ = Type::kBool;
        out->bool_ = true;
        return true;
      case 'f':
        if (!Literal("false")) {
          return false;
        }
        out->type_ = Type::kBool;
        out->bool_ = false;
        return true;
      case '"':
        out->type_ = Type::kString;
        return ParseString(&out->string_);
      case '[':
        return ParseArray(depth, out);
      case '{':
        return ParseObject(depth, out);
      default:
        return ParseNumber(out);
    }
  }

  bool ParseArray(int depth, Value* out) {
    out->type_ = Type::kArray;
    ++at_;  // '['
    SkipWhitespace();
    if (at_ < text_.size() && text_[at_] == ']') {
      ++at_;
      return true;
    }
    for (;;) {
      SkipWhitespace();
      Value element;
      if (!ParseValue(depth + 1, &element)) {
        return false;
      }
      out->elements_.push_back(std::move(element));
      SkipWhitespace();
      if (at_ >= text_.size()) {
        return Fail("unterminated array");
      }
      if (text_[at_] == ',') {
        ++at_;
        continue;
      }
      if (text_[at_] == ']') {
        ++at_;
        return true;
      }
      return Fail("expected ',' or ']' in an array");
    }
  }

  bool ParseObject(int depth, Value* out) {
    out->type_ = Type::kObject;
    ++at_;  // '{'
    SkipWhitespace();
    if (at_ < text_.size() && text_[at_] == '}') {
      ++at_;
      return true;
    }
    for (;;) {
      SkipWhitespace();
      if (at_ >= text_.size() || text_[at_] != '"') {
        return Fail("expected a quoted key in an object");
      }
      Member member;
      const std::size_t key_at = at_;
      if (!ParseString(&member.key)) {
        return false;
      }
      for (const Member& seen : out->members_) {
        if (seen.key == member.key) {
          // Which of the two won would be a parser detail, and the caller would
          // never learn one had been dropped.
          at_ = key_at;
          return Fail("duplicate key in an object");
        }
      }
      SkipWhitespace();
      if (at_ >= text_.size() || text_[at_] != ':') {
        return Fail("expected ':' after an object key");
      }
      ++at_;
      SkipWhitespace();
      if (!ParseValue(depth + 1, &member.value)) {
        return false;
      }
      out->members_.push_back(std::move(member));
      SkipWhitespace();
      if (at_ >= text_.size()) {
        return Fail("unterminated object");
      }
      if (text_[at_] == ',') {
        ++at_;
        continue;
      }
      if (text_[at_] == '}') {
        ++at_;
        return true;
      }
      return Fail("expected ',' or '}' in an object");
    }
  }

  bool ParseString(std::string* out) {
    ++at_;  // '"'
    out->clear();
    for (;;) {
      if (at_ >= text_.size()) {
        return Fail("unterminated string");
      }
      const unsigned char c = static_cast<unsigned char>(text_[at_]);
      if (c == '"') {
        ++at_;
        return true;
      }
      if (c < 0x20) {
        return Fail("unescaped control character in a string");
      }
      if (c != '\\') {
        out->push_back(text_[at_]);
        ++at_;
        continue;
      }
      ++at_;
      if (at_ >= text_.size()) {
        return Fail("unterminated escape in a string");
      }
      const char escape = text_[at_];
      ++at_;
      switch (escape) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u':
          if (!ParseUnicodeEscape(out)) {
            return false;
          }
          break;
        default:
          return Fail("unknown escape in a string");
      }
    }
  }

  bool ParseHex4(std::uint32_t* out) {
    if (at_ + 4 > text_.size()) {
      return Fail("truncated \\u escape");
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const char c = text_[at_ + i];
      std::uint32_t digit = 0;
      if (IsDigit(c)) {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return Fail("non-hex digit in a \\u escape");
      }
      value = (value << 4) | digit;
    }
    at_ += 4;
    *out = value;
    return true;
  }

  bool ParseUnicodeEscape(std::string* out) {
    std::uint32_t unit = 0;
    if (!ParseHex4(&unit)) {
      return false;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      // A high surrogate is half a character. Without its pair the string
      // cannot be encoded, and dropping it silently would corrupt the value.
      if (at_ + 1 >= text_.size() || text_[at_] != '\\' || text_[at_ + 1] != 'u') {
        return Fail("high surrogate without its pair in a \\u escape");
      }
      at_ += 2;
      std::uint32_t low = 0;
      if (!ParseHex4(&low)) {
        return false;
      }
      if (low < 0xDC00 || low > 0xDFFF) {
        return Fail("high surrogate followed by a non-surrogate in a \\u escape");
      }
      unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
    } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
      return Fail("lone low surrogate in a \\u escape");
    }
    AppendUtf8(unit, out);
    return true;
  }

  /// Scans the RFC 8259 number grammar first and converts only then, because
  /// `from_chars` on its own would accept `inf` and `nan`, which JSON has no
  /// syntax for.
  bool ParseNumber(Value* out) {
    const std::size_t start = at_;
    if (at_ < text_.size() && text_[at_] == '-') {
      ++at_;
    }
    if (at_ >= text_.size() || !IsDigit(text_[at_])) {
      at_ = start;
      return Fail("not a JSON value");
    }
    if (text_[at_] == '0') {
      ++at_;
    } else {
      while (at_ < text_.size() && IsDigit(text_[at_])) {
        ++at_;
      }
    }
    bool integral = true;
    if (at_ < text_.size() && text_[at_] == '.') {
      integral = false;
      ++at_;
      if (at_ >= text_.size() || !IsDigit(text_[at_])) {
        return Fail("digit expected after the decimal point");
      }
      while (at_ < text_.size() && IsDigit(text_[at_])) {
        ++at_;
      }
    }
    if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
      integral = false;
      ++at_;
      if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
        ++at_;
      }
      if (at_ >= text_.size() || !IsDigit(text_[at_])) {
        return Fail("digit expected in the exponent");
      }
      while (at_ < text_.size() && IsDigit(text_[at_])) {
        ++at_;
      }
    }

    const char* first = text_.data() + start;
    const char* last = text_.data() + at_;
    out->type_ = Type::kNumber;
    if (integral) {
      std::int64_t as_integer = 0;
      const std::from_chars_result parsed = std::from_chars(first, last, as_integer);
      if (parsed.ec == std::errc() && parsed.ptr == last) {
        out->integral_ = true;
        out->integer_ = as_integer;
        out->number_ = static_cast<double>(as_integer);
        return true;
      }
      // Too wide for an int64. Still a valid JSON number, but `is_integer()`
      // stays false, so a caller that needs an exact id refuses it rather than
      // carrying a rounded one.
    }
    double as_double = 0.0;
    const std::from_chars_result parsed = std::from_chars(first, last, as_double);
    if (parsed.ec != std::errc() || parsed.ptr != last) {
      at_ = start;
      return Fail("number out of range");
    }
    out->number_ = as_double;
    return true;
  }

  std::string_view text_;
  std::size_t at_ = 0;
  Error error_;
};

ParseResult Parse(std::string_view text) {
  Parser parser(text);
  return parser.Run();
}

std::string Quote(std::string_view value) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const char raw : value) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        // Everything below 0x20 is forbidden raw inside a JSON string, NUL
        // included -- and NUL is the one that matters, because a value carrying
        // one is exactly what `Reader::UsableString` refuses on the way back in.
        // Escaping it keeps the file readable by our own parser rather than
        // making it a syntax error nobody can diagnose.
        if (byte < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(byte >> 4) & 0xF]);
          out.push_back(kHex[byte & 0xF]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string QuoteArray(const std::vector<std::string>& values) {
  std::string out("[");
  for (std::size_t at = 0; at < values.size(); ++at) {
    if (at != 0) {
      out.push_back(',');
    }
    out += Quote(values[at]);
  }
  out.push_back(']');
  return out;
}

bool Reader::UsableString(Reader& reader, std::string_view key, const std::string& value) {
  if (value.empty()) {
    return reader.Fail(key, "is empty");
  }
  // `"a\u0000b"` is legal JSON, and `std::string` carries it faithfully -- but
  // everything downstream is a C API that stops at the NUL, so the value that
  // would get used is not the value that was checked. Refuse it here rather
  // than let a truncated token reach an Authorization header.
  if (value.find('\0') != std::string::npos) {
    return reader.Fail(key, "contains an embedded NUL");
  }
  return true;
}

Reader::Reader(const Value& value, std::string_view context) {
  if (value.is_object()) {
    object_ = &value;
    return;
  }
  error_.message =
      std::string(context) + " is " + ToString(value.type()) + ", not the object it must be";
}

bool Reader::Fail(std::string_view key, std::string message) {
  if (error_.ok()) {
    error_.field = std::string(key);
    error_.message = std::move(message);
  }
  return false;
}

const Value* Reader::Lookup(std::string_view key) {
  if (!ok()) {
    return nullptr;
  }
  const Value* member = object_->Find(key);
  if (member == nullptr) {
    Fail(key, "missing");
    return nullptr;
  }
  return member;
}

bool Reader::Required(std::string_view key, std::string* out) {
  const Value* member = Lookup(key);
  if (member == nullptr) {
    return false;
  }
  if (!member->is_string()) {
    return Fail(key, std::string("expected a string, got ") + ToString(member->type()));
  }
  if (!UsableString(*this, key, member->string())) {
    return false;
  }
  *out = member->string();
  return true;
}

bool Reader::Required(std::string_view key, std::int64_t* out) {
  const Value* member = Lookup(key);
  if (member == nullptr) {
    return false;
  }
  if (!member->is_number()) {
    return Fail(key, std::string("expected a number, got ") + ToString(member->type()));
  }
  if (!member->is_integer()) {
    return Fail(key, "expected a whole number that fits 64 bits");
  }
  *out = member->integer();
  return true;
}

bool Reader::Required(std::string_view key, bool* out) {
  const Value* member = Lookup(key);
  if (member == nullptr) {
    return false;
  }
  if (!member->is_bool()) {
    return Fail(key, std::string("expected a boolean, got ") + ToString(member->type()));
  }
  *out = member->boolean();
  return true;
}

bool Reader::Required(std::string_view key, std::vector<std::string>* out) {
  const Value* member = Lookup(key);
  if (member == nullptr) {
    return false;
  }
  if (!member->is_array()) {
    return Fail(key, std::string("expected an array, got ") + ToString(member->type()));
  }
  std::vector<std::string> items;
  items.reserve(member->elements().size());
  for (const Value& element : member->elements()) {
    if (!element.is_string()) {
      return Fail(key, std::string("expected an array of strings, found ") +
                           ToString(element.type()) + " in it");
    }
    items.push_back(element.string());
  }
  *out = std::move(items);
  return true;
}

bool Reader::RequiredNullable(std::string_view key, std::optional<std::string>* out) {
  const Value* member = Lookup(key);
  if (member == nullptr) {
    return false;
  }
  if (member->is_null()) {
    out->reset();
    return true;
  }
  if (!member->is_string()) {
    return Fail(key, std::string("expected a string or null, got ") + ToString(member->type()));
  }
  if (!UsableString(*this, key, member->string())) {
    return false;
  }
  *out = member->string();
  return true;
}

}  // namespace rommsync::json

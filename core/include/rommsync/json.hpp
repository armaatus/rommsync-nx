// A very small, strict JSON reader.
//
// RomM answers in JSON, so the engine needs to read it -- but core/ may include
// only standard headers and rommsync/ ones (core/AGENTS.md, enforced by the
// `static` CI job), and a sysmodule has no room for a general-purpose library
// anyway. This is the smallest thing that can parse a RomM response *safely*:
// it accepts RFC 8259 and almost nothing else, so a truncated or hostile body
// is a named error rather than a plausible-looking value.
//
// The one deliberate narrowing: a number whose magnitude no `double` can hold
// (`1e400`, `1e-400`) is refused rather than saturated to infinity or to zero.
// Both are well-formed JSON. Neither is a value RomM has any way to mean, and
// a silent +inf in a size or a timestamp is worse than a rejected body.
//
// Strict on purpose, because every relaxation here is a way for a broken
// response to look fine:
//
//   - no trailing commas, comments, single quotes, `NaN` or `Infinity`;
//   - no duplicate keys in one object -- the second would silently shadow the
//     first, and nothing RomM generates emits them;
//   - no unescaped control characters, no lone surrogates;
//   - nesting past `kMaxDepth` is refused rather than recursed into, because
//     this parser runs on a sysmodule thread with a small stack.
//
// Accessors never throw and never abort: reading a string off a number returns
// an empty string, and `Find` on a missing key returns nullptr. That is what
// makes `Reader` below able to report *which* field was wrong instead of
// dying on the first surprise.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::json {

enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

/// How deep a document may nest before it is refused.
inline constexpr int kMaxDepth = 64;

class Value;

struct Member;

/// One JSON value.
///
/// Deliberately a plain struct rather than a variant: `Value` is recursive, and
/// a vector of an incomplete type is well-defined where a variant of one is
/// not. It costs a few unused bytes per node, which is the right trade for
/// payloads the size of a RomM response.
class Value {
 public:
  Value() = default;

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::kNull; }
  bool is_bool() const { return type_ == Type::kBool; }
  bool is_number() const { return type_ == Type::kNumber; }
  bool is_string() const { return type_ == Type::kString; }
  bool is_array() const { return type_ == Type::kArray; }
  bool is_object() const { return type_ == Type::kObject; }

  /// False for anything that is not a bool.
  bool boolean() const { return type_ == Type::kBool && bool_; }

  /// Zero for anything that is not a number.
  double number() const { return type_ == Type::kNumber ? number_ : 0.0; }

  /// True when this is a number written without a fraction or exponent and it
  /// fits an `int64_t`. Ids and durations must go through this rather than
  /// through `number()`, which cannot hold every 64-bit integer exactly.
  bool is_integer() const { return type_ == Type::kNumber && integral_; }
  std::int64_t integer() const { return integral_ ? integer_ : 0; }

  /// Empty for anything that is not a string.
  const std::string& string() const;

  /// Empty for anything that is not an array.
  const std::vector<Value>& elements() const;

  /// Empty for anything that is not an object.
  const std::vector<Member>& members() const;

  /// Element count for an array or an object; zero for everything else.
  std::size_t size() const;

  /// The member named `key`, or nullptr when this is not an object or has no
  /// such key. Valid until this value changes.
  const Value* Find(std::string_view key) const;

 private:
  friend class Parser;

  Type type_ = Type::kNull;
  bool bool_ = false;
  bool integral_ = false;
  double number_ = 0.0;
  std::int64_t integer_ = 0;
  std::string string_;
  std::vector<Value> elements_;
  std::vector<Member> members_;
};

struct Member {
  std::string key;
  Value value;
};

/// Where reading a document went wrong. `ok()` when nothing did.
struct Error {
  /// Byte offset into the text, for a syntax error. Meaningless otherwise.
  std::size_t offset = 0;

  /// Which field, for a shape error (`Reader`). Empty for a syntax error.
  std::string field;

  /// Log-friendly, and never quotes a *value*: a body being rejected here may
  /// still be carrying an access token.
  std::string message;

  bool ok() const { return message.empty(); }

  /// "at offset 41: unterminated string" / "field expires_in: expected a
  /// number, got a string". Empty when `ok()`.
  std::string Describe() const;
};

struct ParseResult {
  Value value;
  Error error;
  bool ok() const { return error.ok(); }
};

/// Parse a whole document. Trailing content other than whitespace is an error,
/// so a truncated body followed by a second one cannot be read as the first.
ParseResult Parse(std::string_view text);

/// `value` as a JSON string literal, quotes included.
///
/// The engine has to *write* JSON as well as read it -- a device-init request
/// body, a line of `token.dat` -- and the one way to get that wrong is to build
/// it by concatenation: a device name holding a quote, or a server URL holding a
/// backslash, turns a request body into something the server parses as a
/// different object. Every character RFC 8259 requires escaping is escaped here,
/// control characters included, so a value is carried rather than interpreted.
///
/// Bytes at or above 0x80 pass through unchanged: the input is assumed to be
/// UTF-8, which is what the rest of the engine handles.
std::string Quote(std::string_view value);

/// `["a","b"]` -- each element quoted by `Quote`, `[]` when empty.
std::string QuoteArray(const std::vector<std::string>& values);

/// Reads named fields off an object, remembering the first thing that was
/// wrong with it.
///
/// The point is the error message: a response missing `device_id` has to say
/// so, because the alternative -- a default-constructed struct that pairs
/// against nothing and 401s forever -- is a bug that surfaces hours later on a
/// console. Every getter returns false and records the field once; later calls
/// on a failed reader do nothing, so a caller can read every field and check
/// once at the end.
class Reader {
 public:
  /// `context` names the thing being read, e.g. "device init response". It is
  /// used in the error when `value` is not an object at all.
  Reader(const Value& value, std::string_view context);

  /// A required string that is non-empty and free of embedded NULs.
  ///
  /// Empty is refused because every string RomM is documented to send here is
  /// an identifier or a path: a blank one is a server that has lost it, not a
  /// value worth carrying. A NUL is refused because `"a\u0000b"` is legal JSON
  /// and every C API downstream -- a `Authorization:` header, a URL, a line
  /// written to `token.dat` -- stops at the NUL, so the value that gets used
  /// would not be the value that was validated.
  bool Required(std::string_view key, std::string* out);

  /// A required integer -- rejected if written with a fraction or exponent, or
  /// if it does not fit an `int64_t`.
  bool Required(std::string_view key, std::int64_t* out);

  /// A required boolean. Only `true` and `false`: a `0`, a `1` or a `"true"` is
  /// a field that does not mean what the reader would take it to mean, and the
  /// flags RomM sends this way -- `sync_enabled` -- decide whether the client
  /// syncs at all.
  bool Required(std::string_view key, bool* out);

  /// A required array of strings. May be empty: an approval that granted no
  /// scopes is a real answer, and one the caller has to see.
  bool Required(std::string_view key, std::vector<std::string>* out);

  /// A required field documented as `string | null`. Must be present; `null`
  /// yields an empty optional rather than an error. A *string* is held to the
  /// same bar as `Required`: `""` is refused rather than carried, because every
  /// caller reads "has a value" as "has a value to use", and an empty one would
  /// reach a timestamp parser as though it meant something.
  bool RequiredNullable(std::string_view key, std::optional<std::string>* out);

  /// A required field documented as `integer | null`. Must be present; `null`
  /// yields an empty optional rather than an error.
  ///
  /// Held to exactly the bar the integer `Required` sets -- written without a
  /// fraction or an exponent, and inside an `int64_t`. The field this exists for
  /// is `save_id` on a negotiate operation, which is `null` for a save the
  /// server does not have yet and a row id otherwise; a `0` standing in for the
  /// `null` would be a save id, and one that names no save.
  bool RequiredNullable(std::string_view key, std::optional<std::int64_t>* out);

  bool ok() const { return error_.ok(); }
  const Error& error() const { return error_; }

 private:
  const Value* Lookup(std::string_view key);
  bool Fail(std::string_view key, std::string message);

  /// The bar every string field is held to, shared by `Required` and
  /// `RequiredNullable` so the two cannot drift apart.
  static bool UsableString(Reader& reader, std::string_view key, const std::string& value);

  const Value* object_ = nullptr;
  Error error_;
};

/// The name of a type as it appears in an error message ("a string", "a null").
const char* ToString(Type type);

}  // namespace rommsync::json

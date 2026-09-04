// The JSON reader, on its own. No network, no rig: this never skips.
//
// Most of what is here is the *rejections*. A parser that accepts a truncated
// body is the failure mode that matters -- it hands the auth code a struct that
// looks parsed and is missing half its fields.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/json.hpp"

namespace json = rommsync::json;

namespace {

void Accepts(checks::Checks& c) {
  const json::ParseResult empty_object = json::Parse("{}");
  c.Expect(empty_object.ok(), "{} parses");
  c.Expect(empty_object.value.is_object(), "{} is an object");
  c.ExpectEq(empty_object.value.size(), std::size_t{0}, "{} has no members");

  const json::ParseResult doc = json::Parse(R"({
    "s": "text", "t": true, "f": false, "n": null,
    "i": 42, "neg": -7, "zero": 0, "d": 1.5, "e": 2e3,
    "a": [1, "two", null], "o": {"nested": {"deep": []}}
  })");
  c.Expect(doc.ok(), "a mixed document parses: " + doc.error.Describe());
  const json::Value& v = doc.value;
  c.ExpectEq(v.size(), std::size_t{11}, "member count");

  c.ExpectEq(v.Find("s")->string(), std::string("text"), "string member");
  c.Expect(v.Find("t")->boolean(), "true member");
  c.Expect(!v.Find("f")->boolean(), "false member");
  c.Expect(v.Find("n")->is_null(), "null member");

  c.Expect(v.Find("i")->is_integer(), "42 is an integer");
  c.ExpectEq(v.Find("i")->integer(), std::int64_t{42}, "42");
  c.ExpectEq(v.Find("neg")->integer(), std::int64_t{-7}, "-7");
  c.ExpectEq(v.Find("zero")->integer(), std::int64_t{0}, "0");

  // A fraction or an exponent means the value cannot be trusted as an exact
  // id, even when it happens to land on a whole number.
  c.Expect(!v.Find("d")->is_integer(), "1.5 is not an integer");
  c.ExpectEq(v.Find("d")->number(), 1.5, "1.5");
  c.Expect(!v.Find("e")->is_integer(), "2e3 is not an integer");
  c.ExpectEq(v.Find("e")->number(), 2000.0, "2e3");

  const json::Value* array = v.Find("a");
  c.Expect(array->is_array(), "array member");
  c.ExpectEq(array->size(), std::size_t{3}, "array length");
  c.ExpectEq(array->elements()[1].string(), std::string("two"), "array element");
  c.Expect(v.Find("o")->Find("nested")->Find("deep")->is_array(), "nesting");

  c.Expect(v.Find("absent") == nullptr, "a missing key is nullptr");

  // Accessors on the wrong type answer empty rather than aborting, which is
  // what lets Reader report the field instead of dying on it.
  c.Expect(v.Find("i")->string().empty(), "string() of a number is empty");
  c.ExpectEq(v.Find("s")->number(), 0.0, "number() of a string is zero");
  c.Expect(v.Find("s")->elements().empty(), "elements() of a string is empty");
  c.Expect(v.Find("s")->Find("x") == nullptr, "Find() on a string is nullptr");
  c.ExpectEq(v.Find("s")->size(), std::size_t{0}, "size() of a string is zero");

  // A bare value is a document too, and leading/trailing whitespace is fine.
  c.Expect(json::Parse("  \n\t 12 \r\n").ok(), "a bare number with whitespace");
  c.Expect(json::Parse("\"just a string\"").ok(), "a bare string");
  c.Expect(json::Parse("[]").ok(), "an empty array");

  // Escapes, including a \u for a BMP character and the surrogate-pair form
  // of one outside it.
  const json::ParseResult escapes =
      json::Parse(R"("a\"b\\c\/d\be\ff\ng\rh\ti\u00e9\ud83c\udfae")");
  c.Expect(escapes.ok(), "escapes parse: " + escapes.error.Describe());
  c.ExpectEq(escapes.value.string(),
             std::string("a\"b\\c/d\be\ff\ng\rh\ti\xC3\xA9\xF0\x9F\x8E\xAE"),
             "every escape decodes, \\u as UTF-8");

  // A number too wide for an int64 is still valid JSON -- but it is not an
  // integer any more, so nothing reads it back as an exact id.
  const json::ParseResult huge = json::Parse("99999999999999999999");
  c.Expect(huge.ok(), "a 20-digit number parses");
  c.Expect(!huge.value.is_integer(), "...but is not an integer");
  c.ExpectEq(huge.value.integer(), std::int64_t{0}, "...and reads back as zero");
}

void Rejects(checks::Checks& c) {
  struct Case {
    const char* text;
    const char* what;
  };
  // Each of these is a body a server or a proxy can really produce, or a
  // JSON-ish dialect that is not JSON. None may parse.
  const Case kBad[] = {
      {"", "an empty body"},
      {"   ", "whitespace only"},
      {"{", "an unterminated object"},
      {R"({"a")", "an object cut off after the key"},
      {R"({"a":)", "an object cut off after the colon"},
      {R"({"a":1,})", "a trailing comma in an object"},
      {"[1,]", "a trailing comma in an array"},
      {"[1", "an unterminated array"},
      {R"({"a" 1})", "a missing colon"},
      {R"({a: 1})", "an unquoted key"},
      {"{'a': 1}", "single quotes"},
      {R"({"a": 1} // note)", "a comment"},
      {"{} {}", "two documents"},
      {R"({"a":1}{"a":2})", "a body repeated"},
      {R"({"a": 1, "a": 2})", "a duplicate key"},
      {"NaN", "NaN"},
      {"Infinity", "Infinity"},
      {"-Infinity", "-Infinity"},
      {"01", "a leading zero"},
      {"+1", "a leading plus"},
      {".5", "a bare decimal point"},
      {"1.", "a trailing decimal point"},
      {"1e", "an empty exponent"},
      {"1e+", "an exponent with no digits"},
      {"tru", "a truncated literal"},
      {"undefined", "undefined"},
      {"\"unterminated", "an unterminated string"},
      {"\"a\nb\"", "a raw newline in a string"},
      {R"("\q")", "an unknown escape"},
      {R"("\u00")", "a truncated \\u escape"},
      {R"("\uZZZZ")", "a non-hex \\u escape"},
      {R"("\uD83C")", "a lone high surrogate"},
      {R"("\uDFAE")", "a lone low surrogate"},
      {R"("\uD83CA")", "a high surrogate followed by a normal character"},
  };
  for (const Case& bad : kBad) {
    const json::ParseResult result = json::Parse(bad.text);
    if (result.ok()) {
      c.Expect(false, std::string("accepted ") + bad.what);
      continue;
    }
    c.Expect(!result.error.Describe().empty(),
             std::string("rejecting ") + bad.what + " says why");
  }

  // Deep nesting is refused rather than recursed into: this parser runs on a
  // sysmodule thread whose stack a hostile body must not be able to walk off.
  const std::string ok_depth(json::kMaxDepth - 1, '[');
  const std::string too_deep(json::kMaxDepth + 1, '[');
  c.Expect(json::Parse(ok_depth + std::string(json::kMaxDepth - 1, ']')).ok(),
           "nesting just under the limit parses");
  c.Expect(!json::Parse(too_deep + std::string(json::kMaxDepth + 1, ']')).ok(),
           "nesting past the limit is refused");
}

void ReaderReportsTheField(checks::Checks& c) {
  const json::ParseResult doc = json::Parse(
      R"({"name": "x", "count": 3, "tags": ["a","b"], "note": null, "blank": "",
          "fraction": 1.5, "mixed": ["a", 2]})");
  c.Expect(doc.ok(), "reader fixture parses");

  {
    json::Reader reader(doc.value, "fixture");
    std::string name;
    std::int64_t count = 0;
    std::vector<std::string> tags;
    std::optional<std::string> note;
    reader.Required("name", &name);
    reader.Required("count", &count);
    reader.Required("tags", &tags);
    reader.RequiredNullable("note", &note);
    c.Expect(reader.ok(), "every good field reads: " + reader.error().Describe());
    c.ExpectEq(name, std::string("x"), "name");
    c.ExpectEq(count, std::int64_t{3}, "count");
    c.ExpectEq(tags.size(), std::size_t{2}, "tags");
    c.Expect(!note.has_value(), "a null nullable is empty, not an error");
  }

  struct Case {
    const char* key;
    const char* field;  // the field the error must name
    const char* what;
  };
  const Case kString[] = {
      {"missing", "missing", "a missing string"},
      {"count", "count", "a number where a string belongs"},
      {"note", "note", "a null where a string belongs"},
      {"blank", "blank", "an empty string"},
      {"tags", "tags", "an array where a string belongs"},
  };
  for (const Case& bad : kString) {
    json::Reader reader(doc.value, "fixture");
    std::string out = "untouched";
    c.Expect(!reader.Required(bad.key, &out), std::string("refuses ") + bad.what);
    c.ExpectEq(reader.error().field, std::string(bad.field),
               std::string("names the field for ") + bad.what);
    c.ExpectEq(out, std::string("untouched"),
               std::string("leaves the output alone for ") + bad.what);
  }

  {
    json::Reader reader(doc.value, "fixture");
    std::int64_t out = -1;
    c.Expect(!reader.Required("fraction", &out), "refuses a fraction as an integer");
    c.ExpectEq(reader.error().field, std::string("fraction"), "names fraction");
  }
  {
    json::Reader reader(doc.value, "fixture");
    std::vector<std::string> out;
    c.Expect(!reader.Required("mixed", &out), "refuses a number inside a string array");
    c.ExpectEq(reader.error().field, std::string("mixed"), "names mixed");
    c.Expect(out.empty(), "leaves the output alone");
  }
  {
    json::Reader reader(doc.value, "fixture");
    std::optional<std::string> out;
    c.Expect(!reader.RequiredNullable("absent", &out),
             "a nullable field still has to be present");
  }
  {
    // The first failure is the one reported: a reader that kept overwriting
    // would name whichever field happened to be read last.
    json::Reader reader(doc.value, "fixture");
    std::string first;
    std::string second;
    reader.Required("missing", &first);
    reader.Required("count", &second);
    c.ExpectEq(reader.error().field, std::string("missing"), "the first failure sticks");
  }
  {
    const json::ParseResult array = json::Parse("[1,2]");
    json::Reader reader(array.value, "fixture");
    std::string out;
    c.Expect(!reader.ok(), "an array is not an object");
    c.Expect(!reader.Required("anything", &out), "and reads nothing");
    c.Expect(reader.error().Describe().find("fixture") != std::string::npos,
             "the error names what was being read");
  }
}

}  // namespace

int main() {
  checks::Checks c;
  Accepts(c);
  Rejects(c);
  ReaderReportsTheField(c);
  return c.failures() == 0 ? 0 : 1;
}

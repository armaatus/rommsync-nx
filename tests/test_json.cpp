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

  // The one deliberate narrowing, and the reason it is not in kBad above: these
  // are well-formed JSON that no double can hold. Refusing beats saturating to
  // an infinity or a zero that would then be read as a size or a timestamp.
  for (const char* unrepresentable : {"1e400", "-1e400", "1e-400", "[1e400]"}) {
    const json::ParseResult result = json::Parse(unrepresentable);
    c.Expect(!result.ok(), std::string("refuses the unrepresentable ") + unrepresentable);
  }
  // ...but the merely large is fine, so the narrowing stays narrow.
  c.Expect(json::Parse("1e308").ok(), "a large-but-representable double parses");
  c.Expect(json::Parse("1e-308").ok(), "and a small one");

  // Deep nesting is refused rather than recursed into: this parser runs on a
  // sysmodule thread whose stack a hostile body must not be able to walk off.
  const std::string ok_depth(json::kMaxDepth - 1, '[');
  const std::string too_deep(json::kMaxDepth + 1, '[');
  c.Expect(json::Parse(ok_depth + std::string(json::kMaxDepth - 1, ']')).ok(),
           "nesting just under the limit parses");
  c.Expect(!json::Parse(too_deep + std::string(json::kMaxDepth + 1, ']')).ok(),
           "nesting past the limit is refused");
  // Exactly the limit parses and one more does not. sysmodule/source/
  // stack_budget.hpp prices the worst case at kMaxDepth + 1 levels -- each one
  // accepted, and the frame that refuses the next -- so the boundary is part of
  // the main thread's stack budget, not only of this parser (#216).
  c.Expect(json::Parse(std::string(json::kMaxDepth, '[') + std::string(json::kMaxDepth, ']'))
               .ok(),
           "nesting exactly at the limit parses");
  c.Expect(!json::Parse(std::string(json::kMaxDepth, '[') + "1" +
                        std::string(json::kMaxDepth, ']'))
                .ok(),
           "a value inside the deepest container the limit allows is one level too deep");
  // ...and the limit is not one RomM reaches. A rom row's IGDB multiplayer mode
  // names its platform seven levels down a page of roms, the deepest typed
  // response either pinned OpenAPI snapshot describes (`stack.contract`).
  c.Expect(json::Parse("{\"items\": [{\"igdb_metadata\": {\"multiplayer_modes\": "
                       "[{\"platform\": {\"id\": 1, \"name\": \"Nintendo Switch\"}}]}}]}")
               .ok(),
           "RomM's deepest response shape parses");
}

void ReaderReportsTheField(checks::Checks& c) {
  const json::ParseResult doc = json::Parse(
      "{\"name\": \"x\", \"count\": 3, \"tags\": [\"a\",\"b\"], \"note\": null,"
      " \"blank\": \"\", \"nul\": \"a\\u0000b\", \"fraction\": 1.5,"
      " \"mixed\": [\"a\", 2], \"on\": true, \"off\": false,"
      " \"truthy\": \"true\"}");
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
    bool on = false;
    bool off = true;
    reader.Required("on", &on);
    reader.Required("off", &off);
    c.Expect(reader.ok(), "every good field reads: " + reader.error().Describe());
    c.Expect(on, "a true reads back true");
    c.Expect(!off, "and a false reads back false");
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
      {"nul", "nul", "a string with an embedded NUL"},
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
  // A flag is `true` or `false` and nothing else. `1` and `"true"` are the two
  // shapes a lenient reader would wave through, and both would turn a RomM
  // field the client acts on -- `sync_enabled` -- into whichever answer the
  // coercion happened to give.
  {
    const Case kBool[] = {
        {"missing", "missing", "a missing boolean"},
        {"count", "count", "a 3 where a boolean belongs"},
        {"truthy", "truthy", "the string \"true\""},
        {"note", "note", "a null where a boolean belongs"},
    };
    for (const Case& bad : kBool) {
      json::Reader reader(doc.value, "fixture");
      bool out = true;
      c.Expect(!reader.Required(bad.key, &out), std::string("refuses ") + bad.what);
      c.ExpectEq(reader.error().field, std::string(bad.field),
                 std::string("names the field for ") + bad.what);
      c.Expect(out, std::string("leaves the output alone for ") + bad.what);
    }
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
    // `null` and `""` are not the same answer. Every caller reads "has a value"
    // as "has a value to use", so a blank one has to be refused here rather
    // than handed on to whatever would try to parse it.
    json::Reader reader(doc.value, "fixture");
    std::optional<std::string> out;
    c.Expect(!reader.RequiredNullable("blank", &out),
             "a nullable field refuses a blank string, same as Required");
    c.ExpectEq(reader.error().field, std::string("blank"), "names blank");
    c.Expect(!out.has_value(), "and carries nothing");
  }
  {
    json::Reader reader(doc.value, "fixture");
    std::optional<std::string> out;
    c.Expect(!reader.RequiredNullable("nul", &out),
             "a nullable field refuses an embedded NUL too");
  }
  {
    // `integer | null`, which exists for a negotiate operation's `save_id`:
    // `null` is a save the server does not have yet, and a `0` standing in for
    // it would be a save id that names no save.
    json::Reader reader(doc.value, "fixture");
    std::optional<std::int64_t> present;
    std::optional<std::int64_t> absent;
    c.Expect(reader.RequiredNullable("count", &present), "a nullable integer reads");
    c.Expect(present.has_value() && *present == 3, "...and carries the value");
    c.Expect(reader.RequiredNullable("note", &absent), "a null nullable integer is empty");
    c.Expect(!absent.has_value(), "...rather than a zero");
    c.Expect(reader.ok(), "neither is an error: " + reader.error().Describe());
  }
  {
    const Case kNullableInteger[] = {
        {"absent", "absent", "a missing nullable integer"},
        {"name", "name", "a string where a nullable integer belongs"},
        {"fraction", "fraction", "a fraction where a whole number belongs"},
        {"tags", "tags", "an array where a nullable integer belongs"},
    };
    for (const Case& bad : kNullableInteger) {
      json::Reader reader(doc.value, "fixture");
      std::optional<std::int64_t> out = 7;
      c.Expect(!reader.RequiredNullable(bad.key, &out), std::string("refuses ") + bad.what);
      c.ExpectEq(reader.error().field, std::string(bad.field),
                 std::string("names the field, for ") + bad.what);
    }
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

/// The writer half. Building a request body or a line of `token.dat` by
/// concatenation is the one way to turn a value into syntax, so the assertions
/// here are all about characters that would escape their quotes.
void QuotesWhatItIsGiven(checks::Checks& c) {
  c.ExpectEq(json::Quote("plain"), std::string("\"plain\""), "an ordinary string");
  c.ExpectEq(json::Quote(""), std::string("\"\""), "an empty string");
  c.ExpectEq(json::Quote("say \"hi\""), std::string("\"say \\\"hi\\\"\""), "embedded quotes");
  c.ExpectEq(json::Quote("a\\b"), std::string("\"a\\\\b\""), "a backslash");
  c.ExpectEq(json::Quote("one\ntwo\ttab\r"), std::string("\"one\\ntwo\\ttab\\r\""),
             "the named escapes");
  c.ExpectEq(json::Quote(std::string("a\0b", 3)), std::string("\"a\\u0000b\""),
             "a NUL, which is legal in JSON and lethal in a C string");
  // Split literals on purpose: C++ hex escapes are maximal munch, so
  // "\x01f" is one character (0x1F) rather than 0x01 followed by 'f'.
  c.ExpectEq(json::Quote("\x01" "\x1f"), std::string("\"\\u0001\\u001f\""),
             "the other control characters");
  // UTF-8 goes through as bytes: escaping it would be lossless but unreadable,
  // and RomM speaks UTF-8 on both directions.
  c.ExpectEq(json::Quote("Pokémon"), std::string("\"Pokémon\""), "multi-byte UTF-8");

  c.ExpectEq(json::QuoteArray({}), std::string("[]"), "an empty array");
  c.ExpectEq(json::QuoteArray({"me.read"}), std::string(R"(["me.read"])"), "one element");
  c.ExpectEq(json::QuoteArray({"me.read", "roms.read"}), std::string(R"(["me.read","roms.read"])"),
             "several");

  // The round trip is what actually matters: whatever comes out has to parse
  // back to the same bytes, or a device name with a quote in it silently
  // becomes a different request.
  const std::string kAwkward[] = {"plain", "", "say \"hi\"", "a\\b", "line\nbreak",
                                  "Pokémon", "\x01" "control"};
  for (const std::string& raw : kAwkward) {
    const json::ParseResult document = json::Parse("{\"v\":" + json::Quote(raw) + "}");
    c.Expect(document.ok(), "a quoted value re-parses: " + document.error.Describe());
    const json::Value* value = document.value.Find("v");
    c.Expect(value != nullptr && value->is_string(), "and is still a string");
    if (value != nullptr) {
      c.ExpectEq(value->string(), raw, "and is unchanged");
    }
  }
}

/// `ParseEach`: one array handed over an element at a time (M9-19, #217). What
/// it must keep is everything `Parse` promises -- the strictness, the rest of the
/// document -- while never holding the array it streams.
void StreamsOneArray(checks::Checks& c) {
  std::vector<std::int64_t> seen;
  std::vector<std::size_t> indices;
  const json::EachElement collect = [&](const json::Value& element, std::size_t index) {
    const json::Value* id = element.Find("id");
    seen.push_back(id != nullptr ? id->integer() : -1);
    indices.push_back(index);
    return json::Error{};
  };

  const json::ParseResult page = json::ParseEach(
      R"({"items":[{"id":1},{"id":2},{"id":3}],"total":3,"nested":{"items":[9]}})", "items",
      collect);
  c.Expect(page.ok(), "an envelope parses: " + page.error.Describe());
  c.Expect(seen == std::vector<std::int64_t>{1, 2, 3}, "every element, in order");
  c.Expect(indices == std::vector<std::size_t>{0, 1, 2}, "each with its index");
  const json::Value* items = page.value.Find("items");
  c.Expect(items != nullptr && items->is_array() && items->size() == 0,
           "and the streamed array is left present and empty");
  const json::Value* total = page.value.Find("total");
  c.Expect(total != nullptr && total->integer() == 3, "the rest of the document is kept");
  const json::Value* nested = page.value.Find("nested");
  c.Expect(nested != nullptr && nested->Find("items") != nullptr &&
               nested->Find("items")->size() == 1,
           "and only the top-level member is streamed, not a nested one of the same name");

  seen.clear();
  const json::ParseResult bare = json::ParseEach(R"([{"id":7},{"id":8}])", "", collect);
  c.Expect(bare.ok() && bare.value.is_array() && bare.value.size() == 0,
           "an empty key streams the root array");
  c.Expect(seen == std::vector<std::int64_t>{7, 8}, "element by element");

  // The wrong shape is refused before it is parsed, by the name a caller's own
  // shape check would have given it (M9-20, #246). Parsed first and refused
  // after, a bare array where the envelope belongs cost ~11x its text on the way
  // to a refusal that was always coming.
  seen.clear();
  const json::ParseResult object = json::ParseEach(R"({"items":{"id":1}})", "items", collect);
  c.Expect(!object.ok() && seen.empty(), "something other than an array is not streamed");
  c.ExpectEq(object.error.field, std::string("items"), "it is refused by the member's name");
  c.ExpectEq(object.error.message, std::string("expected an array, got an object"),
             "and says what it was instead");
  const json::ParseResult missing = json::ParseEach(R"({"total":0})", "items", collect);
  c.Expect(missing.ok() && missing.value.Find("items") == nullptr,
           "a member that is not there is the caller's to refuse, as it always was");

  const json::ParseResult bare_page =
      json::ParseEach(R"([{"id":1},{"id":2}])", "items", collect, "rom list");
  c.Expect(!bare_page.ok() && seen.empty(), "a bare array where an envelope belongs is refused");
  c.ExpectEq(bare_page.error.message,
             std::string("rom list is an array, not the object it must be"),
             "in the sentence `Reader` would have used, naming what was being read");
  const json::ParseResult enveloped =
      json::ParseEach(R"({"items":[{"id":1}],"total":1})", "", collect, "platform list");
  c.Expect(!enveloped.ok() && seen.empty(), "and an envelope where a bare array belongs");
  c.ExpectEq(enveloped.error.message,
             std::string("platform list is an object, not the array it must be"),
             "the other way round");
  const json::ParseResult unnamed = json::ParseEach("null", "items", collect);
  c.ExpectEq(unnamed.error.message, std::string("the document is a null, not the object it must be"),
             "with a context of its own when the caller gave none");
  c.Expect(!json::ParseEach("", "items", collect).ok(), "an empty body is still refused");

  // Strict exactly as `Parse` is, including past elements already handed over.
  seen.clear();
  c.Expect(!json::ParseEach(R"({"items":[{"id":1},{"id":2,}]})", "items", collect).ok(),
           "a malformed element fails the document");
  c.ExpectEq(seen.size(), std::size_t{1}, "after the elements before it were handed over");
  c.Expect(!json::ParseEach(R"({"items":[{"id":1}],"items":[]})", "items", collect).ok(),
           "a duplicate key is still refused");
  c.Expect(!json::ParseEach(R"({"items":[{"id":1}]} x)", "items", collect).ok(),
           "and so is content after the document");
  c.Expect(!json::ParseEach(R"({"items":[{"id":1})", "items", collect).ok(),
           "and a body cut short");

  // The caller's refusal stops the walk and is the error.
  seen.clear();
  const json::ParseResult refused =
      json::ParseEach(R"([{"id":1},{"id":2},{"id":3}])", "",
                      [&](const json::Value& element, std::size_t) {
                        seen.push_back(element.Find("id")->integer());
                        json::Error error;
                        if (seen.size() == 2) {
                          error.field = "[1].id";
                          error.message = "not this one";
                        }
                        return error;
                      });
  c.Expect(!refused.ok(), "a refused element fails the parse");
  c.ExpectEq(refused.error.field, std::string("[1].id"), "with the caller's own error");
  c.ExpectEq(seen.size(), std::size_t{2}, "and nothing after it is read");
}

/// `ParseEach`'s keeping form (#248): what a reader names is built, at every
/// depth, and the rest is read past as strictly and never built.
void KeepsWhatIsNamed(checks::Checks& c) {
  constexpr std::string_view kKeep[] = {"id", "name", "total", "file_name"};
  std::vector<json::Value> seen;
  const auto collect = [&seen](const json::Value& element, std::size_t) {
    seen.push_back(element);
    return json::Error{};
  };
  const json::ParseResult page = json::ParseEach(
      R"({"items":[{"id":1,"name":"a","summary":"long \"text\" é","igdb":{"keywords":["x",)"
      R"("y"],"id":9},"files":[{"file_name":"a.bin","size":1}]}],"total":1,"char_index":{"a":0}})",
      "items", kKeep, collect, "rom list");
  c.Expect(page.ok(), "a page parses keeping what is named: " + page.error.Describe());
  c.ExpectEq(seen.size(), std::size_t{1}, "one element");
  if (seen.size() == 1) {
    c.ExpectEq(seen[0].size(), std::size_t{2}, "which holds only the two members named");
    c.Expect(seen[0].Find("id") != nullptr && seen[0].Find("name") != nullptr &&
                 seen[0].Find("summary") == nullptr && seen[0].Find("igdb") == nullptr &&
                 seen[0].Find("files") == nullptr,
             "id and name, and not what nothing reads");
  }
  c.Expect(page.value.Find("total") != nullptr && page.value.Find("char_index") == nullptr,
           "the envelope keeps what is named and drops the rest");
  c.Expect(page.value.Find("items") != nullptr && page.value.Find("items")->is_array(),
           "and the streamed array is there, empty, whatever keep says");

  // At any depth: a kept array's objects are filtered the same way.
  seen.clear();
  constexpr std::string_view kFiles[] = {"files", "file_name"};
  const json::ParseResult nested = json::ParseEach(
      R"([{"files":[{"file_name":"a.bin","crc":"00"},{"file_name":"b.bin","crc":"11"}],"x":1}])",
      "", kFiles, collect);
  c.Expect(nested.ok() && seen.size() == 1, "a nested array of objects parses");
  if (seen.size() == 1 && seen[0].Find("files") != nullptr) {
    const json::Value& files = *seen[0].Find("files");
    c.Expect(files.size() == 2 && files.elements()[0].size() == 1 &&
                 files.elements()[0].Find("file_name")->string() == "a.bin",
             "its objects keep only the named member");
  } else {
    c.Expect(false, "the kept array is there");
  }

  // A skipped member is checked as strictly as a kept one.
  for (const char* broken :
       {R"([{"id":1,"skip":[1,2,]}])", R"([{"id":1,"skip":"\x"}])", R"([{"id":1,"skip":tru}])",
        R"([{"id":1,"skip":{"a" 1}}])", R"([{"id":1,"skip":"\ud800"}])",
        R"([{"id":1,"skip":01}])", R"([{"id":1,"skip":"a)"}) {
    c.Expect(!json::ParseEach(broken, "", kKeep, collect).ok(),
             std::string("a malformed skipped member fails the document: ") + broken);
  }
  std::string deep = R"([{"id":1,"skip":)";
  for (int level = 0; level < json::kMaxDepth; ++level) deep += "[";
  for (int level = 0; level < json::kMaxDepth; ++level) deep += "]";
  deep += "}]";
  c.Expect(!json::ParseEach(deep, "", kKeep, collect).ok(),
           "and nested past kMaxDepth, it is refused");
  // Two kept members of one name are still refused.
  c.Expect(!json::ParseEach(R"([{"id":1,"id":2}])", "", kKeep, collect).ok(),
           "a duplicated kept key is refused");
  c.Expect(json::ParseEach(R"([{"id":1,"skip":1,"skip":2}])", "", kKeep, collect).ok(),
           "a duplicated skipped key is not looked at");
}

}  // namespace

int main() {
  checks::Checks c;
  Accepts(c);
  Rejects(c);
  ReaderReportsTheField(c);
  QuotesWhatItIsGiven(c);
  StreamsOneArray(c);
  KeepsWhatIsNamed(c);
  return c.failures() == 0 ? 0 : 1;
}

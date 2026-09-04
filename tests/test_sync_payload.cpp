// `ClientSaveState` against the PINNED SNAPSHOT, in both directions.
//
// One table below (`kSaveFields`) says what the seven fields are called, which
// are required, and which may be null. It is checked against
// server/contract/romm-openapi-5.2.0.json -- so a RomM that renames, re-types or
// re-requires a field goes red the moment the snapshot is refreshed -- and then
// against a body `EncodeNegotiateRequest` actually produced, so the struct
// cannot drift from the table either. Neither half is worth much alone: the
// snapshot without the encoder pins a document, and the encoder without the
// snapshot pins our own opinion.
//
// What no offline test can pin is whether a *misnamed* optional field would be
// noticed. It would not: RomM answers 200 and ignores it. `sync.understood`
// demonstrates that against a live server, which is why that test exists.
//
// No network here, so this never skips.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"

namespace json = rommsync::json;
namespace sync = rommsync::sync;

namespace {

/// One field of `ClientSaveState`, as the snapshot declares it and as the
/// encoder must emit it.
struct FieldSpec {
  const char* name;
  const char* type;  ///< the JSON type of a non-null value
  bool required;     ///< listed in the schema's `required`, so never omitted
  bool nullable;     ///< declared `T | null`, so the encoder may send `null`
};

constexpr FieldSpec kSaveFields[] = {
    {"rom_id", "integer", true, false},
    {"file_name", "string", true, false},
    {"slot", "string", false, true},
    {"emulator", "string", false, true},
    {"content_hash", "string", false, true},
    {"updated_at", "string", true, false},
    {"file_size_bytes", "integer", true, false},
};

constexpr FieldSpec kPayloadFields[] = {
    {"device_id", "string", false, true},
    {"saves", "array", true, false},
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// `components.schemas.<name>` out of the OpenAPI document.
const json::Value* Schema(const json::Value& document, const char* name) {
  const json::Value* components = document.Find("components");
  const json::Value* schemas = components != nullptr ? components->Find("schemas") : nullptr;
  return schemas != nullptr ? schemas->Find(name) : nullptr;
}

/// True when a property is declared `T | null` -- an `anyOf` with a null branch,
/// which is how the generator spells an optional field's type.
bool DeclaredNullable(const json::Value& property) {
  const json::Value* any_of = property.Find("anyOf");
  if (any_of == nullptr) {
    return false;
  }
  for (const json::Value& branch : any_of->elements()) {
    const json::Value* type = branch.Find("type");
    if (type != nullptr && type->string() == "null") {
      return true;
    }
  }
  return false;
}

/// The declared type of a property's non-null branch, `"string"` / `"integer"`.
std::string DeclaredType(const json::Value& property) {
  if (const json::Value* type = property.Find("type"); type != nullptr) {
    return type->string();
  }
  if (const json::Value* any_of = property.Find("anyOf"); any_of != nullptr) {
    for (const json::Value& branch : any_of->elements()) {
      const json::Value* type = branch.Find("type");
      if (type != nullptr && type->string() != "null") {
        return type->string();
      }
    }
  }
  return {};
}

bool Lists(const json::Value* array, const std::string& wanted) {
  if (array == nullptr) {
    return false;
  }
  const std::vector<json::Value>& elements = array->elements();
  return std::any_of(elements.begin(), elements.end(),
                     [&wanted](const json::Value& item) { return item.string() == wanted; });
}

/// One schema against its table: same field names, same types, same required
/// set, same nullability. Every clause is a way the client could be sending
/// something the server reads as a different save.
void PinSchema(checks::Checks& c, const json::Value& document, const char* name,
               const FieldSpec* fields, std::size_t count) {
  const json::Value* schema = Schema(document, name);
  if (schema == nullptr) {
    c.Expect(false, std::string("the snapshot declares ") + name);
    return;
  }
  const json::Value* properties = schema->Find("properties");
  if (properties == nullptr) {
    c.Expect(false, std::string(name) + " has properties");
    return;
  }
  const json::Value* required = schema->Find("required");

  for (const json::Member& member : properties->members()) {
    const bool known = std::any_of(fields, fields + count, [&member](const FieldSpec& field) {
      return member.key == field.name;
    });
    c.Expect(known, std::string(name) + " grew a field the struct does not carry: " + member.key);
  }

  for (std::size_t index = 0; index < count; ++index) {
    const FieldSpec& field = fields[index];
    const json::Value* property = properties->Find(field.name);
    if (property == nullptr) {
      c.Expect(false, std::string(name) + " no longer declares " + field.name);
      continue;
    }
    c.ExpectEq(DeclaredType(*property), std::string(field.type),
               std::string(name) + "." + field.name + " type");
    c.ExpectEq(DeclaredNullable(*property), field.nullable,
               std::string(name) + "." + field.name + " nullability");
    c.ExpectEq(Lists(required, field.name), field.required,
               std::string(name) + "." + field.name + " is required");
  }

  // A required field that is not in the table would be omitted from every body
  // the encoder builds, which is a 422 on the first real sync.
  if (required != nullptr) {
    for (const json::Value& listed : required->elements()) {
      const bool known = std::any_of(fields, fields + count, [&listed](const FieldSpec& field) {
        return listed.string() == field.name;
      });
      c.Expect(known, std::string(name) + " requires a field the struct does not carry: " +
                          listed.string());
    }
  }
}

void SnapshotPin(checks::Checks& c) {
  const std::string text = ReadFile(ROMMSYNC_OPENAPI);
  c.Expect(!text.empty(), "the OpenAPI snapshot is readable at " ROMMSYNC_OPENAPI);
  const json::ParseResult document = json::Parse(text);
  if (!document.ok()) {
    c.Expect(false, "the OpenAPI snapshot parses: " + document.error.Describe());
    return;
  }

  PinSchema(c, document.value, "ClientSaveState", kSaveFields, std::size(kSaveFields));
  PinSchema(c, document.value, "SyncNegotiatePayload", kPayloadFields, std::size(kPayloadFields));

  // `saves[]` is what this issue is about: entries are ClientSaveState, not some
  // other schema that happens to look like it.
  const json::Value* payload = Schema(document.value, "SyncNegotiatePayload");
  const json::Value* saves = payload != nullptr ? payload->Find("properties") : nullptr;
  saves = saves != nullptr ? saves->Find("saves") : nullptr;
  const json::Value* items = saves != nullptr ? saves->Find("items") : nullptr;
  const json::Value* ref = items != nullptr ? items->Find("$ref") : nullptr;
  c.ExpectEq(ref != nullptr ? ref->string() : std::string(),
             std::string("#/components/schemas/ClientSaveState"), "saves[] element schema");

  // The one field whose *format* the client has to honour: a date-time, which
  // is what FormatTimestamp spells and what the timestamp vectors below check.
  const json::Value* save = Schema(document.value, "ClientSaveState");
  const json::Value* save_properties = save != nullptr ? save->Find("properties") : nullptr;
  const json::Value* updated_at =
      save_properties != nullptr ? save_properties->Find("updated_at") : nullptr;
  const json::Value* format = updated_at != nullptr ? updated_at->Find("format") : nullptr;
  c.ExpectEq(format != nullptr ? format->string() : std::string(), std::string("date-time"),
             "updated_at format");
}

sync::ClientSaveState SampleSave() {
  sync::ClientSaveState save;
  save.rom_id = 4;
  save.file_name = "Game (USA).srm";
  save.slot = "autosave";
  save.emulator = "retroarch";
  save.content_hash = "abd8fff93894e8112c7dd17386e54a5f";
  save.updated_at = std::chrono::system_clock::from_time_t(1788521787);
  save.file_size_bytes = 32768;
  return save;
}

/// Parse an encoded body and hand back `saves[index]`, or nullptr with the
/// reason recorded.
const json::Value* Entry(checks::Checks& c, const json::ParseResult& document, std::size_t index,
                         const char* what) {
  if (!document.ok()) {
    c.Expect(false, std::string(what) + " parses: " + document.error.Describe());
    return nullptr;
  }
  const json::Value* saves = document.value.Find("saves");
  if (saves == nullptr || saves->size() <= index) {
    c.Expect(false, std::string(what) + " carries saves[" + std::to_string(index) + "]");
    return nullptr;
  }
  return &saves->elements()[index];
}

void EncodesTheSnapshotShape(checks::Checks& c) {
  sync::SyncNegotiatePayload payload;
  payload.device_id = "b242014b-5774-44dc-b495-139fc7b856da";
  payload.saves.push_back(SampleSave());

  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  if (!encoded.ok()) {
    c.Expect(false, "a complete save encodes: " + encoded.error.Describe());
    return;
  }

  const json::ParseResult document = json::Parse(encoded.body);
  const json::Value* entry = Entry(c, document, 0, "the encoded body");
  if (entry == nullptr) {
    return;
  }

  // Exactly the table's fields: one missing is a 422 or a save the server
  // arbitrates without, one extra is a field this client invented.
  for (const json::Member& member : entry->members()) {
    const bool known = std::any_of(std::begin(kSaveFields), std::end(kSaveFields),
                                   [&member](const FieldSpec& field) {
                                     return member.key == field.name;
                                   });
    c.Expect(known, "the encoded entry has an unknown field: " + member.key);
  }
  for (const FieldSpec& field : kSaveFields) {
    const json::Value* value = entry->Find(field.name);
    if (value == nullptr) {
      c.Expect(false, std::string("the encoded entry carries ") + field.name);
      continue;
    }
    const std::string type = std::string(field.type);
    const bool right_type = (type == "integer" && value->is_integer()) ||
                            (type == "string" && value->is_string());
    c.Expect(right_type, std::string("the encoded ") + field.name + " is a " + field.type);
  }

  c.ExpectEq(entry->Find("rom_id")->integer(), std::int64_t{4}, "rom_id");
  c.ExpectEq(entry->Find("file_name")->string(), std::string("Game (USA).srm"), "file_name");
  c.ExpectEq(entry->Find("slot")->string(), std::string("autosave"), "slot");
  c.ExpectEq(entry->Find("emulator")->string(), std::string("retroarch"), "emulator");
  c.ExpectEq(entry->Find("content_hash")->string(),
             std::string("abd8fff93894e8112c7dd17386e54a5f"), "content_hash");
  c.ExpectEq(entry->Find("updated_at")->string(), std::string("2026-09-04T11:36:27Z"),
             "updated_at");
  c.ExpectEq(entry->Find("file_size_bytes")->integer(), std::int64_t{32768}, "file_size_bytes");

  const json::Value* device_id = document.value.Find("device_id");
  c.ExpectEq(device_id != nullptr ? device_id->string() : std::string(),
             std::string("b242014b-5774-44dc-b495-139fc7b856da"), "device_id");
}

void EncodesTheAbsentFieldsAsNull(checks::Checks& c) {
  sync::SyncNegotiatePayload payload;
  sync::ClientSaveState save = SampleSave();
  save.slot.reset();
  save.emulator.reset();
  save.content_hash.reset();
  payload.saves.push_back(save);

  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  if (!encoded.ok()) {
    c.Expect(false, "a save with no slot, emulator or hash encodes: " + encoded.error.Describe());
    return;
  }
  const json::ParseResult document = json::Parse(encoded.body);
  const json::Value* entry = Entry(c, document, 0, "the sparse body");
  if (entry == nullptr) {
    return;
  }
  for (const FieldSpec& field : kSaveFields) {
    if (!field.nullable) {
      continue;
    }
    const json::Value* value = entry->Find(field.name);
    c.Expect(value != nullptr && value->is_null(),
             std::string("an absent ") + field.name + " is sent as null, not omitted");
  }
  const json::Value* device_id = document.value.Find("device_id");
  c.Expect(device_id != nullptr && device_id->is_null(),
           "an absent device_id is sent as null -- the token then names the device");
}

void EncodesAnEmptyLibrary(checks::Checks& c) {
  const sync::Encoded encoded = sync::EncodeNegotiateRequest({});
  c.Expect(encoded.ok(), "an empty save list encodes -- it is how a client asks what it is "
                         "missing");
  const json::ParseResult document = json::Parse(encoded.body);
  if (!document.ok()) {
    c.Expect(false, "the empty body parses: " + document.error.Describe());
    return;
  }
  const json::Value* saves = document.value.Find("saves");
  c.Expect(saves != nullptr && saves->is_array() && saves->size() == 0,
           "saves is an empty array, not a missing field");
}

/// A file name RomM is entitled to store and a JSON encoder is entitled to get
/// wrong. Built by concatenation this would end the string early and turn the
/// rest of the entry into keys the server reads at face value.
void QuotesRatherThanConcatenates(checks::Checks& c) {
  sync::SyncNegotiatePayload payload;
  sync::ClientSaveState save = SampleSave();
  save.file_name = "a\"b\\c.srm";
  payload.saves.push_back(save);

  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  if (!encoded.ok()) {
    c.Expect(false, "a quoted file name encodes: " + encoded.error.Describe());
    return;
  }
  const json::ParseResult document = json::Parse(encoded.body);
  const json::Value* entry = Entry(c, document, 0, "the quoted body");
  if (entry == nullptr) {
    return;
  }
  c.ExpectEq(entry->Find("file_name")->string(), std::string("a\"b\\c.srm"),
             "the file name survives encoding unchanged");
  c.ExpectEq(entry->size(), std::size_t{std::size(kSaveFields)},
             "and did not smuggle extra fields into the entry");
}

/// The refusals. Each one is a save the server would accept and arbitrate as
/// something other than what the client meant.
void RefusesWhatItCannotSendFaithfully(checks::Checks& c) {
  struct Case {
    const char* what;
    const char* field;
    sync::ClientSaveState save;
  };

  auto with = [](auto&& mutate) {
    sync::ClientSaveState save = SampleSave();
    mutate(save);
    return save;
  };

  const Case cases[] = {
      {"a save that matched no rom", "rom_id",
       with([](sync::ClientSaveState& s) { s.rom_id = 0; })},
      {"a nameless save", "file_name",
       with([](sync::ClientSaveState& s) { s.file_name.clear(); })},
      {"a path where a name belongs", "file_name",
       with([](sync::ClientSaveState& s) { s.file_name = "saves/Game.srm"; })},
      {"a file name with a NUL in it", "file_name",
       with([](sync::ClientSaveState& s) { s.file_name = std::string("Game\0.srm", 9); })},
      {"a blank slot, which is neither a slot nor archival", "slot",
       with([](sync::ClientSaveState& s) { s.slot = ""; })},
      {"a blank emulator", "emulator",
       with([](sync::ClientSaveState& s) { s.emulator = ""; })},
      {"a SHA1 where the MD5 goes", "content_hash",
       with([](sync::ClientSaveState& s) {
         s.content_hash = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
       })},
      {"an uppercase digest", "content_hash",
       with([](sync::ClientSaveState& s) {
         s.content_hash = "ABD8FFF93894E8112C7DD17386E54A5F";
       })},
      {"a hash that is not hex", "content_hash",
       with([](sync::ClientSaveState& s) {
         s.content_hash = "zzd8fff93894e8112c7dd17386e54a5f";
       })},
      {"an unset clock", "updated_at",
       with([](sync::ClientSaveState& s) { s.updated_at = {}; })},
      {"a negative size", "file_size_bytes",
       with([](sync::ClientSaveState& s) { s.file_size_bytes = -1; })},
  };

  for (const Case& scenario : cases) {
    const json::Error error = sync::Validate(scenario.save);
    c.Expect(!error.ok(), std::string("refused: ") + scenario.what);
    c.ExpectEq(error.field, std::string(scenario.field),
               std::string("...and named the field, for ") + scenario.what);

    sync::SyncNegotiatePayload payload;
    payload.saves.push_back(SampleSave());
    payload.saves.push_back(scenario.save);
    const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
    c.Expect(!encoded.ok(), std::string("...and no body was built around it: ") + scenario.what);
    c.Expect(encoded.body.empty(), "...and the body is empty rather than partial");
    c.ExpectEq(encoded.error.field, std::string("saves[1].") + scenario.field,
               "...and the error names which entry");
  }

  // The payload's own field, which no `saves[i].` prefix belongs on.
  sync::SyncNegotiatePayload blank;
  blank.device_id = "";
  blank.saves.push_back(SampleSave());
  const sync::Encoded encoded = sync::EncodeNegotiateRequest(blank);
  c.Expect(!encoded.ok(), "refused: a blank device_id, which negotiates as nobody");
  c.ExpectEq(encoded.error.field, std::string("device_id"), "...and named the field");

  // The SHA1 case is the one a reader has to be told about by name, since the
  // server accepts it and only the sync results look wrong.
  sync::ClientSaveState sha1 = SampleSave();
  sha1.content_hash = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
  const std::string message = sync::Validate(sha1).message;
  c.Expect(message.find("SHA1") != std::string::npos && message.find("MD5") != std::string::npos,
           "the SHA1 refusal says which hash is which: " + message);

  // Values are never quoted back, on json::Error's rule: a message is for a log.
  sync::ClientSaveState named = SampleSave();
  named.file_name = "Personal Game Name.srm";
  named.rom_id = 0;
  c.Expect(sync::Validate(named).message.find("Personal") == std::string::npos,
           "a refusal does not quote the value back");
}

void FormatsTimestampsAsRomMReadsThem(checks::Checks& c) {
  struct Vector {
    std::int64_t seconds;
    const char* text;
  };
  const Vector vectors[] = {
      {1, "1970-01-01T00:00:01Z"},
      {1788521787, "2026-09-04T11:36:27Z"},  // the instant in the committed captures
      {1709208000, "2024-02-29T12:00:00Z"},  // a leap day
      {951868799, "2000-02-29T23:59:59Z"},   // the leap century that is one
      {4107542399, "2100-02-28T23:59:59Z"},  // the leap century that is not...
      {4107542400, "2100-03-01T00:00:00Z"},  // ...one second later
  };
  for (const Vector& vector : vectors) {
    c.ExpectEq(sync::FormatTimestamp(std::chrono::system_clock::from_time_t(vector.seconds)),
               std::string(vector.text), std::string("formats ") + vector.text);
  }

  // The upper bound is only checkable where system_clock can hold it: its tick
  // is implementation-defined, and a nanosecond tick runs out in 2262.
  const std::int64_t representable =
      sync::UnixSeconds(std::chrono::system_clock::time_point::max());
  if (representable >= sync::kMaxTimestampSeconds) {
    c.ExpectEq(sync::FormatTimestamp(
                   std::chrono::system_clock::from_time_t(sync::kMaxTimestampSeconds)),
               std::string("9999-12-31T23:59:59Z"), "formats the last instant it can spell");
    c.Expect(sync::FormatTimestamp(
                 std::chrono::system_clock::from_time_t(sync::kMaxTimestampSeconds + 1)).empty(),
             "refuses the one after that");
  }

  // Truncated towards the past, never rounded: a save that changed at 11:36:27.9
  // is not newer than a server copy from 11:36:28.
  const auto fractional = std::chrono::system_clock::from_time_t(1788521787) +
                          std::chrono::milliseconds{900};
  c.ExpectEq(sync::FormatTimestamp(fractional), std::string("2026-09-04T11:36:27Z"),
             "drops sub-second precision downwards");

  c.Expect(sync::FormatTimestamp({}).empty(), "refuses the epoch itself");
  c.Expect(sync::FormatTimestamp(std::chrono::system_clock::from_time_t(-1)).empty(),
           "refuses a clock that is behind the epoch");
}

}  // namespace

int main() {
  checks::Checks c;

  SnapshotPin(c);
  EncodesTheSnapshotShape(c);
  EncodesTheAbsentFieldsAsNull(c);
  EncodesAnEmptyLibrary(c);
  QuotesRatherThanConcatenates(c);
  RefusesWhatItCannotSendFaithfully(c);
  FormatsTimestampsAsRomMReadsThem(c);

  if (c.failures() != 0) {
    std::cerr << c.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "sync payload: the snapshot, the encoder and the refusals agree\n";
  return 0;
}

#include "rommsync/log.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rommsync::log {
namespace {

/// The log's own state: the sink, the ordinal, and the ring the tail is read
/// out of. One lock over all three, because a line touches all three.
///
/// A function-local static rather than a namespace-scope object so that a line
/// written from a constructor running before `main` -- which nothing here does
/// today, and which is exactly the kind of thing a later caller does by accident
/// -- initialises it rather than finding it half built.
struct State {
  std::mutex mutex;
  Sink* sink = nullptr;
  std::uint64_t written = 0;
  std::deque<Line> ring;
};

State& TheLog() {
  static State state;
  return state;
}

/// Lowercase `letter`, ASCII only. `std::tolower` takes an `int` whose domain is
/// `unsigned char` or EOF, and every one of the seven call sites below would
/// have to remember the cast.
char Lower(char letter) {
  return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t at = 0; at < a.size(); ++at) {
    if (Lower(a[at]) != Lower(b[at])) {
      return false;
    }
  }
  return true;
}

/// The keys whose *value* is a secret, whatever the syntax around them.
///
/// Matched case-insensitively and on a whole word, so a `token_expires` is not
/// mistaken for a `token`. `authorization` is here rather than only the `Bearer`
/// rule below because a header logged as `authorization: Basic …` carries a
/// credential the `Bearer` rule would walk straight past.
constexpr std::string_view kSecretKeys[] = {
    "token",      "access_token", "refresh_token", "device_code",
    "user_code",  "password",     "secret",        "authorization",
};

bool IsKeyByte(char letter) {
  return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
         (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
}

/// The word ending at `end`, walking back over key bytes. `""` when the byte
/// before `end` is not one.
std::string_view WordEndingAt(std::string_view text, std::size_t end) {
  std::size_t start = end;
  while (start > 0 && IsKeyByte(text[start - 1])) {
    --start;
  }
  return text.substr(start, end - start);
}

/// Where the value that starts at `from` ends.
///
/// A quoted value ends at its closing quote; an unquoted one at the first byte
/// that cannot be in one. The separator set is deliberately wide -- a space, a
/// comma, an ampersand, a brace, a bracket, a quote -- because this has to work
/// on a JSON body, on a query string and on a sentence a `Diagnostic` wrote,
/// and stopping too early only ever leaves *more* of the line intact.
std::size_t ValueEnd(std::string_view text, std::size_t from, char quote) {
  if (quote != '\0') {
    const std::size_t close = text.find(quote, from);
    return close == std::string_view::npos ? text.size() : close;
  }
  std::size_t at = from;
  while (at < text.size()) {
    const char letter = text[at];
    if (letter == ' ' || letter == '\t' || letter == ',' || letter == '&' || letter == '"' ||
        letter == '\'' || letter == '}' || letter == ']' || letter == '(' || letter == ')' ||
        letter == ';' || letter == '\n' || letter == '\r') {
      break;
    }
    ++at;
  }
  return at;
}

/// `scheme://user:password@host` -> `scheme://<redacted>@host`.
///
/// The userinfo is everything between `://` and the `@` that closes it, and the
/// `@` has to be found before the next `/` -- otherwise a path containing one
/// (`…/roms/me@home`) would take the host with it.
void RedactUserInfo(std::string* text) {
  std::size_t at = 0;
  while ((at = text->find("://", at)) != std::string::npos) {
    const std::size_t host = at + 3;
    std::size_t scan = host;
    std::size_t mark = std::string::npos;
    while (scan < text->size()) {
      const char letter = (*text)[scan];
      if (letter == '/' || letter == ' ' || letter == '?' || letter == '#') {
        break;
      }
      if (letter == '@') {
        mark = scan;
      }
      ++scan;
    }
    if (mark == std::string::npos) {
      at = host;
      continue;
    }
    text->replace(host, mark - host, kRedacted);
    at = host + std::strlen(kRedacted);
  }
}

/// Replace the value starting at `from` with `kRedacted`, and answer where to
/// carry on scanning from.
///
/// The tail both redactors below share: skip the spaces, notice a quote and step
/// over it, find where the value ends, and put the placeholder in. An empty
/// value is left alone -- there is no secret in one, and replacing it would
/// claim a field had held something.
std::size_t RedactValueAt(std::string* text, std::size_t from) {
  while (from < text->size() && ((*text)[from] == ' ' || (*text)[from] == '\t')) {
    ++from;
  }
  const char quote =
      from < text->size() && ((*text)[from] == '"' || (*text)[from] == '\'') ? (*text)[from]
                                                                            : '\0';
  if (quote != '\0') {
    ++from;
  }
  const std::size_t end = ValueEnd(*text, from, quote);
  if (end <= from) {
    // Advancing by one rather than by the replacement's length, which would skip
    // bytes nothing replaced.
    return from + 1;
  }
  text->replace(from, end - from, kRedacted);
  return from + std::strlen(kRedacted);
}

/// True when a `Bearer` starting at `at` is a *header value* rather than the
/// English word.
///
/// The distinction is real and this codebase produces both: an `Authorization`
/// header renders as `Authorization: Bearer <token>`, and `config::Diagnostic`
/// writes the sentence "the bearer token and every save cross the network in the
/// clear" for a plain-`http://` server. Redacting the second would delete a word
/// out of a warning a user has to read, so the rule is that a header value comes
/// straight after a separator or starts the line -- never after a word.
bool StartsAValue(std::string_view text, std::size_t at) {
  while (at > 0 && (text[at - 1] == ' ' || text[at - 1] == '\t')) {
    --at;
  }
  if (at == 0) {
    return true;
  }
  const char before = text[at - 1];
  return before == ':' || before == '=' || before == '"' || before == '\'' || before == ',' ||
         before == '{';
}

/// `Bearer <token>` -> `Bearer <redacted>`, wherever it is a header value.
void RedactBearer(std::string* text) {
  static constexpr std::string_view kBearer = "Bearer ";
  std::size_t at = 0;
  while (at + kBearer.size() <= text->size()) {
    if (!EqualsIgnoreCase(std::string_view(*text).substr(at, kBearer.size()), kBearer) ||
        !StartsAValue(*text, at)) {
      ++at;
      continue;
    }
    at = RedactValueAt(text, at + kBearer.size());
  }
}

/// `token=abc`, `"device_code": "abc"`, `password : abc` -> the same with
/// `<redacted>` in place of the value.
void RedactKeyedValues(std::string* text) {
  std::size_t at = 0;
  while (at < text->size()) {
    const char letter = (*text)[at];
    if (letter != '=' && letter != ':') {
      ++at;
      continue;
    }

    // The key is the word before the separator, with an optional closing quote
    // and any spaces between the two skipped: `"device_code": ` is one key.
    std::size_t key_end = at;
    while (key_end > 0 && ((*text)[key_end - 1] == ' ' || (*text)[key_end - 1] == '\t')) {
      --key_end;
    }
    if (key_end > 0 && ((*text)[key_end - 1] == '"' || (*text)[key_end - 1] == '\'')) {
      --key_end;
    }
    const std::string_view key = WordEndingAt(*text, key_end);

    bool secret = false;
    for (const std::string_view candidate : kSecretKeys) {
      if (EqualsIgnoreCase(key, candidate)) {
        secret = true;
        break;
      }
    }
    if (!secret) {
      ++at;
      continue;
    }

    at = RedactValueAt(text, at + 1);
  }
}

/// The largest `at <= limit` that does not fall inside a UTF-8 sequence.
///
/// A line carries a save's file name, which is the user's data and is very often
/// not ASCII. Cutting at a fixed byte count lands mid-sequence about half the
/// time for a name that reaches the bound, and the result is a stray byte that
/// renders as a replacement character in a text editor and in the overlay's
/// font. Backing off costs at most three bytes.
///
/// Continuation bytes are `10xxxxxx`; anything else starts a character. A run of
/// continuations longer than a sequence can be is malformed input, and the loop
/// stopping at `limit - 3` leaves it alone rather than eating the line.
std::size_t Utf8BoundaryAt(std::string_view text, std::size_t limit) {
  // `at == text.size()` is the clamp's own answer and there is nothing to back
  // off from there -- the whole text is being kept. It is also the one index
  // `std::string_view::operator[]` will not take: unlike `std::string`'s, it is
  // undefined at `size()`, so the bound is in the loop rather than assumed away
  // by the only call site.
  std::size_t at = limit < text.size() ? limit : text.size();
  const std::size_t floor = at > 3 ? at - 3 : 0;
  while (at > floor && at < text.size() &&
         (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
    --at;
  }
  return at;
}

/// `text` with every newline, carriage return and tab turned into a space.
///
/// One record is one line, and it is what makes the file countable with `wc -l`
/// and the tail readable in an eight-line overlay pane. A caller with several
/// lines to write uses `WriteEach`, which is the supported way to get several.
void Flatten(std::string* text) {
  for (char& letter : *text) {
    if (letter == '\n' || letter == '\r' || letter == '\t') {
      letter = ' ';
    }
  }
}

}  // namespace

const char* ToString(Level level) {
  switch (level) {
    case Level::kError:
      return "error";
    case Level::kWarn:
      return "warn";
    case Level::kInfo:
      return "info";
  }
  return "info";
}

const char* ToString(Event event) {
  switch (event) {
    case Event::kBoot:
      return "boot";
    case Event::kStoredToken:
      return "auth.token";
    case Event::kAuthRejected:
      return "auth.rejected";
    case Event::kConfigDiagnostic:
      return "config.diagnostic";
    case Event::kNoServer:
      return "config.no_server";
    case Event::kNoSaveDirs:
      return "config.no_save_dirs";
    case Event::kNetOffline:
      return "net.offline";
    case Event::kNetTls:
      return "net.tls";
    case Event::kScanSkipped:
      return "scan.skipped";
    case Event::kSyncRefused:
      return "sync.refused";
    case Event::kSyncTick:
      return "sync.tick";
    case Event::kSaveFailed:
      return "save.failed";
    case Event::kPlayFailed:
      return "play.failed";
  }
  return "unknown";
}

bool IsEvent(std::string_view tag, Event* out) {
  for (const Event event : kAllEvents) {
    if (tag == ToString(event)) {
      if (out != nullptr) {
        *out = event;
      }
      return true;
    }
  }
  return false;
}

std::string Redact(std::string_view text) {
  std::string out(text);
  // Userinfo first: it is the only rule that looks for a `:` inside something
  // the keyed rule would otherwise walk over, and running it second would leave
  // `https://me:hunter2@host` with the password already replaced by a
  // `<redacted>` the URL rule then could not recognise as userinfo.
  RedactUserInfo(&out);
  RedactBearer(&out);
  RedactKeyedValues(&out);
  return out;
}

void SetSink(Sink* sink) {
  State& state = TheLog();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.sink = sink;
}

Sink* GetSink() {
  State& state = TheLog();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.sink;
}

void Write(Level level, Event event, std::string_view detail) {
  std::string line;
  Sink* sink = nullptr;
  {
    State& state = TheLog();
    std::lock_guard<std::mutex> lock(state.mutex);

    const std::uint64_t ordinal = ++state.written;
    line = std::to_string(ordinal);
    line += ' ';
    line += ToString(level);
    line += ' ';
    line += ToString(event);
    if (!detail.empty()) {
      line += ' ';
      line += Redact(detail);
    }
    Flatten(&line);
    if (line.size() > kMaxLineBytes) {
      const std::size_t marker = std::strlen(kTruncationMarker);
      // The marker is part of the bound rather than added past it: the point of
      // the bound is what one line costs, and a line that announced its own
      // truncation by exceeding the limit would defeat it.
      line.resize(Utf8BoundaryAt(line, kMaxLineBytes > marker ? kMaxLineBytes - marker : 0));
      line += kTruncationMarker;
    }

    state.ring.push_back(Line{level, ordinal, line});
    while (state.ring.size() > kTailLines) {
      state.ring.pop_front();
    }
    sink = state.sink;
  }

  // Outside the lock, deliberately: a sink writes to an SD card, and holding the
  // log's lock across that would make every other thread's `log::Write` -- and
  // `GetLog`, which the overlay polls -- wait on the card. The cost is that two
  // threads can reach a sink at once, which the interface says and `FileSink`
  // handles.
  if (sink != nullptr) {
    sink->Write(level, line);
  }
}

void WriteEach(Level level, Event event, const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    if (!line.empty()) {
      Write(level, event, line);
    }
  }
}

void WriteEach(Level level, Event event, std::string_view text) {
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view line = text.substr(at, end - at);
    if (!line.empty()) {
      Write(level, event, line);
    }
    at = end + 1;
  }
}

std::uint64_t Tail(std::size_t count, std::vector<Line>* out) {
  State& state = TheLog();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (out != nullptr) {
    out->clear();
    const std::size_t want = count < state.ring.size() ? count : state.ring.size();
    out->reserve(want);
    for (std::size_t at = state.ring.size() - want; at < state.ring.size(); ++at) {
      out->push_back(state.ring[at]);
    }
  }
  return state.written;
}

void Reset() {
  State& state = TheLog();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.ring.clear();
  state.written = 0;
}

std::string PreviousLogPathFor(std::string_view path) { return std::string(path) + ".old"; }

FileSink::FileSink(std::string path, std::size_t max_bytes)
    : path_(std::move(path)), max_bytes_(max_bytes) {}

void FileSink::RotateLocked() {
  const std::string previous = PreviousLogPathFor(path_);
  // Horizon's rename refuses an existing destination -- the reason
  // `io::WriteAtomically`'s commit is two renames -- so the old one goes first.
  // A failure of either leaves the live file where it is, which is one file over
  // the cap rather than a lost log.
  std::remove(previous.c_str());
  if (std::rename(path_.c_str(), previous.c_str()) == 0) {
    bytes_ = 0;
  }
}

void FileSink::Write(Level, std::string_view line) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!measured_) {
    // What a previous run of this process left behind. Read once: a sysmodule
    // that started a fresh file on every boot would rotate a useful log away on
    // a console that reboots, and one that measured per line would seek the
    // whole file for every line it wrote.
    measured_ = true;
    if (std::FILE* existing = std::fopen(path_.c_str(), "rb"); existing != nullptr) {
      if (std::fseek(existing, 0, SEEK_END) == 0) {
        const long size = std::ftell(existing);
        bytes_ = size > 0 ? static_cast<std::size_t>(size) : 0;
      }
      std::fclose(existing);
    }
  }

  const std::size_t cost = line.size() + 1;  // the newline is on the card too
  if (bytes_ != 0 && bytes_ + cost > max_bytes_) {
    // Rotated *before* the line rather than after it, so the cap is a ceiling on
    // what the file holds rather than a threshold it is allowed to cross once.
    // A line that is longer than the cap on its own still goes to an empty file
    // -- `bytes_ != 0` -- because the alternative is rotating forever and
    // writing nothing.
    RotateLocked();
  }

  std::FILE* file = std::fopen(path_.c_str(), "ab");
  if (file == nullptr) {
    // Silent by contract: there is nowhere left to report this to, and the tail
    // in memory still has the line. See the header.
    return;
  }
  const std::size_t wrote = std::fwrite(line.data(), 1, line.size(), file);
  const bool newline = std::fputc('\n', file) != EOF;
  // `bytes_` counts what reached the file rather than what was offered, so a
  // partial write on a full card does not make the next rotation early.
  bytes_ += wrote + (newline ? 1 : 0);
  std::fclose(file);
}

}  // namespace rommsync::log

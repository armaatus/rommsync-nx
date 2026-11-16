#include "rommsync/server_binding.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/md5.hpp"

namespace rommsync::binding {

namespace {

/// How many `.<n>` names `Archive` tries past the bare one. A card meets a
/// second archive of one server only when it went back to that server and away
/// again; the bound is what keeps a card that never stops from filling.
constexpr int kMaxArchives = 16;

/// The longest suffix: a FAT32 name is 255, and the record's own name and a
/// `.<n>` go in front of and behind it.
constexpr std::size_t kMaxSuffix = 64;

char Lower(char byte) { return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte; }

/// A marker's contents, a trailing newline or `\r` and spaces dropped.
std::string_view Trimmed(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

std::string IdentityOf(std::string_view server_url) {
  while (!server_url.empty() && server_url.back() == '/') {
    server_url.remove_suffix(1);
  }
  // Scheme and host are case-insensitive, a path prefix is not: `/RomM` and
  // `/romm` behind one proxy may be two installations.
  const std::size_t scheme = server_url.find("://");
  const std::size_t host_end =
      scheme == std::string_view::npos ? 0 : server_url.find('/', scheme + 3);
  const std::size_t lowered = scheme == std::string_view::npos ? 0
                              : host_end == std::string_view::npos ? server_url.size()
                                                                   : host_end;
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string identity;
  identity.reserve(server_url.size());
  for (std::size_t at = 0; at < server_url.size(); ++at) {
    const auto byte = static_cast<unsigned char>(server_url[at]);
    if (byte <= 0x20 || byte >= 0x7f) {
      // One token on a header line, whatever `config.ini` let through.
      identity += '%';
      identity += kHex[byte >> 4];
      identity += kHex[byte & 0x0f];
      continue;
    }
    identity += at < lowered ? Lower(server_url[at]) : server_url[at];
  }
  return identity;
}

std::string ArchiveSuffix(std::string_view identity) {
  std::string suffix;
  for (const char byte : identity) {
    const char lower = Lower(byte);
    const bool kept = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ||
                      lower == '.' || lower == '-';
    const char next = kept ? lower : '-';
    if (next == '-' && !suffix.empty() && suffix.back() == '-') {
      continue;
    }
    suffix += next;
  }
  while (!suffix.empty() && (suffix.back() == '-' || suffix.back() == '.')) {
    suffix.pop_back();
  }
  while (!suffix.empty() && (suffix.front() == '-' || suffix.front() == '.')) {
    suffix.erase(suffix.begin());
  }
  if (suffix.size() > kMaxSuffix) {
    suffix.resize(kMaxSuffix);
  }
  // A name ending in `.old` or `.tmp` is one an interrupted commit would leave,
  // and `atomic_file`'s sweep and every loader read it as one.
  const auto ends = [&suffix](std::string_view tail) {
    return suffix.size() >= tail.size() &&
           std::string_view(suffix).substr(suffix.size() - tail.size()) == tail;
  };
  if (ends(".old") || ends(".tmp")) {
    suffix += "-";
  }
  return suffix.empty() ? std::string("unknown") : suffix;
}

Archived Archive(const std::string& path, std::string_view identity) {
  Archived archived;
  std::string from = path;
  if (!io::Exists(from)) {
    from = io::PreviousPathFor(path);
    if (!io::Exists(from)) {
      return archived;
    }
  }
  const std::string base = path + "." + ArchiveSuffix(identity);
  std::string to;
  for (int index = 0; index <= kMaxArchives && to.empty(); ++index) {
    std::string candidate = index == 0 ? base : base + "." + std::to_string(index);
    if (!io::Exists(candidate)) {
      to = std::move(candidate);
    }
  }
  if (to.empty()) {
    archived.message = from + " was not set aside: every name for it under " + base +
                       " is taken";
    return archived;
  }
  // A rename and never a copy: the bytes move whole or stay, and on Horizon a
  // rename refuses a destination that exists, which the search above ruled out.
  if (std::rename(from.c_str(), to.c_str()) != 0) {
    archived.message = from + " was not set aside as " + to + ": the rename was refused";
    return archived;
  }
  archived.moved = true;
  archived.to = std::move(to);
  return archived;
}

std::uint64_t TagOf(std::string_view identity) {
  if (identity.empty()) {
    return 0;
  }
  const std::string digest = crypto::Md5(identity);
  std::uint64_t tag = 0;
  for (std::size_t at = 0; at < 8 && at < digest.size(); ++at) {
    tag = (tag << 8) | static_cast<unsigned char>(digest[at]);
  }
  // Zero is "no server"; one identity in 2^64 gives it up for one.
  return tag == 0 ? 1 : tag;
}

Rebound BindRecords(const std::string& marker, const std::vector<std::string>& records,
                    std::string_view identity) {
  Rebound rebound;
  if (identity.empty()) {
    return rebound;
  }
  std::string text;
  io::BoundedRead read = io::ReadBounded(marker, kMaxMarkerBytes, &text);
  if (read == io::BoundedRead::kMissing) {
    // The commit window `WriteAtomically` leaves, as every record's loader reads it.
    read = io::ReadBounded(io::PreviousPathFor(marker), kMaxMarkerBytes, &text);
  }
  if (read == io::BoundedRead::kUnreadable) {
    rebound.lines.push_back(marker + " could not be read; which server the queue and play "
                                     "sessions belong to is asked again next time");
    return rebound;
  }
  const std::string current(identity);
  if (read == io::BoundedRead::kOk && Trimmed(text) == current) {
    rebound.bound = true;
    return rebound;
  }
  if (read != io::BoundedRead::kMissing) {
    // Another server's, or a marker that names none: the records were written
    // against something that is not this server.
    const std::string old(Trimmed(read == io::BoundedRead::kOk ? text : std::string()));
    rebound.rebound = true;
    rebound.previous = old;
    bool all_aside = true;
    for (const std::string& record : records) {
      const Archived archived = Archive(record, old);
      if (!archived.ok()) {
        // The marker is then left naming the old server, so the next call
        // tries again rather than reading this file as the new one's.
        all_aside = false;
        rebound.lines.push_back(archived.message);
      } else if (archived.moved) {
        rebound.lines.push_back(record + " was written against " +
                                (old.empty() ? std::string("another server") : old) +
                                ", not " + current + ", and is set aside as " + archived.to);
      }
    }
    if (!all_aside) {
      return rebound;
    }
  }
  const io::WriteResult wrote = io::WriteAtomically(marker, current + "\n");
  if (!wrote.ok()) {
    rebound.lines.push_back(marker + " was not written (" + wrote.message +
                            "); which server the records belong to is asked again next time");
    return rebound;
  }
  rebound.bound = true;
  return rebound;
}

}  // namespace rommsync::binding

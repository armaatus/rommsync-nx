// What every on-card record says about a file a later release wrote (#255).
//
// `config.ini`, `token.dat`, `conflicts.db` and `state.db` each meet one after a
// downgrade, or on a card where only one of the two halves was updated. Their
// policies differ -- refused and left as it is, or discarded and rebuilt
// (docs/ARCHITECTURE.md, "Formats across releases") -- but what happened to the
// player is the same, so it is said the same way: which file, which version,
// what this build did about it, and what to install. Four phrasings of that,
// one of which read as corruption, was the bug.
#pragma once

#include <string>
#include <string_view>

namespace rommsync::records {

/// What a player does about a record from a newer release, whatever the record.
inline constexpr const char* kNewerReleaseRemedy =
    "install sys-rommsync and the overlay from the release that wrote it";

/// "`file` is version `version`, written by a newer release than this
/// sys-rommsync, which reads version `reads`; `consequence` -- `kNewerReleaseRemedy`".
///
/// `version` is text so a caller can quote what the file said, digits past what
/// an `int` holds included. `consequence` is the record's own policy, in words.
std::string NewerRelease(std::string_view file, std::string_view version, int reads,
                         std::string_view consequence);

/// The version a `<magic> <version>[ <more>]` header line names when it is above
/// `reads`, else zero -- which is also the answer for a line that is not this
/// header at all, since that is corrupt rather than newer.
int NewerVersion(std::string_view header, std::string_view magic, int reads);

}  // namespace rommsync::records

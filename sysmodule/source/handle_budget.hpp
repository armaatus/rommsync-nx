// The kernel handles this process holds at once, and the handle table the npdm
// sizes for them (M9-11, #210).
//
// `handle_table_size` in `sys-rommsync.json` was the devkitPro template's 64,
// sized for nobody in particular. This is the count it is now derived from,
// term by term, read out of libnx 4.12.0's own `services/*.c` -- because which
// objects are handles and which are not is libnx's decision, not ours:
//
//   * **A session is a handle; a domain object is not.** libnx converts
//     `fsp-srv`, `ssl`, `nifm` and `psc:m` to domains, so an open file or
//     directory, an `SslContext`, an `SslConnection`, nifm's `IGeneralService`
//     and the `PmModule` are object ids on one session and cost nothing here.
//     What fs runs out of first is its own `ResultOpenCountLimit`.
//   * **A socket descriptor is not a handle.** It is an int in bsd's table and
//     a slot in newlib's, so a leaked socket -- the `errno == ENOENT` question
//     in `ssl_http_client.cpp` -- runs into bsd's limit, never this one.
//   * `time` is not a domain: `timeInitialize` opens five sub-sessions beside
//     `time:s` and, on 6.0.0+, maps a shared memory.
//
// The table is **twice** the count rounded up to a power of two, so the next
// service is an edit to a term here rather than a surprise on a console --
// which is what #242's `pm:dmnt` was: one session, `kPm`, and 64 still fits.
// It is not the 1023 maximum other sysmodules declare:
// sessions count against the system resource limit every system module shares,
// so a table sized for a leak lets one spend that budget, where this one fails
// the leaking call in this process first.
//
// `scripts/npdm-check.py` reads `kHandleTableSize` and fails `npdm.grants`
// (the config) and `npdm.svcs` (the built NPDM) when the npdm says otherwise.
// Change a term and the `static_assert` below says whether the table still
// fits; change the table and the npdm has to follow.
#pragma once

#include <bit>
#include <cstddef>

#include "http/ssl_http_client.hpp"
#include "ipc/server.hpp"

namespace rommsync::sysmodule::handles {

/// `__nx_fs_num_sessions`, which `main.cpp` sets from this. libnx's default is
/// 3 -- sessions cloned off the first for parallel fs commands -- and a
/// sysmodule with one card to write wants one.
inline constexpr std::size_t kFsSessions = 1;

/// main, the engine's worker, the pairing thread, and the psc watcher. The
/// three `SizedThread`s are `SdEngine::worker_thread_`, `pairing_thread_` and
/// `PscSubscription::thread_`; a fourth one is a row here.
inline constexpr std::size_t kThreads = 4;

/// The `sm` session, held for the life of the process because libnx re-opens
/// `sfdnsres` off it on every lookup (#195, `main.cpp`).
inline constexpr std::size_t kSm = 1;

/// `fsp-srv`, a domain: every file and directory on the card is an object on
/// these sessions.
inline constexpr std::size_t kFs = kFsSessions;

/// `time:s`, the user/network/steady/local clocks and the time-zone service,
/// and the shared memory 6.0.0+ maps (`_timeInitialize`).
inline constexpr std::size_t kTime = 1 + 5 + 1;

/// `psc:m`, a domain, and the event `PmModule` hands back.
inline constexpr std::size_t kPsc = 1 + 1;

/// `nifm:u`, a domain; `IGeneralService` is an object on it.
inline constexpr std::size_t kNifm = 1;

/// `bsd:u` and its clones, the monitor session, and the transfer memory
/// (`_bsdInitialize`).
inline constexpr std::size_t kBsd = SocketBudget{}.num_bsd_sessions + 1 + 1;

/// `ssl` and its clones, a domain: contexts and connections are objects.
inline constexpr std::size_t kSsl = kSslSessions;

/// The `rommsync` port and one handle per overlay session.
inline constexpr std::size_t kService = 1 + kMaxSessions;

/// `sfdnsres`, opened and closed around each lookup -- one per thread that can
/// be resolving at once, which is the worker and the pairing thread.
inline constexpr std::size_t kDnsInFlight = 2;

/// `pm:dmnt`, the foreground-application probe (M10-16, #242). One session and
/// no event: it is polled, because the event pm:dmnt could hand back is the
/// application-launch hook `dmnt:cht` holds (`main.cpp`). Naming the
/// application for a play session (#261) asks the same session, so still one.
inline constexpr std::size_t kPm = 1;

/// The peak. `set:sys` is not in it: `__appInit` closes it before anything
/// else here is open.
inline constexpr std::size_t kHeld = kThreads + kSm + kFs + kTime + kPsc + kNifm + kBsd +
                                     kSsl + kService + kDnsInFlight + kPm;

/// What `sys-rommsync.json` declares. `scripts/npdm-check.py` reads this line.
///
/// A literal, so the script can read it, held **exactly** to the rule by the
/// assert below: twice the count, rounded up to a power of two. Exactly, and
/// not "at least", because a floor is what a widening passes -- 1023 here and in
/// the json together would satisfy every other check there is.
inline constexpr std::size_t kHandleTableSize = 64;

static_assert(kHeld == 29, "the count moved: update sysmodule/README.md's handle row with it");
static_assert(kHandleTableSize == std::bit_ceil(2 * kHeld),
              "kHandleTableSize is not twice the handles this process holds, rounded up to a "
              "power of two; change it and handle_table_size in sys-rommsync.json together "
              "(#210)");
static_assert(kHandleTableSize <= 1023, "handle_table_size is a 10-bit field");

}  // namespace rommsync::sysmodule::handles

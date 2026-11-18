// Stages 1 and 2: what this console is, and what its SD card does.
//
// Every path here is under `kRoot` (selftest.hpp). The card rows write, rename,
// back up and read back files in the run's own scratch folder; none of them
// opens a save, an emulator folder, or the sysmodule's `/config/rommsync/`.
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cstdio>
#include <ctime>
#include <string>

#include "rommsync/atomic_file.hpp"
#include "rommsync/disk.hpp"
#include "card.hpp"
#include "platform_facts.hpp"
#include "rommsync/sync_execute.hpp"
#include "selftest.hpp"

namespace selftest {
namespace {

using rommsync::fs::Format;

// Enough room for a run: a fixture save, a homebrew rom twice, its backups.
constexpr std::uint64_t kFreeFloor = 64ull * 1024 * 1024;

// One past FAT32's largest file. Created empty-but-sized through fs directly:
// FAT32 refuses it at once, exFAT allocates it without writing a byte.
constexpr s64 kPastFat32 = 0x100000000LL + 1;

std::string Quoted(const std::string& text) { return "\"" + text + "\""; }

/// The sysmodule's own reading of the card (main.cpp's `CardFormat`), which is
/// what decides the 4 GiB ceiling it holds downloads to: Horizon will not say
/// which filesystem a card has, only whether the console could read exFAT.
Format SysmoduleCardFormat() {
  bool exfat = false;
  if (R_FAILED(fsIsExFatSupported(&exfat))) return Format::kUnknown;
  return exfat ? Format::kUnknown : Format::kFat32;
}

/// The card's actual filesystem, by asking it for a file FAT32 cannot hold.
/// "unknown" when the answer is the card being too full to try.
std::string ProbeCardFormat(const std::string& scratch_sd, std::string* detail) {
  FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
  if (sd == nullptr) {
    *detail = "no sdmc device";
    return "unknown";
  }
  const std::string probe = scratch_sd + "/past-fat32.bin";
  char path[FS_MAX_PATH] = {};
  std::snprintf(path, sizeof(path), "%s", probe.c_str());
  const Result rc = fsFsCreateFile(sd, path, kPastFat32, 0);
  if (R_SUCCEEDED(rc)) {
    fsFsDeleteFile(sd, path);
    *detail = "a 4 GiB + 1 file was created (and deleted): exFAT";
    return "exfat";
  }
  struct statvfs volume = {};
  const bool room = statvfs("sdmc:/", &volume) == 0 &&
                    static_cast<std::uint64_t>(volume.f_bavail) * volume.f_frsize >
                        static_cast<std::uint64_t>(kPastFat32);
  char text[160];
  std::snprintf(text, sizeof(text), "a 4 GiB + 1 file was refused, rc=0x%08x%s", rc,
                room ? ", with room for it: FAT32" : ", and the card has no room to tell");
  *detail = text;
  return room ? "fat32" : "unknown";
}

/// One of Atmosphere's config items, in the shape the sysmodule's boot read
/// keeps it (platform_facts.hpp), so both say the same thing about a failure.
rommsync::sysmodule::platform::ConfigRead ReadConfig(std::uint32_t item, const char* name) {
  rommsync::sysmodule::platform::ConfigRead read = {nullptr, 0, 0, false};
  u64 value = 0;
  const Result rc = splGetConfig(static_cast<SplConfigItem>(item), &value);
  if (R_SUCCEEDED(rc)) {
    read.value = value;
    read.read = true;
  } else {
    read.failed = name;
    read.result = rc;
  }
  return read;
}

/// The console's zone offset from UTC, in seconds, at `now`.
std::optional<long long> ZoneOffset(std::time_t now) {
  TimeCalendarTime local = {};
  TimeCalendarAdditionalInfo info = {};
  if (R_FAILED(timeToCalendarTimeWithMyRule(static_cast<u64>(now), &local, &info))) {
    return std::nullopt;
  }
  std::tm as_utc = {};
  as_utc.tm_year = local.year - 1900;
  as_utc.tm_mon = local.month - 1;
  as_utc.tm_mday = local.day;
  as_utc.tm_hour = local.hour;
  as_utc.tm_min = local.minute;
  as_utc.tm_sec = local.second;
  // timegm's arithmetic, without timegm: days from the civil date.
  const int y = as_utc.tm_year + 1900 - (as_utc.tm_mon < 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int m = as_utc.tm_mon + 1;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + as_utc.tm_mday - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = static_cast<long long>(era) * 146097 + doe - 719468;
  const long long seconds =
      days * 86400 + as_utc.tm_hour * 3600 + as_utc.tm_min * 60 + as_utc.tm_sec;
  return seconds - static_cast<long long>(now);
}

// The commit hook the sysmodule installs (main.cpp's `HorizonFileSync`), and
// what it answered: the card.commit_device row reads these.
int g_commits = 0;
int g_commit_failures = 0;
Result g_last_commit = 0;

bool HorizonFileSync(const std::string&) {
  ++g_commits;
  g_last_commit = fsdevCommitDevice("sdmc:");
  if (R_FAILED(g_last_commit)) ++g_commit_failures;
  return R_SUCCEEDED(g_last_commit);
}

}  // namespace

void InstallCommitHook() { rommsync::io::SetFileSync(&HorizonFileSync); }

void RunPlatform(Run& run) {
  Report& report = *run.report;

  SetSysFirmwareVersion firmware = {};
  std::string hos = "unknown";
  if (R_SUCCEEDED(setsysInitialize())) {
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware))) hos = firmware.display_version;
    setsysExit();
  }
  report.Row("platform.hos", hos != "unknown" ? Outcome::kPass : Outcome::kFail,
             "HOS " + hos, Quoted(hos));

  namespace platform = rommsync::sysmodule::platform;
  platform::ConfigRead api = {nullptr, 0, 0, false};
  platform::ConfigRead emummc_type = {nullptr, 0, 0, false};
  if (R_SUCCEEDED(splInitialize())) {
    api = ReadConfig(platform::kExosphereApiVersion, "splGetConfig(ExosphereApiVersion)");
    emummc_type = ReadConfig(platform::kExosphereEmummcType, "splGetConfig(ExosphereEmummcType)");
    splExit();
  }
  const std::string ams = api.read ? platform::AtmosphereVersion(api) : "unknown";
  const bool emummc = emummc_type.read && emummc_type.value != 0;
  report.Row("platform.ams", api.read ? Outcome::kPass : Outcome::kFail,
             "Atmosphere " + platform::AtmosphereVersion(api), Quoted(ams));
  report.Row("platform.emummc", emummc_type.read ? Outcome::kPass : Outcome::kFail,
             platform::Emummc(emummc_type),
             emummc_type.read ? (emummc ? "true" : "false") : "null");

  std::string why;
  run.sd_format = ProbeCardFormat(run.scratch_sd, &why);
  const Format sysmodule = SysmoduleCardFormat();
  report.Row("platform.sd_format", run.sd_format != "unknown" ? Outcome::kPass : Outcome::kFail,
             why + "; the sysmodule reads " + rommsync::fs::ToString(sysmodule),
             Quoted(run.sd_format));
  report.Console(hos, ams, emummc, run.sd_format);

  run.card = rommsync::sysmodule::MakeSdCard(sysmodule);
  const rommsync::fs::Volume volume = run.card->VolumeOf(kRootSd);
  const bool room = volume.free_bytes >= 0 &&
                    static_cast<std::uint64_t>(volume.free_bytes) >= kFreeFloor;
  report.Row("platform.free_space", room ? Outcome::kPass : Outcome::kFail,
             volume.free_bytes < 0 ? std::string("the card did not say")
                                   : Number(volume.free_bytes / (1024 * 1024)) + " MiB free",
             Number(volume.free_bytes));

  // platform.clock needs the server's Date header: RunNetwork writes it.

  // st_mtime against a known UTC instant (SYNC_PROTOCOL.md#the-consoles-clock-295).
  const std::string probe = run.card->Resolve(run.scratch_sd + "/mtime.txt");
  const std::time_t before = std::time(nullptr);
  const rommsync::io::WriteResult wrote = rommsync::io::WriteAtomically(probe, "mtime\n");
  struct stat info = {};
  const std::optional<long long> offset = ZoneOffset(before);
  if (!wrote.ok() || stat(probe.c_str(), &info) != 0) {
    report.Row("platform.mtime_utc", Outcome::kFail, "could not write and stat " + probe);
  } else if (!offset) {
    report.Row("platform.mtime_utc", Outcome::kFail, "the console's time zone could not be read");
  } else {
    const long long skew = static_cast<long long>(info.st_mtime) - before;
    const std::string value = "{\"mtime_minus_utc\": " + Number(skew) +
                              ", \"zone_offset\": " + Number(*offset) + "}";
    const bool utc = skew > -120 && skew < 120;
    const bool local = *offset != 0 && skew - *offset > -120 && skew - *offset < 120;
    if (*offset > -3600 && *offset < 3600) {
      report.Row("platform.mtime_utc", Outcome::kFail,
                 "the console's zone is within an hour of UTC, so UTC and local cannot be told "
                 "apart; set one far from it (CLK-04) and run again",
                 value);
    } else if (utc) {
      report.Row("platform.mtime_utc", Outcome::kPass,
                 "st_mtime is UTC (zone offset " + Number(*offset) + " s)", value);
    } else {
      report.Row("platform.mtime_utc", Outcome::kFail,
                 local ? "st_mtime is LOCAL time: file it against #295"
                       : "st_mtime is " + Number(skew) + " s from now, neither UTC nor local",
                 value);
    }
  }
}

void RunCard(Run& run) {
  Report& report = *run.report;
  rommsync::fs::FileSystem& card = *run.card;
  const std::string dir_sd = run.scratch_sd + "/card";
  card.CreateDirectory(dir_sd);

  // 1. Write, rename over, read back -- io::WriteAtomically, as every config,
  //    token and state file the sysmodule keeps is written.
  const std::string file = card.Resolve(dir_sd + "/atomic.txt");
  const int commits_before = g_commits;
  rommsync::io::WriteResult first = rommsync::io::WriteAtomically(file, "first\n");
  rommsync::io::WriteResult second = rommsync::io::WriteAtomically(file, "second\n");
  const rommsync::io::ReadResult read = rommsync::io::ReadFile(file);
  const bool no_temp = !rommsync::io::Exists(rommsync::io::TempPathFor(file));
  const bool atomic = first.ok() && second.ok() && read.ok() && read.contents == "second\n";
  report.Row("card.atomic_write", atomic && no_temp ? Outcome::kPass : Outcome::kFail,
             !first.ok()    ? "first write: " + first.message
             : !second.ok() ? "overwrite: " + second.message
             : !read.ok()   ? "read back: " + read.message
             : !atomic      ? std::string("read back something other than the overwrite")
             : !no_temp     ? std::string("the .tmp was left beside it")
                            : std::string("written, overwritten by rename, read back"));

  // 2. The commit those writes asked for, through the sysmodule's own hook.
  const int commits = g_commits - commits_before;
  char text[160];
  std::snprintf(text, sizeof(text), "%d commit(s), %d failed, last rc=0x%08x", commits,
                g_commit_failures, g_last_commit);
  report.Row("card.commit_device",
             commits > 0 && g_commit_failures == 0 ? Outcome::kPass : Outcome::kFail, text);

  // 3. The save-overwrite sequence (hard rule 2): back up first, then commit
  //    the new bytes over the old -- sync::BackUpFirst, the function every
  //    download in a tick calls, into this run's own backup folder.
  const std::string save_sd = dir_sd + "/fixture.srm";
  const std::string save = card.Resolve(save_sd);
  const std::string backup_dir_sd = run.scratch_sd + "/backup";
  std::string backup_sd;
  std::string message;
  const bool seeded = rommsync::io::WriteAtomically(save, "old save\n").ok();
  const rommsync::sync::OperationError backed = rommsync::sync::BackUpFirst(
      card, backup_dir_sd, save_sd, run.config.rom_id, "", "fixture.srm",
      static_cast<std::int64_t>(std::time(nullptr)), &backup_sd, &message);
  const bool overwrote =
      backed == rommsync::sync::OperationError::kNone &&
      rommsync::io::WriteAtomically(save, "new save\n").ok();
  const rommsync::io::ReadResult kept =
      backup_sd.empty() ? rommsync::io::ReadResult{} : rommsync::io::ReadFile(card.Resolve(backup_sd));
  const rommsync::io::ReadResult now = rommsync::io::ReadFile(save);
  const bool sequence = seeded && overwrote && kept.ok() && kept.contents == "old save\n" &&
                        now.ok() && now.contents == "new save\n";
  report.Row("card.backup_then_commit", sequence ? Outcome::kPass : Outcome::kFail,
             sequence ? "backed up to " + backup_sd + ", then overwritten"
             : backed != rommsync::sync::OperationError::kNone
                 ? std::string("BackUpFirst: ") + rommsync::sync::ToString(backed) + " " + message
                 : std::string("the backup or the overwrite did not read back"));

  // 4. FAT32's ceiling: the card backend has to hold a download past 4 GiB to
  //    it before a byte is written (download.cpp's RoomOnTheCard reads this).
  const rommsync::fs::Volume volume = card.VolumeOf(dir_sd);
  const bool capped = volume.max_file_bytes != 0 &&
                      volume.max_file_bytes <= rommsync::fs::kFat32MaxFileBytes;
  const std::string ceiling = "the card backend's ceiling is " +
                              (volume.max_file_bytes == 0 ? std::string("none")
                                                          : Number(static_cast<long long>(
                                                                volume.max_file_bytes)));
  if (run.sd_format == "fat32") {
    report.Row("card.fat32_ceiling", capped ? Outcome::kPass : Outcome::kFail,
               ceiling + (capped ? ": a rom past 4 GiB is refused before writing"
                                 : ": a rom past 4 GiB would be written into a wall"));
  } else if (run.sd_format == "exfat") {
    report.Row("card.fat32_ceiling", Outcome::kNa, "exFAT card; " + ceiling);
  } else {
    report.Row("card.fat32_ceiling", Outcome::kFail, "the card's format is unknown; " + ceiling);
  }

  // 5. A long, non-ASCII path: a Japanese title's save folder, near the
  //    lengths RetroArch's own names reach.
  const std::string long_dir_sd =
      dir_sd + "/セーブデータ-Pokémon Édition Spéciale-ゼルダの伝説 夢をみる島 DX-" +
      std::string(60, 'x');
  const std::string leaf = "冒険の記録 (Europe) (En,Fr,De,Es,It) [ÅÄÖ].srm";
  const bool made = card.CreateDirectory(long_dir_sd).ok();
  const std::string long_file = card.Resolve(long_dir_sd + "/" + leaf);
  const bool wrote = made && rommsync::io::WriteAtomically(long_file, "unicode\n").ok();
  const rommsync::io::ReadResult back = rommsync::io::ReadFile(long_file);
  bool listed = false;
  const rommsync::fs::Listing listing = card.List(long_dir_sd);
  for (const rommsync::fs::Entry& entry : listing.entries) listed |= entry.name == leaf;
  const bool round_trip = wrote && back.ok() && back.contents == "unicode\n" && listed;
  report.Row("card.long_unicode_path", round_trip ? Outcome::kPass : Outcome::kFail,
             !made          ? std::string("the folder could not be made")
             : !wrote       ? std::string("the file could not be written")
             : !back.ok()   ? "read back: " + back.message
             : !listed      ? std::string("the listing does not name it as written")
                            : Number(static_cast<long long>(long_file.size())) +
                                  "-byte path written, listed and read back");
}

}  // namespace selftest

#include "card_probe.hpp"

#include <sys/stat.h>

#include <string>
#include <string_view>

#include "rommsync/config.hpp"

namespace rommsync::overlay {
namespace {

/// Where Atmosphère looks for a sysmodule, with this one's id already in it.
///
/// Built per call rather than cached in a static: the prefix is a parameter now
/// (`ProbeCardAt`), and a static would answer the first caller's prefix to every
/// later one. It is three `stat`s every `kPollsBetweenProbes` polls
/// (`status_screen.cpp`), so the string is not what costs anything here.
std::string ContentsDir(std::string_view prefix) {
  return std::string(prefix) + "/atmosphere/contents/" + kProgramIdHex + "/";
}

/// `stat` rather than `fopen`: `exefs.nsp` is the whole sysmodule and opening it
/// to learn that it is there would read a file this overlay has no use for.
bool Present(const std::string& path) {
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0;
}

}  // namespace

CardState ProbeCard() { return ProbeCardAt("sdmc:"); }

CardState ProbeCardAt(std::string_view prefix) {
  const std::string contents = ContentsDir(prefix);
  CardState card;
  card.installed = Present(contents + "exefs.nsp");
  // Not the same question. Atmosphère loads `exefs.nsp` and ignores this file;
  // ovl-sysmodules reads this file and ignores `exefs.nsp`, so an install with
  // one and not the other boots and is absent from the list
  // (`overlay_status_view.hpp`).
  card.listable = Present(contents + "toolbox.json");
  card.set_to_boot = Present(contents + "flags/boot2.flag");

  // The configuration as the card holds it, which is not the same question as
  // "what is the sysmodule running": there is no sysmodule. `LoadConfig` never
  // fails -- a missing or unparseable file is the defaults plus a diagnostic --
  // so presence is asked separately, and a file that is not there reports
  // nothing rather than reporting the default `enabled = true` as the user's
  // setting.
  //
  // It is asked of the *file*, so the instant between `io::WriteAtomically`'s
  // two renames reports nothing even though `LoadConfig` would recover from
  // `config.ini.old`. That is one poll, on a screen that is already saying the
  // sysmodule is not running -- and the alternative is this file knowing about a
  // commit protocol so it can report one line sooner.
  // `config::kConfigSdPath` rather than a second spelling of the directory: the
  // sysmodule joins its own `sdmc:` prefix to the same file name, and a path
  // typed here as well is one the two halves come to disagree about.
  const std::string settings = std::string(prefix) + config::kConfigSdPath;
  if (Present(settings)) {
    card.config_read = true;
    card.sync_enabled = config::LoadConfig(settings).value.sync.enabled;
  }
  return card;
}

}  // namespace rommsync::overlay

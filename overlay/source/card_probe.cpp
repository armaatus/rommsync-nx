#include "card_probe.hpp"

#include <sys/stat.h>

#include <string>

#include "rommsync/config.hpp"

namespace rommsync::overlay {
namespace {

/// Where Atmosphère looks for a sysmodule, with this one's id already in it.
const std::string& ContentsDir() {
  static const std::string dir =
      std::string("sdmc:/atmosphere/contents/") + kProgramIdHex + "/";
  return dir;
}

/// `stat` rather than `fopen`: `exefs.nsp` is the whole sysmodule and opening it
/// to learn that it is there would read a file this overlay has no use for.
bool Present(const std::string& path) {
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0;
}

}  // namespace

CardState ProbeCard() {
  CardState card;
  card.installed = Present(ContentsDir() + "exefs.nsp");
  card.set_to_boot = Present(ContentsDir() + "flags/boot2.flag");

  // The configuration as the card holds it, which is not the same question as
  // "what is the sysmodule running": there is no sysmodule. `LoadConfig` never
  // fails -- a missing or unparseable file is the defaults plus a diagnostic --
  // so presence is asked separately, and a file that is not there reports
  // nothing rather than reporting the default `enabled = true` as the user's
  // setting.
  const std::string settings =
      std::string("sdmc:/config/rommsync/") + config::kConfigFileName;
  if (Present(settings)) {
    card.config_read = true;
    card.sync_enabled = config::LoadConfig(settings).value.sync.enabled;
  }
  return card;
}

}  // namespace rommsync::overlay

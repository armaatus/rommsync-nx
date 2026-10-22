#include "rommsync/newer_release.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace rommsync::records {

std::string NewerRelease(std::string_view file, std::string_view version, int reads,
                         std::string_view consequence) {
  std::string out(file);
  out += " is version ";
  out += version;
  out += ", written by a newer release than this sys-rommsync, which reads version ";
  out += std::to_string(reads);
  out += "; ";
  out += consequence;
  out += " -- ";
  out += kNewerReleaseRemedy;
  return out;
}

int NewerVersion(std::string_view header, std::string_view magic, int reads) {
  if (header.size() <= magic.size() || header.substr(0, magic.size()) != magic ||
      header[magic.size()] != ' ') {
    return 0;
  }
  int version = 0;
  std::size_t at = magic.size() + 1;
  for (; at < header.size() && header[at] >= '0' && header[at] <= '9'; ++at) {
    if (version > 100000) {
      return 0;  // not a version anybody shipped; corrupt, and discarded as such
    }
    version = version * 10 + (header[at] - '0');
  }
  if (at == magic.size() + 1 || (at < header.size() && header[at] != ' ')) {
    return 0;
  }
  return version > reads ? version : 0;
}

}  // namespace rommsync::records

#include "rommsync/server_binding.hpp"

namespace rommsync::binding {

std::string IdentityOf(std::string_view server_url) { return std::string(server_url); }

std::string ArchiveSuffix(std::string_view) { return "unknown"; }

Archived Archive(const std::string&, std::string_view) { return {}; }

Rebound BindRecords(const std::string&, const std::vector<std::string>&, std::string_view) {
  return {};
}

}  // namespace rommsync::binding

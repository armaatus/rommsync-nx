// Read `remote_install.hpp` first -- the exchanges and why there are three.
#include "rommsync/remote_install.hpp"

namespace rommsync::install {

auth::Parsed<ClaimPage> ParseClaim(std::string_view, std::size_t) { return {}; }

const char* ToString(Outcome) { return ""; }

std::optional<Verdict> VerdictFor(const download::QueueEntry&) { return std::nullopt; }

std::string CapabilitiesBody(bool) { return {}; }

std::string ReportBody(const Verdict&) { return {}; }

bool Exchange::transient() const { return false; }

Exchange Register(http::HttpClient&, std::string_view, const auth::StoredToken&, bool,
                  const sync::CallPolicy&) {
  return {};
}

Claimed Claim(http::HttpClient&, std::string_view, const auth::StoredToken&,
              const sync::CallPolicy&, std::size_t) {
  return {};
}

Exchange Report(http::HttpClient&, std::string_view, const auth::StoredToken&, std::string_view,
                const Verdict&, const sync::CallPolicy&) {
  return {};
}

}  // namespace rommsync::install

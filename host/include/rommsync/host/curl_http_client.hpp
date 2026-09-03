// The native HttpClient backend, built with the host toolchain only.
//
// This is the desktop/CI half of the ladder in docs/TESTING.md: the same engine
// sources that will run on Horizon, talking to a real RomM over a real socket,
// on a laptop. Nothing under core/ may include this header -- the engine knows
// only rommsync/http.hpp, and this is the one place libcurl is named.
#pragma once

#include <memory>

#include "rommsync/http.hpp"

namespace rommsync::host {

/// A libcurl-backed `HttpClient`. Never returns null.
///
/// The returned client is safe to share between threads; each request runs on
/// its own handle. libcurl's global initialisation happens once, on the first
/// call, so a caller does not have to remember to do it.
std::unique_ptr<http::HttpClient> MakeCurlHttpClient(
    const http::ClientOptions& options = {});

}  // namespace rommsync::host

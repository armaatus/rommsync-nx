# host/ — desktop backends for the interfaces `core/` declares

`core/` says *what* the engine needs (`rommsync/http.hpp`); this directory says
*how* it happens on a laptop and in CI. Today that is one thing: the libcurl
`HttpClient`. The Horizon equivalent will live in `sysmodule/`.

- **Never built for the Switch.** The devkitPro Makefiles compile `core/` and
  their own sources; they must not reach in here. That separation is what keeps
  "nothing in `core/` may include a host-only header" enforceable rather than
  aspirational.
- **Nothing in `core/` may include a header from here.** The dependency points
  one way: `host/` knows about `core/`, never the reverse.
- This is the only place in the tree allowed to name a concrete HTTP or TLS
  library. If you find yourself wanting `#include <curl/curl.h>` anywhere else,
  the thing you are writing belongs behind an interface instead.
- Sources are globbed by `host/CMakeLists.txt` — add a `.cpp`, no CMake edit.

Layout: `include/rommsync/host/` for public headers, `src/` for implementation.
Tests live in `tests/` and link `rommsync::host`.

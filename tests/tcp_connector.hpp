// The plain-TCP `Connector` the console's HTTP client is driven through here.
//
// There is nothing in this file but a `using`, and that is the point (M1-7,
// #126). `PlainConnector` is the *shipping* connector -- the one
// `SslConnector` delegates to for an `http://` origin -- and it lives in
// `sysmodule/source/http/posix_connection.cpp`, which names no libnx type and so
// is compiled by CMake as well (`sysmodule/AGENTS.md`). A copy of it here would
// have meant `wire.*` proving a twin of the code that ships rather than the code
// that ships.
//
// What the console does that this cannot reach is the `ssl` layer above the
// socket (`ssl_http_client.cpp`), which nothing off a console can execute --
// that is the M8-1 gate's (#43), and it is the whole of the difference.
#pragma once

#include "http/posix_connection.hpp"

namespace rig {

using TcpConnector = rommsync::sysmodule::PlainConnector;

}  // namespace rig

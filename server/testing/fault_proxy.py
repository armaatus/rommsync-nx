#!/usr/bin/env python3
"""Fault-injecting HTTP proxy in front of the real RomM fixture.

The Docker RomM in ``docker-compose.yml`` is the primary test rig, because when
a test passes against a real RomM the behaviour is real. But a *healthy* RomM
never returns 401 in the middle of a sync, never truncates a body, and never
drops a connection half way through a ``Range`` download -- and those are exactly
the paths where a save file gets corrupted.

This proxy closes that gap without giving up fidelity. It forwards every request
to the real RomM untouched, and only when a scenario is armed does it damage one
specific thing about a genuine response. It never synthesises a RomM response of
its own, so it cannot drift from RomM the way a hand-written mock would.

``stall`` is the one mode that does not forward at all, and deliberately: see
``seconds`` below. The other three damage a real response; a stall models a
server that never produced one.

Control API (not forwarded upstream)::

    GET    /__fault          -> the armed scenario, or null
    POST   /__fault          -> arm a scenario (JSON body)
    DELETE /__fault          -> disarm

Scenario fields::

    mode      status | truncate | drop | stall   (required)
    status    HTTP status to return              (mode=status, default 401)
    body      response body                      (mode=status, default "")
    after     pass this many matching requests
              through untouched first            (default 0)
    count     apply to this many requests, then
              auto-disarm                        (default 1)
    path      only match requests whose path
              starts with this prefix            (default: all)
    bytes     cut the body after this many bytes (modes truncate/drop).
              ``drop`` still sends the real Content-Length and then resets, the
              way a genuinely dropped transfer looks; ``truncate`` sends no
              length at all, so only the caller's own expected size can catch it
    seconds   hold the connection open this long and then drop it, WITHOUT
              forwarding -- a stalled request never reaches RomM
                                                (mode=stall, default 30)

Example -- make the 3rd call to /api/sync/negotiate fail with 401, once::

    curl -XPOST $PROXY_BASE_URL/__fault -d '{
      "mode": "status", "status": 401,
      "path": "/api/sync/negotiate", "after": 2
    }'
"""

from __future__ import annotations

import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UPSTREAM = os.environ.get("UPSTREAM", "http://127.0.0.1:8080").rstrip("/")
LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "8080"))
CONTROL_PATH = "/__fault"
CHUNK = 64 * 1024

# Hop-by-hop headers must not be forwarded (RFC 9110 s7.6.1). Content-Length and
# Transfer-Encoding are dropped separately because we re-frame the body.
_SKIP_RESPONSE_HEADERS = {
    "connection",
    "content-encoding",
    "content-length",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
}


def _reset(connection) -> None:
    """Abort a connection with a TCP RST rather than a graceful FIN.

    Half of what separates ``drop`` from ``truncate``; the other half is that
    ``drop`` keeps the real ``Content-Length`` (see ``_stream``). Together they
    make a mid-transfer failure unmistakable -- the client is owed bytes and the
    connection is gone -- where ``truncate`` deliberately leaves a clean, short,
    plausible response that no transport can fault. SO_LINGER with a zero
    timeout is what turns close() into a reset.
    """
    try:
        connection.setsockopt(
            socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
        )
        connection.close()
    except OSError:
        pass


class Fault:
    """The armed scenario, shared across worker threads."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._spec: dict | None = None
        self._seen = 0

    def arm(self, spec: dict) -> None:
        with self._lock:
            self._spec = spec
            self._seen = 0

    def disarm(self) -> None:
        with self._lock:
            self._spec = None
            self._seen = 0

    def peek(self) -> dict | None:
        with self._lock:
            return dict(self._spec) if self._spec else None

    def claim(self, path: str) -> dict | None:
        """Return the scenario if this request should be damaged, else None.

        Counts only requests that match ``path``, so an ``after`` of 2 means
        "the third matching request" regardless of unrelated traffic.
        """
        with self._lock:
            spec = self._spec
            if not spec:
                return None
            prefix = spec.get("path")
            if prefix and not path.startswith(prefix):
                return None

            self._seen += 1
            index = self._seen - 1
            after = int(spec.get("after", 0))
            if index < after:
                return None

            applied = index - after + 1
            if applied >= int(spec.get("count", 1)):
                self._spec = None
                self._seen = 0
            return dict(spec)


FAULT = Fault()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "rommsync-fault-proxy/1"

    # -- plumbing ----------------------------------------------------------
    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        sys.stderr.write("[fault-proxy] %s\n" % (fmt % args))

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    # -- control API -------------------------------------------------------
    def _control(self, method: str) -> None:
        if method == "GET":
            payload = json.dumps(FAULT.peek()).encode()
        elif method == "DELETE":
            FAULT.disarm()
            payload = b'{"armed": null}'
        else:
            try:
                spec = json.loads(self._read_body() or b"{}")
            except json.JSONDecodeError as exc:
                self._respond(400, json.dumps({"error": str(exc)}).encode())
                return
            mode = spec.get("mode")
            if mode not in {"status", "truncate", "drop", "stall"}:
                self._respond(400, b'{"error": "unknown mode"}')
                return
            # Without a byte count there is nothing to cut, so the fault would
            # relay the whole body untouched *and* consume itself -- a test that
            # looks green having exercised nothing. Refuse it instead.
            if mode in {"truncate", "drop"} and not isinstance(spec.get("bytes"), int):
                self._respond(400, b'{"error": "truncate and drop require an integer \'bytes\'"}')
                return
            FAULT.arm(spec)
            payload = json.dumps({"armed": spec}).encode()
        self._respond(200, payload, "application/json")

    def _respond(self, status: int, body: bytes, ctype: str = "application/json",
                 write_body: bool = True) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if write_body:  # a HEAD response carries the headers but no content
            self.wfile.write(body)

    # -- proxying ----------------------------------------------------------
    def _proxy(self, method: str) -> None:
        if self.path.split("?", 1)[0] == CONTROL_PATH:
            self._control(method)
            return

        # Drain the request body BEFORE any fault can short-circuit. On a
        # keep-alive connection unread bytes are parsed as the next request
        # line, so a 401 armed on a POST endpoint would desync the connection
        # instead of testing the 401 — and every interesting endpoint here
        # (/api/sync/negotiate, /api/auth/device/token) is a POST.
        body = self._read_body()

        fault = FAULT.claim(self.path)

        if fault and fault["mode"] == "stall":
            # Hold the connection, then drop it -- and never forward. This is
            # the one mode whose request must not reach RomM, and the `return`
            # is the whole fix for issue #109.
            #
            # Every caller of `stall` sets a client timeout well under the
            # sleep, because what each of them asserts is that the CLIENT gives
            # up first. So by the time this wakes, the request has been
            # abandoned: nobody is waiting for the answer, and the test that
            # armed it has usually exited. Forwarding it there replayed an
            # abandoned POST into whatever test was running by then. Measured,
            # by `harness.stall_dropped`: an `/api/saves` that wrote a save row
            # and an `/api/sync/negotiate` that opened a session, each landing
            # exactly `seconds` after a client that had already given up. None
            # of that is a stalled server; it is a second client nothing in the
            # suite can see.
            #
            # Which flakes that explains is a separate question and an open one
            # -- see #109. This is a defect on its own terms either way.
            #
            # Nothing observes what happens after the sleep, so there is no
            # fidelity to lose: dropping the connection is what a server that
            # accepted a connection and then said nothing finally does.
            time.sleep(float(fault.get("seconds", 30)))
            self.close_connection = True
            return
        if fault and fault["mode"] == "status":
            payload = str(fault.get("body", "")).encode()
            self._respond(int(fault.get("status", 401)), payload,
                          write_body=method != "HEAD")
            return

        headers = {
            k: v for k, v in self.headers.items()
            if k.lower() not in {"host", "content-length", "accept-encoding"}
        }
        request = urllib.request.Request(
            UPSTREAM + self.path, data=body or None, headers=headers, method=method
        )

        try:
            upstream = urllib.request.urlopen(request, timeout=60)
        except urllib.error.HTTPError as exc:
            upstream = exc  # an error response is still a real response: forward it
        except urllib.error.URLError as exc:
            self._respond(502, json.dumps({"error": f"upstream: {exc.reason}"}).encode())
            return

        with upstream:
            self._stream(upstream, fault)

    def _stream(self, upstream, fault: dict | None) -> None:
        """Relay the upstream response, optionally cutting the body short."""
        limit = int(fault["bytes"]) if fault and "bytes" in fault else None
        cutting = fault is not None and fault["mode"] in {"truncate", "drop"}

        self.send_response(upstream.status)
        for key, value in upstream.headers.items():
            if key.lower() not in _SKIP_RESPONSE_HEADERS:
                self.send_header(key, value)

        length = upstream.headers.get("Content-Length")
        if cutting:
            self.close_connection = True
            if fault["mode"] == "drop" and length is not None:
                # Keep the promise the real server made. A connection that dies
                # mid-transfer is a server that said "N bytes" and delivered
                # fewer -- dropping Content-Length here would instead look to
                # the client like a complete, shorter response, and the reset
                # would be indistinguishable from a clean close. That is the one
                # thing this fault exists to prove a downloader notices.
                self.send_header("Content-Length", length)
            else:
                # `truncate` is the contrast case on purpose: a clean short
                # close with nothing to compare against, which no transport can
                # detect. Only the caller's own size/hash check catches it.
                self.send_header("Transfer-Encoding", "identity")
        elif length is not None:
            self.send_header("Content-Length", length)
        else:
            self.close_connection = True
        self.end_headers()

        sent = 0
        while True:
            chunk = upstream.read(CHUNK)
            if not chunk:
                break
            if limit is not None and sent + len(chunk) >= limit:
                self.wfile.write(chunk[: max(0, limit - sent)])
                self.wfile.flush()
                if fault and fault["mode"] == "drop":
                    _reset(self.connection)
                return
            self.wfile.write(chunk)
            sent += len(chunk)

    # BaseHTTPRequestHandler dispatches on do_<METHOD>.
    def do_GET(self) -> None: self._proxy("GET")          # noqa: N802
    def do_HEAD(self) -> None: self._proxy("HEAD")        # noqa: N802
    def do_POST(self) -> None: self._proxy("POST")        # noqa: N802
    def do_PUT(self) -> None: self._proxy("PUT")          # noqa: N802
    def do_PATCH(self) -> None: self._proxy("PATCH")      # noqa: N802
    def do_DELETE(self) -> None: self._proxy("DELETE")    # noqa: N802


def main() -> int:
    server = ThreadingHTTPServer(("0.0.0.0", LISTEN_PORT), Handler)
    server.daemon_threads = True
    sys.stderr.write(f"[fault-proxy] :{LISTEN_PORT} -> {UPSTREAM}\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

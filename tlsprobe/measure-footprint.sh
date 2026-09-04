#!/usr/bin/env bash
# What each TLS option costs an image, measured rather than guessed.
#
# M0-1 has to choose between borrowing Horizon's TLS through the `ssl` service
# and carrying our own (trimmed mbedTLS) -- docs/DEVELOPMENT.md#tls-in-a-sysmodule.
# The argument for `ssl` is that the crypto lives in another process; this script
# is what turns that argument into two numbers.
#
# Three aarch64 images, same flags, same libnx, differing only in what they link:
#
#   baseline   libnx + newlib stdio and nothing else. The floor: every number
#              below is only interesting as a delta from this one.
#   ssl        baseline + socketInitialize + the full ssl-service call sequence.
#   mbedtls    baseline + an mbedTLS client doing the same handshake in-process.
#
# The mbedtls image needs the switch-mbedtls portlib, which the script installs
# into the throwaway container (so it needs network, which is why this is a
# script you run and quote, not a ctest -- docs/TESTING.md's gate forbids a test
# that needs anything off this machine).
#
#   ./tlsprobe/measure-footprint.sh
#
# It builds nothing in the worktree and leaves nothing behind.
set -euo pipefail

IMAGE="devkitpro/devkita64:latest"

command -v docker >/dev/null 2>&1 || { echo "no docker" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "docker daemon not running" >&2; exit 1; }

docker run --rm "$IMAGE" bash -leuo pipefail -c '
mkdir -p /tmp/fp && cd /tmp/fp

# Only the mbedtls image needs a portlib. Installed here rather than baked into
# the repo image so the two candidates are compiled by the same toolchain.
dkp-pacman -Sy --noconfirm --needed switch-mbedtls >/dev/null 2>&1 || {
  echo "could not install switch-mbedtls; the mbedtls row will be missing" >&2
}

cat > baseline.cpp <<"EOF"
#include <switch.h>
#include <cstdio>
int main(int, char**) {
  consoleInit(nullptr);
  std::printf("baseline\n");
  consoleExit(nullptr);
  return 0;
}
EOF

# The same call sequence tlsprobe/source/tls_get.cpp makes, minus the
# measurement scaffolding: this is the code that has to fit in a sysmodule.
cat > ssl.cpp <<"EOF"
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
int main(int, char**) {
  consoleInit(nullptr);
  socketInitializeDefault();
  sslInitialize(1);
  SslContext ctx{};
  SslConnection conn{};
  sslCreateContext(&ctx, SslVersion_Auto);
  sslContextImportServerPki(&ctx, "", 0, SslCertificateFormat_Pem, nullptr);
  sslContextCreateConnection(&ctx, &conn);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(443);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socketSslConnectionSetSocketDescriptor(&conn, fd);
  sslConnectionSetHostName(&conn, "h", 1);
  sslConnectionSetVerifyOption(&conn, SslVerifyOption_PeerCa | SslVerifyOption_HostName);
  sslConnectionSetIoMode(&conn, SslIoMode_Blocking);
  sslConnectionDoHandshake(&conn, nullptr, nullptr, nullptr, 0);
  char buf[4096];
  u32 got = 0;
  sslConnectionWrite(&conn, "GET / HTTP/1.1\r\n\r\n", 18, &got);
  sslConnectionRead(&conn, buf, sizeof(buf), &got);
  std::printf("%u\n", got);
  sslConnectionClose(&conn);
  sslContextClose(&ctx);
  sslExit();
  socketExit();
  consoleExit(nullptr);
  return 0;
}
EOF

# The fallback: the same handshake, in our own process. Entropy, RNG, X.509 and
# the record layer all become our .text and our heap.
cat > mbed.cpp <<"EOF"
#include <switch.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <cstdio>
int main(int, char**) {
  consoleInit(nullptr);
  socketInitializeDefault();
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt cacert;
  mbedtls_net_context server;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);
  mbedtls_x509_crt_init(&cacert);
  mbedtls_net_init(&server);
  mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
  mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                              MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conf, &cacert, nullptr);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
  mbedtls_ssl_setup(&ssl, &conf);
  mbedtls_ssl_set_hostname(&ssl, "h");
  mbedtls_net_connect(&server, "127.0.0.1", "443", MBEDTLS_NET_PROTO_TCP);
  mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, mbedtls_net_recv, nullptr);
  mbedtls_ssl_handshake(&ssl);
  unsigned char buf[4096];
  mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>("GET / HTTP/1.1\r\n\r\n"), 18);
  int got = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
  std::printf("%d\n", got);
  mbedtls_ssl_close_notify(&ssl);
  mbedtls_net_free(&server);
  mbedtls_x509_crt_free(&cacert);
  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&conf);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  socketExit();
  consoleExit(nullptr);
  return 0;
}
EOF

# switch.mk`s flags, verbatim, so these numbers are comparable to the ones a
# real target produces.
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
CXXFLAGS="-g -Wall -O2 -ffunction-sections -fdata-sections $ARCH -D__SWITCH__ \
  -std=c++20 -fno-rtti -fno-exceptions \
  -isystem /opt/devkitpro/libnx/include -isystem /opt/devkitpro/portlibs/switch/include"
LDFLAGS="-specs=/opt/devkitpro/libnx/switch.specs -g $ARCH -Wl,--gc-sections \
  -L/opt/devkitpro/libnx/lib -L/opt/devkitpro/portlibs/switch/lib"

CXX=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++
SIZE=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-size

build() {  # name, source, extra libs
  $CXX $CXXFLAGS -c "$2" -o "$1.o"
  $CXX $LDFLAGS "$1.o" -o "$1.elf" ${3:-} -lnx
}

build baseline baseline.cpp
build ssl ssl.cpp
if [ -f /opt/devkitpro/portlibs/switch/lib/libmbedtls.a ]; then
  build mbedtls mbed.cpp "-lmbedtls -lmbedx509 -lmbedcrypto"
fi

echo
echo "toolchain: $(dkp-pacman -Q devkitA64 libnx switch-mbedtls 2>/dev/null | tr "\n" " ")"
printf "\n%-10s %10s %10s %10s %12s\n" image text data bss "delta text"
base_text=""
for image in baseline ssl mbedtls; do
  [ -f "$image.elf" ] || continue
  read -r text data bss _ < <($SIZE "$image.elf" | tail -1)
  [ -n "$base_text" ] || base_text="$text"
  printf "%-10s %10s %10s %10s %12s\n" "$image" "$text" "$data" "$bss" \
    "$((text - base_text))"
done
'

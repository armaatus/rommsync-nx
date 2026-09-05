#!/usr/bin/env bash
# Register this Mac as the repo's self-hosted Actions runner.
#
# `ci.yml`'s host-tests job is the long one -- a RomM image, a full build, 105
# tests -- and it is what would eat the 3000-minute Actions budget. On this
# machine the images are already pulled, the ccache is warm and Docker is up, so
# it is both free and faster here.
#
# ## The one thing to understand before running this
#
# rommsync-nx is a PUBLIC repo. A self-hosted runner that executes a fork's code
# is arbitrary code execution on your laptop. `ci.yml` therefore routes to this
# runner ONLY for a pull request that is (a) from a branch in this repo, not a
# fork, and (b) opened by the repo owner. Everything else stays on a
# GitHub-hosted runner. Do not widen that condition, and do not add this runner
# to a repo whose PRs you do not control.
#
# The label `rommsync` is what ci.yml selects on. Keep it.
#
#   ./scripts/orca/runner-setup.sh          # install and register
#   ./scripts/orca/runner-setup.sh --status # is it up?
#   ./scripts/orca/runner-setup.sh --remove # deregister and delete
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNNER_DIR="${ACTIONS_RUNNER_DIR:-$HOME/.rommsync-fleet/actions-runner}"
LABELS="rommsync"

nwo="$(cd "$REPO_ROOT" && GH_PAGER=cat gh repo view --json nameWithOwner --jq .nameWithOwner)"

case "${1:-}" in
  --status)
    GH_PAGER=cat gh api "repos/$nwo/actions/runners" \
      --jq '.runners[] | "\(.name)  \(.status)  [\([.labels[].name]|join(","))]"' \
      2>/dev/null || echo "could not read the runner list (needs admin on $nwo)"
    exit 0 ;;
  --remove)
    [ -d "$RUNNER_DIR" ] || { echo "nothing installed at $RUNNER_DIR"; exit 0; }
    token="$(GH_PAGER=cat gh api -X POST "repos/$nwo/actions/runners/remove-token" --jq .token)"
    ( cd "$RUNNER_DIR" && ./svc.sh uninstall 2>/dev/null || true
      ./config.sh remove --token "$token" )
    rm -rf "$RUNNER_DIR"
    echo "runner removed."
    exit 0 ;;
  ""|--install) ;;
  *) echo "usage: $0 [--status|--remove]" >&2; exit 2 ;;
esac

echo "Registering a self-hosted runner for $nwo"
echo "  directory: $RUNNER_DIR"
echo "  labels:    self-hosted, $LABELS"
echo
echo "This repo is public. ci.yml routes to this runner only for non-fork PRs"
echo "opened by the repo owner; everything else stays GitHub-hosted."
echo
read -r -p "Continue? [y/N] " ok
[ "$ok" = y ] || [ "$ok" = Y ] || { echo "nothing done."; exit 0; }

mkdir -p "$RUNNER_DIR"
cd "$RUNNER_DIR"

if [ ! -x ./config.sh ]; then
  arch="$(uname -m)"
  case "$arch" in
    arm64) pkg_arch=arm64 ;;
    x86_64) pkg_arch=x64 ;;
    *) echo "unsupported architecture: $arch" >&2; exit 1 ;;
  esac
  version="$(GH_PAGER=cat gh api repos/actions/runner/releases/latest --jq '.tag_name' | sed 's/^v//')"
  tarball="actions-runner-osx-${pkg_arch}-${version}.tar.gz"
  echo "==> downloading runner $version ($pkg_arch)"
  curl -fsSLO "https://github.com/actions/runner/releases/download/v${version}/${tarball}"
  tar xzf "$tarball"
  rm -f "$tarball"
fi

token="$(GH_PAGER=cat gh api -X POST "repos/$nwo/actions/runners/registration-token" --jq .token)"
./config.sh --unattended --replace \
  --url "https://github.com/$nwo" \
  --token "$token" \
  --name "$(scutil --get ComputerName 2>/dev/null || hostname)" \
  --labels "$LABELS" \
  --work _work

echo
echo "==> installing it as a login service so it comes back after a reboot"
./svc.sh install
./svc.sh start

echo
echo "done. Check it with: $0 --status"
echo "Docker has to be running for host-tests to pass here -- the RomM fixture is a"
echo "compose stack, and the job fails fast with a clear message if it is not."

// ovl-rommsync entry point.
//
// Skeleton: it does not draw, and Ultrahand will list it and find nothing to
// show. The real overlay is M4-1, on libultrahand; overlay UI is one of the few
// things an emulator cannot exercise, so it is verified last, on hardware,
// after the M8-1 gate (overlay/AGENTS.md).
//
// What exists today is the packaging: a devkitA64 link that produces a signed
// .ovl, so CI publishes an artifact and a broken Switch toolchain is a red
// build rather than a discovery in M4.

#include <switch.h>

int main(int, char**) { return 0; }

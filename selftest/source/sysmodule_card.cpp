// The sysmodule's own SD card backend, compiled into the selftest unchanged.
//
// SOURCES takes a directory whole, and sysmodule/source also holds the
// sysmodule's main() -- so the one file is included instead (selftest/Makefile).
// Including it rather than copying it is the point: the card rows test the file
// that ships, not a second spelling of it.
#include "../../sysmodule/source/card.cpp"

#include "rommsync/clock_check.hpp"

namespace rommsync::sync {

const char* ToString(ClockVerdict) { return "trusted"; }

const char* ToString(ClockReference) { return "none"; }

std::string ClockCheck::Describe() const { return ""; }

ClockCheck CheckClock(const ClockReading&) { return {}; }

std::optional<Timestamp> ParseHttpDate(std::string_view) { return std::nullopt; }

std::optional<Timestamp> ServerDate(const http::Headers&) { return std::nullopt; }

bool DecidedByClock(const SyncOperation&) { return false; }

bool TlsClockFault(http::Error, std::string_view, const ClockCheck&) { return false; }

}  // namespace rommsync::sync

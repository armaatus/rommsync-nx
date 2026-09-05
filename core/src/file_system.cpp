#include "rommsync/file_system.hpp"

namespace rommsync::fs {

const char* ToString(ListError error) {
  switch (error) {
    case ListError::kNone:
      return "none";
    case ListError::kMissing:
      return "missing";
    case ListError::kNotADirectory:
      return "not a directory";
    case ListError::kUnreadable:
      return "unreadable";
    case ListError::kTooManyEntries:
      return "too many entries";
  }
  return "unknown";
}

}  // namespace rommsync::fs

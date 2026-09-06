#include "rommsync/host/file_sync.hpp"

#include <cerrno>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "rommsync/atomic_file.hpp"

namespace rommsync::host {
namespace {

/// `fsync` on the file the staged path names.
///
/// Re-opened rather than handed a descriptor, because that is the contract
/// `io::FileSync` can hold on both platforms: Horizon has no `fsync` at all and
/// commits a whole filesystem instead (atomic_file.hpp). The extra `open` costs
/// nothing next to the sync it is there to perform.
///
/// Strict about the answer on purpose: the caller this exists for is the backup
/// an overwrite depends on, so "the card would not take these bytes" has to stop
/// the overwrite rather than be rounded to success. `EINTR` is the one refusal
/// that is not one -- a signal arriving mid-call says nothing about the data --
/// so it is retried rather than reported.
bool PosixFileSync(const std::string& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return false;
  }
  bool synced = true;
  while (::fsync(descriptor) != 0) {
    if (errno != EINTR) {
      synced = false;
      break;
    }
  }
  ::close(descriptor);
  return synced;
}

}  // namespace

void InstallPosixFileSync() { io::SetFileSync(&PosixFileSync); }

}  // namespace rommsync::host

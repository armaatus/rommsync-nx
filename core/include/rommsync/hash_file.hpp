// Hashing a file that will not fit in memory.
//
// Three callers want the same loop and nothing else: `state::HashFile` over a
// save state, `crypto::Md5FileHex` and `crypto::Sha1FileHex` over a rom. A rom
// is gigabytes and the sysmodule's inner heap is 512 KiB (core/AGENTS.md), so
// none of them may read the file whole -- and three copies of "read a chunk,
// feed the hasher" is three places for the chunk to be sized by how fast it
// felt on a desktop.
//
// **The chunk is 4 KiB and stays that way.** `sysmodule/sys-rommsync.json` gives
// the main thread a 16 KiB stack, so a 32 KiB buffer here is a stack overflow on
// the console that compiles and passes every host test. The read count is not
// the cost anyway: stdio buffers underneath, and the digest cannot depend on the
// chunk size -- `core.md5` and `core.sha1` assert exactly that.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace rommsync::crypto {

/// Feed every byte of `path` to `hasher`, 4 KiB at a time.
///
/// `hasher` is anything with `Update(std::string_view)` -- `Md5Hasher` and
/// `Sha1Hasher` both are. It is left holding the file's bytes and *not*
/// finished, so the caller decides between `Finish` and `FinishHex`.
///
/// False when the file could not be read to the end, and `*opened` is what
/// separates the two ways that happens: false means there is no such file (or it
/// would not open at all), true means it opened and a read failed part way. A
/// caller that reports a diagnostic needs the difference; one that only wants a
/// digest can pass a pointer it ignores. Never nullptr.
///
/// A partial read is a failure, not a short digest: a digest over the first half
/// of a file is a perfectly plausible 40 characters that matches nothing, which
/// is the failure every caller here exists to rule out.
template <class Hasher>
bool StreamFile(const std::string& path, Hasher& hasher, bool* opened) {
  *opened = false;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  *opened = true;

  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    hasher.Update(std::string_view(buffer, got));
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  return !failed;
}

/// `StreamFile` plus `FinishHex`: the digest of `path` as lowercase hex, or
/// **empty** when the file could not be read to the end.
///
/// Empty is a value no digest collides with -- they are all a fixed even number
/// of hex characters -- so a caller that only wants the digest needs no second
/// return channel. `crypto::Md5FileHex` and `crypto::Sha1FileHex` are this with
/// their hasher filled in, and exist so a caller names an algorithm rather than
/// a template.
template <class Hasher>
std::string FileHex(const std::string& path) {
  Hasher hasher;
  bool opened = false;
  if (!StreamFile(path, hasher, &opened)) {
    return {};
  }
  return hasher.FinishHex();
}

}  // namespace rommsync::crypto

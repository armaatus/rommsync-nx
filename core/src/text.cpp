#include "rommsync/text.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace rommsync::text {
namespace {

/// The largest cut at or below `limit` that does not land inside a code point.
///
/// A UTF-8 continuation byte is `10xxxxxx`; walking back off them lands on a
/// lead byte or on zero. Zero is a legitimate answer -- a single code point
/// longer than `limit` becomes the ellipsis alone, which is honest.
std::size_t BoundaryAt(std::string_view text, std::size_t limit) {
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return cut;
}

}  // namespace

std::string Shorten(std::string_view text, std::size_t limit) {
  if (text.size() <= limit) {
    return std::string(text);
  }
  return std::string(text.substr(0, BoundaryAt(text, limit))) + kEllipsis;
}

void ShortenInPlace(std::string* text, std::size_t limit) {
  if (text->size() <= limit) {
    return;
  }
  text->resize(BoundaryAt(*text, limit));
  *text += kEllipsis;
}

}  // namespace rommsync::text

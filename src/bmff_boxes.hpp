// Internal: the ISO base media file format's boxes, walked with bounds checks
// and a budget, for the HEIF/CR3/JPEG XL reader (bmff.cpp) and the movie
// reader (movie.cpp).
#pragma once

#include <lumenlib/io.hpp>

#include "bytes.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace lumenlib::detail {

// How many boxes one file may hold, all levels together.
constexpr std::size_t kMaxBoxes = 100000;

struct Box {
  std::string type;
  std::uint64_t offset;   // of the header
  std::uint64_t payload;  // start of the payload (after any uuid)
  std::uint64_t end;
  std::uint8_t uuid[16] = {};
  std::uint64_t size() const { return end - payload; }
};

// The boxes in [begin, end). A box that runs past `end` is corrupt data.
inline std::vector<Box> boxes(const InputSource& src, std::uint64_t begin, std::uint64_t end, std::size_t& budget) {
  std::vector<Box> out;
  std::uint64_t pos = begin;
  while (inBounds(end, pos, 8)) {
    if (budget-- == 0) corrupt("too many boxes");
    std::uint8_t h[16];
    src.read(pos, h, 8);
    std::uint64_t size = getBe32(h);
    Box b;
    b.type = toText(h + 4, 4);
    b.offset = pos;
    std::uint64_t header = 8;
    if (size == 1) {
      if (!inBounds(end, pos, 16)) corrupt("truncated box header");
      src.read(pos + 8, h + 8, 8);
      size = getBe64(h + 8);
      header = 16;
    } else if (size == 0) {
      size = end - pos;
    }
    if (size < header || !inBounds(end, pos, size)) corrupt("box '" + b.type + "' runs past its parent");
    if (b.type == "uuid") {
      if (size < header + 16) corrupt("truncated uuid box");
      src.read(pos + header, b.uuid, 16);
      header += 16;
    }
    b.payload = pos + header;
    b.end = pos + size;
    out.push_back(b);
    pos += size;
  }
  return out;
}

// Big-endian reads within a box payload that is in memory.
class Reader {
 public:
  explicit Reader(Bytes data) : d_(std::move(data)) {}
  bool has(std::size_t n) const { return inBounds(d_.size(), pos_, n); }
  std::uint64_t read(std::size_t n) {
    if (n > 8 || !has(n)) corrupt("truncated or malformed box");
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | d_[pos_ + i];
    pos_ += n;
    return v;
  }
  void skip(std::size_t n) {
    if (!has(n)) corrupt("truncated or malformed box");
    pos_ += n;
  }
  std::string cstring() {
    const auto begin = d_.begin() + static_cast<std::ptrdiff_t>(pos_);
    const auto nul = std::find(begin, d_.end(), 0);
    std::string s(begin, nul);
    pos_ = static_cast<std::size_t>(nul - d_.begin()) + (nul == d_.end() ? 0 : 1);
    return s;
  }
  std::string fourcc() {
    if (!has(4)) corrupt("truncated box");
    std::string s = toText(d_.data() + pos_, 4);
    pos_ += 4;
    return s;
  }
  std::size_t pos() const { return pos_; }
  std::size_t left() const { return d_.size() - pos_; }
  const Bytes& data() const { return d_; }

 private:
  Bytes d_;
  std::size_t pos_ = 0;
};

}  // namespace lumenlib::detail

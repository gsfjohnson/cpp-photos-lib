#include <photos/error.hpp>
#include <photos/io.hpp>

#include "formats.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace photos {

InputSource::~InputSource() = default;
OutputSink::~OutputSink() = default;

bool InputSource::contains(std::uint64_t offset, std::uint64_t n) const {
  const std::uint64_t s = size();
  return offset <= s && n <= s - offset;
}

Bytes InputSource::readBytes(std::uint64_t offset, std::size_t n) const {
  if (!contains(offset, n)) throw Error(ErrorCode::corruptData, "read past the end of the data");
  Bytes out(n);
  if (n) read(offset, out.data(), n);
  return out;
}

void MemorySource::read(std::uint64_t offset, void* dst, std::size_t n) const {
  if (!contains(offset, n)) throw Error(ErrorCode::corruptData, "read past the end of the data");
  if (n) std::memcpy(dst, data_.data() + offset, n);
}

FileSource::FileSource(const std::filesystem::path& path) : path_(path) {
  stream_.open(path, std::ios::binary);
  if (!stream_) throw Error(ErrorCode::io, "cannot open " + detail::pathText(path));
  stream_.seekg(0, std::ios::end);
  const auto end = stream_.tellg();
  if (end < 0) throw Error(ErrorCode::io, "cannot size " + detail::pathText(path));
  size_ = static_cast<std::uint64_t>(end);
}

void FileSource::read(std::uint64_t offset, void* dst, std::size_t n) const {
  if (!contains(offset, n)) throw Error(ErrorCode::corruptData, "read past the end of " + detail::pathText(path_));
  if (n == 0) return;
  stream_.clear();
  stream_.seekg(static_cast<std::streamoff>(offset));
  stream_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
  if (!stream_ || static_cast<std::size_t>(stream_.gcount()) != n) {
    throw Error(ErrorCode::io, "cannot read " + detail::pathText(path_));
  }
}

void OutputSink::copyFrom(const InputSource& source, std::uint64_t offset, std::uint64_t n) {
  if (!source.contains(offset, n)) throw Error(ErrorCode::corruptData, "copy past the end of the data");
  Bytes buffer(static_cast<std::size_t>(std::min<std::uint64_t>(n, 1 << 20)));
  while (n > 0) {
    const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(n, buffer.size()));
    source.read(offset, buffer.data(), chunk);
    write(buffer.data(), chunk);
    offset += chunk;
    n -= chunk;
  }
}

void MemorySink::write(const void* data, std::size_t n) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  data_.insert(data_.end(), p, p + n);
}

FileSink::FileSink(const std::filesystem::path& path) : path_(path) {
  stream_.open(path, std::ios::binary | std::ios::trunc);
  if (!stream_) throw Error(ErrorCode::io, "cannot create " + detail::pathText(path));
}

void FileSink::write(const void* data, std::size_t n) {
  stream_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
  if (!stream_) throw Error(ErrorCode::io, "cannot write " + detail::pathText(path_));
}

void FileSink::close() {
  stream_.flush();
  stream_.close();
  if (!stream_) throw Error(ErrorCode::io, "cannot write " + detail::pathText(path_));
}

}  // namespace photos

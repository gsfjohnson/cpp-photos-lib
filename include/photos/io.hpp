// Random-access input and sequential output. Images read their file through an
// InputSource, so a platform can supply its own (an Android file descriptor
// from a content URI, an iOS NSData) by subclassing it.
#pragma once

#include <photos/export.hpp>
#include <photos/types.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

namespace photos {

class PHOTOS_EXPORT InputSource {
 public:
  virtual ~InputSource();

  virtual std::uint64_t size() const = 0;
  // Reads exactly n bytes at offset. Throws Error(corruptData) when the range
  // is past the end and Error(io) when reading fails.
  virtual void read(std::uint64_t offset, void* dst, std::size_t n) const = 0;

  // n bytes at offset.
  Bytes readBytes(std::uint64_t offset, std::size_t n) const;
  // Whether [offset, offset + n) lies within the source.
  bool contains(std::uint64_t offset, std::uint64_t n) const;
};

class PHOTOS_EXPORT MemorySource final : public InputSource {
 public:
  explicit MemorySource(Bytes data) : data_(std::move(data)) {}

  std::uint64_t size() const override { return data_.size(); }
  void read(std::uint64_t offset, void* dst, std::size_t n) const override;
  const Bytes& data() const noexcept { return data_; }

 private:
  Bytes data_;
};

// Not thread-safe: reads share one stream.
class PHOTOS_EXPORT FileSource final : public InputSource {
 public:
  // Throws Error(io) if the file cannot be opened.
  explicit FileSource(const std::filesystem::path& path);

  std::uint64_t size() const override { return size_; }
  void read(std::uint64_t offset, void* dst, std::size_t n) const override;
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
  mutable std::ifstream stream_;
  std::uint64_t size_ = 0;
};

class PHOTOS_EXPORT OutputSink {
 public:
  virtual ~OutputSink();
  // Throws Error(io) when writing fails.
  virtual void write(const void* data, std::size_t n) = 0;

  void write(const Bytes& data) { write(data.data(), data.size()); }
  // Copies [offset, offset + n) of the source.
  void copyFrom(const InputSource& source, std::uint64_t offset, std::uint64_t n);
};

class PHOTOS_EXPORT MemorySink final : public OutputSink {
 public:
  void write(const void* data, std::size_t n) override;
  const Bytes& data() const noexcept { return data_; }
  Bytes release() noexcept { return std::move(data_); }

 private:
  Bytes data_;
};

class PHOTOS_EXPORT FileSink final : public OutputSink {
 public:
  // Creates or truncates the file. Throws Error(io).
  explicit FileSink(const std::filesystem::path& path);

  void write(const void* data, std::size_t n) override;
  // Flushes and closes; throws Error(io) if that fails.
  void close();

 private:
  std::filesystem::path path_;
  std::ofstream stream_;
};

}  // namespace photos

// Errors thrown by the library. Every failure is a photos::Error carrying an
// ErrorCode, so callers can tell a damaged file from an unsupported one.
#pragma once

#include <photos/export.hpp>

#include <stdexcept>
#include <string>

namespace photos {

enum class ErrorCode {
  io,                    // the file or stream could not be read or written
  corruptData,           // the data is malformed or truncated
  unsupportedFormat,     // the file type is not recognised
  unsupportedOperation,  // recognised, but this operation is not implemented for it
  invalidArgument,       // a key, value or parameter is not valid
  dataTooLarge,          // the metadata does not fit the container's limits
};

PHOTOS_EXPORT const char* toString(ErrorCode code) noexcept;

class PHOTOS_EXPORT Error : public std::runtime_error {
 public:
  Error(ErrorCode code, const std::string& message);
  ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

}  // namespace photos

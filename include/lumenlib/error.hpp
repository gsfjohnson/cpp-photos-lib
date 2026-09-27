// Errors thrown by the library, and the hook for its warnings. Every failure
// is a lumenlib::Error carrying an ErrorCode, so callers can tell a damaged
// file from an unsupported one.
#pragma once

#include <lumenlib/export.hpp>

#include <functional>
#include <stdexcept>
#include <string>

namespace lumenlib {

enum class ErrorCode {
  io,                    // the file or stream could not be read or written
  corruptData,           // the data is malformed or truncated
  unsupportedFormat,     // the file type is not recognised
  unsupportedOperation,  // recognised, but this operation is not implemented for it
  invalidArgument,       // a key, value or parameter is not valid
  dataTooLarge,          // the metadata does not fit the container's limits
};

LUMENLIB_EXPORT const char* toString(ErrorCode code) noexcept;

class LUMENLIB_EXPORT Error : public std::runtime_error {
 public:
  Error(ErrorCode code, const std::string& message);
  ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

// Problems the library works around rather than fails on: a damaged XMP
// packet left out, an IFD entry skipped, a maker note that cannot be decoded.
// The handler is called on the thread that met the problem, so it must be
// thread-safe; by default warnings are dropped. Pass nullptr to drop them
// again.
using WarningHandler = std::function<void(const std::string& message)>;
LUMENLIB_EXPORT void setWarningHandler(WarningHandler handler);

}  // namespace lumenlib

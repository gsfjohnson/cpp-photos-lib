#include <photos/error.hpp>

namespace photos {

const char* toString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::io:
      return "I/O error";
    case ErrorCode::corruptData:
      return "corrupt data";
    case ErrorCode::unsupportedFormat:
      return "unsupported format";
    case ErrorCode::unsupportedOperation:
      return "unsupported operation";
    case ErrorCode::invalidArgument:
      return "invalid argument";
    case ErrorCode::dataTooLarge:
      return "data too large";
  }
  return "unknown error";
}

Error::Error(ErrorCode code, const std::string& message)
    : std::runtime_error(std::string(toString(code)) + ": " + message), code_(code) {}

}  // namespace photos

#include <lumenlib/error.hpp>

#include "bytes.hpp"

#include <memory>
#include <mutex>

namespace lumenlib {

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

namespace {

// The handler is shared, so a call in progress keeps the one it started with
// while another thread replaces it.
std::mutex warningMutex;
std::shared_ptr<const WarningHandler> warningHandler;

}  // namespace

void setWarningHandler(WarningHandler handler) {
  std::lock_guard<std::mutex> lock(warningMutex);
  warningHandler = handler ? std::make_shared<const WarningHandler>(std::move(handler)) : nullptr;
}

void detail::warn(const std::string& message) {
  std::shared_ptr<const WarningHandler> handler;
  {
    std::lock_guard<std::mutex> lock(warningMutex);
    handler = warningHandler;
  }
  if (!handler) return;
  try {
    (*handler)(message);
  } catch (...) {
    // A handler's failure is not the file's.
  }
}

}  // namespace lumenlib

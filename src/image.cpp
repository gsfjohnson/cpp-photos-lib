#include <photos/error.hpp>
#include <photos/image.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

namespace photos {

const char* toString(ImageType type) noexcept {
  switch (type) {
    case ImageType::unknown:
      return "unknown";
    case ImageType::jpeg:
      return "JPEG";
    case ImageType::png:
      return "PNG";
    case ImageType::webp:
      return "WebP";
    case ImageType::tiff:
      return "TIFF";
    case ImageType::heif:
      return "HEIF";
    case ImageType::avif:
      return "AVIF";
    case ImageType::cr3:
      return "CR3";
    case ImageType::jxl:
      return "JPEG XL";
    case ImageType::xmpSidecar:
      return "XMP";
  }
  return "unknown";
}

const char* mimeType(ImageType type) noexcept {
  switch (type) {
    case ImageType::unknown:
      return "application/octet-stream";
    case ImageType::jpeg:
      return "image/jpeg";
    case ImageType::png:
      return "image/png";
    case ImageType::webp:
      return "image/webp";
    case ImageType::tiff:
      return "image/tiff";
    case ImageType::heif:
      return "image/heif";
    case ImageType::avif:
      return "image/avif";
    case ImageType::cr3:
      return "image/x-canon-cr3";
    case ImageType::jxl:
      return "image/jxl";
    case ImageType::xmpSidecar:
      return "application/rdf+xml";
  }
  return "application/octet-stream";
}

namespace {

ImageType bmffBrand(const std::uint8_t* b, std::size_t n) {
  // ftyp: major brand at 8, minor version at 12, compatible brands from 16.
  auto brandIs = [](const std::uint8_t* p, const char* s) { return std::memcmp(p, s, 4) == 0; };
  auto classify = [&](const std::uint8_t* p) -> ImageType {
    if (brandIs(p, "avif") || brandIs(p, "avis")) return ImageType::avif;
    if (brandIs(p, "heic") || brandIs(p, "heix") || brandIs(p, "heim") || brandIs(p, "heis") || brandIs(p, "hevc") ||
        brandIs(p, "hevx")) {
      return ImageType::heif;
    }
    if (brandIs(p, "crx ")) return ImageType::cr3;
    if (brandIs(p, "jxl ")) return ImageType::jxl;
    return ImageType::unknown;
  };
  if (n < 12) return ImageType::unknown;
  if (auto t = classify(b + 8); t != ImageType::unknown) return t;
  const std::size_t boxSize = std::min<std::size_t>(detail::getBe32(b), n);
  ImageType found = ImageType::unknown;
  for (std::size_t p = 16; p + 4 <= boxSize; p += 4) {
    const auto t = classify(b + p);
    if (t == ImageType::avif) return t;
    if (found == ImageType::unknown) found = t;
  }
  if (found != ImageType::unknown) return found;
  // Generic HEIF brands.
  if (brandIs(b + 8, "mif1") || brandIs(b + 8, "msf1") || brandIs(b + 8, "mif2")) return ImageType::heif;
  return ImageType::unknown;
}

bool looksLikeXmp(const std::uint8_t* b, std::size_t n) {
  std::size_t i = 0;
  if (n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) i = 3;
  while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) ++i;
  if (i >= n || b[i] != '<') return false;
  const std::string head(reinterpret_cast<const char*>(b + i), n - i);
  return head.find("<?xpacket") == 0 || head.find("xmpmeta") != std::string::npos ||
         head.find("rdf:RDF") != std::string::npos;
}

}  // namespace

ImageType detectImageType(const InputSource& source) {
  std::uint8_t b[1024] = {};
  const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), sizeof b));
  source.read(0, b, n);
  if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return ImageType::jpeg;
  if (n >= 8 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return ImageType::png;
  if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return ImageType::webp;
  if (detail::isTiffHeader(b, n, true)) return ImageType::tiff;
  if (n >= 12 && std::memcmp(b, "\0\0\0\x0cJXL \r\n\x87\n", 12) == 0) return ImageType::jxl;
  if (n >= 2 && b[0] == 0xff && b[1] == 0x0a) return ImageType::jxl;  // bare codestream
  if (n >= 12 && std::memcmp(b + 4, "ftyp", 4) == 0) return bmffBrand(b, n);
  if (looksLikeXmp(b, n)) return ImageType::xmpSidecar;
  return ImageType::unknown;
}

void detail::SpanSource::read(std::uint64_t offset, void* dst, std::size_t n) const {
  if (!contains(offset, n)) throw Error(ErrorCode::corruptData, "read past the end of the data");
  if (n) std::memcpy(dst, data_ + offset, n);
}

// ---- Image --------------------------------------------------------------------

Image::Image(ImageType type, std::unique_ptr<InputSource> source, unsigned readable, unsigned writable)
    : type_(type), source_(std::move(source)), readable_(readable), writable_(writable) {}

Image::~Image() = default;

std::unique_ptr<Image> Image::create(ImageType type, std::unique_ptr<InputSource> source) {
  switch (type) {
    case ImageType::jpeg:
      return detail::newJpegImage(std::move(source));
    case ImageType::png:
      return detail::newPngImage(std::move(source));
    case ImageType::webp:
      return detail::newWebpImage(std::move(source));
    case ImageType::tiff:
      return detail::newTiffImage(std::move(source));
    case ImageType::heif:
    case ImageType::avif:
    case ImageType::cr3:
    case ImageType::jxl:
      return detail::newBmffImage(type, std::move(source));
    case ImageType::xmpSidecar:
      return detail::newXmpSidecar(std::move(source));
    case ImageType::unknown:
      break;
  }
  throw Error(ErrorCode::unsupportedFormat, "unrecognised image format");
}

std::unique_ptr<Image> Image::open(const std::filesystem::path& path) {
  auto source = std::make_unique<FileSource>(path);
  const auto type = detectImageType(*source);
  if (type == ImageType::unknown)
    throw Error(ErrorCode::unsupportedFormat, detail::pathText(path) + ": unrecognised image format");
  auto image = create(type, std::move(source));
  image->path_ = path;
  return image;
}

std::unique_ptr<Image> Image::open(Bytes data) { return open(std::make_unique<MemorySource>(std::move(data))); }

std::unique_ptr<Image> Image::open(std::unique_ptr<InputSource> source) {
  if (!source) throw Error(ErrorCode::invalidArgument, "Image::open: no source");
  const auto type = detectImageType(*source);
  return create(type, std::move(source));
}

std::unique_ptr<Image> Image::createXmpSidecar() {
  return detail::newXmpSidecar(std::make_unique<MemorySource>(Bytes()));
}

const Bytes* Image::buffer() const noexcept {
  const auto* mem = dynamic_cast<const MemorySource*>(source_.get());
  return mem ? &mem->data() : nullptr;
}

bool Image::canRead(MetadataKind kind) const noexcept { return (readable_ & static_cast<unsigned>(kind)) != 0; }
bool Image::canWrite(MetadataKind kind) const noexcept { return (writable_ & static_cast<unsigned>(kind)) != 0; }

void Image::readMetadata() {
  exif_.clear();
  iptc_.clear();
  xmp_.clear();
  comment_.clear();
  icc_.clear();
  xmpPacket_.clear();
  width_ = height_ = 0;
  doReadMetadata();
}

void Image::doWriteMetadata(OutputSink&) const {
  throw Error(ErrorCode::unsupportedOperation,
              std::string("writing metadata to ") + toString(type_) + " is not supported");
}

void Image::writeMetadata(OutputSink& sink) const {
  if (writable_ == 0) {
    throw Error(ErrorCode::unsupportedOperation,
                std::string("writing metadata to ") + toString(type_) + " is not supported");
  }
  doWriteMetadata(sink);
}

void Image::writeMetadata() {
  if (dynamic_cast<const MemorySource*>(source_.get())) {
    MemorySink sink;
    writeMetadata(sink);
    source_ = std::make_unique<MemorySource>(sink.release());
    return;
  }
  if (!path_) {
    throw Error(ErrorCode::unsupportedOperation,
                "image has no file or buffer to write to; use writeMetadata(OutputSink&)");
  }
  // Write beside the file, then replace it, so a failure leaves it intact.
  static std::atomic<unsigned> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path tmp = *path_;
  tmp += ".photos-" + std::to_string(stamp) + "-" + std::to_string(counter++) + ".tmp";
  try {
    {
      FileSink sink(tmp);
      writeMetadata(sink);
      sink.close();
    }
    source_.reset();  // Windows cannot replace an open file.
    std::error_code ec;
    std::filesystem::rename(tmp, *path_, ec);
    if (ec) throw Error(ErrorCode::io, "cannot replace " + detail::pathText(*path_) + ": " + ec.message());
    source_ = std::make_unique<FileSource>(*path_);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    if (!source_) source_ = std::make_unique<FileSource>(*path_);
    throw;
  }
}

void Image::setXmpPacket(std::string packet) {
  xmpPacket_ = std::move(packet);
  // Trailing NULs appear in the wild.
  while (!xmpPacket_.empty() && xmpPacket_.back() == '\0') xmpPacket_.pop_back();
  try {
    xmp_ = XmpData::parse(xmpPacket_);
  } catch (const Error&) {
    // A damaged packet leaves the XMP empty; the rest of the metadata is
    // still usable. The raw packet stays in xmpPacket().
    xmp_.clear();
  }
}

}  // namespace photos

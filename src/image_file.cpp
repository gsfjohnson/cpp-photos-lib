#include <lumenlib/error.hpp>
#include <lumenlib/image_file.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

namespace lumenlib {

const char* formatName(FileFormat format) noexcept {
  switch (format) {
    case FileFormat::unknown:
      return "unknown";
    case FileFormat::jpeg:
      return "JPEG";
    case FileFormat::png:
      return "PNG";
    case FileFormat::webp:
      return "WebP";
    case FileFormat::tiff:
      return "TIFF";
    case FileFormat::rw2:
      return "RW2";
    case FileFormat::raf:
      return "RAF";
    case FileFormat::heif:
      return "HEIF";
    case FileFormat::avif:
      return "AVIF";
    case FileFormat::cr3:
      return "CR3";
    case FileFormat::jxl:
      return "JPEG XL";
    case FileFormat::xmp:
      return "XMP";
  }
  return "unknown";
}

const char* mimeType(FileFormat format) noexcept {
  switch (format) {
    case FileFormat::unknown:
      return "application/octet-stream";
    case FileFormat::jpeg:
      return "image/jpeg";
    case FileFormat::png:
      return "image/png";
    case FileFormat::webp:
      return "image/webp";
    case FileFormat::tiff:
      return "image/tiff";
    case FileFormat::rw2:
      return "image/x-panasonic-rw2";
    case FileFormat::raf:
      return "image/x-fujifilm-raf";
    case FileFormat::heif:
      return "image/heif";
    case FileFormat::avif:
      return "image/avif";
    case FileFormat::cr3:
      return "image/x-canon-cr3";
    case FileFormat::jxl:
      return "image/jxl";
    case FileFormat::xmp:
      return "application/rdf+xml";
  }
  return "application/octet-stream";
}

namespace {

FileFormat bmffBrand(const std::uint8_t* b, std::size_t n) {
  // ftyp: major brand at 8, minor version at 12, compatible brands from 16.
  auto brandIs = [](const std::uint8_t* p, const char* s) { return std::memcmp(p, s, 4) == 0; };
  auto classify = [&](const std::uint8_t* p) -> FileFormat {
    if (brandIs(p, "avif") || brandIs(p, "avis")) return FileFormat::avif;
    if (brandIs(p, "heic") || brandIs(p, "heix") || brandIs(p, "heim") || brandIs(p, "heis") || brandIs(p, "hevc") ||
        brandIs(p, "hevx")) {
      return FileFormat::heif;
    }
    if (brandIs(p, "crx ")) return FileFormat::cr3;
    if (brandIs(p, "jxl ")) return FileFormat::jxl;
    return FileFormat::unknown;
  };
  if (n < 12) return FileFormat::unknown;
  if (auto t = classify(b + 8); t != FileFormat::unknown) return t;
  const std::size_t boxSize = std::min<std::size_t>(detail::getBe32(b), n);
  FileFormat found = FileFormat::unknown;
  for (std::size_t p = 16; p + 4 <= boxSize; p += 4) {
    const auto t = classify(b + p);
    if (t == FileFormat::avif) return t;
    if (found == FileFormat::unknown) found = t;
  }
  if (found != FileFormat::unknown) return found;
  // Generic HEIF brands.
  if (brandIs(b + 8, "mif1") || brandIs(b + 8, "msf1") || brandIs(b + 8, "mif2")) return FileFormat::heif;
  return FileFormat::unknown;
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

constexpr unsigned kExif = detail::kExif, kIptc = detail::kIptc, kXmp = detail::kXmp, kComment = detail::kComment,
                   kIcc = detail::kIcc;

unsigned readable(FileFormat format) noexcept {
  switch (format) {
    case FileFormat::jpeg:
      return kExif | kIptc | kXmp | kComment | kIcc;
    case FileFormat::png:
    case FileFormat::tiff:
    case FileFormat::rw2:
      return kExif | kIptc | kXmp | kIcc;
    case FileFormat::raf:
      return kExif | kIptc | kXmp | kIcc;
    case FileFormat::webp:
      return kExif | kXmp | kIcc;
    case FileFormat::heif:
    case FileFormat::avif:
    case FileFormat::cr3:
    case FileFormat::jxl:
      return kExif | kXmp;
    case FileFormat::xmp:
      return kXmp;
    case FileFormat::unknown:
      break;
  }
  return 0;
}

unsigned writable(FileFormat format) noexcept {
  switch (format) {
    case FileFormat::jpeg:
      return kExif | kIptc | kXmp | kComment | kIcc;
    case FileFormat::png:
    case FileFormat::tiff:
      return kExif | kIptc | kXmp | kIcc;
    case FileFormat::webp:
      return kExif | kXmp | kIcc;
    case FileFormat::xmp:
      return kXmp;
    default:
      return 0;
  }
}

}  // namespace

FileFormat detectFormat(const InputSource& source) {
  std::uint8_t b[1024] = {};
  const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), sizeof b));
  source.read(0, b, n);
  if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return FileFormat::jpeg;
  if (n >= 8 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return FileFormat::png;
  if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return FileFormat::webp;
  if (n >= 4 && b[0] == 'I' && b[1] == 'I' && b[2] == 0x55 && b[3] == 0) return FileFormat::rw2;
  if (detail::isTiffHeader(b, n, true)) return FileFormat::tiff;
  if (n >= 16 && std::memcmp(b, "FUJIFILMCCD-RAW ", 16) == 0) return FileFormat::raf;
  if (n >= 12 && std::memcmp(b, "\0\0\0\x0cJXL \r\n\x87\n", 12) == 0) return FileFormat::jxl;
  if (n >= 2 && b[0] == 0xff && b[1] == 0x0a) return FileFormat::jxl;  // bare codestream
  if (n >= 12 && std::memcmp(b + 4, "ftyp", 4) == 0) return bmffBrand(b, n);
  if (looksLikeXmp(b, n)) return FileFormat::xmp;
  return FileFormat::unknown;
}

FileFormat detectFormat(const std::filesystem::path& path) { return detectFormat(FileSource(path)); }

bool formatCanRead(FileFormat format, MetadataKind kind) noexcept {
  return (readable(format) & static_cast<unsigned>(kind)) != 0;
}

bool formatCanWrite(FileFormat format, MetadataKind kind) noexcept {
  return (writable(format) & static_cast<unsigned>(kind)) != 0;
}

void detail::SpanSource::read(std::uint64_t offset, void* dst, std::size_t n) const {
  if (!contains(offset, n)) throw Error(ErrorCode::corruptData, "read past the end of the data");
  if (n) std::memcpy(dst, data_ + offset, n);
}

// ---- ImageFile ----------------------------------------------------------------

ImageFile::ImageFile(FileFormat format, std::unique_ptr<InputSource> source)
    : format_(format), source_(std::move(source)) {}

ImageFile::~ImageFile() = default;

std::unique_ptr<ImageFile> ImageFile::create(FileFormat format, std::unique_ptr<InputSource> source) {
  switch (format) {
    case FileFormat::jpeg:
      return detail::newJpegFile(std::move(source));
    case FileFormat::png:
      return detail::newPngFile(std::move(source));
    case FileFormat::webp:
      return detail::newWebpFile(std::move(source));
    case FileFormat::tiff:
    case FileFormat::rw2:
      return detail::newTiffFile(format, std::move(source));
    case FileFormat::raf:
      return detail::newRafFile(std::move(source));
    case FileFormat::heif:
    case FileFormat::avif:
    case FileFormat::cr3:
    case FileFormat::jxl:
      return detail::newBmffFile(format, std::move(source));
    case FileFormat::xmp:
      return detail::newXmpSidecarFile(std::move(source));
    case FileFormat::unknown:
      break;
  }
  throw Error(ErrorCode::unsupportedFormat, "unrecognised image format");
}

std::unique_ptr<ImageFile> ImageFile::open(const std::filesystem::path& path) {
  auto source = std::make_unique<FileSource>(path);
  const auto format = detectFormat(*source);
  if (format == FileFormat::unknown) {
    throw Error(ErrorCode::unsupportedFormat, detail::pathText(path) + ": unrecognised image format");
  }
  auto file = create(format, std::move(source));
  file->path_ = path;
  return file;
}

std::unique_ptr<ImageFile> ImageFile::open(Bytes data) { return open(std::make_unique<MemorySource>(std::move(data))); }

std::unique_ptr<ImageFile> ImageFile::open(std::unique_ptr<InputSource> source) {
  if (!source) throw Error(ErrorCode::invalidArgument, "ImageFile::open: no source");
  const auto format = detectFormat(*source);
  return create(format, std::move(source));
}

std::unique_ptr<ImageFile> ImageFile::newXmpSidecar() {
  return detail::newXmpSidecarFile(std::make_unique<MemorySource>(Bytes()));
}

const Bytes* ImageFile::buffer() const noexcept {
  const auto* mem = dynamic_cast<const MemorySource*>(source_.get());
  return mem ? &mem->data() : nullptr;
}

bool ImageFile::canRead(MetadataKind kind) const noexcept { return formatCanRead(format_, kind); }
bool ImageFile::canWrite(MetadataKind kind) const noexcept { return formatCanWrite(format_, kind); }

void ImageFile::setIccProfile(Bytes profile) {
  icc_ = std::move(profile);
  iccChanged_ = true;
}

void ImageFile::clearMetadata() {
  exif_.clear();
  iptc_.clear();
  xmp_.clear();
  comment_.clear();
}

void ImageFile::load() {
  exif_.clear();
  iptc_.clear();
  xmp_.clear();
  comment_.clear();
  icc_.clear();
  iccChanged_ = false;
  xmpPacket_.clear();
  width_ = height_ = 0;
  doLoad();
}

void ImageFile::doSave(OutputSink&) const {
  throw Error(ErrorCode::unsupportedOperation,
              std::string("writing metadata to ") + formatName(format_) + " is not supported");
}

void ImageFile::saveTo(OutputSink& sink) const {
  if (writable(format_) == 0) {
    throw Error(ErrorCode::unsupportedOperation,
                std::string("writing metadata to ") + formatName(format_) + " is not supported");
  }
  doSave(sink);
}

void ImageFile::save() {
  if (dynamic_cast<const MemorySource*>(source_.get())) {
    MemorySink sink;
    saveTo(sink);
    source_ = std::make_unique<MemorySource>(sink.release());
    iccChanged_ = false;
    return;
  }
  if (!path_) {
    throw Error(ErrorCode::unsupportedOperation,
                "the image has no file or buffer to write to; use saveTo(OutputSink&)");
  }
  // Write beside the file, then replace it, so a failure leaves it intact.
  static std::atomic<unsigned> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path tmp = *path_;
  tmp += ".lumenlib-" + std::to_string(stamp) + "-" + std::to_string(counter++) + ".tmp";
  try {
    {
      FileSink sink(tmp);
      saveTo(sink);
      sink.close();
    }
    source_.reset();  // Windows cannot replace an open file.
    std::error_code ec;
    std::filesystem::rename(tmp, *path_, ec);
    if (ec) throw Error(ErrorCode::io, "cannot replace " + detail::pathText(*path_) + ": " + ec.message());
    source_ = std::make_unique<FileSource>(*path_);
    iccChanged_ = false;
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    if (!source_) source_ = std::make_unique<FileSource>(*path_);
    throw;
  }
}

void ImageFile::setXmpPacket(std::string packet) {
  xmpPacket_ = std::move(packet);
  // Trailing NULs appear in the wild.
  while (!xmpPacket_.empty() && xmpPacket_.back() == '\0') xmpPacket_.pop_back();
  try {
    xmp_ = XmpMetadata::parse(xmpPacket_);
  } catch (const Error& e) {
    // A damaged packet leaves the XMP empty; the rest of the metadata is
    // still usable. The raw packet stays in xmpPacket().
    xmp_.clear();
    detail::warn(std::string("XMP packet not read: ") + e.what());
  }
}

}  // namespace lumenlib

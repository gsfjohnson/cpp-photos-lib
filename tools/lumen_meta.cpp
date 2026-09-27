// lumen-meta: print and edit photo metadata from the command line.
//
//   lumen-meta [-p MODE] [-n] file...                  print
//   lumen-meta -M "set FAMILY KEY VALUE" [-M ...] file...
//   lumen-meta -M "add iptc KEY VALUE" file...           another value of a repeatable dataset
//   lumen-meta -M "del FAMILY KEY" file...
//
// FAMILY is exif, iptc or xmp; KEY is as the library names it
// (exif ifd0.Artist, iptc Keywords, xmp dc:title).
//
// Print modes: a all (the default), e Exif, i IPTC, x XMP, c comment,
// t Exif with each value described ("1/250 s"), m the maker note, s summary
// (what a photo album shows), X the raw XMP packet. Each line is the family,
// key, type, count and value; -n writes Exif and IPTC keys by number
// (exif ifd0.0x010f, iptc 2:25).
#include <lumenlib/lumenlib.hpp>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
  std::fprintf(
      stderr,
      "usage: lumen-meta [-p a|e|i|x|c|t|m|s|X] [-n] [-M \"set FAMILY KEY VALUE\" | -M \"add iptc KEY VALUE\" |\n"
      "                  -M \"del FAMILY KEY\"]... file...\n"
      "lumenlib %s\n",
      LUMENLIB_VERSION_STRING);
}

bool numeric = false;

void line(const char* family, const std::string& key, const char* type, std::size_t count, const std::string& value) {
  std::printf("%-4s %-50s %-9s %4zu  %s\n", family, key.c_str(), type, count, value.c_str());
}

std::string hex(unsigned v, int digits) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%0*x", digits, v);
  return b;
}

std::string printable(const lumenlib::FieldValue& v) {
  // Long binary values are summarised.
  if ((v.type() == lumenlib::FieldType::undefined || v.type() == lumenlib::FieldType::u8) && v.count() > 64) {
    return "(" + std::to_string(v.count()) + " bytes)";
  }
  return v.text();
}

void printExif(const lumenlib::ImageFile& file, bool described) {
  for (const auto& e : file.exif()) {
    const std::string key =
        numeric ? std::string(lumenlib::ifdName(e.ifd())) + "." + hex(e.number(), 4) : e.tag().str();
    line("exif", key, lumenlib::fieldTypeName(e.type()), e.count(), described ? e.describe() : printable(e.value()));
  }
  if (!file.exif().thumbnail().empty()) {
    line("exif", "(thumbnail)", "JPEG", file.exif().thumbnail().size(), "bytes");
  }
}

void printMakerNote(const lumenlib::ImageFile& file) {
  const auto* note = file.exif().makerNote();
  if (!note) return;
  for (const auto& e : note->entries()) {
    const std::string key = e.group + "." + (numeric ? hex(e.number, 4) : e.name);
    line("mknt", key, lumenlib::fieldTypeName(e.value.type()), e.value.count(), printable(e.value));
  }
  if (auto lens = lumenlib::lensDescription(file.exif())) line("mknt", "(lens)", "", 0, *lens);
}

void printIptc(const lumenlib::ImageFile& file) {
  for (const auto& e : file.iptc()) {
    const std::string key = numeric ? std::to_string(e.record()) + ":" + std::to_string(e.dataset()) : e.name();
    line("iptc", key, "text", e.value().size(), e.value());
  }
}

void printXmp(const lumenlib::ImageFile& file) {
  for (const auto& e : file.xmp()) {
    const auto& v = e.value();
    const std::size_t count = v.isArray()                                       ? v.items().size()
                              : v.kind() == lumenlib::XmpValue::Kind::langAlt   ? v.languages().size()
                              : v.kind() == lumenlib::XmpValue::Kind::structure ? 0
                                                                                : v.text().size();
    line("xmp", e.path(), lumenlib::xmpKindName(e.kind()), count, e.summary());
  }
}

template <typename T>
void field(const char* name, const std::optional<T>& v) {
  if (v) std::cout << "  " << name << ": " << *v << "\n";
}

void field(const char* name, const std::string& v) {
  if (!v.empty()) std::cout << "  " << name << ": " << v << "\n";
}

void printSummary(const lumenlib::ImageFile& file) {
  const auto info = lumenlib::readPhotoInfo(file);
  std::cout << "  format: " << lumenlib::formatName(file.format()) << "\n";
  if (info.width) std::cout << "  size: " << info.width << " x " << info.height << "\n";
  if (info.dateTaken) std::cout << "  date taken: " << info.dateTaken->toIso8601() << "\n";
  field("orientation", info.orientation);
  field("camera", (info.cameraMake + " " + info.cameraModel).substr(info.cameraMake.empty() ? 1 : 0));
  field("lens", info.lensModel);
  field("exposure time", info.exposureTime);
  field("f-number", info.fNumber);
  field("focal length", info.focalLength);
  field("ISO", info.iso);
  if (info.gps) {
    std::printf("  GPS: %.6f, %.6f", info.gps->latitude, info.gps->longitude);
    if (info.gps->altitude) std::printf(", %.1f m", *info.gps->altitude);
    std::printf("\n");
  }
  field("title", info.title);
  field("description", info.description);
  if (!info.keywords.empty()) {
    std::cout << "  keywords:";
    for (const auto& k : info.keywords) std::cout << " [" << k << "]";
    std::cout << "\n";
  }
  field("creator", info.creator);
  field("copyright", info.copyright);
  field("rating", info.rating);
}

// "verb family key [value]".
void modify(lumenlib::ImageFile& file, const std::string& command) {
  std::vector<std::string> words;
  std::size_t pos = 0;
  for (int i = 0; i < 3 && pos < command.size(); ++i) {
    const auto space = command.find(' ', pos);
    words.push_back(command.substr(pos, space == std::string::npos ? std::string::npos : space - pos));
    pos = space == std::string::npos ? command.size() : space + 1;
  }
  const std::string value = pos < command.size() ? command.substr(pos) : std::string();
  const auto bad = [&] {
    return lumenlib::Error(lumenlib::ErrorCode::invalidArgument, "bad command '" + command + "'");
  };
  if (words.size() < 3) throw bad();
  const std::string &verb = words[0], &family = words[1], &key = words[2];
  if (verb == "del") {
    if (family == "exif")
      file.exif().remove(key);
    else if (family == "iptc")
      file.iptc().remove(key);
    else if (family == "xmp")
      file.xmp().remove(key);
    else
      throw bad();
  } else if (verb == "set") {
    if (family == "exif")
      file.exif().setText(key, value);
    else if (family == "iptc")
      file.iptc().set(key, value);
    else if (family == "xmp")
      file.xmp().setText(key, value);
    else
      throw bad();
  } else if (verb == "add" && family == "iptc") {
    file.iptc().append(key, value);
  } else {
    throw bad();
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "a";
  std::vector<std::string> commands;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "-p" || arg == "-M") && i + 1 < argc) {
      (arg == "-p" ? mode : commands.emplace_back()) = argv[++i];
    } else if (arg == "-n") {
      numeric = true;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      usage();
      return 2;
    } else {
      files.push_back(arg);
    }
  }
  if (files.empty()) {
    usage();
    return 2;
  }

  int status = 0;
  for (const auto& name : files) {
    try {
      auto file = lumenlib::ImageFile::open(std::filesystem::u8path(name));
      file->load();
      if (!commands.empty()) {
        for (const auto& c : commands) modify(*file, c);
        file->save();
        continue;
      }
      if (files.size() > 1) std::printf("%s:\n", name.c_str());
      if (mode == "a" || mode == "e") printExif(*file, false);
      if (mode == "t") printExif(*file, true);
      if (mode == "a" || mode == "i") printIptc(*file);
      if (mode == "a" || mode == "x") printXmp(*file);
      if ((mode == "a" || mode == "c") && !file->comment().empty()) {
        line("com", "(comment)", "text", file->comment().size(), file->comment());
      }
      if (mode == "m") printMakerNote(*file);
      if (mode == "s") printSummary(*file);
      if (mode == "X") std::fwrite(file->xmpPacket().data(), 1, file->xmpPacket().size(), stdout);
    } catch (const lumenlib::Error& e) {
      std::fprintf(stderr, "lumen-meta: %s: %s\n", name.c_str(), e.what());
      status = 1;
    }
  }
  return status;
}

// Camera makers' notes (the Exif MakerNote tag), decoded read-only.
//
// A maker note is a private block in a format of the maker's choosing; most
// are TIFF-style IFDs behind a signature, with offsets counted from the
// note, from the Exif block's TIFF header or from a TIFF header of their own.
// The formats recognised here are Canon, Nikon (both generations), Sony,
// Olympus / OM System (with its equipment and camera-settings IFDs),
// Panasonic, Pentax, Fujifilm, Samsung and Apple. Their entries keep their
// numbers; the tags this library knows get names ("LensModel",
// "SerialNumber", ...), the rest print as "0xhhhh".
//
// The note itself stays in the Exif data (exif.MakerNote) as bytes, and is
// written back unchanged, so nothing here can damage it.
#pragma once

#include <lumenlib/exif.hpp>
#include <lumenlib/export.hpp>
#include <lumenlib/field_value.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lumenlib {

enum class MakerNoteFormat {
  canon,
  nikon,     // "Nikon\0" and a TIFF header of its own (type 3)
  nikonOld,  // "Nikon\0\1\0" (type 1) or a bare IFD from early Nikons
  sony,
  olympus,     // "OLYMPUS\0II" / "OM SYSTEM", offsets from the note
  olympusOld,  // "OLYMP\0", offsets from the Exif TIFF header
  panasonic,
  pentax,
  fujifilm,
  samsung,
  apple,
};

LUMENLIB_EXPORT const char* makerNoteFormatName(MakerNoteFormat format) noexcept;

struct MakerNoteEntry {
  // The IFD the entry came from: "canon", "nikon", "olympus.equipment", ...
  std::string group;
  std::uint16_t number = 0;
  // The tag's name, or "0xhhhh".
  std::string name;
  FieldValue value;
};

class LUMENLIB_EXPORT MakerNote {
 public:
  MakerNote(MakerNoteFormat format, std::vector<MakerNoteEntry> entries)
      : format_(format), entries_(std::move(entries)) {}

  MakerNoteFormat format() const noexcept { return format_; }
  const std::vector<MakerNoteEntry>& entries() const noexcept { return entries_; }
  // The first entry with the name ("LensModel") or group-qualified name
  // ("olympus.equipment.LensModel"), or nullptr.
  const MakerNoteEntry* find(std::string_view name) const;

 private:
  MakerNoteFormat format_;
  std::vector<MakerNoteEntry> entries_;
};

// The lens, as the camera recorded it: exif.LensModel, else the maker note's
// own lens name (Canon, Olympus and OM System, Panasonic), else the focal and
// aperture range the maker note (Nikon, Sony, Fujifilm) or
// exif.LensSpecification gives ("18-55mm F3.5-5.6"). Makers' numeric lens
// codes are not looked up. std::nullopt when nothing says.
LUMENLIB_EXPORT std::optional<std::string> lensDescription(const ExifMetadata& exif);

}  // namespace lumenlib

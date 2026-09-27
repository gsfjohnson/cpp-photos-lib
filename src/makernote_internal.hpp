// Internal: recognising, decoding and moving maker notes.
#pragma once

#include <lumenlib/io.hpp>
#include <lumenlib/makernote.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lumenlib::detail {

// Where a maker note's offsets count from.
enum class MakerNoteBase {
  note,  // the start of the note
  tiff,  // the TIFF header of the Exif block the note is in
  own,   // a TIFF header inside the note (Nikon), at ownHeader
};

struct MakerNoteLayout {
  MakerNoteFormat format;
  ByteOrder order;
  std::size_t ifd;  // the note's IFD, from the start of the note
  MakerNoteBase base;
  std::size_t ownHeader;  // for MakerNoteBase::own
};

// The layout of a note stored at `offset` (in the TIFF block `tiff`) with
// `size` bytes, for a camera whose ifd0.Make is `make`. std::nullopt for an
// unknown format.
std::optional<MakerNoteLayout> identifyMakerNote(const InputSource& tiff, std::uint64_t offset, std::size_t size,
                                                 ByteOrder exifOrder, const std::string& make);

// The note decoded; nullptr when it cannot be. Never throws for bad data.
std::shared_ptr<const MakerNote> decodeMakerNote(const InputSource& tiff, std::uint64_t offset, std::size_t size,
                                                 const MakerNoteLayout& layout);

// Moves the offsets inside a note whose offsets count from the TIFF header,
// for a note that was at `from` in its block and will be at `to` in the new
// one. Offsets that pointed into the note follow it; others are left alone.
// False when the note cannot be parsed (it is then left as it was).
bool relocateMakerNote(Bytes& note, const MakerNoteLayout& layout, std::uint64_t from, std::uint64_t to);

}  // namespace lumenlib::detail

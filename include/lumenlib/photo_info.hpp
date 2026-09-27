// What a photo album shows and edits, read from whichever of Exif, XMP and
// IPTC holds it, and written to all the places readers look.
//
// Reading follows the Metadata Working Group's guidance: Exif first for
// capture data (date, camera, exposure, GPS), XMP first for descriptive data
// (title, description, keywords, rating), IPTC after XMP.
#pragma once

#include <lumenlib/export.hpp>
#include <lumenlib/image_file.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lumenlib {

struct LUMENLIB_EXPORT DateTime {
  int year = 0, month = 0, day = 0;
  int hour = 0, minute = 0, second = 0;
  // Milliseconds, from SubSecTime* or a fractional XMP second.
  std::optional<int> millisecond;
  // Offset from UTC in minutes, from OffsetTime* or the XMP date.
  std::optional<int> utcOffsetMinutes;

  // "YYYY-MM-DDTHH:MM:SS[.mmm][+HH:MM]"
  std::string toIso8601() const;
  // "YYYY:MM:DD HH:MM:SS", as Exif writes dates.
  std::string toExif() const;
  // Accepts Exif ("YYYY:MM:DD HH:MM:SS") and ISO 8601 / XMP forms, with
  // missing trailing parts. std::nullopt when unparseable or all zero.
  static std::optional<DateTime> parse(const std::string& text);
};

struct GpsPosition {
  double latitude = 0;             // degrees, north positive
  double longitude = 0;            // degrees, east positive
  std::optional<double> altitude;  // metres above sea level
};

struct PhotoInfo {
  std::optional<DateTime> dateTaken;
  std::optional<DateTime> dateDigitized;
  // Exif orientation, 1..8.
  std::optional<int> orientation;
  // From the image's headers, or the Exif pixel dimensions.
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  std::string cameraMake;
  std::string cameraModel;
  // exif.LensModel, XMP's lens, or what the maker note says (see
  // lensDescription in makernote.hpp).
  std::string lensModel;
  std::optional<double> exposureTime;  // seconds
  std::optional<double> fNumber;
  std::optional<double> focalLength;  // mm
  std::optional<int> focalLength35mm;
  std::optional<int> iso;
  std::optional<bool> flashFired;

  std::optional<GpsPosition> gps;

  std::string title;
  std::string description;
  std::vector<std::string> keywords;
  std::string creator;
  std::string copyright;
  // XMP rating: -1 (rejected) or 0..5.
  std::optional<int> rating;
};

// Reads everything above from the file's metadata (call load() first).
LUMENLIB_EXPORT PhotoInfo readPhotoInfo(const ImageFile& file);

// The setters update every place the value is kept, so other applications
// see the change whichever they read. They change the file's metadata only;
// call ImageFile::save() to write it.

// ifd0.Orientation (and tiff:Orientation when present, or on a file without
// Exif).
LUMENLIB_EXPORT void setOrientation(ImageFile& file, int orientation);
// xmp:Rating (and MicrosoftPhoto:Rating as a percentage, ifd0.Rating and
// ifd0.RatingPercent when present). Throws Error(invalidArgument) outside
// -1..5.
LUMENLIB_EXPORT void setRating(ImageFile& file, int rating);
// dc:subject, and the IPTC Keywords when the file has IPTC.
LUMENLIB_EXPORT void setKeywords(ImageFile& file, const std::vector<std::string>& keywords);
// dc:title, and the IPTC ObjectName when the file has IPTC.
LUMENLIB_EXPORT void setTitle(ImageFile& file, const std::string& title);
// dc:description, ifd0.ImageDescription, and the IPTC CaptionAbstract when
// the file has IPTC.
LUMENLIB_EXPORT void setDescription(ImageFile& file, const std::string& description);
// exif.DateTimeOriginal (+ OffsetTimeOriginal, SubSecTimeOriginal),
// photoshop:DateCreated, and the IPTC DateCreated/TimeCreated when present.
LUMENLIB_EXPORT void setDateTaken(ImageFile& file, const DateTime& date);
// The GPS IFD (version 2.3), replacing any GPS position; exif:GPS* on a
// file without Exif.
LUMENLIB_EXPORT void setGpsPosition(ImageFile& file, const GpsPosition& position);
LUMENLIB_EXPORT void eraseGpsPosition(ImageFile& file);

}  // namespace lumenlib

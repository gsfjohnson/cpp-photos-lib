// What a photo album shows and edits, read from whichever of Exif, XMP and
// IPTC holds it, and written to all the places readers look.
//
// Reading follows the Metadata Working Group's guidance: Exif first for
// capture data (date, camera, exposure, GPS), XMP first for descriptive data
// (title, description, keywords, rating), IPTC after XMP.
#pragma once

#include <photos/export.hpp>
#include <photos/image.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace photos {

struct PHOTOS_EXPORT DateTime {
  int year = 0, month = 0, day = 0;
  int hour = 0, minute = 0, second = 0;
  // Milliseconds, from SubSecTime* or a fractional XMP second.
  std::optional<int> millisecond;
  // Offset from UTC in minutes, from OffsetTime* or the XMP date.
  std::optional<int> utcOffsetMinutes;

  // "YYYY-MM-DDTHH:MM:SS[.mmm][+HH:MM]"
  std::string toIso8601() const;
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
  // From Exif pixel dimensions or the image's headers.
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  std::string cameraMake;
  std::string cameraModel;
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

// Reads everything above from the image's metadata (call readMetadata first).
PHOTOS_EXPORT PhotoInfo readPhotoInfo(const Image& image);

// The setters update every place the value is kept, so other applications
// see the change whichever they read. They change the image's metadata only;
// call Image::writeMetadata() to save.

// Exif.Image.Orientation (and Xmp.tiff.Orientation when present).
PHOTOS_EXPORT void setOrientation(Image& image, int orientation);
// Xmp.xmp.Rating (and Xmp.MicrosoftPhoto.Rating as a percentage when present).
// Throws Error(invalidArgument) outside -1..5.
PHOTOS_EXPORT void setRating(Image& image, int rating);
// Xmp.dc.subject, and Iptc.Application2.Keywords when the image has IPTC.
PHOTOS_EXPORT void setKeywords(Image& image, const std::vector<std::string>& keywords);
// Xmp.dc.title, and Iptc.Application2.ObjectName when the image has IPTC.
PHOTOS_EXPORT void setTitle(Image& image, const std::string& title);
// Xmp.dc.description, Exif.Image.ImageDescription, and
// Iptc.Application2.Caption when the image has IPTC.
PHOTOS_EXPORT void setDescription(Image& image, const std::string& description);
// Exif DateTimeOriginal (+ OffsetTimeOriginal, SubSecTimeOriginal),
// Xmp.photoshop.DateCreated, and IPTC DateCreated/TimeCreated when present.
PHOTOS_EXPORT void setDateTaken(Image& image, const DateTime& date);
// The Exif GPS IFD (version 2.3), replacing any GPS position.
PHOTOS_EXPORT void setGpsPosition(Image& image, const GpsPosition& position);
PHOTOS_EXPORT void eraseGpsPosition(Image& image);

}  // namespace photos

#include "testing.hpp"

using namespace photos;
using testing::load;
using testing::roundTrip;

TEST(date_time_parsing) {
  auto d = DateTime::parse("2024:05:17 18:42:07");
  CHECK(d.has_value());
  CHECK_EQ(d->toIso8601(), "2024-05-17T18:42:07");
  d = DateTime::parse("2024-05-17T18:42:07.25+02:00");
  CHECK_EQ(*d->millisecond, 250);
  CHECK_EQ(*d->utcOffsetMinutes, 120);
  CHECK_EQ(d->toIso8601(), "2024-05-17T18:42:07.250+02:00");
  CHECK_EQ(DateTime::parse("2024-05-17T18:42Z")->utcOffsetMinutes.value(), 0);
  CHECK_EQ(DateTime::parse("20240517")->day, 17);
  CHECK_EQ(DateTime::parse("2024")->month, 1);
  CHECK(!DateTime::parse("0000:00:00 00:00:00"));
  CHECK(!DateTime::parse("    :  :     :  :  "));
  CHECK(!DateTime::parse("2024:13:01 00:00:00"));
  CHECK(!DateTime::parse("yesterday"));
}

TEST(photo_info_from_jpeg) {
  const auto image = load("photo.jpg");
  const PhotoInfo info = readPhotoInfo(*image);
  CHECK(info.dateTaken.has_value());
  CHECK_EQ(info.dateTaken->toIso8601(), "2024-05-17T18:42:07.250+02:00");
  CHECK_EQ(*info.orientation, 6);
  CHECK_EQ(info.width, 64u);
  CHECK_EQ(info.cameraMake, "PhotoCo");
  CHECK_EQ(info.cameraModel, "Model X");
  CHECK_EQ(info.lensModel, "Prime 35mm F2");
  CHECK_NEAR(*info.exposureTime, 0.004, 1e-9);
  CHECK_NEAR(*info.fNumber, 2.8, 1e-9);
  CHECK_EQ(*info.iso, 200);
  CHECK(info.gps.has_value());
  CHECK_NEAR(info.gps->latitude, 55.676111, 1e-5);
  CHECK_NEAR(info.gps->longitude, 12.568333, 1e-5);
  CHECK_NEAR(*info.gps->altitude, 14.5, 1e-9);
  // XMP wins over IPTC for descriptive data.
  CHECK_EQ(info.title, "Harbour at dusk");
  CHECK((info.keywords == std::vector<std::string>{"harbour", "boats", testing::kKobenhavn}));
  CHECK_EQ(info.creator, "Ada Lovelace");
  CHECK_EQ(info.description, "Boats in the harbour");
  CHECK_EQ(*info.rating, 4);
}

TEST(photo_info_setters_round_trip) {
  auto image = load("photo.jpg");
  setOrientation(*image, 8);
  setRating(*image, 5);
  setKeywords(*image, {"sea", "night"});
  setTitle(*image, "New title");
  setDescription(*image, "New description");
  DateTime date;
  date.year = 2023;
  date.month = 12;
  date.day = 31;
  date.hour = 23;
  date.minute = 59;
  date.second = 58;
  date.millisecond = 5;
  date.utcOffsetMinutes = -330;
  setDateTaken(*image, date);
  setGpsPosition(*image, {-33.856784, 151.215297, -2.5});

  const auto again = roundTrip(*image);
  const PhotoInfo info = readPhotoInfo(*again);
  CHECK_EQ(*info.orientation, 8);
  CHECK_EQ(*info.rating, 5);
  CHECK((info.keywords == std::vector<std::string>{"sea", "night"}));
  CHECK((again->iptcData().values("Iptc.Application2.Keywords") == std::vector<std::string>{"sea", "night"}));
  CHECK_EQ(info.title, "New title");
  CHECK_EQ(info.description, "New description");
  CHECK_EQ(again->exifData().find("Exif.Image.ImageDescription")->toString(), "New description");
  CHECK_EQ(info.dateTaken->toIso8601(), "2023-12-31T23:59:58.005-05:30");
  CHECK_EQ(again->exifData().find("Exif.Photo.OffsetTimeOriginal")->toString(), "-05:30");
  CHECK_EQ(again->iptcData().find("Iptc.Application2.TimeCreated")->toString(), "235958-0530");
  CHECK_NEAR(info.gps->latitude, -33.856784, 1e-6);
  CHECK_NEAR(info.gps->longitude, 151.215297, 1e-6);
  CHECK_NEAR(*info.gps->altitude, -2.5, 1e-9);
  CHECK_EQ(again->exifData().find("Exif.GPSInfo.GPSLatitudeRef")->toString(), "S");

  CHECK_THROWS(setRating(*image, 6), ErrorCode::invalidArgument);
  CHECK_THROWS(setOrientation(*image, 0), ErrorCode::invalidArgument);
  CHECK_THROWS(setGpsPosition(*image, {91, 0, std::nullopt}), ErrorCode::invalidArgument);
}

TEST(photo_info_setters_on_a_sidecar_use_xmp) {
  auto image = Image::createXmpSidecar();
  setOrientation(*image, 6);
  setGpsPosition(*image, {48.8584, 2.2945, 35});
  DateTime date;
  date.year = 2020;
  date.month = 2;
  date.day = 29;
  setDateTaken(*image, date);
  CHECK(image->exifData().empty());
  const auto again = roundTrip(*image);
  const PhotoInfo info = readPhotoInfo(*again);
  CHECK_EQ(*info.orientation, 6);
  CHECK_NEAR(info.gps->latitude, 48.8584, 1e-6);
  CHECK_NEAR(info.gps->longitude, 2.2945, 1e-6);
  CHECK_NEAR(*info.gps->altitude, 35, 1e-9);
  CHECK_EQ(info.dateTaken->toIso8601(), "2020-02-29T00:00:00");
}

TEST(photo_info_clearing) {
  auto image = load("photo.jpg");
  setKeywords(*image, {});
  setTitle(*image, "");
  eraseGpsPosition(*image);
  const auto again = roundTrip(*image);
  const PhotoInfo info = readPhotoInfo(*again);
  CHECK(info.keywords.empty());
  CHECK(info.title.empty());
  CHECK(!info.gps);
}

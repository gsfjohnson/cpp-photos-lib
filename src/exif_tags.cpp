// The Exif tag table: TIFF 6.0, Exif 3.0, GPS, interoperability and the DNG
// tags a photo library meets. Names are exiv2's. IFD1 (Exif.Thumbnail) uses
// IFD0's names.
#include <photos/exif.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <utility>

namespace photos {
namespace {

using T = TypeId;
constexpr T kByte = T::unsignedByte;
constexpr T kAscii = T::asciiString;
constexpr T kShort = T::unsignedShort;
constexpr T kLong = T::unsignedLong;
constexpr T kRational = T::unsignedRational;
constexpr T kSShort = T::signedShort;
constexpr T kSRational = T::signedRational;
constexpr T kUndefined = T::undefined;

const std::vector<ExifTagInfo>& table() {
  static const std::vector<ExifTagInfo> tags = {
      // IFD0: TIFF 6.0 and TIFF/EP
      {0x000b, IfdId::ifd0, "ProcessingSoftware", kAscii, -1},
      {0x00fe, IfdId::ifd0, "NewSubfileType", kLong, 1},
      {0x00ff, IfdId::ifd0, "SubfileType", kShort, 1},
      {0x0100, IfdId::ifd0, "ImageWidth", kLong, 1},
      {0x0101, IfdId::ifd0, "ImageLength", kLong, 1},
      {0x0102, IfdId::ifd0, "BitsPerSample", kShort, -1},
      {0x0103, IfdId::ifd0, "Compression", kShort, 1},
      {0x0106, IfdId::ifd0, "PhotometricInterpretation", kShort, 1},
      {0x0107, IfdId::ifd0, "Thresholding", kShort, 1},
      {0x010a, IfdId::ifd0, "FillOrder", kShort, 1},
      {0x010d, IfdId::ifd0, "DocumentName", kAscii, -1},
      {0x010e, IfdId::ifd0, "ImageDescription", kAscii, -1},
      {0x010f, IfdId::ifd0, "Make", kAscii, -1},
      {0x0110, IfdId::ifd0, "Model", kAscii, -1},
      {0x0111, IfdId::ifd0, "StripOffsets", kLong, -1},
      {0x0112, IfdId::ifd0, "Orientation", kShort, 1},
      {0x0115, IfdId::ifd0, "SamplesPerPixel", kShort, 1},
      {0x0116, IfdId::ifd0, "RowsPerStrip", kLong, 1},
      {0x0117, IfdId::ifd0, "StripByteCounts", kLong, -1},
      {0x011a, IfdId::ifd0, "XResolution", kRational, 1},
      {0x011b, IfdId::ifd0, "YResolution", kRational, 1},
      {0x011c, IfdId::ifd0, "PlanarConfiguration", kShort, 1},
      {0x011d, IfdId::ifd0, "PageName", kAscii, -1},
      {0x0122, IfdId::ifd0, "GrayResponseUnit", kShort, 1},
      {0x0123, IfdId::ifd0, "GrayResponseCurve", kShort, -1},
      {0x0124, IfdId::ifd0, "T4Options", kLong, 1},
      {0x0125, IfdId::ifd0, "T6Options", kLong, 1},
      {0x0128, IfdId::ifd0, "ResolutionUnit", kShort, 1},
      {0x0129, IfdId::ifd0, "PageNumber", kShort, 2},
      {0x012d, IfdId::ifd0, "TransferFunction", kShort, -1},
      {0x0131, IfdId::ifd0, "Software", kAscii, -1},
      {0x0132, IfdId::ifd0, "DateTime", kAscii, 20},
      {0x013b, IfdId::ifd0, "Artist", kAscii, -1},
      {0x013c, IfdId::ifd0, "HostComputer", kAscii, -1},
      {0x013d, IfdId::ifd0, "Predictor", kShort, 1},
      {0x013e, IfdId::ifd0, "WhitePoint", kRational, 2},
      {0x013f, IfdId::ifd0, "PrimaryChromaticities", kRational, 6},
      {0x0140, IfdId::ifd0, "ColorMap", kShort, -1},
      {0x0141, IfdId::ifd0, "HalftoneHints", kShort, 2},
      {0x0142, IfdId::ifd0, "TileWidth", kLong, 1},
      {0x0143, IfdId::ifd0, "TileLength", kLong, 1},
      {0x0144, IfdId::ifd0, "TileOffsets", kLong, -1},
      {0x0145, IfdId::ifd0, "TileByteCounts", kLong, -1},
      {0x014a, IfdId::ifd0, "SubIFDs", kLong, -1},
      {0x014c, IfdId::ifd0, "InkSet", kShort, 1},
      {0x014d, IfdId::ifd0, "InkNames", kAscii, -1},
      {0x014e, IfdId::ifd0, "NumberOfInks", kShort, 1},
      {0x0150, IfdId::ifd0, "DotRange", kByte, -1},
      {0x0151, IfdId::ifd0, "TargetPrinter", kAscii, -1},
      {0x0152, IfdId::ifd0, "ExtraSamples", kShort, -1},
      {0x0153, IfdId::ifd0, "SampleFormat", kShort, -1},
      {0x0154, IfdId::ifd0, "SMinSampleValue", kShort, -1},
      {0x0155, IfdId::ifd0, "SMaxSampleValue", kShort, -1},
      {0x0156, IfdId::ifd0, "TransferRange", kShort, 6},
      {0x0157, IfdId::ifd0, "ClipPath", kByte, -1},
      {0x0158, IfdId::ifd0, "XClipPathUnits", kSShort, 1},
      {0x0159, IfdId::ifd0, "YClipPathUnits", kSShort, 1},
      {0x015a, IfdId::ifd0, "Indexed", kShort, 1},
      {0x015b, IfdId::ifd0, "JPEGTables", kUndefined, -1},
      {0x015f, IfdId::ifd0, "OPIProxy", kShort, 1},
      {0x0200, IfdId::ifd0, "JPEGProc", kLong, 1},
      {0x0201, IfdId::ifd0, "JPEGInterchangeFormat", kLong, 1},
      {0x0202, IfdId::ifd0, "JPEGInterchangeFormatLength", kLong, 1},
      {0x0203, IfdId::ifd0, "JPEGRestartInterval", kShort, 1},
      {0x0205, IfdId::ifd0, "JPEGLosslessPredictors", kShort, -1},
      {0x0206, IfdId::ifd0, "JPEGPointTransforms", kShort, -1},
      {0x0207, IfdId::ifd0, "JPEGQTables", kLong, -1},
      {0x0208, IfdId::ifd0, "JPEGDCTables", kLong, -1},
      {0x0209, IfdId::ifd0, "JPEGACTables", kLong, -1},
      {0x0211, IfdId::ifd0, "YCbCrCoefficients", kRational, 3},
      {0x0212, IfdId::ifd0, "YCbCrSubSampling", kShort, 2},
      {0x0213, IfdId::ifd0, "YCbCrPositioning", kShort, 1},
      {0x0214, IfdId::ifd0, "ReferenceBlackWhite", kRational, 6},
      {0x02bc, IfdId::ifd0, "XMLPacket", kByte, -1},
      {0x4746, IfdId::ifd0, "Rating", kShort, 1},
      {0x4749, IfdId::ifd0, "RatingPercent", kShort, 1},
      {0x800d, IfdId::ifd0, "ImageID", kAscii, -1},
      {0x828d, IfdId::ifd0, "CFARepeatPatternDim", kShort, 2},
      {0x828e, IfdId::ifd0, "CFAPattern", kByte, -1},
      {0x828f, IfdId::ifd0, "BatteryLevel", kRational, 1},
      {0x8298, IfdId::ifd0, "Copyright", kAscii, -1},
      {0x829a, IfdId::ifd0, "ExposureTime", kRational, 1},
      {0x829d, IfdId::ifd0, "FNumber", kRational, 1},
      {0x83bb, IfdId::ifd0, "IPTCNAA", kLong, -1},
      {0x8649, IfdId::ifd0, "ImageResources", kByte, -1},
      {0x8769, IfdId::ifd0, "ExifTag", kLong, 1},
      {0x8773, IfdId::ifd0, "InterColorProfile", kUndefined, -1},
      {0x8822, IfdId::ifd0, "ExposureProgram", kShort, 1},
      {0x8824, IfdId::ifd0, "SpectralSensitivity", kAscii, -1},
      {0x8825, IfdId::ifd0, "GPSTag", kLong, 1},
      {0x8827, IfdId::ifd0, "ISOSpeedRatings", kShort, -1},
      {0x8828, IfdId::ifd0, "OECF", kUndefined, -1},
      {0x8829, IfdId::ifd0, "Interlace", kShort, 1},
      {0x882a, IfdId::ifd0, "TimeZoneOffset", kSShort, -1},
      {0x882b, IfdId::ifd0, "SelfTimerMode", kShort, 1},
      {0x9003, IfdId::ifd0, "DateTimeOriginal", kAscii, 20},
      {0x9102, IfdId::ifd0, "CompressedBitsPerPixel", kRational, 1},
      {0x9201, IfdId::ifd0, "ShutterSpeedValue", kSRational, 1},
      {0x9202, IfdId::ifd0, "ApertureValue", kRational, 1},
      {0x9203, IfdId::ifd0, "BrightnessValue", kSRational, 1},
      {0x9204, IfdId::ifd0, "ExposureBiasValue", kSRational, 1},
      {0x9205, IfdId::ifd0, "MaxApertureValue", kRational, 1},
      {0x9206, IfdId::ifd0, "SubjectDistance", kSRational, 1},
      {0x9207, IfdId::ifd0, "MeteringMode", kShort, 1},
      {0x9208, IfdId::ifd0, "LightSource", kShort, 1},
      {0x9209, IfdId::ifd0, "Flash", kShort, 1},
      {0x920a, IfdId::ifd0, "FocalLength", kRational, 1},
      {0x9216, IfdId::ifd0, "TIFFEPStandardID", kByte, 4},
      {0x9217, IfdId::ifd0, "SensingMethod", kShort, 1},
      {0x9c9b, IfdId::ifd0, "XPTitle", kByte, -1},
      {0x9c9c, IfdId::ifd0, "XPComment", kByte, -1},
      {0x9c9d, IfdId::ifd0, "XPAuthor", kByte, -1},
      {0x9c9e, IfdId::ifd0, "XPKeywords", kByte, -1},
      {0x9c9f, IfdId::ifd0, "XPSubject", kByte, -1},
      {0xc4a5, IfdId::ifd0, "PrintImageMatching", kUndefined, -1},
      // IFD0: DNG
      {0xc612, IfdId::ifd0, "DNGVersion", kByte, 4},
      {0xc613, IfdId::ifd0, "DNGBackwardVersion", kByte, 4},
      {0xc614, IfdId::ifd0, "UniqueCameraModel", kAscii, -1},
      {0xc615, IfdId::ifd0, "LocalizedCameraModel", kByte, -1},
      {0xc62f, IfdId::ifd0, "CameraSerialNumber", kAscii, -1},
      {0xc630, IfdId::ifd0, "LensInfo", kRational, 4},
      {0xc634, IfdId::ifd0, "DNGPrivateData", kByte, -1},
      {0xc65a, IfdId::ifd0, "CalibrationIlluminant1", kShort, 1},
      {0xc65b, IfdId::ifd0, "CalibrationIlluminant2", kShort, 1},
      {0xc68b, IfdId::ifd0, "OriginalRawFileName", kByte, -1},

      // Exif IFD
      {0x829a, IfdId::exif, "ExposureTime", kRational, 1},
      {0x829d, IfdId::exif, "FNumber", kRational, 1},
      {0x8822, IfdId::exif, "ExposureProgram", kShort, 1},
      {0x8824, IfdId::exif, "SpectralSensitivity", kAscii, -1},
      {0x8827, IfdId::exif, "ISOSpeedRatings", kShort, -1},
      {0x8828, IfdId::exif, "OECF", kUndefined, -1},
      {0x8830, IfdId::exif, "SensitivityType", kShort, 1},
      {0x8831, IfdId::exif, "StandardOutputSensitivity", kLong, 1},
      {0x8832, IfdId::exif, "RecommendedExposureIndex", kLong, 1},
      {0x8833, IfdId::exif, "ISOSpeed", kLong, 1},
      {0x8834, IfdId::exif, "ISOSpeedLatitudeyyy", kLong, 1},
      {0x8835, IfdId::exif, "ISOSpeedLatitudezzz", kLong, 1},
      {0x9000, IfdId::exif, "ExifVersion", kUndefined, 4},
      {0x9003, IfdId::exif, "DateTimeOriginal", kAscii, 20},
      {0x9004, IfdId::exif, "DateTimeDigitized", kAscii, 20},
      {0x9010, IfdId::exif, "OffsetTime", kAscii, 7},
      {0x9011, IfdId::exif, "OffsetTimeOriginal", kAscii, 7},
      {0x9012, IfdId::exif, "OffsetTimeDigitized", kAscii, 7},
      {0x9101, IfdId::exif, "ComponentsConfiguration", kUndefined, 4},
      {0x9102, IfdId::exif, "CompressedBitsPerPixel", kRational, 1},
      {0x9201, IfdId::exif, "ShutterSpeedValue", kSRational, 1},
      {0x9202, IfdId::exif, "ApertureValue", kRational, 1},
      {0x9203, IfdId::exif, "BrightnessValue", kSRational, 1},
      {0x9204, IfdId::exif, "ExposureBiasValue", kSRational, 1},
      {0x9205, IfdId::exif, "MaxApertureValue", kRational, 1},
      {0x9206, IfdId::exif, "SubjectDistance", kRational, 1},
      {0x9207, IfdId::exif, "MeteringMode", kShort, 1},
      {0x9208, IfdId::exif, "LightSource", kShort, 1},
      {0x9209, IfdId::exif, "Flash", kShort, 1},
      {0x920a, IfdId::exif, "FocalLength", kRational, 1},
      {0x9214, IfdId::exif, "SubjectArea", kShort, -1},
      {0x927c, IfdId::exif, "MakerNote", kUndefined, -1},
      {0x9286, IfdId::exif, "UserComment", kUndefined, -1},
      {0x9290, IfdId::exif, "SubSecTime", kAscii, -1},
      {0x9291, IfdId::exif, "SubSecTimeOriginal", kAscii, -1},
      {0x9292, IfdId::exif, "SubSecTimeDigitized", kAscii, -1},
      {0x9400, IfdId::exif, "Temperature", kSRational, 1},
      {0x9401, IfdId::exif, "Humidity", kRational, 1},
      {0x9402, IfdId::exif, "Pressure", kRational, 1},
      {0x9403, IfdId::exif, "WaterDepth", kSRational, 1},
      {0x9404, IfdId::exif, "Acceleration", kRational, 1},
      {0x9405, IfdId::exif, "CameraElevationAngle", kSRational, 1},
      {0xa000, IfdId::exif, "FlashpixVersion", kUndefined, 4},
      {0xa001, IfdId::exif, "ColorSpace", kShort, 1},
      {0xa002, IfdId::exif, "PixelXDimension", kLong, 1},
      {0xa003, IfdId::exif, "PixelYDimension", kLong, 1},
      {0xa004, IfdId::exif, "RelatedSoundFile", kAscii, 13},
      {0xa005, IfdId::exif, "InteroperabilityTag", kLong, 1},
      {0xa20b, IfdId::exif, "FlashEnergy", kRational, 1},
      {0xa20c, IfdId::exif, "SpatialFrequencyResponse", kUndefined, -1},
      {0xa20e, IfdId::exif, "FocalPlaneXResolution", kRational, 1},
      {0xa20f, IfdId::exif, "FocalPlaneYResolution", kRational, 1},
      {0xa210, IfdId::exif, "FocalPlaneResolutionUnit", kShort, 1},
      {0xa214, IfdId::exif, "SubjectLocation", kShort, 2},
      {0xa215, IfdId::exif, "ExposureIndex", kRational, 1},
      {0xa217, IfdId::exif, "SensingMethod", kShort, 1},
      {0xa300, IfdId::exif, "FileSource", kUndefined, 1},
      {0xa301, IfdId::exif, "SceneType", kUndefined, 1},
      {0xa302, IfdId::exif, "CFAPattern", kUndefined, -1},
      {0xa401, IfdId::exif, "CustomRendered", kShort, 1},
      {0xa402, IfdId::exif, "ExposureMode", kShort, 1},
      {0xa403, IfdId::exif, "WhiteBalance", kShort, 1},
      {0xa404, IfdId::exif, "DigitalZoomRatio", kRational, 1},
      {0xa405, IfdId::exif, "FocalLengthIn35mmFilm", kShort, 1},
      {0xa406, IfdId::exif, "SceneCaptureType", kShort, 1},
      {0xa407, IfdId::exif, "GainControl", kShort, 1},
      {0xa408, IfdId::exif, "Contrast", kShort, 1},
      {0xa409, IfdId::exif, "Saturation", kShort, 1},
      {0xa40a, IfdId::exif, "Sharpness", kShort, 1},
      {0xa40b, IfdId::exif, "DeviceSettingDescription", kUndefined, -1},
      {0xa40c, IfdId::exif, "SubjectDistanceRange", kShort, 1},
      {0xa420, IfdId::exif, "ImageUniqueID", kAscii, 33},
      {0xa430, IfdId::exif, "CameraOwnerName", kAscii, -1},
      {0xa431, IfdId::exif, "BodySerialNumber", kAscii, -1},
      {0xa432, IfdId::exif, "LensSpecification", kRational, 4},
      {0xa433, IfdId::exif, "LensMake", kAscii, -1},
      {0xa434, IfdId::exif, "LensModel", kAscii, -1},
      {0xa435, IfdId::exif, "LensSerialNumber", kAscii, -1},
      {0xa436, IfdId::exif, "ImageTitle", kAscii, -1},
      {0xa437, IfdId::exif, "Photographer", kAscii, -1},
      {0xa438, IfdId::exif, "ImageEditor", kAscii, -1},
      {0xa439, IfdId::exif, "CameraFirmware", kAscii, -1},
      {0xa43a, IfdId::exif, "RAWDevelopingSoftware", kAscii, -1},
      {0xa43b, IfdId::exif, "ImageEditingSoftware", kAscii, -1},
      {0xa43c, IfdId::exif, "MetadataEditingSoftware", kAscii, -1},
      {0xa460, IfdId::exif, "CompositeImage", kShort, 1},
      {0xa461, IfdId::exif, "SourceImageNumberOfCompositeImage", kShort, 2},
      {0xa462, IfdId::exif, "SourceExposureTimesOfCompositeImage", kUndefined, -1},
      {0xa500, IfdId::exif, "Gamma", kRational, 1},

      // GPS IFD
      {0x0000, IfdId::gps, "GPSVersionID", kByte, 4},
      {0x0001, IfdId::gps, "GPSLatitudeRef", kAscii, 2},
      {0x0002, IfdId::gps, "GPSLatitude", kRational, 3},
      {0x0003, IfdId::gps, "GPSLongitudeRef", kAscii, 2},
      {0x0004, IfdId::gps, "GPSLongitude", kRational, 3},
      {0x0005, IfdId::gps, "GPSAltitudeRef", kByte, 1},
      {0x0006, IfdId::gps, "GPSAltitude", kRational, 1},
      {0x0007, IfdId::gps, "GPSTimeStamp", kRational, 3},
      {0x0008, IfdId::gps, "GPSSatellites", kAscii, -1},
      {0x0009, IfdId::gps, "GPSStatus", kAscii, 2},
      {0x000a, IfdId::gps, "GPSMeasureMode", kAscii, 2},
      {0x000b, IfdId::gps, "GPSDOP", kRational, 1},
      {0x000c, IfdId::gps, "GPSSpeedRef", kAscii, 2},
      {0x000d, IfdId::gps, "GPSSpeed", kRational, 1},
      {0x000e, IfdId::gps, "GPSTrackRef", kAscii, 2},
      {0x000f, IfdId::gps, "GPSTrack", kRational, 1},
      {0x0010, IfdId::gps, "GPSImgDirectionRef", kAscii, 2},
      {0x0011, IfdId::gps, "GPSImgDirection", kRational, 1},
      {0x0012, IfdId::gps, "GPSMapDatum", kAscii, -1},
      {0x0013, IfdId::gps, "GPSDestLatitudeRef", kAscii, 2},
      {0x0014, IfdId::gps, "GPSDestLatitude", kRational, 3},
      {0x0015, IfdId::gps, "GPSDestLongitudeRef", kAscii, 2},
      {0x0016, IfdId::gps, "GPSDestLongitude", kRational, 3},
      {0x0017, IfdId::gps, "GPSDestBearingRef", kAscii, 2},
      {0x0018, IfdId::gps, "GPSDestBearing", kRational, 1},
      {0x0019, IfdId::gps, "GPSDestDistanceRef", kAscii, 2},
      {0x001a, IfdId::gps, "GPSDestDistance", kRational, 1},
      {0x001b, IfdId::gps, "GPSProcessingMethod", kUndefined, -1},
      {0x001c, IfdId::gps, "GPSAreaInformation", kUndefined, -1},
      {0x001d, IfdId::gps, "GPSDateStamp", kAscii, 11},
      {0x001e, IfdId::gps, "GPSDifferential", kShort, 1},
      {0x001f, IfdId::gps, "GPSHPositioningError", kRational, 1},

      // Interoperability IFD
      {0x0001, IfdId::interop, "InteroperabilityIndex", kAscii, -1},
      {0x0002, IfdId::interop, "InteroperabilityVersion", kUndefined, 4},
      {0x1000, IfdId::interop, "RelatedImageFileFormat", kAscii, -1},
      {0x1001, IfdId::interop, "RelatedImageWidth", kLong, 1},
      {0x1002, IfdId::interop, "RelatedImageLength", kLong, 1},
  };
  return tags;
}

// IFD1 shares IFD0's table.
IfdId tableIfd(IfdId ifd) { return ifd == IfdId::ifd1 ? IfdId::ifd0 : ifd; }

struct Index {
  std::map<std::pair<IfdId, std::uint16_t>, const ExifTagInfo*> byTag;
  std::map<std::pair<IfdId, std::string>, const ExifTagInfo*, std::less<>> byName;
};

const Index& index() {
  static const Index idx = [] {
    Index i;
    for (const auto& t : table()) {
      i.byTag.emplace(std::make_pair(t.ifd, t.tag), &t);
      i.byName.emplace(std::make_pair(t.ifd, std::string(t.name)), &t);
    }
    return i;
  }();
  return idx;
}

}  // namespace

const std::vector<ExifTagInfo>& exifTagList() { return table(); }

const ExifTagInfo* findExifTag(IfdId ifd, std::uint16_t tag) noexcept {
  const auto& m = index().byTag;
  const auto it = m.find({tableIfd(ifd), tag});
  return it == m.end() ? nullptr : it->second;
}

const ExifTagInfo* findExifTag(IfdId ifd, std::string_view name) noexcept {
  const auto& m = index().byName;
  const auto it = m.find(std::make_pair(tableIfd(ifd), std::string(name)));
  return it == m.end() ? nullptr : it->second;
}

}  // namespace photos

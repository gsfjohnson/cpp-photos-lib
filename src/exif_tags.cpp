// The Exif tag table: TIFF 6.0, Exif 3.0, GPS, interoperability and the DNG
// tags a photo library meets, named as the TIFF 6.0, TIFF/EP, Exif 3.0 and DNG
// specifications name them. IFD1 uses IFD0's names.
#include <lumenlib/exif.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <utility>

namespace lumenlib {
namespace {

using T = FieldType;
constexpr T kByte = T::u8;
constexpr T kAscii = T::ascii;
constexpr T kShort = T::u16;
constexpr T kLong = T::u32;
constexpr T kRational = T::urational;
constexpr T kSShort = T::i16;
constexpr T kSRational = T::srational;
constexpr T kUndefined = T::undefined;

const std::vector<ExifTagSpec>& table() {
  static const std::vector<ExifTagSpec> tags = {
      // IFD0: TIFF 6.0 and TIFF/EP
      {0x000b, Ifd::ifd0, "ProcessingSoftware", kAscii, -1},
      {0x00fe, Ifd::ifd0, "NewSubfileType", kLong, 1},
      {0x00ff, Ifd::ifd0, "SubfileType", kShort, 1},
      {0x0100, Ifd::ifd0, "ImageWidth", kLong, 1},
      {0x0101, Ifd::ifd0, "ImageLength", kLong, 1},
      {0x0102, Ifd::ifd0, "BitsPerSample", kShort, -1},
      {0x0103, Ifd::ifd0, "Compression", kShort, 1},
      {0x0106, Ifd::ifd0, "PhotometricInterpretation", kShort, 1},
      {0x0107, Ifd::ifd0, "Thresholding", kShort, 1},
      {0x010a, Ifd::ifd0, "FillOrder", kShort, 1},
      {0x010d, Ifd::ifd0, "DocumentName", kAscii, -1},
      {0x010e, Ifd::ifd0, "ImageDescription", kAscii, -1},
      {0x010f, Ifd::ifd0, "Make", kAscii, -1},
      {0x0110, Ifd::ifd0, "Model", kAscii, -1},
      {0x0111, Ifd::ifd0, "StripOffsets", kLong, -1},
      {0x0112, Ifd::ifd0, "Orientation", kShort, 1},
      {0x0115, Ifd::ifd0, "SamplesPerPixel", kShort, 1},
      {0x0116, Ifd::ifd0, "RowsPerStrip", kLong, 1},
      {0x0117, Ifd::ifd0, "StripByteCounts", kLong, -1},
      {0x011a, Ifd::ifd0, "XResolution", kRational, 1},
      {0x011b, Ifd::ifd0, "YResolution", kRational, 1},
      {0x011c, Ifd::ifd0, "PlanarConfiguration", kShort, 1},
      {0x011d, Ifd::ifd0, "PageName", kAscii, -1},
      {0x0122, Ifd::ifd0, "GrayResponseUnit", kShort, 1},
      {0x0123, Ifd::ifd0, "GrayResponseCurve", kShort, -1},
      {0x0124, Ifd::ifd0, "T4Options", kLong, 1},
      {0x0125, Ifd::ifd0, "T6Options", kLong, 1},
      {0x0128, Ifd::ifd0, "ResolutionUnit", kShort, 1},
      {0x0129, Ifd::ifd0, "PageNumber", kShort, 2},
      {0x012d, Ifd::ifd0, "TransferFunction", kShort, -1},
      {0x0131, Ifd::ifd0, "Software", kAscii, -1},
      {0x0132, Ifd::ifd0, "DateTime", kAscii, 20},
      {0x013b, Ifd::ifd0, "Artist", kAscii, -1},
      {0x013c, Ifd::ifd0, "HostComputer", kAscii, -1},
      {0x013d, Ifd::ifd0, "Predictor", kShort, 1},
      {0x013e, Ifd::ifd0, "WhitePoint", kRational, 2},
      {0x013f, Ifd::ifd0, "PrimaryChromaticities", kRational, 6},
      {0x0140, Ifd::ifd0, "ColorMap", kShort, -1},
      {0x0141, Ifd::ifd0, "HalftoneHints", kShort, 2},
      {0x0142, Ifd::ifd0, "TileWidth", kLong, 1},
      {0x0143, Ifd::ifd0, "TileLength", kLong, 1},
      {0x0144, Ifd::ifd0, "TileOffsets", kLong, -1},
      {0x0145, Ifd::ifd0, "TileByteCounts", kLong, -1},
      {0x014a, Ifd::ifd0, "SubIFDs", kLong, -1},
      {0x014c, Ifd::ifd0, "InkSet", kShort, 1},
      {0x014d, Ifd::ifd0, "InkNames", kAscii, -1},
      {0x014e, Ifd::ifd0, "NumberOfInks", kShort, 1},
      {0x0150, Ifd::ifd0, "DotRange", kByte, -1},
      {0x0151, Ifd::ifd0, "TargetPrinter", kAscii, -1},
      {0x0152, Ifd::ifd0, "ExtraSamples", kShort, -1},
      {0x0153, Ifd::ifd0, "SampleFormat", kShort, -1},
      {0x0154, Ifd::ifd0, "SMinSampleValue", kShort, -1},
      {0x0155, Ifd::ifd0, "SMaxSampleValue", kShort, -1},
      {0x0156, Ifd::ifd0, "TransferRange", kShort, 6},
      {0x0157, Ifd::ifd0, "ClipPath", kByte, -1},
      {0x0158, Ifd::ifd0, "XClipPathUnits", kSShort, 1},
      {0x0159, Ifd::ifd0, "YClipPathUnits", kSShort, 1},
      {0x015a, Ifd::ifd0, "Indexed", kShort, 1},
      {0x015b, Ifd::ifd0, "JPEGTables", kUndefined, -1},
      {0x015f, Ifd::ifd0, "OPIProxy", kShort, 1},
      {0x0200, Ifd::ifd0, "JPEGProc", kLong, 1},
      {0x0201, Ifd::ifd0, "JPEGInterchangeFormat", kLong, 1},
      {0x0202, Ifd::ifd0, "JPEGInterchangeFormatLength", kLong, 1},
      {0x0203, Ifd::ifd0, "JPEGRestartInterval", kShort, 1},
      {0x0205, Ifd::ifd0, "JPEGLosslessPredictors", kShort, -1},
      {0x0206, Ifd::ifd0, "JPEGPointTransforms", kShort, -1},
      {0x0207, Ifd::ifd0, "JPEGQTables", kLong, -1},
      {0x0208, Ifd::ifd0, "JPEGDCTables", kLong, -1},
      {0x0209, Ifd::ifd0, "JPEGACTables", kLong, -1},
      {0x0211, Ifd::ifd0, "YCbCrCoefficients", kRational, 3},
      {0x0212, Ifd::ifd0, "YCbCrSubSampling", kShort, 2},
      {0x0213, Ifd::ifd0, "YCbCrPositioning", kShort, 1},
      {0x0214, Ifd::ifd0, "ReferenceBlackWhite", kRational, 6},
      {0x02bc, Ifd::ifd0, "XMP", kByte, -1},
      {0x4746, Ifd::ifd0, "Rating", kShort, 1},
      {0x4749, Ifd::ifd0, "RatingPercent", kShort, 1},
      {0x800d, Ifd::ifd0, "ImageID", kAscii, -1},
      {0x828d, Ifd::ifd0, "CFARepeatPatternDim", kShort, 2},
      {0x828e, Ifd::ifd0, "CFAPattern", kByte, -1},
      {0x828f, Ifd::ifd0, "BatteryLevel", kRational, 1},
      {0x8298, Ifd::ifd0, "Copyright", kAscii, -1},
      {0x829a, Ifd::ifd0, "ExposureTime", kRational, 1},
      {0x829d, Ifd::ifd0, "FNumber", kRational, 1},
      {0x83bb, Ifd::ifd0, "IPTC", kLong, -1},
      {0x8649, Ifd::ifd0, "Photoshop", kByte, -1},
      {0x8769, Ifd::ifd0, "ExifIFDPointer", kLong, 1},
      {0x8773, Ifd::ifd0, "ICCProfile", kUndefined, -1},
      {0x8822, Ifd::ifd0, "ExposureProgram", kShort, 1},
      {0x8824, Ifd::ifd0, "SpectralSensitivity", kAscii, -1},
      {0x8825, Ifd::ifd0, "GPSInfoIFDPointer", kLong, 1},
      {0x8827, Ifd::ifd0, "PhotographicSensitivity", kShort, -1},
      {0x8828, Ifd::ifd0, "OECF", kUndefined, -1},
      {0x8829, Ifd::ifd0, "Interlace", kShort, 1},
      {0x882a, Ifd::ifd0, "TimeZoneOffset", kSShort, -1},
      {0x882b, Ifd::ifd0, "SelfTimerMode", kShort, 1},
      {0x9003, Ifd::ifd0, "DateTimeOriginal", kAscii, 20},
      {0x9102, Ifd::ifd0, "CompressedBitsPerPixel", kRational, 1},
      {0x9201, Ifd::ifd0, "ShutterSpeedValue", kSRational, 1},
      {0x9202, Ifd::ifd0, "ApertureValue", kRational, 1},
      {0x9203, Ifd::ifd0, "BrightnessValue", kSRational, 1},
      {0x9204, Ifd::ifd0, "ExposureBiasValue", kSRational, 1},
      {0x9205, Ifd::ifd0, "MaxApertureValue", kRational, 1},
      {0x9206, Ifd::ifd0, "SubjectDistance", kSRational, 1},
      {0x9207, Ifd::ifd0, "MeteringMode", kShort, 1},
      {0x9208, Ifd::ifd0, "LightSource", kShort, 1},
      {0x9209, Ifd::ifd0, "Flash", kShort, 1},
      {0x920a, Ifd::ifd0, "FocalLength", kRational, 1},
      {0x9216, Ifd::ifd0, "TIFFEPStandardID", kByte, 4},
      {0x9217, Ifd::ifd0, "SensingMethod", kShort, 1},
      {0x9c9b, Ifd::ifd0, "XPTitle", kByte, -1},
      {0x9c9c, Ifd::ifd0, "XPComment", kByte, -1},
      {0x9c9d, Ifd::ifd0, "XPAuthor", kByte, -1},
      {0x9c9e, Ifd::ifd0, "XPKeywords", kByte, -1},
      {0x9c9f, Ifd::ifd0, "XPSubject", kByte, -1},
      {0xc4a5, Ifd::ifd0, "PrintImageMatching", kUndefined, -1},
      // IFD0: DNG
      {0xc612, Ifd::ifd0, "DNGVersion", kByte, 4},
      {0xc613, Ifd::ifd0, "DNGBackwardVersion", kByte, 4},
      {0xc614, Ifd::ifd0, "UniqueCameraModel", kAscii, -1},
      {0xc615, Ifd::ifd0, "LocalizedCameraModel", kByte, -1},
      {0xc616, Ifd::ifd0, "CFAPlaneColor", kByte, -1},
      {0xc617, Ifd::ifd0, "CFALayout", kShort, 1},
      {0xc618, Ifd::ifd0, "LinearizationTable", kShort, -1},
      {0xc619, Ifd::ifd0, "BlackLevelRepeatDim", kShort, 2},
      {0xc61a, Ifd::ifd0, "BlackLevel", kRational, -1},
      {0xc61b, Ifd::ifd0, "BlackLevelDeltaH", kSRational, -1},
      {0xc61c, Ifd::ifd0, "BlackLevelDeltaV", kSRational, -1},
      {0xc61d, Ifd::ifd0, "WhiteLevel", kLong, -1},
      {0xc61e, Ifd::ifd0, "DefaultScale", kRational, 2},
      {0xc61f, Ifd::ifd0, "DefaultCropOrigin", kRational, 2},
      {0xc620, Ifd::ifd0, "DefaultCropSize", kRational, 2},
      {0xc621, Ifd::ifd0, "ColorMatrix1", kSRational, -1},
      {0xc622, Ifd::ifd0, "ColorMatrix2", kSRational, -1},
      {0xc623, Ifd::ifd0, "CameraCalibration1", kSRational, -1},
      {0xc624, Ifd::ifd0, "CameraCalibration2", kSRational, -1},
      {0xc625, Ifd::ifd0, "ReductionMatrix1", kSRational, -1},
      {0xc626, Ifd::ifd0, "ReductionMatrix2", kSRational, -1},
      {0xc627, Ifd::ifd0, "AnalogBalance", kRational, -1},
      {0xc628, Ifd::ifd0, "AsShotNeutral", kRational, -1},
      {0xc629, Ifd::ifd0, "AsShotWhiteXY", kRational, 2},
      {0xc62a, Ifd::ifd0, "BaselineExposure", kSRational, 1},
      {0xc62b, Ifd::ifd0, "BaselineNoise", kRational, 1},
      {0xc62c, Ifd::ifd0, "BaselineSharpness", kRational, 1},
      {0xc62d, Ifd::ifd0, "BayerGreenSplit", kLong, 1},
      {0xc62e, Ifd::ifd0, "LinearResponseLimit", kRational, 1},
      {0xc62f, Ifd::ifd0, "CameraSerialNumber", kAscii, -1},
      {0xc630, Ifd::ifd0, "LensInfo", kRational, 4},
      {0xc631, Ifd::ifd0, "ChromaBlurRadius", kRational, 1},
      {0xc632, Ifd::ifd0, "AntiAliasStrength", kRational, 1},
      {0xc633, Ifd::ifd0, "ShadowScale", kRational, 1},
      {0xc634, Ifd::ifd0, "DNGPrivateData", kByte, -1},
      {0xc635, Ifd::ifd0, "MakerNoteSafety", kShort, 1},
      {0xc65a, Ifd::ifd0, "CalibrationIlluminant1", kShort, 1},
      {0xc65b, Ifd::ifd0, "CalibrationIlluminant2", kShort, 1},
      {0xc68b, Ifd::ifd0, "OriginalRawFileName", kByte, -1},
      {0xc68c, Ifd::ifd0, "OriginalRawFileData", kUndefined, -1},
      {0xc68d, Ifd::ifd0, "ActiveArea", kLong, 4},
      {0xc68e, Ifd::ifd0, "MaskedAreas", kLong, -1},
      {0xc6f8, Ifd::ifd0, "ProfileName", kByte, -1},

      // Exif IFD
      {0x829a, Ifd::exif, "ExposureTime", kRational, 1},
      {0x829d, Ifd::exif, "FNumber", kRational, 1},
      {0x8822, Ifd::exif, "ExposureProgram", kShort, 1},
      {0x8824, Ifd::exif, "SpectralSensitivity", kAscii, -1},
      {0x8827, Ifd::exif, "PhotographicSensitivity", kShort, -1},
      {0x8828, Ifd::exif, "OECF", kUndefined, -1},
      {0x8830, Ifd::exif, "SensitivityType", kShort, 1},
      {0x8831, Ifd::exif, "StandardOutputSensitivity", kLong, 1},
      {0x8832, Ifd::exif, "RecommendedExposureIndex", kLong, 1},
      {0x8833, Ifd::exif, "ISOSpeed", kLong, 1},
      {0x8834, Ifd::exif, "ISOSpeedLatitudeyyy", kLong, 1},
      {0x8835, Ifd::exif, "ISOSpeedLatitudezzz", kLong, 1},
      {0x9000, Ifd::exif, "ExifVersion", kUndefined, 4},
      {0x9003, Ifd::exif, "DateTimeOriginal", kAscii, 20},
      {0x9004, Ifd::exif, "DateTimeDigitized", kAscii, 20},
      {0x9010, Ifd::exif, "OffsetTime", kAscii, 7},
      {0x9011, Ifd::exif, "OffsetTimeOriginal", kAscii, 7},
      {0x9012, Ifd::exif, "OffsetTimeDigitized", kAscii, 7},
      {0x9101, Ifd::exif, "ComponentsConfiguration", kUndefined, 4},
      {0x9102, Ifd::exif, "CompressedBitsPerPixel", kRational, 1},
      {0x9201, Ifd::exif, "ShutterSpeedValue", kSRational, 1},
      {0x9202, Ifd::exif, "ApertureValue", kRational, 1},
      {0x9203, Ifd::exif, "BrightnessValue", kSRational, 1},
      {0x9204, Ifd::exif, "ExposureBiasValue", kSRational, 1},
      {0x9205, Ifd::exif, "MaxApertureValue", kRational, 1},
      {0x9206, Ifd::exif, "SubjectDistance", kRational, 1},
      {0x9207, Ifd::exif, "MeteringMode", kShort, 1},
      {0x9208, Ifd::exif, "LightSource", kShort, 1},
      {0x9209, Ifd::exif, "Flash", kShort, 1},
      {0x920a, Ifd::exif, "FocalLength", kRational, 1},
      {0x9214, Ifd::exif, "SubjectArea", kShort, -1},
      {0x927c, Ifd::exif, "MakerNote", kUndefined, -1},
      {0x9286, Ifd::exif, "UserComment", kUndefined, -1},
      {0x9290, Ifd::exif, "SubSecTime", kAscii, -1},
      {0x9291, Ifd::exif, "SubSecTimeOriginal", kAscii, -1},
      {0x9292, Ifd::exif, "SubSecTimeDigitized", kAscii, -1},
      {0x9400, Ifd::exif, "Temperature", kSRational, 1},
      {0x9401, Ifd::exif, "Humidity", kRational, 1},
      {0x9402, Ifd::exif, "Pressure", kRational, 1},
      {0x9403, Ifd::exif, "WaterDepth", kSRational, 1},
      {0x9404, Ifd::exif, "Acceleration", kRational, 1},
      {0x9405, Ifd::exif, "CameraElevationAngle", kSRational, 1},
      {0xa000, Ifd::exif, "FlashpixVersion", kUndefined, 4},
      {0xa001, Ifd::exif, "ColorSpace", kShort, 1},
      {0xa002, Ifd::exif, "PixelXDimension", kLong, 1},
      {0xa003, Ifd::exif, "PixelYDimension", kLong, 1},
      {0xa004, Ifd::exif, "RelatedSoundFile", kAscii, 13},
      {0xa005, Ifd::exif, "InteroperabilityIFDPointer", kLong, 1},
      {0xa20b, Ifd::exif, "FlashEnergy", kRational, 1},
      {0xa20c, Ifd::exif, "SpatialFrequencyResponse", kUndefined, -1},
      {0xa20e, Ifd::exif, "FocalPlaneXResolution", kRational, 1},
      {0xa20f, Ifd::exif, "FocalPlaneYResolution", kRational, 1},
      {0xa210, Ifd::exif, "FocalPlaneResolutionUnit", kShort, 1},
      {0xa214, Ifd::exif, "SubjectLocation", kShort, 2},
      {0xa215, Ifd::exif, "ExposureIndex", kRational, 1},
      {0xa217, Ifd::exif, "SensingMethod", kShort, 1},
      {0xa300, Ifd::exif, "FileSource", kUndefined, 1},
      {0xa301, Ifd::exif, "SceneType", kUndefined, 1},
      {0xa302, Ifd::exif, "CFAPattern", kUndefined, -1},
      {0xa401, Ifd::exif, "CustomRendered", kShort, 1},
      {0xa402, Ifd::exif, "ExposureMode", kShort, 1},
      {0xa403, Ifd::exif, "WhiteBalance", kShort, 1},
      {0xa404, Ifd::exif, "DigitalZoomRatio", kRational, 1},
      {0xa405, Ifd::exif, "FocalLengthIn35mmFilm", kShort, 1},
      {0xa406, Ifd::exif, "SceneCaptureType", kShort, 1},
      {0xa407, Ifd::exif, "GainControl", kShort, 1},
      {0xa408, Ifd::exif, "Contrast", kShort, 1},
      {0xa409, Ifd::exif, "Saturation", kShort, 1},
      {0xa40a, Ifd::exif, "Sharpness", kShort, 1},
      {0xa40b, Ifd::exif, "DeviceSettingDescription", kUndefined, -1},
      {0xa40c, Ifd::exif, "SubjectDistanceRange", kShort, 1},
      {0xa420, Ifd::exif, "ImageUniqueID", kAscii, 33},
      {0xa430, Ifd::exif, "CameraOwnerName", kAscii, -1},
      {0xa431, Ifd::exif, "BodySerialNumber", kAscii, -1},
      {0xa432, Ifd::exif, "LensSpecification", kRational, 4},
      {0xa433, Ifd::exif, "LensMake", kAscii, -1},
      {0xa434, Ifd::exif, "LensModel", kAscii, -1},
      {0xa435, Ifd::exif, "LensSerialNumber", kAscii, -1},
      {0xa436, Ifd::exif, "ImageTitle", kAscii, -1},
      {0xa437, Ifd::exif, "Photographer", kAscii, -1},
      {0xa438, Ifd::exif, "ImageEditor", kAscii, -1},
      {0xa439, Ifd::exif, "CameraFirmware", kAscii, -1},
      {0xa43a, Ifd::exif, "RAWDevelopingSoftware", kAscii, -1},
      {0xa43b, Ifd::exif, "ImageEditingSoftware", kAscii, -1},
      {0xa43c, Ifd::exif, "MetadataEditingSoftware", kAscii, -1},
      {0xa460, Ifd::exif, "CompositeImage", kShort, 1},
      {0xa461, Ifd::exif, "SourceImageNumberOfCompositeImage", kShort, 2},
      {0xa462, Ifd::exif, "SourceExposureTimesOfCompositeImage", kUndefined, -1},
      {0xa500, Ifd::exif, "Gamma", kRational, 1},

      // GPS IFD
      {0x0000, Ifd::gps, "GPSVersionID", kByte, 4},
      {0x0001, Ifd::gps, "GPSLatitudeRef", kAscii, 2},
      {0x0002, Ifd::gps, "GPSLatitude", kRational, 3},
      {0x0003, Ifd::gps, "GPSLongitudeRef", kAscii, 2},
      {0x0004, Ifd::gps, "GPSLongitude", kRational, 3},
      {0x0005, Ifd::gps, "GPSAltitudeRef", kByte, 1},
      {0x0006, Ifd::gps, "GPSAltitude", kRational, 1},
      {0x0007, Ifd::gps, "GPSTimeStamp", kRational, 3},
      {0x0008, Ifd::gps, "GPSSatellites", kAscii, -1},
      {0x0009, Ifd::gps, "GPSStatus", kAscii, 2},
      {0x000a, Ifd::gps, "GPSMeasureMode", kAscii, 2},
      {0x000b, Ifd::gps, "GPSDOP", kRational, 1},
      {0x000c, Ifd::gps, "GPSSpeedRef", kAscii, 2},
      {0x000d, Ifd::gps, "GPSSpeed", kRational, 1},
      {0x000e, Ifd::gps, "GPSTrackRef", kAscii, 2},
      {0x000f, Ifd::gps, "GPSTrack", kRational, 1},
      {0x0010, Ifd::gps, "GPSImgDirectionRef", kAscii, 2},
      {0x0011, Ifd::gps, "GPSImgDirection", kRational, 1},
      {0x0012, Ifd::gps, "GPSMapDatum", kAscii, -1},
      {0x0013, Ifd::gps, "GPSDestLatitudeRef", kAscii, 2},
      {0x0014, Ifd::gps, "GPSDestLatitude", kRational, 3},
      {0x0015, Ifd::gps, "GPSDestLongitudeRef", kAscii, 2},
      {0x0016, Ifd::gps, "GPSDestLongitude", kRational, 3},
      {0x0017, Ifd::gps, "GPSDestBearingRef", kAscii, 2},
      {0x0018, Ifd::gps, "GPSDestBearing", kRational, 1},
      {0x0019, Ifd::gps, "GPSDestDistanceRef", kAscii, 2},
      {0x001a, Ifd::gps, "GPSDestDistance", kRational, 1},
      {0x001b, Ifd::gps, "GPSProcessingMethod", kUndefined, -1},
      {0x001c, Ifd::gps, "GPSAreaInformation", kUndefined, -1},
      {0x001d, Ifd::gps, "GPSDateStamp", kAscii, 11},
      {0x001e, Ifd::gps, "GPSDifferential", kShort, 1},
      {0x001f, Ifd::gps, "GPSHPositioningError", kRational, 1},

      // Interoperability IFD
      {0x0001, Ifd::interop, "InteroperabilityIndex", kAscii, -1},
      {0x0002, Ifd::interop, "InteroperabilityVersion", kUndefined, 4},
      {0x1000, Ifd::interop, "RelatedImageFileFormat", kAscii, -1},
      {0x1001, Ifd::interop, "RelatedImageWidth", kLong, 1},
      {0x1002, Ifd::interop, "RelatedImageLength", kLong, 1},
  };
  return tags;
}

// IFD1 shares IFD0's table.
Ifd tableIfd(Ifd ifd) { return ifd == Ifd::ifd1 ? Ifd::ifd0 : ifd; }

struct Index {
  std::map<std::pair<Ifd, std::uint16_t>, const ExifTagSpec*> byTag;
  std::map<std::pair<Ifd, std::string>, const ExifTagSpec*, std::less<>> byName;
};

const Index& index() {
  static const Index idx = [] {
    Index i;
    for (const auto& t : table()) {
      i.byTag.emplace(std::make_pair(t.ifd, t.number), &t);
      i.byName.emplace(std::make_pair(t.ifd, std::string(t.name)), &t);
    }
    return i;
  }();
  return idx;
}

}  // namespace

const std::vector<ExifTagSpec>& exifTagTable() { return table(); }

const ExifTagSpec* lookupExifTag(Ifd ifd, std::uint16_t tag) noexcept {
  const auto& m = index().byTag;
  const auto it = m.find({tableIfd(ifd), tag});
  return it == m.end() ? nullptr : it->second;
}

const ExifTagSpec* lookupExifTag(Ifd ifd, std::string_view name) noexcept {
  const auto& m = index().byName;
  const auto it = m.find(std::make_pair(tableIfd(ifd), std::string(name)));
  return it == m.end() ? nullptr : it->second;
}

}  // namespace lumenlib

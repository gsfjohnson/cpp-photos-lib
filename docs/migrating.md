# Moving Lumen from exiv2 to lumenlib

Lumen reads and writes metadata through exiv2 in
`src/core/src/metadata/` (`exiv2_support`, `metadata_reader`,
`export_metadata`, `sidecar`). This page maps each exiv2 call and key used
there onto lumenlib, and lists the differences in behaviour the port has to
account for. Once the port is done, exiv2 and expat can leave
`lumen-ios-deps` ([ios-deps.md](ios-deps.md)).

## Linking

```cmake
add_subdirectory(third_party/lumenlib)   # or find_package(lumenlib CONFIG REQUIRED)
target_link_libraries(photocore PRIVATE lumenlib::lumenlib)
```

lumenlib builds from source for every platform, iOS and Android included, so
it needs no prebuilt package. Include `<lumenlib/lumenlib.hpp>`.

## Setting up

| exiv2 (Lumen) | lumenlib |
| --- | --- |
| `ensureExiv2()`: `XmpParser::initialize` with a lock | nothing: there is no global state to set up, and the XMP namespace registry locks itself |
| `LogMsg::setLevel` / `setHandler(&logExiv2)` | `lumenlib::setWarningHandler([](const std::string& m) { spdlog::debug("lumenlib: {}", m); })` |
| `ReadOnlyFileIo` (narrow paths, `mmap`) | not needed: `ImageFile::open(const std::filesystem::path&)` opens by wide path on Windows and reads with seeks, never mapping |

## Opening and reading

| exiv2 | lumenlib |
| --- | --- |
| `ImageFactory::open(io)` then `readMetadata()` | `auto file = ImageFile::open(path); file->load();` |
| `ImageFactory::open(bytes, size)` | `ImageFile::open(Bytes(...))` |
| a null `Image::UniquePtr` for an unknown format | `Error` with `code() == ErrorCode::unsupportedFormat` (or ask `detectFormat(path)` first) |
| `ImageFactory::getType(io)` | `detectFormat(path)` |
| `ImageFactory::checkMode(type, mdExif) == amReadWrite` | `formatCanWrite(format, MetadataKind::exif)` |
| `image->imageType() == ImageType::jpeg` | `file->format() == FileFormat::jpeg` |
| `image->exifData()`, `iptcData()`, `xmpData()` | `file->exif()`, `iptc()`, `xmp()` |
| `image->pixelWidth()`, `pixelHeight()` | `file->width()`, `height()` |
| `image->iccProfile()` | `file->iccProfile()` |

## Exif

| exiv2 | lumenlib |
| --- | --- |
| `exif.findKey(ExifKey(k))`, `it != exif.end()` | `exif.find(k)` (a pointer, `nullptr` when absent) |
| `datum.toString()` | `entry.text()` |
| `datum.toInt64(n)` | `entry.asInt(n)` |
| `datum.toRational(n)` (`first`, `second`) | `entry.asRational(n)` (`numerator`, `denominator`) |
| `datum.count()`, `typeId()` | `entry.count()`, `type()` (`FieldType::u16` is SHORT, `urational` RATIONAL, ...) |
| `datum.groupName() == "GPSInfo"` / `"Image"` / `"Photo"` | `entry.ifd() == Ifd::gps` / `Ifd::ifd0` / `Ifd::exif` |
| `datum.tagName()` | `entry.name()` |
| `datum.print(&exif)` | `entry.describe()` |
| the erase-while-found loop (`eraseExif`) | `exif.remove(k)` |
| `for (it = ...) it = pred ? exif.erase(it) : next(it)` | `exif.removeIf(pred)` |
| `exif[k] = uint16_t{1}` | `exif.setInt(k, 1)` |
| `exif[k] = text` | `exif.setText(k, text)` |
| `exif.add(ExifKey(k), &value)` | `exif.set(k, value)` (replaces) or `exif.append(ExifTag(k), value)` |
| `Value::create(unsignedByte)->read("2 3 0 0")` | `FieldValue::parse(FieldType::u8, "2 3 0 0")` |
| `URationalValue` with `value_.emplace_back(n, d)` | `FieldValue::rationals(FieldType::urational, {{n, d}, ...})` |
| `exif[key].setValue(&datum.value())` | `exif.set(entry.tag(), entry.value())` |
| `image->setExifData(exif)` | `file->exif() = exif` |
| `ExifParser::encode(blob, littleEndian, exif)` | `exif.setByteOrder(ByteOrder::little); Bytes blob = exif.encode();` |
| `lensName(exif)` then `print` | `lensDescription(exif)` (see below) |

The keys Lumen uses:

| exiv2 | lumenlib |
| --- | --- |
| `Exif.Image.Make`, `Model`, `Orientation`, `Rating`, `RatingPercent`, `ImageDescription`, `DateTime` | `ifd0.Make`, `ifd0.Model`, ... (same names) |
| `Exif.Photo.ISOSpeedRatings` | `exif.PhotographicSensitivity` (its name since Exif 2.3) |
| `Exif.Photo.RecommendedExposureIndex`, `ISOSpeed`, `ExposureTime`, `ShutterSpeedValue`, `FNumber`, `ApertureValue`, `FocalLength`, `FocalLengthIn35mmFilm`, `LensModel`, `PixelXDimension`, `PixelYDimension`, `MakerNote`, `CompressedBitsPerPixel` | `exif.RecommendedExposureIndex`, ... (same names) |
| `Exif.Photo.DateTimeOriginal`, `SubSecTimeOriginal`, `OffsetTimeOriginal` (and the `Digitized` and plain ones) | `exif.DateTimeOriginal`, ... (same names) |
| `Exif.GPSInfo.GPSLatitude` and the other GPS tags | `gps.GPSLatitude`, ... (same names) |
| `Exif.Photo.InteroperabilityTag` | not an entry: the IFD pointers are written from the layout, so `keptExif` no longer meets it |

The IFD can be left out when the name says it: `"DateTimeOriginal"` is
`exif.DateTimeOriginal`.

## XMP

| exiv2 | lumenlib |
| --- | --- |
| `XmpParser::decode(xmp, packet) != 0` | `XmpMetadata::parse(packet)`, which throws `Error(corruptData)` |
| `XmpParser::encode(packet, xmp)` | `xmp.serialize()` |
| `encode(packet, xmp, omitPacketWrapper \| useCompactFormat)` | `XmpWriteOptions o; o.packetWrapper = false; o.compact = true; xmp.serialize(o)` |
| `xmp.findKey(XmpKey(k))` | `xmp.find(k)`, or `xmp.text(k)` for the text of a simple property, the x-default of a language alternative or the first item of an array |
| `eraseXmpKey(xmp, key)` | `xmp.remove(k)` (also removes what is nested under it) |
| `datum.typeId() == langAlt`, `LangAltValue::value_` | `entry.kind() == XmpValue::Kind::langAlt`, `entry.value().languages()` / `langText()` |
| `datum.toString(i)` of a bag | `entry.value().items()[i]` |
| `Value::create(xmpBag)` + `read` for each keyword | `xmp.setItems("dc:subject", keywords)` |
| `setXmpDefaultLanguage(xmp, k, text)` | `text.empty() ? xmp.remove(k) : xmp.setLangText(k, "x-default", text)` (`setLangText` with empty text removes only that language) |
| `xmp["Xmp.xmp.Rating"] = s` | `xmp.setText("xmp:Rating", s)` |
| `it->key()` starting with `"Xmp.exif.GPS"` | `it->path()` starting with `"exif:GPS"` |

Keys are the XMP specification's paths: the prefix, a colon, the name.

| exiv2 | lumenlib |
| --- | --- |
| `Xmp.xmp.Rating`, `CreateDate`, `MetadataDate`, `CreatorTool` | `xmp:Rating`, ... |
| `Xmp.dc.subject`, `title`, `description` | `dc:subject`, `dc:title`, `dc:description` |
| `Xmp.exif.DateTimeOriginal`, `GPSLatitude`, `GPSLongitude`, `GPSAltitude`, `GPSAltitudeRef`, `ExposureTime`, `FNumber`, `FocalLength`, `FocalLengthIn35mmFilm`, `ISOSpeedRatings` | `exif:DateTimeOriginal`, ... |
| `Xmp.exifEX.LensModel`, `PhotographicSensitivity` | `exifEX:LensModel`, `exifEX:PhotographicSensitivity` |
| `Xmp.aux.Lens` | `aux:Lens` |
| `Xmp.tiff.Orientation`, `Make`, `Model`, `ImageWidth`, `ImageLength` | `tiff:Orientation`, ... |
| `Xmp.photoshop.DateCreated`, `City`, `State`, `Country` | `photoshop:DateCreated`, ... |
| `Xmp.iptc.Location`, `CountryCode` | `Iptc4xmpCore:Location`, `Iptc4xmpCore:CountryCode` |
| `Xmp.iptcExt.LocationCreated...`, `LocationShown...` | `Iptc4xmpExt:LocationCreated...`, `Iptc4xmpExt:LocationShown...` (and `Iptc4xmpExt:` in the nested steps) |
| the prefixes `Xmp.crs.`, `Xmp.xmpMM.` | `crs:`, `xmpMM:` |

## IPTC

| exiv2 | lumenlib |
| --- | --- |
| `iptc.findKey(IptcKey(k))`, `toString()` | `iptc.value(k)` (a `std::optional<std::string>`) |
| every datum whose `key()` is the keywords | `iptc.values("Keywords")` |
| `eraseIptc`, then `addIptcText` for each value | `iptc.setValues(k, values)` |
| `iptc.add(IptcKey(k), &StringValue(text))` | `iptc.append(k, text)` |
| setting `Iptc.Envelope.CharacterSet` to UTF-8 | not needed: `encode()` declares UTF-8 whenever a value needs it |
| `writable(type, mdIptc)` | `formatCanWrite(format, MetadataKind::iptc)` |

Datasets are named as the IIM 4.2 specification names them:

| exiv2 | lumenlib |
| --- | --- |
| `Iptc.Application2.Keywords`, `ObjectName`, `DateCreated`, `TimeCreated`, `City`, `SubLocation`, `ProvinceState` | `Keywords`, `ObjectName`, `DateCreated`, `TimeCreated`, `City`, `SubLocation`, `ProvinceState` |
| `Iptc.Application2.Caption` | `CaptionAbstract` |
| `Iptc.Application2.CountryCode`, `CountryName` | `CountryPrimaryLocationCode`, `CountryPrimaryLocationName` |
| `Iptc.Application2.LocationCode`, `LocationName` | `ContentLocationCode`, `ContentLocationName` |
| `Iptc.Envelope.CharacterSet` | `CodedCharacterSet` |

## Writing

| exiv2 | lumenlib |
| --- | --- |
| `image->writeMetadata()`, then `bytesOf(*image)` for one in memory | `file->save()`, then `*file->buffer()` |
| `image->setXmpData(xmp)`, `setIptcData(iptc)` | `file->xmp() = xmp`, `file->iptc() = iptc` |
| `clearMetadata()`, then `setIccProfile(kept)` | `file->clearMetadata()`: the ICC profile stays by itself |
| `setIccProfile(profile)` | `file->setIccProfile(profile)` (empty removes it) |

`save()` on a file opened from a path writes a temporary file beside it and
replaces the original, as `writeFileAtomically` does.

## What behaves differently

- **Raw originals can be rewritten.** `formatCanWrite(FileFormat::tiff, ...)`
  is true, so `canRewriteMetadata()` now says yes for DNG, CR2, NEF, ARW,
  ORF, PEF and SRW, and `removeMetadata()` works on them: the image data and
  every IFD after IFD0 stay as they were, and what is removed is overwritten
  with zeros. Keep `exif.MakerNote` when stripping "everything" from a raw:
  raw developers (libraw included) read colour and white balance data from
  it, and exiv2's `clearMetadata()` removed it too. RW2, CR3, RAF, HEIF and
  AVIF stay read-only (`FileFormat::rw2` is its own format for that reason:
  an RW2 repeats its metadata in the JPEG it carries).
- **The lens.** `lensDescription()` returns `exif.LensModel`, else the
  maker's own lens text (Canon's LensModel, Olympus's equipment LensModel,
  Panasonic's LensType), else the focal and aperture range that the maker
  note (Nikon, Sony, Fujifilm, Canon's camera settings) or
  `exif.LensSpecification` gives, such as `"18-55mm F3.5-5.6"`. exiv2 looked
  numeric lens codes up in its own tables; lumenlib has no such tables, so an
  older Canon or Pentax body gives a range or nothing instead of a product
  name. Lumen's filter for placeholder names still applies.
- **A bad Exif block does not lose the file.** exiv2 0.28 rejected a JPEG
  whose Exif segment is not TIFF (`bad_exif.jpg` in Lumen's fixtures);
  lumenlib reads the rest of the file and reports a warning. Lumen's
  metadata tests that expect the import warning need adjusting.
- **Unknown formats throw.** A file of a few bytes, or an unknown format,
  is `ErrorCode::unsupportedFormat` from `ImageFile::open`, not a null
  image; `readEmbeddedMetadata` should turn it into "nothing to say".
- **CR3 maker notes** are decoded from Canon's CMT3 box and reached through
  `exif.makerNote()`; there is no `exif.MakerNote` entry for them.
- **RW2 ISO.** exiv2 merged an RW2's embedded JPEG's Exif into the RW2's.
  lumenlib reads the RW2's own IFDs, which lack the ISO: `readPhotoInfo`
  takes it from the RW2's tag 0x0017; code reading tags itself can use
  `ifd0.0x0017`.
- **Type names.** Values print as the TIFF specification writes their types
  (`SHORT`, `RATIONAL`) and XMP kinds are `text`, `bag`, `seq`, `alt`,
  `lang-alt` and `struct`.

## Checked against real cameras

These files from [raw.pixls.us](https://raw.pixls.us) (CC0), the ones
Lumen's `RawSamples` test uses, read the same in lumenlib as in exiv2 0.28.8
(every Exif and IPTC tag by number, every XMP property), except the RW2,
which the two read from different places (see above). The writable ones
(CR2, NEF, ARW, ORF, PEF, DNG) were edited with `lumen-meta` and read back
by exiv2 with their maker notes intact, and the Canon, Sony, Pentax and
Fujifilm maker notes, moved into a JPEG, decode the same in exiv2:

| File | Maker note | Lens from `lensDescription` |
| --- | --- | --- |
| Canon EOS 350D (CR2) | Canon | `70-300mm` |
| Canon EOS M50 (CR3) | Canon (CMT3) | `EF-M18-150mm f/3.5-6.3 IS STM` |
| Nikon D40 (NEF) | Nikon | `18-55mm F3.5-5.6` |
| Sony NEX-3 (ARW) | Sony | `E 18-55mm F3.5-5.6 OSS` |
| Fujifilm FinePix S5600 (RAF) | Fujifilm | - (a compact) |
| Olympus E-420 (ORF) | Olympus | `OLYMPUS 14-42mm Lens` |
| Panasonic DMC-LX3 (RW2) | Panasonic | - (a compact) |
| Pentax K100D (PEF) | Pentax | - (numeric lens code only) |
| Google Pixel 4a (DNG) | - | - |

Run the unit tests with `LUMENLIB_SAMPLES` pointing at a folder of such
files to repeat the reading and rewriting checks.

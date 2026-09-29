#!/usr/bin/env python3
"""Regenerates the test images in this directory.

Needs Pillow (with AVIF), pillow-heif, exiftool and libheif's heif-enc:

    pip install pillow pillow-heif && apt install libimage-exiftool-perl libheif-examples
    python3 tests/data/generate.py

The images are small and committed, so the tests need none of this. The
metadata values are what tests/unit checks for.
"""
import io
import os
import subprocess

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

XMP = """<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:dc="http://purl.org/dc/elements/1.1/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/"
    xmlns:mwg-rs="http://www.metadataworkinggroup.com/schemas/regions/"
    xmlns:stDim="http://ns.adobe.com/xap/1.0/sType/Dimensions#"
    xmlns:stArea="http://ns.adobe.com/xmp/sType/Area#"
    xmp:Rating="4">
   <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Harbour at dusk</rdf:li><rdf:li xml:lang="de">Hafen</rdf:li></rdf:Alt></dc:title>
   <dc:subject><rdf:Bag><rdf:li>harbour</rdf:li><rdf:li>boats</rdf:li><rdf:li>København</rdf:li></rdf:Bag></dc:subject>
   <dc:creator><rdf:Seq><rdf:li>Ada Lovelace</rdf:li></rdf:Seq></dc:creator>
   <mwg-rs:Regions rdf:parseType="Resource">
    <mwg-rs:AppliedToDimensions stDim:w="64" stDim:h="48" stDim:unit="pixel"/>
    <mwg-rs:RegionList>
     <rdf:Bag>
      <rdf:li>
       <rdf:Description mwg-rs:Name="Ada" mwg-rs:Type="Face">
        <mwg-rs:Area stArea:x="0.5" stArea:y="0.25" stArea:w="0.1" stArea:h="0.2" stArea:unit="normalized"/>
       </rdf:Description>
      </rdf:li>
     </rdf:Bag>
    </mwg-rs:RegionList>
   </mwg-rs:Regions>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>"""

EXIF_ARGS = [
    "-Make=PhotoCo",
    "-Model=Model X",
    "-DateTimeOriginal=2024:05:17 18:42:07",
    "-SubSecTimeOriginal=250",
    "-OffsetTimeOriginal=+02:00",
    "-Orientation#=6",
    "-ExposureTime=1/250",
    "-FNumber=2.8",
    "-ISO=200",
    "-FocalLength=35",
    "-LensModel=Prime 35mm F2",
    "-GPSLatitude=55.676111",
    "-GPSLatitudeRef=N",
    "-GPSLongitude=12.568333",
    "-GPSLongitudeRef=E",
    "-GPSAltitude=14.5",
    "-GPSAltitudeRef=0",
    "-ImageDescription=A harbour",
]

IPTC_ARGS = [
    "-IPTC:CodedCharacterSet=UTF8",
    "-IPTC:Keywords=harbour",
    "-IPTC:Keywords=København",
    "-IPTC:Caption-Abstract=Boats in the harbour",
    "-IPTC:By-line=Ada Lovelace",
    "-IPTC:City=Copenhagen",
]


def picture(size=(64, 48)):
    im = Image.new("RGB", size)
    px = im.load()
    for y in range(size[1]):
        for x in range(size[0]):
            px[x, y] = (x * 4 % 256, y * 5 % 256, (x + y) * 3 % 256)
    return im


def exiftool(path, *args):
    subprocess.run(["exiftool", "-q", "-overwrite_original", *args, path], check=True)


def path(name):
    return os.path.join(HERE, name)


def xmp_file():
    p = path("_tmp.xmp")
    with open(p, "w", encoding="utf-8") as f:
        f.write(XMP)
    return p


def main():
    xmp = xmp_file()

    # JPEG with everything: Exif (with a thumbnail), IPTC, XMP, a comment.
    thumb = path("_thumb.jpg")
    picture((16, 12)).save(thumb, quality=70)
    picture().save(path("photo.jpg"), quality=80)
    exiftool(path("photo.jpg"), *EXIF_ARGS, *IPTC_ARGS, "-Comment=hello comment", f"-ThumbnailImage<={thumb}",
             f"-XMP<={xmp}")

    # Big-endian Exif.
    picture().save(path("motorola.jpg"), quality=80)
    exiftool(path("motorola.jpg"), "-ExifByteOrder=MM", *EXIF_ARGS)

    # No metadata at all.
    picture().save(path("bare.jpg"), quality=80)

    # A multi-picture file (MPF), as phones write for depth or gain maps.
    first, second = picture(), picture((32, 24))
    first.save(path("multi.mpo"), format="MPO", save_all=True, append_images=[second], quality=80)
    exiftool(path("multi.mpo"), "-Make=PhotoCo")

    im = picture()
    im.save(path("photo.png"))
    exiftool(path("photo.png"), *EXIF_ARGS, f"-XMP<={xmp}")
    im.save(path("bare.png"))

    im.save(path("photo.webp"), quality=80)
    exiftool(path("photo.webp"), *EXIF_ARGS, f"-XMP<={xmp}")
    im.save(path("lossless.webp"), lossless=True)
    im.save(path("lossy.webp"), quality=80)

    im.save(path("photo.tif"), compression="tiff_deflate")
    exiftool(path("photo.tif"), *EXIF_ARGS, *IPTC_ARGS, f"-XMP<={xmp}")

    exif = Image.Exif()
    exif[0x010F] = "PhotoCo"
    exif[0x0110] = "Model H"
    exif[0x0112] = 1
    exif.get_ifd(0x8769)[0x9003] = "2024:05:17 18:42:07"
    xmp_bytes = XMP.encode("utf-8")
    im.save(path("photo.avif"), exif=exif.tobytes(), xmp=xmp_bytes, quality=60)
    try:
        import pillow_heif

        pillow_heif.register_heif_opener()
        im.save(path("photo.heic"), exif=exif.tobytes(), xmp=xmp_bytes, quality=60)
    except ImportError:
        print("pillow-heif missing: photo.heic not regenerated")

    # Turned AVIFs: heif-enc keeps a JPEG's pixels as coded and turns its
    # Exif orientation into irot (6) and irot then imir (7).
    for name, orientation in (("turned.avif", 6), ("transverse.avif", 7)):
        source = path("turned-source.jpg")
        exif = Image.Exif()
        exif[0x0112] = orientation
        picture().save(source, quality=90, exif=exif.tobytes())
        subprocess.run(["heif-enc", "-A", "-q", "60", source, "-o", path(name)], check=True,
                       capture_output=True)
        os.remove(source)

    with open(path("photo.xmp"), "w", encoding="utf-8") as f:
        f.write(XMP)

    for p in (xmp, thumb):
        os.remove(p)


if __name__ == "__main__":
    main()

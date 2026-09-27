#!/usr/bin/env bash
# Compares photos-meta's listing with exiv2's for each file given: every
# Exif, IPTC and XMP key, type, count and raw value. exiv2 is used only here,
# as a reference; the library does not link it.
#
#   tests/compare/compare_exiv2.sh build/tools/photos-meta tests/data/*.jpg ...
#
# Differences that are by design are filtered out: exiv2 lists the structural
# pointer tags (ExifTag, GPSTag, InteroperabilityTag, the thumbnail's offset
# and length), which this library writes from the layout; IPTC types are
# exiv2's own (this library keeps IPTC as text); exiv2 calls a struct, or an
# array of structs, XmpText with a count of 0; the XMP and IPTC blocks of a
# TIFF are XMP and IPTC here, not Exif tags; and exiv2 converts a sidecar's XMP
# to Exif and IPTC, so only a sidecar's XMP is compared.
set -euo pipefail

tool=$1
shift
status=0
normalise() {
  sed -E 's/[[:space:]]+/ /g; s/ $//' |
    sed -E 's/^(Iptc\.[^ ]+) [^ ]+ /\1 - /; s/^(Xmp\.[^ ]+) (XmpStruct|XmpBag|XmpSeq|XmpAlt) 0$/\1 XmpText 0/' |
    { grep -vE '^Exif\.(Image\.(ExifTag|GPSTag|XMLPacket|IPTCNAA|ImageResources)|Photo\.InteroperabilityTag|Thumbnail\.JPEGInterchangeFormat(Length)?) ' || true; } |
    { grep -vE '^\(' || true; } |
    sort
}
for f in "$@"; do
  ours=$("$tool" -p a "$f" | { grep -v '(Binary value suppressed)' || true; } | awk '{ key=$1; type=$2; count=$3; $1=$2=$3=""; sub(/^ +/, ""); print key, type, count, $0 }' | normalise)
  theirs=$({ exiv2 -q -Pkycv "$f" 2> /dev/null || true; } | { grep -v '(Binary value suppressed)' || true; } | normalise)
  if [[ $f == *.xmp ]]; then
    theirs=$(echo "$theirs" | { grep '^Xmp\.' || true; })
  fi
  if diff <(echo "$theirs") <(echo "$ours") > /tmp/photos-compare.diff; then
    echo "same     $f"
  else
    echo "DIFFERS  $f"
    sed 's/^/    /' /tmp/photos-compare.diff
    status=1
  fi
done
exit $status

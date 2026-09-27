#!/usr/bin/env bash
# Compares lumen-meta's reading with exiv2's for each file given: every Exif
# and IPTC tag (by number), XMP property, type, count and raw value. exiv2 is
# an independent reader, run only as a program, here; the library does not
# link it or share its code.
#
#   tests/compare/compare_exiv2.sh build/tools/lumen-meta tests/data/*.jpg ...
#
# Both listings are put in one neutral form: "E <IFD> <tag> <type> <count>
# <value>", "I <record> <dataset> <count> <value>" and "X <path> <kind>
# <count> <value>", with each side's names for IFDs, records, types and XMP
# prefixes mapped to the same words.
#
# Differences that are by design are filtered out: exiv2 lists the structural
# pointer tags (the Exif, GPS and interoperability IFD pointers, the
# thumbnail's offset and length), which this library writes from the layout;
# the XMP, IPTC, Photoshop and ICC blocks of a TIFF are XMP, IPTC and the ICC
# profile here, not Exif tags; exiv2 calls a struct, or an array of structs,
# XmpText (with a count of 0, or the text type="Struct" or type="Bag"); maker
# notes are compared only as the MakerNote
# tag; binary values either side summarises are compared by presence; and
# exiv2 converts a sidecar's XMP to Exif and IPTC, so only a sidecar's XMP is
# compared. exiv2 prints UserComment decoded even as a plain value, so it is
# compared by presence. (exiv2 also merges a Panasonic RW2's embedded JPEG's
# Exif into the RW2's; this library reads the RW2's own IFDs, so RW2 files
# differ by design and are best left out.)
set -euo pipefail

tool=$1
shift
status=0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# lumen-meta -n: "family key type count value".
ours() {
  "$tool" -n -p a "$1" | awk '
    BEGIN {
      ifd["ifd0"] = "IFD0"; ifd["exif"] = "EXIF"; ifd["gps"] = "GPS"; ifd["interop"] = "INTEROP"; ifd["ifd1"] = "IFD1"
      kind["text"] = "text"; kind["bag"] = "bag"; kind["seq"] = "seq"; kind["alt"] = "alt"
      kind["lang-alt"] = "langalt"; kind["struct"] = "text"
    }
    {
      family = $1; key = $2; type = $3; count = $4
      value = $0; sub(/^[^ ]+ +[^ ]+ +[^ ]+ +[^ ]+  /, "", value)
      if (family == "exif") {
        if (key == "(thumbnail)") next
        split(key, k, "."); print "E", ifd[k[1]], tolower(k[2]), tolower(type), count, value
      } else if (family == "iptc") {
        split(key, k, ":"); printf "I %s 0x%04x %s %s\n", k[1], k[2], count, value
      } else if (family == "xmp") {
        if (type == "struct") count = 0
        print "X", key, kind[type], count, value
      }
    }'
}

# exiv2 -P: Exif and IPTC as "tag group type count value", XMP as "key type
# count value".
theirs() {
  {
    exiv2 -q -PExgycv "$1" 2> /dev/null || true
    exiv2 -q -PIxgycv "$1" 2> /dev/null | sed 's/^/IPTC /' || true
    exiv2 -q -PXkycv "$1" 2> /dev/null || true
  } | awk '
    BEGIN {
      group["Image"] = "IFD0"; group["Photo"] = "EXIF"; group["GPSInfo"] = "GPS"; group["Iop"] = "INTEROP"
      group["Thumbnail"] = "IFD1"
      record["Envelope"] = 1; record["Application2"] = 2
      kind["XmpText"] = "text"; kind["XmpBag"] = "bag"; kind["XmpSeq"] = "seq"; kind["XmpAlt"] = "alt"
      kind["LangAlt"] = "langalt"
      # Prefixes exiv2 names differently from the schemas.
      prefix["iptc"] = "Iptc4xmpCore"; prefix["iptcExt"] = "Iptc4xmpExt"
    }
    $1 == "IPTC" {
      value = $0; sub(/^IPTC +[^ ]+ +[^ ]+ +[^ ]+ +[^ ]+ ?/, "", value)
      if ($3 in record) print "I", record[$3], $2, $5, value
      next
    }
    $1 ~ /^Xmp\./ {
      value = $0; sub(/^[^ ]+ +[^ ]+ +[^ ]+ ?/, "", value)
      key = substr($1, 5); dot = index(key, ".")
      p = substr(key, 1, dot - 1); rest = substr(key, dot + 1)
      if (p in prefix) p = prefix[p]
      rest = "/" rest
      gsub(/\/iptcExt:/, "/Iptc4xmpExt:", rest)
      gsub(/\/iptc:/, "/Iptc4xmpCore:", rest)
      rest = substr(rest, 2)
      print "X", p ":" rest, kind[$2], $3, value
      next
    }
    $1 ~ /^0x/ {
      if (!($2 in group)) next  # maker notes and the raw formats own IFDs
      value = $0; sub(/^[^ ]+ +[^ ]+ +[^ ]+ +[^ ]+ ?/, "", value)
      print "E", group[$2], $1, tolower($3), $4, value
    }'
}

# The by-design differences, and each side's summaries of binary values.
normalise() {
  sed -E 's/[[:space:]]+/ /g; s/ $//' |
    { grep -vE '^E IFD0 0x(8769|8825|02bc|83bb|8649|8773) |^E EXIF 0xa005 |^E IFD1 0x020[12] ' || true; } |
    sed -E 's/^(X [^ ]+) text 0 .*$/\1 text 0/' |
    sed -E 's/^(X [^ ]+) text [0-9]+ type="Struct"$/\1 text 0/' |
    sed -E 's/^(X [^ ]+) text [0-9]+ type="(Bag|Seq|Alt)"$/\1 \L\2\E 0/' |
    sed -E 's/^(E [^ ]+ [^ ]+ [^ ]+ [0-9]+) (\([0-9]+ bytes\)|\(Binary value suppressed\))$/\1 (binary)/' |
    sed -E 's/^(E EXIF 0x9286 [^ ]+ [0-9]+).*$/\1 (comment)/; s/^(E GPS 0x001[bc] [^ ]+ [0-9]+).*$/\1 (comment)/' |
    sort
}

# The two print floating-point values to different precisions: a FLOAT or
# DOUBLE entry of theirs within 1e-6 of ours, value by value, is taken as
# ours.
sameReals() {
  awk 'function real(t) { return $1 == "E" && ($4 == "float" || $4 == "double") }
       NR == FNR { if (real()) ours[$1 " " $2 " " $3 " " $4 " " $5] = $0; next }
       real() {
         key = $1 " " $2 " " $3 " " $4 " " $5
         if (key in ours) {
           n = split(ours[key], o, " ")
           same = n == NF
           for (i = 6; same && i <= NF; ++i) {
             d = o[i] - $i; if (d < 0) d = -d
             m = $i < 0 ? -$i : $i
             if (d > 1e-6 * m && d > 1e-30) same = 0
           }
           if (same) { print ours[key]; next }
         }
       }
       { print }' "$1" "$2"
}

for f in "$@"; do
  ours "$f" | normalise > "$work/ours"
  theirs "$f" | normalise > "$work/theirs"
  if [[ $f == *.xmp ]]; then
    grep '^X ' "$work/theirs" > "$work/t" || true
    mv "$work/t" "$work/theirs"
  fi
  # A value only one side summarises as binary is compared by presence.
  for side in ours theirs; do
    other=$([ $side = ours ] && echo theirs || echo ours)
    { grep ' (binary)$' "$work/$side" || true; } | cut -d' ' -f1-5 | while read -r k; do
      sed -i -E "s|^(${k//|/\\|}) .*\$|\1 (binary)|" "$work/$other"
    done
  done
  sameReals "$work/ours" "$work/theirs" | sort > "$work/t"
  mv "$work/t" "$work/theirs"
  if diff "$work/theirs" "$work/ours" > "$work/diff"; then
    echo "same     $f"
  else
    echo "DIFFERS  $f"
    sed 's/^/    /' "$work/diff"
    status=1
  fi
done
exit $status

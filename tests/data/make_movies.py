#!/usr/bin/env python3
"""Regenerates the movie fixtures in this directory (movie.hpp's reader).

    python3 tests/data/make_movies.py

Needs only Python. Each movie is the boxes a camera or an encoder writes,
with a few bytes of stand-in samples: the reader never reads a sample, so
nothing here needs a codec. The values are what tests/unit/test_movie.cpp
checks for.

- iphone.mov    QuickTime, as an iPhone held upright writes it: HEVC turned a
                quarter turn (orientation 6), AAC, a timed-metadata track;
                Apple's keys at moov/meta (a QuickTime meta, no version),
                text, a float and an integer among them.
- android.mp4   H.264 at 90 kHz with a short last sample, AAC; ©xyz in udta
                as QuickTime text; Android's keys at moov/meta (an ISO meta).
- ffmpeg.mp4    What `ffmpeg -movflags use_metadata_tags` writes: the keys at
                moov/udta/meta; a movie header time, 25 fps.
- mirrored.mp4  A mirrored display matrix (orientation 2); iTunes items at
                moov/udta/meta (hdlr mdir: ©too, ©day, a "----" item, UTF-16).
- anamorphic.mov  Version 1 headers (64-bit times), a 64-bit mdat size, a
                720x576 picture with pasp 64:45, encrypted (encv, frma avc1).
- fragmented.mp4  An mvex with mehd, a movie header duration of 0, and
                moof/mdat pairs after the movie box.
"""
import os
import struct
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ONE = 0x10000
IDENTITY = [ONE, 0, 0, 0, ONE, 0, 0, 0, 0x40000000]


def u8(v): return struct.pack(">B", v)
def u16(v): return struct.pack(">H", v)
def u32(v): return struct.pack(">I", v)
def i32(v): return struct.pack(">i", v)
def u64(v): return struct.pack(">Q", v)


def box(kind, *parts):
    payload = b"".join(parts)
    if isinstance(kind, str):
        kind = kind.encode("latin-1")
    return u32(8 + len(payload)) + kind + payload


def large_box(kind, *parts):
    payload = b"".join(parts)
    return u32(1) + kind.encode("latin-1") + u64(16 + len(payload)) + payload


def full(kind, version, flags, *parts):
    return box(kind, u8(version), struct.pack(">I", flags)[1:], *parts)


def movie_time(iso):
    moment = datetime.fromisoformat(iso).astimezone(timezone.utc)
    return int(moment.timestamp()) + 2082844800


def matrix(values):
    return b"".join(i32(v) for v in values)


def mvhd(created, timescale, duration, next_track, version=0):
    if version == 1:
        times = u64(created) + u64(created) + u32(timescale) + u64(duration)
    else:
        times = u32(created) + u32(created) + u32(timescale) + u32(duration)
    return full("mvhd", version, 0, times, u32(ONE), u16(0x0100), bytes(10),
                matrix(IDENTITY), bytes(24), u32(next_track))


def tkhd(track_id, duration, width, height, values=IDENTITY, version=0, enabled=True):
    if version == 1:
        head = u64(0) + u64(0) + u32(track_id) + u32(0) + u64(duration)
    else:
        head = u32(0) + u32(0) + u32(track_id) + u32(0) + u32(duration)
    return full("tkhd", version, 3 if enabled else 2, head, bytes(8), u16(0), u16(0),
                u16(0x0100 if width == 0 else 0), u16(0), matrix(values),
                u32(width << 16), u32(height << 16))


def mdhd(timescale, duration, version=0):
    if version == 1:
        times = u64(0) + u64(0) + u32(timescale) + u64(duration)
    else:
        times = u32(0) + u32(0) + u32(timescale) + u32(duration)
    return full("mdhd", version, 0, times, u16(0x55C4), u16(0))


def hdlr(handler, component=b"\0\0\0\0", name=b"\0"):
    return full("hdlr", 0, 0, component, handler.encode(), bytes(12), name)


def visual(kind, width, height, *children):
    return box(kind, bytes(6), u16(1), u16(0), u16(0), bytes(12), u16(width), u16(height),
               u32(0x00480000), u32(0x00480000), u32(0), u16(1), bytes(32), u16(0x18),
               struct.pack(">h", -1), *children)


def sound(kind, *children):
    return box(kind, bytes(6), u16(1), u16(0), u16(0), u32(0), u16(2), u16(16), u16(0), u16(0),
               u32(44100 << 16), *children)


def stts(*entries):
    return full("stts", 0, 0, u32(len(entries)), *(u32(n) + u32(d) for n, d in entries))


def stbl(entry, durations):
    return box("stbl", full("stsd", 0, 0, u32(1), entry), durations,
               full("stsc", 0, 0, u32(0)), full("stsz", 0, 0, u32(0), u32(0)),
               full("stco", 0, 0, u32(0)))


def trak(track_id, handler, timescale, duration_ticks, movie_ticks, entry, durations,
         size=(0, 0), values=IDENTITY, version=0, component=b"\0\0\0\0"):
    return box("trak",
               tkhd(track_id, movie_ticks, size[0], size[1], values, version),
               box("mdia", mdhd(timescale, duration_ticks, version), hdlr(handler, component),
                   box("minf", stbl(entry, durations))))


def text_item(value):
    return box("data", u32(1), u32(0), value.encode("utf-8"))


def keys_meta(entries, quicktime):
    """A meta box of QuickTime keys: `entries` are (key, data box)."""
    keys = full("keys", 0, 0, u32(len(entries)),
                *(u32(8 + len(k)) + b"mdta" + k.encode() for k, _ in entries))
    items = box("ilst", *(box(u32(i + 1), data) for i, (_, data) in enumerate(entries)))
    handler = hdlr("mdta")
    if quicktime:
        return box("meta", handler, keys, items)
    return full("meta", 0, 0, handler, keys, items)


def qt_text(kind, text, language=0x15C7):
    data = text.encode("utf-8")
    return box(kind, u16(len(data)), u16(language), data)


def ftyp(major, *compatible):
    return box("ftyp", major.encode(), u32(0), *(c.encode() for c in compatible))


def samples(n=64):
    return box("mdat", bytes(range(n)))


def write(name, *parts):
    with open(os.path.join(HERE, name), "wb") as out:
        out.write(b"".join(parts))
    print(f"{name}: {sum(len(p) for p in parts)} bytes")


def iphone():
    # 30 fps HEVC at 600 ticks a second, 1.5 s; stored 1920x1080 and shown
    # turned a quarter turn clockwise.
    turned = [0, ONE, 0, -ONE, 0, 0, 1080 << 16, 0, 0x40000000]
    video = trak(1, "vide", 600, 900, 900, visual(b"hvc1", 1920, 1080, box("hvcC", bytes(23))),
                 stts((45, 20)), (1920, 1080), turned, component=b"mhlr")
    audio = trak(2, "soun", 44100, 66150, 900, sound(b"mp4a", box("esds", bytes(20))),
                 stts((64, 1024), (1, 614)), component=b"mhlr")
    timed = trak(3, "meta", 600, 900, 900, box("mebx", bytes(6), u16(1)), stts((1, 900)),
                 component=b"mhlr")
    meta = keys_meta([
        ("com.apple.quicktime.location.accuracy.horizontal", box("data", u32(23), u32(0), struct.pack(">f", 14.5))),
        ("com.apple.quicktime.location.ISO6709", text_item("-22.9068-043.1729+010.000/")),
        ("com.apple.quicktime.make", text_item("Apple")),
        ("com.apple.quicktime.model", text_item("iPhone 15 Pro")),
        ("com.apple.quicktime.software", text_item("17.2")),
        ("com.apple.quicktime.creationdate", text_item("2023-12-24T18:05:09-0300")),
        ("com.apple.quicktime.live-photo.auto", box("data", u32(21), u32(0), u8(1))),
        ("com.apple.quicktime.full-frame-rate-playback-intent", box("data", u32(21), u32(0), u8(0xFF))),
    ], quicktime=True)
    moov = box("moov", mvhd(movie_time("2023-12-24T21:05:09+00:00"), 600, 900, 4),
               video, audio, timed, meta)
    write("iphone.mov", ftyp("qt  ", "qt  "), box("wide"), samples(), moov)


def android():
    video = trak(1, "vide", 90000, 90000 + 1234 - 3000, 2000,
                 visual(b"avc1", 1280, 720, box("avcC", bytes(12))),
                 stts((29, 3000), (1, 1234)), (1280, 720))
    audio = trak(2, "soun", 48000, 96000, 2000, sound(b"mp4a", box("esds", bytes(20))),
                 stts((93, 1024), (1, 768)))
    udta = box("udta", qt_text(b"\xa9xyz", "+37.7858-122.4064/"))
    meta = keys_meta([
        ("com.android.version", text_item("14")),
        ("com.android.manufacturer", text_item("Google")),
        ("com.android.model", text_item("Pixel 8")),
        ("com.android.capture.fps", box("data", u32(23), u32(0), struct.pack(">f", 30.0))),
    ], quicktime=False)
    moov = box("moov", mvhd(movie_time("2024-06-01T12:30:15+00:00"), 1000, 2000, 3),
               video, audio, udta, meta)
    write("android.mp4", ftyp("mp42", "isom", "mp42"), moov, samples())


def ffmpeg():
    video = trak(1, "vide", 12800, 12800, 1000, visual(b"avc1", 160, 120, box("avcC", bytes(12))),
                 stts((25, 512)), (160, 120))
    audio = trak(2, "soun", 44100, 44100, 1000, sound(b"mp4a", box("esds", bytes(20))),
                 stts((43, 1024), (1, 68)))
    udta = box("udta", keys_meta([
        ("major_brand", text_item("isom")),
        ("minor_version", text_item("512")),
        ("compatible_brands", text_item("isomiso2avc1mp41")),
        ("com.apple.quicktime.make", text_item("Apple")),
        ("location", text_item("+48.8583+002.2945/")),
        ("encoder", text_item("Lavf61.7.100")),
    ], quicktime=False))
    moov = box("moov", mvhd(movie_time("2024-06-01T12:30:15+00:00"), 1000, 1000, 3),
               video, audio, udta)
    write("ffmpeg.mp4", ftyp("isom", "isom", "iso2", "avc1", "mp41"), box("free"), samples(), moov)


def mirrored():
    flipped = [-ONE, 0, 0, 0, ONE, 0, 320 << 16, 0, 0x40000000]
    video = trak(1, "vide", 30000, 30030, 1001, visual(b"avc1", 320, 240, box("avcC", bytes(12))),
                 stts((30, 1001)), (320, 240), flipped)
    title = "Fjord — \U0001F30A"
    items = box("ilst",
                box(b"\xa9too", box("data", u32(1), u32(0), b"Lavf61.7.100")),
                box(b"\xa9day", box("data", u32(1), u32(0), b"2022-08-14T09:10:11.000000Z")),
                box(b"\xa9nam", box("data", u32(2), u32(0), title.encode("utf-16-be"))),
                box("covr", box("data", u32(13), u32(0), b"\xff\xd8\xff\xd9")),
                box("----", full("mean", 0, 0, b"com.apple.iTunes"), full("name", 0, 0, b"iTunSMPB"),
                    box("data", u32(1), u32(0), b" 00000000 00000840")))
    udta = box("udta", full("meta", 0, 0, hdlr("mdir", name=b"appl"), items))
    moov = box("moov", mvhd(0, 1000, 1001, 2), video, udta)
    write("mirrored.mp4", ftyp("isom", "isom"), samples(), moov)


def anamorphic():
    entry = visual(b"encv", 720, 576, box("avcC", bytes(12)), box("pasp", u32(64), u32(45)),
                   box("sinf", box("frma", b"avc1"), full("schm", 0, 0, b"cenc", u32(0x10000))))
    video = trak(1, "vide", 25, 50, 2000, entry, stts((50, 1)), (1024, 576), version=1)
    moov = box("moov", mvhd(movie_time("2021-03-04T05:06:07+00:00"), 1000, 2000, 2, version=1), video)
    write("anamorphic.mov", ftyp("qt  ", "qt  "), large_box("mdat", bytes(32)), moov)


def fragmented():
    video = trak(1, "vide", 15360, 0, 0, visual(b"avc1", 640, 360, box("avcC", bytes(12))),
                 full("stts", 0, 0, u32(0)), (640, 360))
    mvex = box("mvex", full("mehd", 0, 0, u32(4000)), full("trex", 0, 0, u32(1), u32(1), u32(0), u32(0), u32(0)))
    moov = box("moov", mvhd(0, 1000, 0, 2), video, mvex)
    fragment = box("moof", full("mfhd", 0, 0, u32(1))) + samples(16)
    write("fragmented.mp4", ftyp("iso6", "iso6", "dash"), moov, fragment, fragment)


def main():
    iphone()
    android()
    ffmpeg()
    mirrored()
    anamorphic()
    fragmented()


if __name__ == "__main__":
    main()

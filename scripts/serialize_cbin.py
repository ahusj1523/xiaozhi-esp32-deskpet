"""Serialize lv_font_conv C output into the CBIN format used by cbin_font.c.

Mirrors exactly what the compiler would emit for this project's config:
  - LV_FONT_FMT_TXT_LARGE=1  -> glyph_dsc entries are 16 bytes
  - 32-bit pointers stored as offsets (loader adds the base back)
  - lv_font_t = 36 bytes, lv_font_fmt_txt_dsc_t = 24 bytes
  - cmap records = 20 bytes; sparse unicode lists are uint16, relative to
    range_start, offsets relative to the cmaps section
No kerning (--no-kerning), bitmap_format = PLAIN, stride = 0.
"""
import re
import struct
import sys

SRC = sys.argv[1]
OUT = sys.argv[2]

src = open(SRC, encoding="utf-8").read()


def grab(pattern, default=None):
    m = re.search(pattern, src)
    if m:
        return m.group(1)
    if default is not None:
        return default
    raise SystemExit("missing pattern: " + pattern)


line_height = int(grab(r"\.line_height = (-?\d+)"))
base_line = int(grab(r"\.base_line = (-?\d+)"))
ul_pos = int(grab(r"\.underline_position = (-?\d+)", "0"))
ul_thk = int(grab(r"\.underline_thickness = (-?\d+)", "0"))
kerning_none = bool(re.search(r"\.kerning = LV_FONT_KERNING_NONE", src))

# --- bitmap (slice up to the next declaration: per-glyph comments may
#     contain a literal '}' character, e.g. /* U+007D "}" */) ---
i = src.find("uint8_t glyph_bitmap[]")
if i < 0:
    raise SystemExit("glyph_bitmap not found")
j = src.find("glyph_dsc[]", i)
if j < 0:
    raise SystemExit("glyph_dsc boundary not found")
bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", src[i:j]))

# --- glyph descriptors (LV_FONT_FMT_TXT_LARGE=1 -> IIHHhh = 16 bytes) ---
gd_block = re.search(
    r"static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};", src, re.S
).group(1)
entries = re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
    r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}",
    gd_block,
)
if not entries:
    raise SystemExit("no glyph_dsc entries parsed")
gd = b"".join(
    struct.pack("<IIHHhh", int(a), int(b), int(c), int(d), int(e), int(f))
    for a, b, c, d, e, f in entries
)

# --- sparse unicode lists ---
lists = {}
for m2 in re.finditer(r"static const uint16_t (unicode_list_\w+)\[\]\s*=\s*\{([^}]*)\}", src):
    lists[m2.group(1)] = [
        int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m2.group(2))
    ]

# --- cmaps ---
cm_block = re.search(
    r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};", src, re.S
).group(1)
TYPE = {
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL": 0,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL": 1,
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY": 2,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY": 3,
}
recs = re.findall(
    r"\{\s*\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*"
    r"\.unicode_list = (NULL|unicode_list_\w+), \.glyph_id_ofs_list = (NULL|\w+), "
    r"\.list_length = (\d+), \.type = (LV_FONT_FMT_TXT_CMAP_\w+)\s*\}",
    cm_block,
)
if not recs:
    raise SystemExit("no cmaps parsed")

# --- layout ---
DESC_SIZE = 24
FONT_SIZE = 36
off_bitmap = DESC_SIZE
off_gd = off_bitmap + len(bitmap)
off_gd += (4 - off_gd % 4) % 4
off_cm = off_gd + len(gd)
off_cm += (4 - off_cm % 4) % 4

n_rec = len(recs)
assert off_cm % 4 == 0
cursor = off_cm + n_rec * 20
list_offsets = {}
list_blobs = b""
for name in lists:
    assert cursor % 2 == 0, "u16 list misaligned"
    list_offsets[name] = cursor - off_cm
    blob = b"".join(struct.pack("<H", v) for v in lists[name])
    list_blobs += blob
    cursor += len(blob)

rec_blob = b""
for rs, rl, gid, ul, ofs, llen, typ in recs:
    if ofs != "NULL":
        raise SystemExit("glyph_id_ofs_list not supported")
    ul_off = 0
    if ul != "NULL":
        if ul not in lists:
            raise SystemExit("unknown list " + ul)
        if int(llen) != len(lists[ul]):
            raise SystemExit(f"{ul} length mismatch {llen} != {len(lists[ul])}")
        ul_off = list_offsets[ul]
    rec_blob += struct.pack(
        "<IHHIIHBB", int(rs), int(rl), int(gid), ul_off, 0, int(llen), TYPE[typ], 0
    )
cmaps_blob = rec_blob + list_blobs

cmap_num = n_rec
assert cmap_num < 512 and 1 <= 4 < 16
bitfield = cmap_num | (4 << 9)  # bpp=4, kern_classes=0, bitmap_format=PLAIN
dsc = struct.pack("<IIIIHHBxxx", off_bitmap, off_gd, off_cm, 0, 0, bitfield, 0)

font_bitfield = (1 << 2) if kerning_none else 0  # subpx=0, kerning, static=0
font = struct.pack(
    "<IIIiiBbbxIII",
    0, 0, 0,  # function pointers (loader overwrites them)
    line_height,
    base_line,
    font_bitfield,
    ul_pos,
    ul_thk,
    FONT_SIZE,  # dsc offset (file start -> right after lv_font_t)
    0,  # fallback
    0,  # user_data
)
assert len(font) == FONT_SIZE, len(font)
assert len(dsc) == DESC_SIZE, len(dsc)

# deterministic layout: [lv_font_t][dsc][pad?][bitmap][pad4][glyph_dsc][pad4][cmaps]
head = font + dsc
pad1 = b"\x00" * (off_gd - DESC_SIZE - len(bitmap))
pad2 = b"\x00" * (off_cm - off_gd - len(gd))
blob = head + bitmap + pad1 + gd + pad2 + cmaps_blob

assert len(blob) == FONT_SIZE + off_cm + len(cmaps_blob), (len(blob), FONT_SIZE + off_cm + len(cmaps_blob))

# offsets are relative to descriptor (file offset FONT_SIZE)
assert struct.unpack_from("<I", blob, FONT_SIZE)[0] == off_bitmap
assert struct.unpack_from("<I", blob, FONT_SIZE + 4)[0] == off_gd
assert struct.unpack_from("<I", blob, FONT_SIZE + 8)[0] == off_cm

open(OUT, "wb").write(blob)

# ---------------- round-trip validation (independent parse) ----------------
data = blob
dsc_off = struct.unpack_from("<I", data, 24)[0]
assert dsc_off == FONT_SIZE
cmaps_rel = struct.unpack_from("<I", data, dsc_off + 8)[0]
word18 = struct.unpack_from("<H", data, dsc_off + 18)[0]
cmap_num2 = word18 & 0x1FF
bpp2 = (word18 >> 9) & 0xF
cmaps_addr = dsc_off + cmaps_rel
cps = set()
for i in range(cmap_num2):
    rec = cmaps_addr + i * 20
    rstart, rlen, gid, ul_off, ofs_off, llen, typ, _pad = struct.unpack_from(
        "<IHHIIHBB", data, rec
    )
    if ul_off:
        ul = cmaps_addr + ul_off
        for j in range(llen):
            cps.add(rstart + struct.unpack_from("<H", data, ul + j * 2)[0])
    else:
        cps.update(range(rstart, rstart + rlen))

expected = set(ord(c) for c in open(sys.argv[3], encoding="utf-8").read())
print(f"file_size      = {len(blob)}")
print(f"line_height    = {line_height}, base_line = {base_line}")
print(f"glyphs         = {len(entries)}")
print(f"bitmap_bytes   = {len(bitmap)}")
print(f"cmaps          = {cmap_num2} (bpp={bpp2})")
print(f"codepoints     = parsed {len(cps)} vs expected {len(expected)}")
if cps != expected:
    missing = sorted(expected - cps)[:20]
    extra = sorted(cps - expected)[:20]
    print("MISMATCH missing:", [hex(c) for c in missing])
    print("MISMATCH extra:", [hex(c) for c in extra])
    raise SystemExit("ROUND-TRIP FAILED")
print("ROUND_TRIP_OK")

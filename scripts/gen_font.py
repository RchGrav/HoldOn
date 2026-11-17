#!/usr/bin/env python3
"""Generate src/screen/font_data.h from GNU Unifont and Noto Emoji.

Usage: python3 -I scripts/gen_font.py --emoji 'NotoEmoji[wght].ttf' \
           unifont-16.0.04.hex.gz unifont_upper-16.0.04.hex.gz \
           > src/screen/font_data.h

Sources and versions: third_party/unifont/README.md and
third_party/noto-emoji/README.md.

Unifont: keeps every glyph in the Basic Multilingual Plane (all modern
scripts: Latin, Greek, Cyrillic, Hebrew, Arabic, Indic, Thai, Ethiopic, CJK,
Hangul, symbols, box drawing, Braille, ...) except surrogates and private
use, and from the upper-plane file the blocks terminal programs draw with:
math alphanumerics, emoji and pictographs, and symbols for legacy computing
(sextants, smooth mosaics). Glyphs are 8x16 (16 bytes) or 16x16 (32 bytes,
wide). Anything absent renders as a hollow box.

Noto Emoji (monochrome): each emoji the terminal grid makes double-width
(libvterm's third_party/libvterm/src/fullwidth.inc) is rasterized here, from
the font's TrueType outlines, to a 32x32 anti-aliased coverage mask at 4 bits
per pixel. The renderer prefers it over Unifont's line-art emoji in a
double-width cell. Standard library only: no font tooling needed.
"""
import gzip
import os
import re
import struct
import sys

RANGES = [
    (0x0020, 0x007E), (0x00A0, 0xD7FF),  # BMP up to the surrogates (no C0/C1 controls)
    (0xF900, 0xFFFD),                    # after private use: compatibility, presentation forms, specials
    (0x1CC00, 0x1CEBF),                  # symbols for legacy computing supplement
    (0x1D400, 0x1D7FF),                  # mathematical alphanumeric symbols
    (0x1F000, 0x1FAFF),                  # mahjong/cards, enclosed, emoji, pictographs, symbols
    (0x1FB00, 0x1FBFF),                  # symbols for legacy computing (sextants, mosaics)
]

EMOJI_PX = 32  # mask size; the renderer resamples it to the cell (16 px per scale step)
SUBROWS = 8    # vertical samples per pixel row; horizontal coverage is exact


def wanted(cp):
    return any(lo <= cp <= hi for lo, hi in RANGES)


def load_unifont(paths):
    seen = {}
    for path in paths:
        opener = gzip.open if path.endswith(".gz") else open
        with opener(path, "rt") as f:
            for line in f:
                cp_hex, _, bits = line.strip().partition(":")
                cp = int(cp_hex, 16)
                if wanted(cp) and len(bits) in (32, 64) and cp not in seen:
                    seen[cp] = bytes.fromhex(bits)
    return sorted(seen.items())


# ---- TrueType (glyf) outlines -----------------------------------------------


class TrueType:
    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        n, = struct.unpack(">H", d[4:6])
        self.t = {d[12 + 16 * i:16 + 16 * i].decode("latin1"): struct.unpack(">II", d[20 + 16 * i:28 + 16 * i])
                  for i in range(n)}
        head = self.t["head"][0]
        self.upem, = struct.unpack(">H", d[head + 18:head + 20])
        self.long_loca = struct.unpack(">h", d[head + 50:head + 52])[0] == 1
        hhea = self.t["hhea"][0]
        self.ascent, self.descent = struct.unpack(">hh", d[hhea + 4:hhea + 8])
        self.num_hmetrics, = struct.unpack(">H", d[hhea + 34:hhea + 36])
        self.cmap = self._cmap()

    def _cmap(self):
        d, base = self.d, self.t["cmap"][0]
        n, = struct.unpack(">H", d[base + 2:base + 4])
        subs = {}
        for i in range(n):
            pid, eid, off = struct.unpack(">HHI", d[base + 4 + 8 * i:base + 12 + 8 * i])
            subs[(pid, eid)] = base + off
        out = {}
        for key in ((3, 10), (0, 4), (3, 1), (0, 3)):
            if key not in subs:
                continue
            o = subs[key]
            fmt, = struct.unpack(">H", d[o:o + 2])
            if fmt == 12:
                ngroups, = struct.unpack(">I", d[o + 12:o + 16])
                for g in range(ngroups):
                    start, end, gid = struct.unpack(">III", d[o + 16 + 12 * g:o + 28 + 12 * g])
                    for cp in range(start, end + 1):
                        out.setdefault(cp, gid + cp - start)
            elif fmt == 4:
                segx2, = struct.unpack(">H", d[o + 6:o + 8])
                seg = segx2 // 2
                ends = struct.unpack(">%dH" % seg, d[o + 14:o + 14 + segx2])
                starts = struct.unpack(">%dH" % seg, d[o + 16 + segx2:o + 16 + 2 * segx2])
                deltas = struct.unpack(">%dh" % seg, d[o + 16 + 2 * segx2:o + 16 + 3 * segx2])
                ro_at = o + 16 + 3 * segx2
                ranges = struct.unpack(">%dH" % seg, d[ro_at:ro_at + segx2])
                for s in range(seg):
                    for cp in range(starts[s], ends[s] + 1):
                        if cp == 0xFFFF:
                            continue
                        if ranges[s] == 0:
                            gid = (cp + deltas[s]) & 0xFFFF
                        else:
                            at = ro_at + 2 * s + ranges[s] + 2 * (cp - starts[s])
                            gid, = struct.unpack(">H", d[at:at + 2])
                            if gid:
                                gid = (gid + deltas[s]) & 0xFFFF
                        if gid:
                            out.setdefault(cp, gid)
        return out

    def advance(self, gid):
        o = self.t["hmtx"][0] + 4 * min(gid, self.num_hmetrics - 1)
        return struct.unpack(">H", self.d[o:o + 2])[0]

    def _glyph_range(self, gid):
        loca = self.t["loca"][0]
        if self.long_loca:
            a, b = struct.unpack(">II", self.d[loca + 4 * gid:loca + 4 * gid + 8])
        else:
            a, b = (2 * v for v in struct.unpack(">HH", self.d[loca + 2 * gid:loca + 2 * gid + 4]))
        g = self.t["glyf"][0]
        return g + a, g + b

    def contours(self, gid, depth=0):
        """List of contours; each a list of (x, y, on_curve) in font units."""
        start, end = self._glyph_range(gid)
        if start == end or depth > 8:
            return []
        d = self.d
        ncont, = struct.unpack(">h", d[start:start + 2])
        p = start + 10
        if ncont >= 0:
            ends = struct.unpack(">%dH" % ncont, d[p:p + 2 * ncont])
            p += 2 * ncont
            ilen, = struct.unpack(">H", d[p:p + 2])
            p += 2 + ilen
            npts = ends[-1] + 1 if ends else 0
            flags = []
            while len(flags) < npts:
                f = d[p]
                p += 1
                flags.append(f)
                if f & 8:
                    flags.extend([f] * d[p])
                    p += 1
            coords = []
            for short, same in ((2, 16), (4, 32)):
                v, vals = 0, []
                for f in flags:
                    if f & short:
                        delta = d[p]
                        p += 1
                        v += delta if f & same else -delta
                    elif not f & same:
                        v += struct.unpack(">h", d[p:p + 2])[0]
                        p += 2
                    vals.append(v)
                coords.append(vals)
            pts = [(coords[0][i], coords[1][i], bool(flags[i] & 1)) for i in range(npts)]
            out, s = [], 0
            for e in ends:
                out.append(pts[s:e + 1])
                s = e + 1
            return out
        out = []
        while True:  # composite
            flags, comp = struct.unpack(">HH", d[p:p + 4])
            p += 4
            if flags & 1:
                dx, dy = struct.unpack(">hh", d[p:p + 4])
                p += 4
            else:
                dx, dy = struct.unpack(">bb", d[p:p + 2])
                p += 2
            a, b, c, e = 1.0, 0.0, 0.0, 1.0
            if flags & 8:
                a = e = struct.unpack(">h", d[p:p + 2])[0] / 16384
                p += 2
            elif flags & 0x40:
                a, e = (v / 16384 for v in struct.unpack(">hh", d[p:p + 4]))
                p += 4
            elif flags & 0x80:
                a, b, c, e = (v / 16384 for v in struct.unpack(">hhhh", d[p:p + 8]))
                p += 8
            if not flags & 2:
                dx = dy = 0  # point-matched placement: rare in emoji fonts; place at origin
            for cont in self.contours(comp, depth + 1):
                out.append([(a * x + c * y + dx, b * x + e * y + dy, on) for x, y, on in cont])
            if not flags & 0x20:
                return out


def flatten(contour, steps=6):
    """Contour with implied on-curve points -> closed polyline."""
    n = len(contour)
    if n < 2:
        return []
    pts = []
    for i in range(n):  # make every off-off pair explicit with its midpoint
        x, y, on = contour[i]
        nx, ny, non = contour[(i + 1) % n]
        pts.append((x, y, on))
        if not on and not non:
            pts.append(((x + nx) / 2, (y + ny) / 2, True))
    k = next((i for i, q in enumerate(pts) if q[2]), None)
    if k is None:
        return []
    pts = pts[k:] + pts[:k]
    poly = [(pts[0][0], pts[0][1])]
    i, m = 1, len(pts)
    while i <= m:
        x, y, on = pts[i % m]
        if on:
            poly.append((x, y))
            i += 1
        else:
            x0, y0 = poly[-1]
            x2, y2, _ = pts[(i + 1) % m]
            for t in range(1, steps + 1):
                u = t / steps
                poly.append(((1 - u) ** 2 * x0 + 2 * (1 - u) * u * x + u * u * x2,
                             (1 - u) ** 2 * y0 + 2 * (1 - u) * u * y + u * u * y2))
            i += 2
    return poly


def rasterize(polys, size):
    """Nonzero-winding coverage in [0, 1] per pixel of a size x size grid."""
    rows = size * SUBROWS
    crossings = [[] for _ in range(rows)]
    for poly in polys:
        for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
            if y0 == y1:
                continue
            w = 1 if y1 > y0 else -1
            if y0 > y1:
                x0, y0, x1, y1 = x1, y1, x0, y0
            first = max(0, int((y0 * SUBROWS) - 0.5) - 1)
            last = min(rows - 1, int(y1 * SUBROWS + 0.5) + 1)
            for r in range(first, last + 1):
                yc = (r + 0.5) / SUBROWS
                if y0 <= yc < y1:
                    crossings[r].append((x0 + (yc - y0) * (x1 - x0) / (y1 - y0), w))
    cov = [[0.0] * size for _ in range(size)]
    for r, xs in enumerate(crossings):
        if not xs:
            continue
        xs.sort()
        line = cov[r // SUBROWS]
        wind = 0
        for (xa, wa), (xb, _) in zip(xs, xs[1:]):
            wind += wa
            if wind == 0 or xb <= xa:
                continue
            xa, xb = max(0.0, xa), min(float(size), xb)
            if xb <= xa:
                continue
            pa, pb = int(xa), min(size - 1, int(xb))
            if pa == pb:
                line[pa] += (xb - xa) / SUBROWS
                continue
            line[pa] += (pa + 1 - xa) / SUBROWS
            for px in range(pa + 1, pb):
                line[px] += 1.0 / SUBROWS
            line[pb] += (xb - pb) / SUBROWS
    return cov


def wide_intervals(repo):
    """libvterm's double-width ranges (emoji among them)."""
    path = os.path.join(repo, "third_party", "libvterm", "src", "fullwidth.inc")
    txt = open(path).read()
    return [(int(a, 16), int(b, 16)) for a, b in re.findall(r"\{\s*0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+)\s*\}", txt)]


def load_emoji(path):
    repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    wide = wide_intervals(repo)
    tt = TrueType(path)
    box_h = tt.ascent - tt.descent
    out = []
    for cp, gid in sorted(tt.cmap.items()):
        if not any(lo <= cp <= hi for lo, hi in wide):
            continue
        conts = tt.contours(gid)
        if not conts:
            continue
        adv = tt.advance(gid) or tt.upem
        scale = (EMOJI_PX - 1) / max(box_h, adv)  # one pixel of air; relative sizes kept
        ox = (EMOJI_PX - adv * scale) / 2
        oy = (EMOJI_PX - box_h * scale) / 2
        polys = [[(ox + x * scale, oy + (tt.ascent - y) * scale) for x, y in flatten(c)] for c in conts]
        cov = rasterize([p for p in polys if len(p) > 2], EMOJI_PX)
        nib = [min(15, int(v * 15 + 0.5)) for row in cov for v in row]
        out.append((cp, bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))))
    return out


# ---- output ----------------------------------------------------------------


def write_bytes(out, blob):
    for i in range(0, len(blob), 32):  # decimal bytes keep the generated source compact
        out.write(",".join("%d" % x for x in blob[i:i + 32]) + ",\n")


def main():
    args = sys.argv[1:]
    emoji_path = None
    if args[:1] == ["--emoji"]:
        emoji_path, args = args[1], args[2:]
    glyphs = load_unifont(args)
    emoji = load_emoji(emoji_path) if emoji_path else []
    out = sys.stdout
    out.write("/* Generated by scripts/gen_font.py from GNU Unifont and Noto Emoji (both\n"
              " * SIL OFL 1.1; see third_party/unifont/ and third_party/noto-emoji/).\n"
              " * Do not edit. */\n")
    out.write("#define HOLD_FONT_GLYPHS %d\n" % len(glyphs))
    out.write("static const uint32_t font_cp[HOLD_FONT_GLYPHS] = {\n")
    for i in range(0, len(glyphs), 10):
        out.write("    " + ", ".join("0x%05x" % g[0] for g in glyphs[i:i + 10]) + ",\n")
    out.write("};\n/* offset into font_bits; bit 31 marks a 16-pixel-wide glyph */\n")
    out.write("static const uint32_t font_off[HOLD_FONT_GLYPHS] = {\n")
    offs, pos = [], 0
    for cp, b in glyphs:
        offs.append(pos | (0x80000000 if len(b) == 32 else 0))
        pos += len(b)
    for i in range(0, len(offs), 8):
        out.write("    " + ", ".join("0x%08xu" % o for o in offs[i:i + 8]) + ",\n")
    out.write("};\nstatic const unsigned char font_bits[%d] = {\n" % pos)
    write_bytes(out, b"".join(g[1] for g in glyphs))
    out.write("};\n\n/* Noto Emoji coverage masks: %dx%d, 4 bits per pixel (high nibble first),\n"
              " * row-major, for double-width cells. */\n" % (EMOJI_PX, EMOJI_PX))
    out.write("#define HOLD_EMOJI_PX %d\n#define HOLD_EMOJI_GLYPHS %d\n" % (EMOJI_PX, len(emoji)))
    out.write("static const uint32_t emoji_cp[HOLD_EMOJI_GLYPHS + 1] = {\n")
    for i in range(0, len(emoji), 10):
        out.write("    " + ", ".join("0x%05x" % e[0] for e in emoji[i:i + 10]) + ",\n")
    out.write("    0};\nstatic const unsigned char emoji_mask[%d] = {\n" % (len(emoji) * EMOJI_PX * EMOJI_PX // 2 or 1))
    write_bytes(out, b"".join(e[1] for e in emoji) or b"\0")
    out.write("};\n")


main()

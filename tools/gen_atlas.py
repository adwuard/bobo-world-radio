#!/usr/bin/env python3
# Builds files/uifont-480.bin + uifont-684.bin for World Radio's screenshot
# feature: every character the UI can draw, at SMALL/BODY/TITLE, rendered by
# the firmware's own Lua runtime (same font, same rasterizer) and packed.
#
# The runtime falls back to the SD system font (BoBoSerifSC-Regular.ttf), so
# install it in the simulator first to fill CJK glyphs the embedded base font
# lacks:
#   python3 bobo-sdk/tools/luasim/system_font.py install <SD card root>
# The renderer is the simulator (BOBO_SDK or ../bobo-sdk by default); the
# atlas is written into files/ and read by src/main.c at capture time.
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.environ.get("BOBO_SDK") or os.path.join(
    os.path.dirname(os.path.dirname(ROOT)), "bobo-sdk")
SIM = os.path.join(SDK, "tools/luasim-wasm/dist/cli.mjs")
WORK = os.path.join(tempfile.gettempdir(), "wr_atlas_work")
BOARDS = [("s3", 480, 800, "uifont-480.bin"),
          ("plus", 684, 1216, "uifont-684.bin")]
FONTS = ["small", "body", "title"]
SHOTS = 10


def read_png(path):
    """1-bit PNG -> (w, h, rows); avoids a Pillow dependency."""
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path
    i, idat = 8, b""
    w = h = None
    while i < len(d):
        ln = struct.unpack(">I", d[i:i + 4])[0]
        typ = d[i + 4:i + 8]
        dat = d[i + 8:i + 8 + ln]
        i += 12 + ln
        if typ == b"IHDR":
            w, h = struct.unpack(">II", dat[:8])
        elif typ == b"IDAT":
            idat += dat
    raw = zlib.decompress(idat)
    stride = (w + 7) // 8
    rows, prev, pos = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:
            for x in range(stride):
                line[x] = (line[x] + (line[x - 1] if x else 0)) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                line[x] = (line[x] + ((line[x - 1] if x else 0) + prev[x]) // 2) & 255
        elif f == 4:
            def paeth(a, b, c):
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                return a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            for x in range(stride):
                line[x] = (line[x] + paeth(line[x - 1] if x else 0, prev[x],
                                           prev[x - 1] if x else 0)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def bit(rows, x, y):
    return ((rows[y][x >> 3] >> (7 - (x & 7))) & 1) == 0

LUA = r'''local c = bobo.canvas
local FONT = "%(font)s"
local W, H = %(w)d, %(h)d
local HEX = "%(hex)s"
local chars = {}
for h in HEX:gmatch("%%x+") do chars[#chars + 1] = tonumber(h, 16) end
local CW, CH, COLS, ROWS, OX, OY, PER, PAGES
local pi = 0

local function layout()
  local maxw = 0
  for i = 1, #chars do
    local w = c.measure(utf8.char(chars[i]), FONT)
    if w > maxw then maxw = w end
  end
  CW = maxw + 10
  if CW < 14 then CW = 14 end
  local LH = c.line_height(FONT)
  CH = LH + 10
  COLS = math.floor((W - 16) / CW)
  ROWS = math.floor((H - 16) / CH)
  PER = COLS * ROWS
  PAGES = math.ceil(#chars / PER)
  OX = math.floor((W - COLS * CW) / 2)
  OY = 8
  bobo.log(string.format("GRID font=%%s cw=%%d ch=%%d cols=%%d rows=%%d per=%%d pages=%%d ox=%%d oy=%%d lh=%%d n=%%d",
    FONT, CW, CH, COLS, ROWS, PER, PAGES, OX, OY, LH, #chars))
end

local function draw(pg)
  c.clear()
  for k = 0, PER - 1 do
    local i = pg * PER + k + 1
    if i > #chars then break end
    local col = k %% COLS
    local row = math.floor(k / COLS)
    local cx = OX + col * CW
    local cy = OY + row * CH
    c.text(cx + 5, cy + 5, utf8.char(chars[i]), {font = FONT})
    bobo.log(string.format("CELL %%d %%X %%d %%d", i - 1, chars[i], cx, cy))
  end
  c.commit()
  bobo.log(string.format("PAGE %%d", pg))
end

function start()
  layout()
  draw(0)
end
function event(e)
  if e.type == "tap" then
    pi = pi + 1
    if pi >= PAGES then pi = PAGES - 1 end
    draw(pi)
  end
end
function stop() end
return {start = start, event = event, stop = stop}
'''

APP_JSON = '''{
  "id": "atlasgen",
  "name": "atlasgen",
  "version": "0.1.0",
  "api": 7,
  "runtime": "lua",
  "entry": "main.lua",
  "type": "light",
  "category": "tools",
  "description": "glyph atlas source",
  "permissions": []
}
'''


def charset():
    cps = set(range(0x20, 0x7F))
    skipped = []
    for path in [ROOT + "/src/main.c", ROOT + "/files/live.tsv"]:
        for ch in open(path, encoding="utf-8").read():
            cp = ord(ch)
            if cp < 0x80 or cp == 0xA0:
                continue
            if cp > 0xFFFF:
                skipped.append(cp)
                continue
            cps.add(cp)
    if skipped:
        print("skipped non-BMP:", sorted(set(skipped)))
    return sorted(cps)


def lua_chars(hexstr):
    """Hex string -> list of codepoints (same order as the Lua app)."""
    return [int(h, 16) for h in hexstr.split(",")]


def run_font(board, w, h, font, cps):
    app = os.path.join(WORK, "app_%s_%s" % (board, font))
    out = os.path.join(WORK, "out_%s_%s" % (board, font))
    os.makedirs(app, exist_ok=True)
    hexstr = ",".join("%X" % cp for cp in cps)
    open(os.path.join(app, "app.json"), "w").write(APP_JSON)
    open(os.path.join(app, "main.lua"), "w").write(
        LUA % {"font": font, "w": w, "h": h, "hex": hexstr})
    script = ["shot p0"]
    for i in range(1, SHOTS):
        script.append("tap %d %d" % (w // 2, h // 2))
        script.append("shot p%d" % i)
    open(os.path.join(WORK, "script_%s_%s.txt" % (board, font)), "w").write(
        "\n".join(script) + "\n")
    r = subprocess.run(
        ["node", SIM, "run", app, os.path.join(WORK, "script_%s_%s.txt" % (board, font)),
         out, "--board", board, "--shots", "named"],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        raise SystemExit("sim run failed: %s %s" % (board, font))
    res = json.load(open(os.path.join(out, board, "result.json")))
    logs = []
    for step in res["steps"]:
        logs.extend(step.get("logs") or [])
    grid = None
    cells = []
    pages = []
    for line in logs:
        if ": " in line:
            line = line.split(": ", 1)[1]
        if line.startswith("GRID "):
            grid = dict(kv.split("=") for kv in line[5:].split())
            grid = {k: int(v) for k, v in grid.items() if v.lstrip("-").isdigit()}
        elif line.startswith("CELL "):
            p = line.split()
            cells.append((int(p[1]), int(p[2], 16), int(p[3]), int(p[4])))
        elif line.startswith("PAGE "):
            pages.append(int(line.split()[1]))
    if grid is None:
        raise SystemExit("no GRID log: %s %s" % (board, font))
    print("%s %s: grid %s pages %s cells %d" %
          (board, font, grid, sorted(set(pages)), len(cells)))
    draw_cells = {}
    for idx, cp, cx, cy in cells:
        draw_cells[idx] = (cp, cx, cy)
    # decode every page image directly (log lines are capped per step, so the
    # grid metrics decide the layout, not the CELL lines)
    glyphs = {}
    seen = set()
    for pg in range(grid["pages"]):
        png = os.path.join(out, board, "p%d.png" % pg)
        if not os.path.exists(png):
            raise SystemExit("missing shot " + png)
        iw, ih, rows = read_png(png)
        per = grid["per"]
        for k in range(per):
            idx = pg * per + k
            if idx >= len(cps) or idx in seen:
                continue
            seen.add(idx)
            cp = cps[idx]
            cx = grid["ox"] + (k % grid["cols"]) * grid["cw"]
            cy = grid["oy"] + (k // grid["cols"]) * grid["ch"]
            x0 = y0 = 10 ** 9
            x1 = y1 = -1
            for yy in range(cy, min(cy + grid["ch"], ih)):
                for xx in range(cx, min(cx + grid["cw"], iw)):
                    if bit(rows, xx, yy):
                        if xx < x0: x0 = xx
                        if xx > x1: x1 = xx
                        if yy < y0: y0 = yy
                        if yy > y1: y1 = yy
            if x1 < 0:
                glyphs[cp] = None
                continue
            bx = x0 - (cx + 5)
            by = y0 - (cy + 5)
            gw = x1 - x0 + 1
            gh = y1 - y0 + 1
            if not (-128 <= bx <= 127 and -128 <= by <= 127 and gw <= 255 and
                    gh <= 255):
                raise SystemExit("glyph out of range %X bx=%d by=%d %dx%d" %
                                 (cp, bx, by, gw, gh))
            nbytes = (gw + 7) // 8
            bm = bytearray(nbytes * gh)
            for yy in range(gh):
                for xx in range(gw):
                    if bit(rows, x0 + xx, y0 + yy):
                        bm[yy * nbytes + (xx >> 3)] |= 0x80 >> (xx & 7)
            glyphs[cp] = (bx, by, gw, gh, bytes(bm))
    missing = [cps[i] for i in range(len(cps)) if i not in seen]
    if missing:
        raise SystemExit("chars not rendered: %s" %
                         ",".join("%X" % c for c in missing[:10]))
    blanks = [cps[i] for i in range(len(cps)) if glyphs.get(cps[i]) is None]
    print("%s %s: glyphs %d, blank %d" % (board, font, len(glyphs), len(blanks)))
    return glyphs, blanks


def pack_section(glyphs):
    recs = []
    for cp in sorted(glyphs):
        g = glyphs[cp]
        if g is None:
            continue
        bx, by, w, h, bm = g
        recs.append((cp, struct.pack("<bbBB", bx, by, w, h) + bm))
    off = 16 + len(recs) * 8
    rec_off = off
    index = bytearray()
    body = bytearray()
    for cp, rec in recs:
        index += struct.pack("<HHI", cp, 0, off)
        body += rec
        off += len(rec)
    sec = bytearray(b"WRF3")
    sec += struct.pack("<HHI", len(recs), 0, rec_off)
    sec += bytes(16 - len(sec))
    sec += index
    sec += body
    return bytes(sec)


def main():
    cps = charset()
    print("charset: %d codepoints (non-ASCII %d)" %
          (len(cps), sum(1 for c in cps if c >= 0x80)))
    os.makedirs(WORK, exist_ok=True)
    all_blank = set()
    for board, w, h, name in BOARDS:
        sections = []
        for font in FONTS:
            glyphs, blanks = run_font(board, w, h, font, cps)
            all_blank.update(blanks)
            sections.append(pack_section(glyphs))
        hdr = bytearray(b"WRF2")
        hdr += struct.pack("<HH", 1, 3)
        off = 32
        offs = []
        for sec in sections:
            offs.append(off)
            off += len(sec)
        for o in offs:
            hdr += struct.pack("<I", o)
        hdr += bytes(32 - len(hdr))
        assert len(hdr) == 32
        data = bytes(hdr) + b"".join(sections)
        path = ROOT + "/files/" + name
        open(path, "wb").write(data)
        print("%s: %s %d bytes (sections %s)" %
              (board, name, len(data), [len(s) for s in sections]))
    print("blank (missing in base font): %d -> %s" %
          (len(all_blank), " ".join("%X" % c for c in sorted(all_blank))))


if __name__ == "__main__":
    main()
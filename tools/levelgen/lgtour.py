#!/usr/bin/env python3
"""Contact sheets for a room tour.

  pd.x86_64.exe --levelgen-tour SEED [--levelgen-size 0|1|2]
  python3 tools/levelgen/lgtour.py source/test-results/tour-SEED-SIZE [per-sheet [tile-width]]

Reads manifest.txt in the tour folder and writes sheet-01.png, sheet-02.png, ...
next to it: one row per area, one tile per view (the four turns, then the hub's
two end-to-end views), each tile labelled
"A<area> R<room> <kind> floor/ceil v<yaw>" (red when the player did not end up
where the tour put him). Prints which areas have no usable picture and which
pictures are blank (nearly one colour).
"""
import os
import sys
from PIL import Image, ImageDraw, ImageStat


def load(folder):
    stops, header = [], ""
    with open(os.path.join(folder, "manifest.txt")) as fh:
        for line in fh:
            if line.startswith("# tour"):
                header = line[2:].strip()
            if line.startswith("#") or not line.strip():
                continue
            f = line.split()
            stops.append(dict(stop=int(f[0]), area=int(f[1]), room=int(f[2]), kind=f[3], flags=int(f[4]),
                              floor=int(f[5]), ceil=int(f[6]), ramp=int(f[7]), view=int(f[8]), yaw=float(f[9]),
                              pitch=float(f[10]), x=float(f[11]), z=float(f[12]), placed=int(f[13]),
                              playerroom=int(f[14]), file=f[15]))
    return header, stops


def main():
    folder = sys.argv[1]
    per_sheet = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    tw = int(sys.argv[3]) if len(sys.argv) > 3 else 320
    header, stops = load(folder)
    rows = {}
    for s in stops:
        rows.setdefault(s["area"], []).append(s)
    areas = sorted(rows)
    th = tw * 9 // 16
    cols = 4
    blank, missing = [], []

    sheets = [areas[i:i + per_sheet] for i in range(0, len(areas), per_sheet)]
    for si, group in enumerate(sheets, 1):
        nrows = sum((len(rows[a]) + cols - 1) // cols for a in group)
        sheet = Image.new("RGB", (cols * tw, nrows * th + 18), (20, 20, 24))
        draw = ImageDraw.Draw(sheet)
        draw.text((4, 3), header + "   sheet %d of %d" % (si, len(sheets)), fill=(220, 220, 220))
        y = 18
        for a in group:
            views = rows[a]
            for i, s in enumerate(views):
                x = (i % cols) * tw
                yy = y + (i // cols) * th
                path = os.path.join(folder, s["file"])
                try:
                    im = Image.open(path).convert("RGB")
                    st = ImageStat.Stat(im)
                    if max(st.stddev) < 6:
                        blank.append(s["file"])
                    sheet.paste(im.resize((tw, th)), (x, yy))
                except Exception:
                    missing.append(s["file"])
                    draw.rectangle((x, yy, x + tw - 1, yy + th - 1), fill=(60, 0, 0))
                ok = s["placed"] == 1
                tag = "A%d R%d %s f%d c%d%s v%d" % (a, s["room"], s["kind"], s["floor"], s["ceil"],
                                                    " ramp" if s["ramp"] else "", s["yaw"])
                draw.rectangle((x, yy, x + tw - 1, yy + 11), fill=(0, 0, 0) if ok else (160, 0, 0))
                draw.text((x + 2, yy), tag, fill=(255, 255, 255))
            y += ((len(views) + cols - 1) // cols) * th
        out = os.path.join(folder, "sheet-%02d.png" % si)
        sheet.save(out)
        print("wrote", out, "areas", group[0], "-", group[-1])

    bad = sorted({s["area"] for s in stops} - {s["area"] for s in stops if s["placed"]})
    print("areas:", len(areas), " stops:", len(stops), " not placed:", len([s for s in stops if not s["placed"]]))
    if bad:
        print("areas never reached:", bad)
    if missing:
        print("missing pictures:", missing)
    if blank:
        print("blank-looking pictures:", blank)


if __name__ == "__main__":
    main()

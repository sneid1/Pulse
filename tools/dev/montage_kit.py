# Montage the rendered kit thumbnails (build/kit_thumbs/<Cat>/*.png) into one labeled contact
# sheet per category, written into the upload handoff folder.
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = r"C:/Users/rq27/Pulse"
THUMBS = ROOT + "/build/kit_thumbs"
OUT = ROOT + "/quaternius_room_design_kit"
os.makedirs(OUT, exist_ok=True)

CATS = ["Walls", "Platforms", "Columns", "Props", "Decals"]
THUMB = 176          # thumbnail size in the sheet
COLS = 6
LABEL_H = 30
PAD = 8
BG = (38, 40, 46)
CELLBG = (58, 60, 66)
FG = (228, 230, 234)
try:
    font = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 12)
except Exception:
    font = ImageFont.load_default()


def wrap(name, draw, maxw):
    if draw.textlength(name, font=font) <= maxw:
        return [name]
    parts = name.split("_")
    line1, line2 = "", ""
    for i, p in enumerate(parts):
        seg = ("_" if line1 else "") + p
        if draw.textlength(line1 + seg, font=font) <= maxw or not line1:
            line1 += seg
        else:
            line2 = "_".join(parts[i:]); break
    while line2 and draw.textlength(line2, font=font) > maxw:
        line2 = line2[:-1]
    return [line1, line2] if line2 else [line1]


for cat in CATS:
    d = THUMBS + "/" + cat
    if not os.path.isdir(d):
        continue
    names = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".png"))
    rows = (len(names) + COLS - 1) // COLS
    cellw = THUMB + PAD
    cellh = THUMB + LABEL_H + PAD
    W = COLS * cellw + PAD
    H = rows * cellh + PAD + 40
    sheet = Image.new("RGB", (W, H), BG)
    dr = ImageDraw.Draw(sheet)
    dr.text((PAD, 12), f"Quaternius MegaKit  -  {cat}  ({len(names)})", font=font, fill=FG)
    for idx, nm in enumerate(names):
        r, c = divmod(idx, COLS)
        x = PAD + c * cellw
        y = 40 + r * cellh
        dr.rectangle([x, y, x + THUMB, y + THUMB], fill=CELLBG)
        try:
            im = Image.open(d + "/" + nm + ".png").convert("RGBA")
            im.thumbnail((THUMB, THUMB))
            sheet.paste(im, (x + (THUMB - im.width) // 2, y + (THUMB - im.height) // 2), im)
        except Exception as e:  # noqa: BLE001
            print("skip", nm, e)
        ty = y + THUMB + 2
        for li, line in enumerate(wrap(nm, dr, THUMB)):
            tw = dr.textlength(line, font=font)
            dr.text((x + (THUMB - tw) // 2, ty + li * 13), line, font=font, fill=FG)
    out = OUT + "/kit_sheet_" + cat + ".png"
    sheet.save(out)
    print("WROTE", out, sheet.size)
print("MONTAGE_DONE")

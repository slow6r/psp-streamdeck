#!/usr/bin/env python3
"""Генератор растрового шрифта 12x12 для PSP Deck.

Рендерит глифы из системного шрифта macOS в большом размере, уменьшает
с сохранением пропорций и вписывает в ячейку 12x12. Выдаёт:
  psp/font12x12.c   — массив C (по u16 на строку, бит 0x800 = левый пиксель)
  tools/preview.png — превью для визуальной проверки

Таблица из 162 глифов:
  0..94    ASCII 0x20..0x7E
  95       (не используется)
  96..127  А..Я   (U+0410..U+042F)
  128..143 а..п   (U+0430..U+043F)
  144..159 р..я   (U+0440..U+044F)
  160      Ё     (U+0401)
  161      ё     (U+0451)
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

CELL_W = 12
CELL_H = 12
MAX_H = 11
MAX_W = 12

CHARS = (
    [chr(c) for c in range(0x20, 0x7F)]
    + [None]                                    # слот 95 — резерв
    + [chr(c) for c in range(0x0410, 0x0430)]   # 96..127  А..Я
    + [chr(c) for c in range(0x0430, 0x0440)]   # 128..143 а..п
    + [chr(c) for c in range(0x0440, 0x0450)]   # 144..159 р..я
    + ["Ё", "ё"]                                # 160, 161
)
assert len(CHARS) == 162

FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]
RENDER_SIZE = 128  # рендерим крупно, потом уменьшаем — так глифы чётче


def load_font_path():
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return path
    sys.exit("Не найден подходящий системный шрифт")


def render_glyph(font, ch):
    """Символ -> 12 чисел u16 (по числу на строку, бит 0x800 = левый пиксель)."""
    if ch is None:
        return [0] * CELL_H

    canvas = Image.new("L", (RENDER_SIZE * 2, RENDER_SIZE * 2), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((0, 0), ch, font=font, fill=255)
    bbox = draw.textbbox((0, 0), ch, font=font)
    if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
        return [0] * CELL_H

    pad = 6
    img = canvas.crop((bbox[0] - pad, bbox[1] - pad, bbox[2] + pad, bbox[3] + pad))
    w, h = img.size

    # пропорциональное уменьшение до пределов ячейки
    scale = min(MAX_H / h, MAX_W / w, 1.0)
    nw = max(1, min(MAX_W, int(round(w * scale))))
    nh = max(1, min(CELL_H, int(round(h * scale))))
    img = img.resize((nw, nh), Image.LANCZOS)

    cell = Image.new("L", (CELL_W, CELL_H), 0)
    cell.paste(img, ((CELL_W - nw) // 2, (CELL_H - nh) // 2))

    rows = []
    px = cell.load()
    for ry in range(CELL_H):
        val = 0
        for rx in range(CELL_W):
            if px[rx, ry] > 96:
                val |= 0x800 >> rx
        rows.append(val)
    return rows


def glyph_index(ch):
    o = ord(ch)
    if 0x20 <= o < 0x7F:
        return o - 0x20
    if 0x0410 <= o <= 0x042F:
        return 96 + o - 0x0410
    if 0x0430 <= o <= 0x043F:
        return 128 + o - 0x0430
    if 0x0440 <= o <= 0x044F:
        return 144 + o - 0x0440
    if ch == "Ё":
        return 160
    if ch == "ё":
        return 161
    return ord("?") - 0x20


def main():
    out_dir = Path(__file__).resolve().parent.parent
    font_path = load_font_path()
    font = ImageFont.truetype(font_path, RENDER_SIZE)
    print(f"Шрифт: {font_path}")

    glyphs = [render_glyph(font, ch) for ch in CHARS]

    lines = [
        "/* Сгенерировано tools/genfont.py — не редактировать руками. */",
        "#include \"font12x12.h\"",
        "",
        "const unsigned short font12x12[162][12] = {",
    ]
    for i, rows in enumerate(glyphs):
        ch = CHARS[i]
        label = ch if ch and ch.isprintable() else "-"
        comment = f"/* {i:3d} {label} */"
        lines.append("    {" + ",".join(f"0x{v:03X}" for v in rows) + "}, " + comment)
    lines.append("};")
    (out_dir / "psp" / "font12x12.c").write_text("\n".join(lines) + "\n")
    print(f"psp/font12x12.c: {len(glyphs)} глифов")

    # Превью: две строки теми же глифами, масштаб 2x
    scale = 2
    samples = ["PSP DECK 0123456789 ABCXYZ", "Привет, Герои! Ёё"]
    line_h = CELL_H * scale + 4
    width = max(len(s) for s in samples) * CELL_W * scale
    img = Image.new("L", (width, line_h * len(samples) + 4), 255)
    px = img.load()
    for li, text in enumerate(samples):
        for ci, ch in enumerate(text):
            rows = glyphs[glyph_index(ch)]
            base_x = ci * CELL_W * scale
            base_y = li * line_h
            for ry in range(CELL_H):
                for rx in range(CELL_W):
                    if rows[ry] & (0x800 >> rx):
                        for sy in range(scale):
                            for sx in range(scale):
                                px[base_x + rx * scale + sx,
                                   base_y + ry * scale + sy] = 0
    preview = Path(__file__).resolve().parent / "preview.png"
    img.save(preview)
    print(f"Превью: {preview}")


if __name__ == "__main__":
    main()


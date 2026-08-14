#!/usr/bin/env python3
"""Генератор растрового шрифта 8x8 для PSP Deck.

Рендерит глифы из системного шрифта macOS в большом размере, уменьшает
с сохранением пропорций и вписывает в ячейку 8x8. Выдаёт:
  psp/font8x8.c     — массив C (1 бит/пиксель, старший бит = левый пиксель)
  tools/preview.png — превью для визуальной проверки

Таблица из 162 глифов:
  0..94    ASCII 0x20..0x7E
  95       (не используется)
  96..127  А..Я   (U+0410..U+042F)
  128..143 а..п   (U+0430..U+043F)
  144..159 р..я   (U+0440..U+044F)
  160      Ё      (U+0401)
  161      ё      (U+0451)
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

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
    "/System/Library/Fonts/Menlo.tcc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]
RENDER_SIZE = 96   # рендерим крупно, потом уменьшаем — так глифы чётче
MAX_H = 7          # вписываемся в 7 пикселей высоты
MAX_W = 8


def load_font_path():
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return path
    sys.exit("Не найден подходящий системный шрифт")


def render_glyph(font, ch):
    """Символ -> 8 байт (по байту на строку, бит 0x80 = левый пиксель)."""
    if ch is None:
        return [0] * 8

    canvas = Image.new("L", (RENDER_SIZE * 2, RENDER_SIZE * 2), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((0, 0), ch, font=font, fill=255)
    bbox = draw.textbbox((0, 0), ch, font=font)
    if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
        return [0] * 8

    pad = 4
    img = canvas.crop((bbox[0] - pad, bbox[1] - pad, bbox[2] + pad, bbox[3] + pad))
    w, h = img.size

    # пропорциональное уменьшение до пределов ячейки
    scale = min(MAX_H / h, MAX_W / w, 1.0)
    nw = max(1, min(MAX_W, int(round(w * scale))))
    nh = max(1, min(8, int(round(h * scale))))
    img = img.resize((nw, nh), Image.LANCZOS)

    cell = Image.new("L", (8, 8), 0)
    cell.paste(img, ((8 - nw) // 2, (8 - nh) // 2))

    rows = []
    px = cell.load()
    for ry in range(8):
        byte = 0
        for rx in range(8):
            if px[rx, ry] > 96:
                byte |= 0x80 >> rx
        rows.append(byte)
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
        "#include \"font8x8.h\"",
        "",
        "const unsigned char font8x8[162][8] = {",
    ]
    for i, rows in enumerate(glyphs):
        ch = CHARS[i]
        label = ch if ch and ch.isprintable() else "-"
        comment = f"/* {i:3d} {label} */"
        lines.append("    {" + ",".join(f"0x{b:02X}" for b in rows) + "}, " + comment)
    lines.append("};")
    (out_dir / "psp" / "font8x8.c").write_text("\n".join(lines) + "\n")
    print(f"psp/font8x8.c: {len(glyphs)} глифов")

    # Превью: две строки теми же глифами, масштаб 3x
    scale = 3
    samples = ["PSP DECK 0123456789 ABCXYZ", "Привет, Герои! Ёё"]
    line_h = 10 * scale
    width = max(len(s) for s in samples) * 8 * scale
    img = Image.new("L", (width, line_h * len(samples) + scale * 2), 255)
    px = img.load()
    for li, text in enumerate(samples):
        for ci, ch in enumerate(text):
            rows = glyphs[glyph_index(ch)]
            base_x = ci * 8 * scale
            base_y = li * line_h
            for ry in range(8):
                for rx in range(8):
                    if rows[ry] & (0x80 >> rx):
                        for sy in range(scale):
                            for sx in range(scale):
                                px[base_x + rx * scale + sx, base_y + ry * scale + sy] = 0
    preview = Path(__file__).resolve().parent / "preview.png"
    img.save(preview)
    print(f"Превью: {preview}")


if __name__ == "__main__":
    main()

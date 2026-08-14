#ifndef FONT8X8_H
#define FONT8X8_H

/*
 * Растровый шрифт 8x8, генерируется tools/genfont.py.
 * Бит 0x80 первого байта = верхний-левый пиксель глифа.
 *
 * Индексы:
 *   0..94    ASCII 0x20..0x7E
 *   95       резерв
 *   96..127  А..Я  (U+0410..U+042F, UTF-8: D0 90..D0 AF)
 *   128..143 а..п  (U+0430..U+043F, UTF-8: D0 B0..D0 BF)
 *   144..159 р..я  (U+0440..U+044F, UTF-8: D1 80..D1 8F)
 *   160      Ё     (UTF-8: D0 81)
 *   161      ё     (UTF-8: D1 91)
 */
extern const unsigned char font8x8[162][8];

#define FONT_GLYPH_COUNT 162
#define FONT_GLYPH_W 8
#define FONT_GLYPH_H 8

#endif

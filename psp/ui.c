#include <stdio.h>
#include <string.h>

#include <pspdisplay.h>

#include "font8x8.h"
#include "ui.h"

/*
 * Отрисовка напрямую в framebuffer (формат 8888), двойная буферизация.
 * Плитка: сетка 4x2, у каждой свой цвет-акцент и подпись.
 */

static unsigned int __attribute__((aligned(16))) fbs[2][UI_STRIDE * UI_SCREEN_H];
static int fb_index = 0;
static unsigned int *draw = fbs[0];

/* окно обрезки по горизонтали для подписей плиток */
static int clip_on = 0;
static int clip_x0 = 0, clip_x1 = UI_SCREEN_W;

#define RGB(r, g, b) \
    (0xFF000000u | ((unsigned int)(b) << 16) | ((unsigned int)(g) << 8) | (unsigned int)(r))

#define COL_BG         RGB(16, 18, 24)
#define COL_HEADER_BG  RGB(24, 27, 36)
#define COL_FOOTER_BG  RGB(20, 22, 29)
#define COL_TILE       RGB(32, 36, 46)
#define COL_TILE_DIM   RGB(24, 27, 34)
#define COL_TILE_SEL   RGB(48, 54, 68)
#define COL_BORDER     RGB(60, 66, 82)
#define COL_BORDER_SEL RGB(235, 238, 248)
#define COL_TEXT       RGB(232, 234, 242)
#define COL_TEXT_DIM   RGB(126, 132, 148)
#define COL_OK         RGB(74, 200, 112)
#define COL_ERR        RGB(228, 92, 82)
#define COL_WARN       RGB(238, 190, 72)
#define COL_WHITE      RGB(255, 255, 255)

#define HDR_H 26
#define FTR_H 16
#define GRID_X0 4
#define GRID_Y0 (HDR_H + 4)
#define TILE_W 116
#define TILE_H 111
#define TILE_GAP 2

static unsigned int mix(unsigned int a, unsigned int b, int t /* 0..255 */)
{
    unsigned int ar = (a >> 0) & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    unsigned int br = (b >> 0) & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
    unsigned int r = (ar * (255 - t) + br * t) / 255;
    unsigned int g = (ag * (255 - t) + bg * t) / 255;
    unsigned int bl = (ab * (255 - t) + bb * t) / 255;
    return 0xFF000000u | (bl << 16) | (g << 8) | r;
}

static void px(int x, int y, unsigned int c)
{
    if (x < 0 || x >= UI_SCREEN_W || y < 0 || y >= UI_SCREEN_H)
        return;
    if (clip_on && (x < clip_x0 || x >= clip_x1))
        return;
    draw[y * UI_STRIDE + x] = c;
}

static void fill_rect(int x, int y, int w, int h, unsigned int c)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            px(i, j, c);
}

static void rect(int x, int y, int w, int h, int t, unsigned int c)
{
    fill_rect(x, y, w, t, c);
    fill_rect(x, y + h - t, w, t, c);
    fill_rect(x, y, t, h, c);
    fill_rect(x + w - t, y, t, h, c);
}

/* ---------- текст ---------- */

/* Один символ UTF-8 -> индекс глифа. Возвращает длину символа в байтах,
   0 на конце строки. Нераспознанное -> -1 (нарисуется '?'). */
static int utf8_next(const char *s, int i, int *gidx)
{
    unsigned char b0 = (unsigned char)s[i];

    if (b0 == 0)
        return 0;

    if (b0 < 0x80) {
        *gidx = (b0 >= 0x20 && b0 < 0x7F) ? b0 - 0x20 : -1;
        return 1;
    }

    {
        unsigned char b1 = (unsigned char)s[i + 1];

        if (b0 == 0xD0) {
            if (b1 == 0x81) { *gidx = 160; return 2; }
            if (b1 >= 0x90 && b1 <= 0xAF) { *gidx = 96 + (b1 - 0x90); return 2; }
            if (b1 >= 0xB0)               { *gidx = 128 + (b1 - 0xB0); return 2; }
        } else if (b0 == 0xD1) {
            if (b1 == 0x91)               { *gidx = 161; return 2; }
            if (b1 >= 0x80 && b1 <= 0x8F) { *gidx = 144 + (b1 - 0x80); return 2; }
        }
    }

    *gidx = -1;
    return 1;
}

static void draw_glyph(int x, int y, int scale, unsigned int c, int gidx)
{
    int rx, ry, sx, sy;
    const unsigned char *g;

    if (gidx < 0)
        gidx = '?' - 0x20;
    if (gidx >= FONT_GLYPH_COUNT)
        gidx = '?' - 0x20;

    g = font8x8[gidx];
    for (ry = 0; ry < 8; ry++) {
        unsigned char row = g[ry];
        if (!row)
            continue;
        for (rx = 0; rx < 8; rx++) {
            if (row & (0x80 >> rx)) {
                for (sy = 0; sy < scale; sy++)
                    for (sx = 0; sx < scale; sx++)
                        px(x + rx * scale + sx, y + ry * scale + sy, c);
            }
        }
    }
}

int ui_text(int x, int y, int scale, unsigned int c, const char *s)
{
    int i = 0, adv, gidx;

    while ((adv = utf8_next(s, i, &gidx)) != 0) {
        draw_glyph(x, y, scale, c, gidx);
        x += 8 * scale;
        i += adv;
    }
    return x;
}

static int text_glyph_count(const char *s)
{
    int i = 0, adv, gidx, n = 0;

    while ((adv = utf8_next(s, i, &gidx)) != 0) {
        n++;
        i += adv;
    }
    return n;
}

int ui_text_width(int scale, const char *s)
{
    return text_glyph_count(s) * 8 * scale;
}

void ui_text_centered(int cx, int y, int scale, unsigned int c, const char *s)
{
    ui_text(cx - ui_text_width(scale, s) / 2, y, scale, c, s);
}

void ui_text_right(int right, int y, int scale, unsigned int c, const char *s)
{
    ui_text(right - ui_text_width(scale, s), y, scale, c, s);
}

/* ---------- подпись плитки: масштаб 2 в одну строку, иначе 1 с переносом ---------- */

static void draw_tile_label(const char *label, int x, int y, int w, int h)
{
    enum { MAX_LINES = 5, MAX_WORDS = 12 };

    int inner_w = w - 8;

    if (ui_text_width(2, label) <= inner_w) {
        ui_text_centered(x + w / 2, y + (h - 16) / 2, 2, COL_TEXT, label);
        return;
    }

    {
        /* разбор по словам с байтовыми смещениями (UTF-8 не рвём) */
        const char *line_ptr[MAX_LINES];
        int line_len[MAX_LINES];
        int nlines = 0;

        int woff[MAX_WORDS], wlen[MAX_WORDS], wgl[MAX_WORDS];
        int nwords = 0;

        int i = 0, adv, gidx;

        while (label[i] == ' ')
            i++;
        while (label[i]) {
            int start = i, gl = 0;
            while (label[i] && label[i] != ' ') {
                adv = utf8_next(label, i, &gidx);
                i += adv;
                gl++;
            }
            if (nwords < MAX_WORDS) {
                woff[nwords] = start;
                wlen[nwords] = i - start;
                wgl[nwords] = gl;
                nwords++;
            }
            while (label[i] == ' ')
                i++;
        }

        if (nwords == 0)
            return;

        {
            int max_chars = inner_w / 8;
            int first = 0, acc = 0;

            for (int wi = 0; wi < nwords; wi++) {
                int sep = (wi > first) ? 1 : 0;
                if (acc + sep + wgl[wi] > max_chars && acc > 0) {
                    if (nlines < MAX_LINES) {
                        line_ptr[nlines] = &label[woff[first]];
                        line_len[nlines] = woff[wi] - woff[first];
                        nlines++;
                    }
                    first = wi;
                    acc = wgl[wi];
                } else {
                    acc += sep + wgl[wi];
                }
            }
            if (nlines < MAX_LINES) {
                line_ptr[nlines] = &label[woff[first]];
                line_len[nlines] = wlen[nwords - 1] + (woff[nwords - 1] - woff[first]);
                nlines++;
            }
        }

        {
            int block_h = nlines * 10;
            int ty = y + (h - block_h) / 2;

            for (int li = 0; li < nlines; li++) {
                char buf[DECK_LABEL_MAX];
                int bl = line_len[li];

                if (bl >= DECK_LABEL_MAX)
                    bl = DECK_LABEL_MAX - 1;
                memcpy(buf, line_ptr[li], bl);
                buf[bl] = 0;
                ui_text_centered(x + w / 2, ty + li * 10, 1, COL_TEXT, buf);
            }
        }
    }
}

/* ---------- кадр ---------- */

void ui_init(void)
{
    memset(fbs, 0, sizeof(fbs));
    fb_index = 0;
    draw = fbs[0];
    sceDisplaySetFrameBuf(draw, UI_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
}

void ui_set_vram(int system_dialog_active)
{
    if (system_dialog_active) {
        /* системным диалогам нужен framebuffer в VRAM */
        sceDisplaySetFrameBuf((void *)0x44000000, UI_STRIDE,
                              PSP_DISPLAY_PIXEL_FORMAT_8888,
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
    } else {
        sceDisplaySetFrameBuf(draw, UI_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
    }
}

void ui_begin_frame(void)
{
    draw = fbs[fb_index];
    clip_on = 0;
    fill_rect(0, 0, UI_SCREEN_W, UI_SCREEN_H, COL_BG);
}

void ui_end_frame(void)
{
    sceDisplaySetFrameBuf(draw, UI_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    fb_index ^= 1;
    sceDisplayWaitVblankStart();
}

/* ---------- шапка / сетка / подвал ---------- */

void ui_draw_header(UiLinkState link, int battery, int page, int page_count,
                    const char *page_name)
{
    const char *status;
    unsigned int status_col;
    char buf[48];

    fill_rect(0, 0, UI_SCREEN_W, HDR_H, COL_HEADER_BG);
    fill_rect(0, HDR_H, UI_SCREEN_W, 1, COL_BORDER);

    ui_text(6, 9, 1, COL_TEXT, "PSP DECK");

    /* страница по центру */
    if (page_count > 0) {
        snprintf(buf, sizeof(buf), "%d/%d  %s", page + 1, page_count,
                 page_name && page_name[0] ? page_name : "");
        ui_text_centered(UI_SCREEN_W / 2, 9, 1, COL_TEXT_DIM, buf);
    }

    /* статус связи */
    switch (link) {
    case UI_LINK_OK:      status = "LINK OK";  status_col = COL_OK;   break;
    case UI_LINK_SEARCH:  status = "SCAN...";  status_col = COL_WARN; break;
    case UI_LINK_CONNECT: status = "CONN...";  status_col = COL_WARN; break;
    case UI_LINK_WIFI:    status = "WIFI...";  status_col = COL_WARN; break;
    default:              status = "WLAN OFF"; status_col = COL_ERR;  break;
    }
    ui_text_right(UI_SCREEN_W - 44, 9, 1, status_col, status);

    /* батарея справа */
    if (battery >= 0)
        snprintf(buf, sizeof(buf), "%d%%", battery);
    else
        snprintf(buf, sizeof(buf), "AC");
    ui_text_right(UI_SCREEN_W - 6, 9, 1, COL_TEXT_DIM, buf);
}

void ui_draw_grid(const Deck *deck, int page, int selected, int flash_button)
{
    int row, col;

    if (!deck || deck->page_count <= 0) {
        ui_text_centered(UI_SCREEN_W / 2, UI_SCREEN_H / 2 - 4, 1, COL_TEXT_DIM,
                         "Нет раскладки (нет связи с ПК)");
        return;
    }

    if (page >= deck->page_count)
        page = 0;

    for (row = 0; row < 2; row++) {
        for (col = 0; col < 4; col++) {
            int i = row * 4 + col;
            int x = GRID_X0 + col * (TILE_W + TILE_GAP);
            int y = GRID_Y0 + row * (TILE_H + TILE_GAP);
            const DeckButton *b = &deck->pages[page].buttons[i];
            int is_sel = (i == selected);
            unsigned int base, border;
            int bt = is_sel ? 2 : 1;

            if (b->used) {
                unsigned int accent = 0xFF000000u | (b->color & 0xFFFFFF);
                base = mix(COL_TILE, accent, 90); /* лёгкий тон цвета кнопки */
                border = is_sel ? COL_BORDER_SEL : mix(COL_BORDER, accent, 128);
                if (i == flash_button)
                    base = mix(base, COL_WHITE, 120);
            } else {
                base = COL_TILE_DIM;
                border = COL_TILE_DIM;
            }

            if (is_sel)
                base = mix(base, COL_TILE_SEL, 160);

            fill_rect(x, y, TILE_W, TILE_H, base);
            rect(x, y, TILE_W, TILE_H, bt, border);

            if (b->used) {
                /* полоска акцентного цвета сверху */
                unsigned int accent = 0xFF000000u | (b->color & 0xFFFFFF);
                fill_rect(x + bt, y + bt, TILE_W - bt * 2, 4,
                          mix(accent, COL_WHITE, 40));

                clip_on = 1;
                clip_x0 = x + 2;
                clip_x1 = x + TILE_W - 2;
                draw_tile_label(b->label, x, y + 6, TILE_W, TILE_H - 10);
                clip_on = 0;
            }
        }
    }
}

void ui_draw_footer(void)
{
    fill_rect(0, UI_SCREEN_H - FTR_H, UI_SCREEN_W, FTR_H, COL_FOOTER_BG);
    fill_rect(0, UI_SCREEN_H - FTR_H, UI_SCREEN_W, 1, COL_BORDER);
    ui_text_centered(UI_SCREEN_W / 2, UI_SCREEN_H - FTR_H + 4, 1, COL_TEXT_DIM,
                     "X - нажать   L/R - страницы   START - выход");
}

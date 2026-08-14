#include <stdio.h>
#include <string.h>

#include <pspdisplay.h>

#include "font12x12.h"
#include "ui.h"

/*
 * Отрисовка напрямую в framebuffer (формат 8888), двойная буферизация.
 * Стиль: тёмная тема с градиентом, плитки с тенью и акцентной полосой,
 * «прицельные уголки» на выбранной плитке, статус-пилюля в шапке.
 */

static unsigned int __attribute__((aligned(16))) fbs[2][UI_STRIDE * UI_SCREEN_H];
static int fb_index = 0;
static unsigned int *draw = fbs[0];

/* окно обрезки по горизонтали для подписей плиток */
static int clip_on = 0;
static int clip_x0 = 0, clip_x1 = UI_SCREEN_W;

#define RGB(r, g, b) \
    (0xFF000000u | ((unsigned int)(b) << 16) | ((unsigned int)(g) << 8) | (unsigned int)(r))

/* палитра: тёмная тема */
#define COL_BG_TOP     RGB(16, 20, 28)
#define COL_BG_BOTTOM  RGB(6, 8, 12)
#define COL_HDR_BG     RGB(22, 27, 36)
#define COL_HDR_LINE   RGB(48, 56, 70)
#define COL_TILE       RGB(30, 36, 47)
#define COL_TILE_DIM   RGB(20, 24, 31)
#define COL_TILE_HOV   RGB(43, 51, 66)
#define COL_SHADOW     RGB(3, 4, 6)
#define COL_BORDER     RGB(52, 61, 77)
#define COL_ACC        RGB(88, 166, 255)
#define COL_TEXT       RGB(235, 240, 246)
#define COL_TEXT_DIM   RGB(140, 150, 165)
#define COL_OK         RGB(74, 210, 115)
#define COL_ERR        RGB(248, 93, 88)
#define COL_WARN       RGB(245, 190, 80)
#define COL_WHITE      RGB(255, 255, 255)

#define HDR_H 28
#define FTR_H 18
#define GRID_X0 4
#define GRID_Y0 (HDR_H + 6)
#define TILE_W 116
#define TILE_H 106
#define TILE_GAP 2

static unsigned int mix(unsigned int a, unsigned int b, int t /* 0..255 */)
{
    unsigned int ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    unsigned int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
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

static void hline(int x, int y, int w, unsigned int c)
{
    int i;
    for (i = x; i < x + w; i++)
        px(i, y, c);
}

static void vline(int x, int y, int h, unsigned int c)
{
    int j;
    for (j = y; j < y + h; j++)
        px(x, j, c);
}

/* «прицельные уголки» — как в камере: 4 уголка по периметру */
static void focus_brackets(int x, int y, int w, int h, int len, int t,
                           unsigned int c)
{
    hline(x, y, len, c);                 vline(x, y, len, c);
    hline(x + w - len, y, len, c);       vline(x + w - t, y, len, c);
    hline(x, y + h - t, len, c);         vline(x, y + h - len, len, c);
    hline(x + w - len, y + h - t, len, c); vline(x + w - t, y + h - len, len, c);
}

/* ---------- текст (12x12) ---------- */

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
    const unsigned short *g;

    if (gidx < 0 || gidx >= FONT_GLYPH_COUNT)
        gidx = '?' - 0x20;

    g = font12x12[gidx];
    for (ry = 0; ry < FONT_H; ry++) {
        unsigned short row = g[ry];
        if (!row)
            continue;
        for (rx = 0; rx < FONT_W; rx++) {
            if (row & (0x800 >> rx)) {
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
        x += FONT_W * scale;
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
    return text_glyph_count(s) * FONT_W * scale;
}

void ui_text_centered(int cx, int y, int scale, unsigned int c, const char *s)
{
    ui_text(cx - ui_text_width(scale, s) / 2, y, scale, c, s);
}

void ui_text_right(int right, int y, int scale, unsigned int c, const char *s)
{
    ui_text(right - ui_text_width(scale, s), y, scale, c, s);
}

/* ---------- подпись плитки: перенос по словам, авт. масштаб ---------- */

static void draw_tile_label(const char *label, int x, int y, int w, int h)
{
    enum { MAX_LINES = 5, MAX_WORDS = 12 };

    int inner_w = w - 8;

    if (ui_text_width(2, label) <= inner_w) {
        ui_text_centered(x + w / 2, y + (h - FONT_H * 2) / 2, 2, COL_TEXT, label);
        return;
    }

    {
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
            int max_chars = inner_w / FONT_W; /* 8 символов при 108px */
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
            int block_h = nlines * (FONT_H + 2);
            int ty = y + (h - block_h) / 2;

            for (int li = 0; li < nlines; li++) {
                char buf[DECK_LABEL_MAX];
                int bl = line_len[li];

                if (bl >= DECK_LABEL_MAX)
                    bl = DECK_LABEL_MAX - 1;
                memcpy(buf, line_ptr[li], bl);
                buf[bl] = 0;
                ui_text_centered(x + w / 2, ty + li * (FONT_H + 2), 1, COL_TEXT, buf);
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
    int y;

    draw = fbs[fb_index];
    clip_on = 0;

    /* вертикальный градиент фона */
    for (y = 0; y < UI_SCREEN_H; y++) {
        unsigned int c = mix(COL_BG_TOP, COL_BG_BOTTOM, y * 255 / UI_SCREEN_H);
        fill_rect(0, y, UI_SCREEN_W, 1, c);
    }
}

void ui_end_frame(void)
{
    sceDisplaySetFrameBuf(draw, UI_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    fb_index ^= 1;
    sceDisplayWaitVblankStart();
}

/* ---------- шапка / сетка / подвал ---------- */

static const char *link_status_text(UiLinkState link, unsigned int *col)
{
    switch (link) {
    case UI_LINK_OK:      *col = COL_OK;   return "ONLINE";
    case UI_LINK_SEARCH:  *col = COL_WARN; return "SCAN";
    case UI_LINK_CONNECT: *col = COL_WARN; return "CONN";
    case UI_LINK_JOIN:    *col = COL_WARN; return "JOIN";
    case UI_LINK_WIFI:    *col = COL_WARN; return "NO NET";
    default:              *col = COL_ERR;  return "WLAN OFF";
    }
}

void ui_draw_header(UiLinkState link, int battery, int page, int page_count,
                    const char *page_name)
{
    unsigned int st_col;
    const char *status;
    char buf[48];
    int st_w, pill_x, pill_y = 6, pill_h = HDR_H - 12;

    fill_rect(0, 0, UI_SCREEN_W, HDR_H, COL_HDR_BG);
    hline(0, HDR_H, UI_SCREEN_W, COL_HDR_LINE);

    /* логотип слева с акцентной чертой */
    fill_rect(6, 8, 3, 12, COL_ACC);
    ui_text(14, 8, 1, COL_WHITE, "PSP DECK");

    /* страница по центру */
    if (page_count > 0) {
        snprintf(buf, sizeof(buf), "%d/%d · %s", page + 1, page_count,
                 page_name && page_name[0] ? page_name : "");
        ui_text_centered(UI_SCREEN_W / 2 + 20, 8, 1, COL_TEXT_DIM, buf);
    }

    /* статус-пилюля справа */
    status = link_status_text(link, &st_col);
    st_w = ui_text_width(1, status);
    pill_x = UI_SCREEN_W - 6 - 40 - 10 - st_w - 12;

    fill_rect(pill_x - 6, pill_y, st_w + 12, pill_h, mix(st_col, COL_HDR_BG, 190));
    fill_rect(pill_x - 6, pill_y, st_w + 12, 1, mix(st_col, COL_HDR_BG, 80));
    fill_rect(pill_x - 6, pill_y + pill_h - 1, st_w + 12, 1,
              mix(st_col, COL_HDR_BG, 80));
    ui_text(pill_x, pill_y + 3, 1, st_col, status);

    /* батарея в самом правом углу */
    if (battery >= 0)
        snprintf(buf, sizeof(buf), "%d%%", battery);
    else
        snprintf(buf, sizeof(buf), "AC");
    ui_text_right(UI_SCREEN_W - 6, 8, 1, COL_TEXT_DIM, buf);
}

void ui_draw_grid(const Deck *deck, int page, int selected, int flash_button)
{
    int row, col;

    if (!deck || deck->page_count <= 0) {
        ui_text_centered(UI_SCREEN_W / 2, UI_SCREEN_H / 2 - FONT_H, 1,
                         COL_TEXT, "Нет связи с ПК");
        ui_text_centered(UI_SCREEN_W / 2, UI_SCREEN_H / 2 + FONT_H / 2, 1,
                         COL_TEXT_DIM, "Wi-Fi настраивается: Настройки -> Сеть");
        return;
    }

    if (page >= deck->page_count)
        page = 0;

    /* проход 1: тени (чтобы не перекрывали соседние плитки) */
    for (row = 0; row < 2; row++)
        for (col = 0; col < 4; col++) {
            int x = GRID_X0 + col * (TILE_W + TILE_GAP);
            int y = GRID_Y0 + row * (TILE_H + TILE_GAP);
            fill_rect(x + 2, y + 3, TILE_W, TILE_H, COL_SHADOW);
        }

    /* проход 2: плитки */
    for (row = 0; row < 2; row++) {
        for (col = 0; col < 4; col++) {
            int i = row * 4 + col;
            int x = GRID_X0 + col * (TILE_W + TILE_GAP);
            int y = GRID_Y0 + row * (TILE_H + TILE_GAP);
            const DeckButton *b = &deck->pages[page].buttons[i];
            int is_sel = (i == selected);
            unsigned int base, border;

            if (b->used) {
                unsigned int accent = 0xFF000000u | (b->color & 0xFFFFFF);
                base = mix(COL_TILE, accent, 40);
                border = mix(COL_BORDER, accent, 170);
            } else {
                base = COL_TILE_DIM;
                border = COL_TILE_DIM;
            }

            if (is_sel)
                base = mix(base, COL_TILE_HOV, 180);
            if (i == flash_button)
                base = mix(base, COL_WHITE, 110);

            fill_rect(x, y, TILE_W, TILE_H, base);

            /* аккуратная рамка: только верх и низ + бока по 1px */
            hline(x, y, TILE_W, border);
            hline(x, y + TILE_H - 1, TILE_W, border);
            vline(x, y, TILE_H, border);
            vline(x + TILE_W - 1, y, TILE_H, border);

            if (b->used) {
                unsigned int accent = 0xFF000000u | (b->color & 0xFFFFFF);
                /* акцентная полоса сверху, чуть светлее цвет */
                fill_rect(x + 1, y + 1, TILE_W - 2, 5,
                          mix(accent, COL_WHITE, 60));

                clip_on = 1;
                clip_x0 = x + 2;
                clip_x1 = x + TILE_W - 2;
                draw_tile_label(b->label, x, y + 10, TILE_W, TILE_H - 12);
                clip_on = 0;
            }

            if (is_sel)
                focus_brackets(x - 2, y - 2, TILE_W + 4, TILE_H + 4, 12, 2,
                               COL_WHITE);
        }
    }
}

void ui_draw_footer(void)
{
    fill_rect(0, UI_SCREEN_H - FTR_H, UI_SCREEN_W, FTR_H, COL_HDR_BG);
    hline(0, UI_SCREEN_H - FTR_H, UI_SCREEN_W, COL_HDR_LINE);
    ui_text_centered(UI_SCREEN_W / 2, UI_SCREEN_H - FTR_H + 3, 1, COL_TEXT_DIM,
                     "X — нажать · L/R — страницы · START — выход");
}

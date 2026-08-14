#ifndef UI_H
#define UI_H

#include "deck.h"

#define UI_SCREEN_W 480
#define UI_SCREEN_H 272
#define UI_STRIDE   512 /* аппаратное требование: строка кратна 64 байтам */

typedef enum {
    UI_LINK_OFF = 0, /* тумблер WLAN выключен */
    UI_LINK_WIFI,    /* нет сети, SELECT открывает диалог Wi-Fi */
    UI_LINK_JOIN,    /* подключаемся к сохранённой сети */
    UI_LINK_SEARCH,  /* ищем ПК в сети */
    UI_LINK_CONNECT, /* подключаемся к ПК */
    UI_LINK_OK       /* связь установлена */
} UiLinkState;

void ui_init(void);
void ui_begin_frame(void);
void ui_end_frame(void);

/* Отдаёт экран системному диалогу Wi-Fi и возвращает обратно. */
void ui_set_vram(int system_dialog_active);

void ui_draw_header(UiLinkState link, int battery, int page, int page_count,
                    const char *page_name);
void ui_draw_grid(const Deck *deck, int page, int selected, int flash_button);
void ui_draw_footer(void);

#endif

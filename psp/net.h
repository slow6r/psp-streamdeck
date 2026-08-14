#ifndef NET_H
#define NET_H

#include "deck.h"

typedef enum {
    NET_WLAN_OFF = 0, /* тумблер WLAN выключен */
    NET_INIT,         /* инициализация сетевого стека */
    NET_DIALOG,       /* системный диалог выбора Wi-Fi */
    NET_SEARCH,       /* broadcast-поиск ПК */
    NET_CONNECT,      /* TCP-подключение к ПК */
    NET_LINKED        /* связь установлена */
} NetState;

/* Загружает сетевые модули и инициализирует стек (вызывать один раз).
   0 = успех. deck — куда складывать полученную раскладку. */
int net_start(Deck *deck);

/* Прокачка конечного автомата; вызывать каждый кадр. */
void net_tick(void);

NetState net_state(void);

/* Просит открыть системный диалог выбора Wi-Fi (кнопка SELECT).
   Работает, когда мы не в сети и диалог не открыт. */
void net_open_dialog(void);

/* 1, пока открыт системный диалог Wi-Fi (в это времяmain не должен
   трогать framebuffer — экран принадлежит диалогу). */
int net_dialog_active(void);

int net_linked(void);
int net_send_press(int page, int button);
void net_shutdown(void);

#endif

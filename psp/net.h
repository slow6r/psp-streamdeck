#ifndef NET_H
#define NET_H

#include "deck.h"

/*
 * Сеть без системных диалогов: подключение делается через профиль,
 * созданный в родных настройках PSP (XMB -> Настройки сети).
 * Приложение само подключается к первому рабочему профилю.
 */

typedef enum {
    NET_WLAN_OFF = 0, /* тумблер WLAN выключен */
    NET_INIT,         /* инициализация сетевого стека */
    NET_NOCONN,       /* нет профиля/сети — настроить в XMB */
    NET_AUTOCONN,     /* подключаемся сохранённым профилем */
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
int net_linked(void);
int net_send_press(int page, int button);
void net_shutdown(void);

#endif

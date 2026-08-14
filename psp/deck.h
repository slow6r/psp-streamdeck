#ifndef DECK_H
#define DECK_H

/*
 * Модель раскладки кнопок, получаемой от ПК-сервера.
 */

#define DECK_MAX_PAGES   8
#define DECK_PAGE_BUTTONS 8
#define DECK_LABEL_MAX   64
#define DECK_NAME_MAX    32

typedef struct {
    char label[DECK_LABEL_MAX]; /* UTF-8, может быть с кириллицей */
    unsigned int color;         /* 0x00RRGGBB */
    int used;                   /* 0 = слот пустой */
} DeckButton;

typedef struct {
    char name[DECK_NAME_MAX];
    DeckButton buttons[DECK_PAGE_BUTTONS];
} DeckPage;

typedef struct {
    int page_count;
    DeckPage pages[DECK_MAX_PAGES];
} Deck;

#endif

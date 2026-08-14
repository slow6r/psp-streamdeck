#include <string.h>

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>

#include "deck.h"
#include "net.h"
#include "ui.h"

PSP_MODULE_INFO("PSPDECK", 0, 1, 3);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);

static int running = 1;

/* стандартный выход через HOME */
static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    {
        int cb = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
        if (cb >= 0)
            sceKernelRegisterExitCallback(cb);
    }
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int id = sceKernelCreateThread("cb_update", callback_thread, 0x11, 0xFA0,
                                   THREAD_ATTR_USER, 0);
    if (id >= 0)
        sceKernelStartThread(id, 0, 0);
}

static UiLinkState link_state(void)
{
    switch (net_state()) {
    case NET_LINKED:
        return UI_LINK_OK;
    case NET_CONNECT:
        return UI_LINK_CONNECT;
    case NET_SEARCH:
        return UI_LINK_SEARCH;
    case NET_AUTOCONN:
        return UI_LINK_JOIN;
    case NET_NOCONN:
    case NET_INIT:
        return UI_LINK_WIFI;
    default:
        return UI_LINK_OFF;
    }
}

int main(void)
{
    static Deck deck;
    unsigned int old_buttons = 0;
    int col = 0, row = 0;
    int page = 0;
    int flash_timer = 0, flash_button = -1;

    setup_callbacks();

    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    memset(&deck, 0, sizeof(deck));
    ui_init();

    if (net_start(&deck) < 0) {
        /* без сети приложение всё равно работает как демонстрация UI */
    }

    while (running) {
        SceCtrlData pad;
        unsigned int buttons, edge;
        int selected = row * 4 + col;

        sceCtrlReadBufferPositive(&pad, 1);
        buttons = pad.Buttons;
        edge = buttons & ~old_buttons;
        old_buttons = buttons;

        if (edge & PSP_CTRL_START)
            break;

        if (edge & PSP_CTRL_LEFT)
            col = (col + 3) % 4;
        if (edge & PSP_CTRL_RIGHT)
            col = (col + 1) % 4;
        if (edge & PSP_CTRL_UP)
            row ^= 1;
        if (edge & PSP_CTRL_DOWN)
            row ^= 1;

        /* пересчитываем после навигации, чтобы RIGHT+CROSS в одном кадре
           нажимали уже новую кнопку */
        selected = row * 4 + col;

        /* страница могла сброситься после переподключения */
        if (page >= deck.page_count)
            page = 0;

        if (deck.page_count > 0) {
            if (edge & PSP_CTRL_RTRIGGER)
                page = (page + 1) % deck.page_count;
            if (edge & PSP_CTRL_LTRIGGER)
                page = (page + deck.page_count - 1) % deck.page_count;

            if (edge & PSP_CTRL_CROSS) {
                const DeckButton *b = &deck.pages[page].buttons[selected];
                if (b->used && net_linked()) {
                    net_send_press(page, selected);
                    flash_timer = 9;
                    flash_button = selected;
                }
            }

            ui_begin_frame();
            ui_draw_header(link_state(), scePowerGetBatteryLifePercent(),
                           page, deck.page_count, deck.pages[page].name);
            ui_draw_grid(&deck, page, selected,
                         flash_timer > 0 ? flash_button : -1);
            ui_draw_footer();
            ui_end_frame();
        } else {
            ui_begin_frame();
            ui_draw_header(link_state(), scePowerGetBatteryLifePercent(),
                           0, 0, "");
            ui_draw_grid(&deck, 0, -1, -1);
            ui_draw_footer();
            ui_end_frame();
        }

        net_tick();

        if (flash_timer > 0)
            flash_timer--;
    }

    net_shutdown();
    sceKernelExitGame();
    return 0;
}

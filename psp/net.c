#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_modules.h>
#include <psputility_netconf.h>
#include <pspwlan.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "net.h"
#include "ui.h"

/*
 * Конечный автомат сети:
 *   WLAN_OFF -> DIALOG (системный выбор Wi-Fi) -> SEARCH (broadcast)
 *   -> CONNECT (TCP) -> LINKED, при обрыве снова SEARCH/DIALOG.
 */

#define DISCOVERY_PORT 50000
#define TCP_PORT_DEFAULT 50001
#define DISCOVER_MSG "PSPDECK_DISCOVER"
#define SERVER_REPLY "PSPDECK_SERVER"
#define PING_PERIOD_MS 5000
#define PONG_TIMEOUT_MS 12000
#define BROADCAST_PERIOD_MS 400

static NetState state = NET_WLAN_OFF;
static Deck *deck = NULL;

static int udp_fd = -1;
static int tcp_fd = -1;
static unsigned int pc_ip_bytes[4];
static int pc_port = TCP_PORT_DEFAULT;

static pspUtilityNetconfData netconf;
static int netconf_started = 0;
static int pending_dialog = 0; /* 1 = открыть диалог при следующем tick */

static char rxbuf[1024];
static int rxlen = 0;

static unsigned int last_broadcast_ms = 0;
static unsigned int last_ping_ms = 0;
static unsigned int last_pong_ms = 0;

/* промежуточный разбор раскладки */
static int parse_page = -1;
static int parse_btn = 0;

static u32 now_ms(void)
{
    return sceKernelGetSystemTimeLow() / 1000;
}

static void set_addr(struct sockaddr_in *a, const unsigned int ip[4], int port)
{
    unsigned char *dst;
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((unsigned short)port);
    dst = (unsigned char *)&a->sin_addr.s_addr;
    dst[0] = (unsigned char)ip[0];
    dst[1] = (unsigned char)ip[1];
    dst[2] = (unsigned char)ip[2];
    dst[3] = (unsigned char)ip[3];
}

static int wifi_connected(void)
{
    int st = 0;

    if (sceNetApctlGetState(&st) < 0)
        return 0;
    return st == PSP_NET_APCTL_STATE_GOT_IP;
}

/* ---------- UDP discovery ---------- */

static void udp_close(void)
{
    if (udp_fd >= 0) {
        sceNetInetClose(udp_fd);
        udp_fd = -1;
    }
}

static int udp_open(void)
{
    int one = 1;

    udp_close();
    udp_fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0)
        return -1;
    if (sceNetInetSetsockopt(udp_fd, SOL_SOCKET, SO_BROADCAST, &one,
                             sizeof(one)) < 0) {
        udp_close();
        return -1;
    }
    if (sceNetInetSetsockopt(udp_fd, SOL_SOCKET, SO_NONBLOCK, &one,
                             sizeof(one)) < 0) {
        udp_close();
        return -1;
    }
    return 0;
}

static void broadcast_discover(void)
{
    static const unsigned int bcast_ip[4] = {255, 255, 255, 255};
    struct sockaddr_in a;
    const char *msg = DISCOVER_MSG;

    if (udp_fd < 0)
        return;
    set_addr(&a, bcast_ip, DISCOVERY_PORT);
    sceNetInetSendto(udp_fd, msg, strlen(msg), 0,
                     (struct sockaddr *)&a, sizeof(a));
}

/* Пробует принять ответ сервера. 1 = адрес получен. */
static int discovery_poll(void)
{
    char buf[128];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n;
    char ipstr[20];
    int a, b, c, d, port;

    for (;;) {
        n = sceNetInetRecvfrom(udp_fd, buf, sizeof(buf) - 1, 0,
                               (struct sockaddr *)&from, &fromlen);
        if (n <= 0)
            return 0;
        buf[n] = 0;
        if (sscanf(buf, SERVER_REPLY " %15[0-9.] %d", ipstr, &port) == 2 &&
            sscanf(ipstr, "%d.%d.%d.%d", &a, &b, &c, &d) == 4 &&
            a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
            c >= 0 && c <= 255 && d >= 0 && d <= 255 &&
            port > 0 && port < 65536) {
            pc_ip_bytes[0] = (unsigned int)a;
            pc_ip_bytes[1] = (unsigned int)b;
            pc_ip_bytes[2] = (unsigned int)c;
            pc_ip_bytes[3] = (unsigned int)d;
            pc_port = port;
            return 1;
        }
    }
}

/* ---------- TCP-сессия ---------- */

static void tcp_drop(void)
{
    if (tcp_fd >= 0) {
        sceNetInetClose(tcp_fd);
        tcp_fd = -1;
    }
    rxlen = 0;
    state = wifi_connected() ? NET_SEARCH : NET_DIALOG;
    if (state == NET_SEARCH && udp_open() < 0)
        state = NET_DIALOG;
    /* если ушли в NET_DIALOG — ждём SELECT, сами не открываем */
}

static int tcp_connect(void)
{
    struct sockaddr_in a;
    int one = 1;
    static const char hello[] = "HELLO PSPDECK 1\n";

    tcp_fd = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0)
        return -1;

    set_addr(&a, pc_ip_bytes, pc_port);
    if (sceNetInetConnect(tcp_fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        sceNetInetClose(tcp_fd);
        tcp_fd = -1;
        return -1;
    }

    /* неблокирующий режим на всю дальнейшую жизнь сокета */
    sceNetInetSetsockopt(tcp_fd, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));

    rxlen = 0;
    deck->page_count = 0;
    parse_page = -1;
    last_pong_ms = now_ms();
    last_ping_ms = last_pong_ms;

    if (sceNetInetSend(tcp_fd, hello, sizeof(hello) - 1, 0) < 0) {
        tcp_drop();
        return -1;
    }
    return 0;
}

static int tcp_send_line(const char *line)
{
    int len = (int)strlen(line);

    if (tcp_fd < 0)
        return -1;
    if (sceNetInetSend(tcp_fd, line, len, 0) != len)
        return -1;
    return 0;
}

int net_send_press(int page, int button)
{
    char buf[48];

    if (state != NET_LINKED || tcp_fd < 0)
        return -1;
    snprintf(buf, sizeof(buf), "PRESS %d %d\n", page, button);
    return tcp_send_line(buf);
}

/* ---------- разбор строк протокола ---------- */

static void hex_to_color(const char *hex, unsigned int *out)
{
    unsigned int v = 0;
    int i;

    for (i = 0; i < 6 && hex[i]; i++) {
        char c = hex[i];
        int d;

        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            d = 0;
        v = (v << 4) | (unsigned int)d;
    }
    *out = v & 0xFFFFFF;
}

static void handle_line(const char *line)
{
    if (strncmp(line, "LAYOUT ", 7) == 0) {
        int n = atoi(line + 7);

        if (n < 0)
            n = 0;
        if (n > DECK_MAX_PAGES)
            n = DECK_MAX_PAGES;
        deck->page_count = n;
        parse_page = -1;
    } else if (strncmp(line, "PAGE ", 5) == 0) {
        if (deck->page_count > 0 && parse_page + 1 < deck->page_count) {
            parse_page++;
            snprintf(deck->pages[parse_page].name, DECK_NAME_MAX, "%.31s",
                     line + 5);
            parse_btn = 0;
        }
    } else if (strncmp(line, "BTN ", 4) == 0) {
        char color[8];
        const char *label;
        DeckButton *b;

        if (parse_page < 0 || parse_page >= deck->page_count ||
            parse_btn >= DECK_PAGE_BUTTONS)
            return;

        b = &deck->pages[parse_page].buttons[parse_btn];
        memset(color, 0, sizeof(color));
        sscanf(line + 4, "%7s", color);
        hex_to_color(color, &b->color);

        label = line + 4 + strlen(color);
        while (*label == ' ')
            label++;

        if (label[0] == 0 || strcmp(label, "-") == 0) {
            b->used = 0;
            b->label[0] = 0;
        } else {
            snprintf(b->label, DECK_LABEL_MAX, "%.63s", label);
            b->used = 1;
        }
        parse_btn++;
    } else if (strcmp(line, "PONG") == 0) {
        last_pong_ms = now_ms();
    }
    /* "OK" и прочее молча игнорируем */
}

static void tcp_poll(void)
{
    char buf[256];
    int n;

    for (;;) {
        n = sceNetInetRecv(tcp_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            int i, start = 0;

            for (i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    int len = i - start;

                    if (len > 0 && rxlen + len + 1 < (int)sizeof(rxbuf)) {
                        memcpy(rxbuf + rxlen, buf + start, len);
                        rxbuf[rxlen + len] = 0;
                        handle_line(rxbuf);
                    }
                    rxlen = 0;
                    start = i + 1;
                }
            }
            if (start < n) {
                if (rxlen + (n - start) < (int)sizeof(rxbuf)) {
                    memcpy(rxbuf + rxlen, buf + start, n - start);
                    rxlen += n - start;
                }
            }
            continue;
        }
        if (n == 0) {
            tcp_drop(); /* сервер закрыл соединение */
            return;
        }
        if (sceNetInetGetErrno() != EAGAIN)
            tcp_drop();
        return;
    }
}

/* ---------- системный диалог Wi-Fi ---------- */

static void netconf_begin(void)
{
    memset(&netconf, 0, sizeof(netconf));
    netconf.base.size = (unsigned int)sizeof(netconf);
    netconf.base.language = 1;            /* английский */
    netconf.base.buttonSwap = 1;          /* X = подтверждение */
    netconf.base.graphicsThread = 0x11;
    netconf.base.accessThread = 0x13;
    netconf.base.fontThread = 0x12;
    netconf.base.soundThread = 0x10;
    netconf.action = PSP_NETCONF_ACTION_CONNECTAP;
    netconf.hotspot = 0;
    netconf.wifisp = 0;

    if (sceUtilityNetconfInitStart(&netconf) < 0) {
        /* диалог не стартовал: экран остаётся наш, ждём SELECT */
        netconf_started = 0;
        ui_set_vram(0);
        return;
    }

    ui_set_vram(1); /* экран отдаем системному диалогу */
    netconf_started = 1;
}

static void netconf_finish(void)
{
    ui_set_vram(0);
    netconf_started = 0;

    if (wifi_connected()) {
        state = NET_SEARCH;
        last_broadcast_ms = 0;
    }
    /* нет сети — на главный экран, диалог откроют кнопкой SELECT */
}

static void netconf_tick(void)
{
    int st = sceUtilityNetconfGetStatus();

    switch (st) {
    case PSP_UTILITY_DIALOG_INIT:
        break;
    case PSP_UTILITY_DIALOG_VISIBLE:
        sceUtilityNetconfUpdate(1);
        break;
    case PSP_UTILITY_DIALOG_QUIT:
        sceUtilityNetconfShutdownStart();
        break;
    case PSP_UTILITY_DIALOG_FINISHED:
        netconf_finish();
        break;
    case PSP_UTILITY_DIALOG_NONE:
    default:
        /* диалог не инициализировался — выходим, не висим чёрным экраном */
        if (netconf_started)
            netconf_finish();
        break;
    }
}

/* ---------- публичный интерфейс ---------- */

int net_start(Deck *d)
{
    deck = d;
    state = NET_WLAN_OFF;

    if (sceUtilityLoadModule(PSP_MODULE_NET_COMMON) < 0)
        return -1;
    if (sceUtilityLoadModule(PSP_MODULE_NET_INET) < 0)
        return -1;
    if (sceNetInit(0x20000, 42, 0, 42, 0) < 0)
        return -1;
    if (sceNetInetInit() < 0)
        return -1;
    if (sceNetApctlInit(0x8000, 0x30) < 0)
        return -1;
    if (sceNetResolverInit() < 0)
        return -1;

    /* если Wi-Fi уже подключён (например, запуск из браузера) — идем сразу в поиск */
    state = wifi_connected() ? NET_SEARCH : NET_WLAN_OFF;
    if (state == NET_SEARCH && udp_open() < 0)
        state = NET_WLAN_OFF;
    return 0;
}

NetState net_state(void)
{
    return state;
}

int net_dialog_active(void)
{
    return state == NET_DIALOG && netconf_started;
}

void net_open_dialog(void)
{
    if (state == NET_DIALOG && !netconf_started)
        pending_dialog = 1;
}

int net_linked(void)
{
    return state == NET_LINKED && tcp_fd >= 0;
}

void net_tick(void)
{
    u32 now = now_ms();

    switch (state) {
    case NET_WLAN_OFF:
        if (sceWlanGetSwitchState() > 0) {
            state = NET_DIALOG;
            pending_dialog = 1; /* первый запуск — открываем сразу */
        }
        break;

    case NET_INIT:
        state = NET_DIALOG;
        pending_dialog = 1;
        break;

    case NET_DIALOG:
        if (netconf_started) {
            netconf_tick();
        } else if (pending_dialog) {
            pending_dialog = 0;
            netconf_begin();
        }
        /* иначе: ждём SELECT от пользователя, экран остаётся наш */
        break;

    case NET_SEARCH:
        if (!wifi_connected()) {
            state = NET_DIALOG;
            break;
        }
        if (udp_fd < 0 && udp_open() < 0)
            break;
        if (now - last_broadcast_ms > BROADCAST_PERIOD_MS) {
            last_broadcast_ms = now;
            broadcast_discover();
        }
        if (discovery_poll()) {
            udp_close();
            if (tcp_connect() < 0) {
                state = NET_SEARCH;
                udp_open();
            } else {
                state = NET_LINKED;
            }
        }
        break;

    case NET_CONNECT:
        /* переходное: подключение выполняется синхронно в NET_SEARCH */
        state = NET_SEARCH;
        break;

    case NET_LINKED:
        if (tcp_fd < 0 || !wifi_connected()) {
            tcp_drop();
            break;
        }
        tcp_poll();
        if (state != NET_LINKED)
            break;
        if (now - last_ping_ms > PING_PERIOD_MS) {
            last_ping_ms = now;
            tcp_send_line("PING\n");
        }
        if (now - last_pong_ms > PONG_TIMEOUT_MS)
            tcp_drop(); /* сервер молчит — связь потеряна */
        break;

    default:
        break;
    }
}

void net_shutdown(void)
{
    if (tcp_fd >= 0) {
        sceNetInetClose(tcp_fd);
        tcp_fd = -1;
    }
    udp_close();
}

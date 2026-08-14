#!/usr/bin/env python3
"""PSP Deck — сервер, исполняющий команды с PSP.

Запуск:  python pspdeck_server.py
Настройка кнопок: config.json рядом с этим файлом.

Что делает:
  1. Отвечает на broadcast-запрос обнаружения от PSP (UDP 50000).
  2. Принимает TCP-сессию (порт 50001), отправляет раскладку кнопок.
  3. По нажатию кнопки на PSP выполняет действие из config.json:
     run / hotkey / text / url.
"""

import json
import os
import socket
import subprocess
import sys
import threading
import time
import webbrowser

UDP_PORT = 50000
TCP_PORT = 50001
DISCOVER_MSG = "PSPDECK_DISCOVER"
MAX_PAGES = 8
BUTTONS_PER_PAGE = 8

CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "config.json")

CONFIG = {"pages": []}

try:
    from pynput import keyboard as _pynput_keyboard

    KB = _pynput_keyboard.Controller()
except ImportError:
    KB = None

# имена клавиш в конфиге -> атрибуты pynput.keyboard.Key
SPECIAL_KEYS = {
    "ctrl": "ctrl", "control": "ctrl", "alt": "alt", "altgr": "alt_gr",
    "shift": "shift", "win": "cmd", "super": "cmd", "cmd": "cmd",
    "meta": "cmd", "enter": "enter", "return": "enter", "tab": "tab",
    "space": "space", "backspace": "backspace", "delete": "delete",
    "esc": "esc", "escape": "esc", "up": "up", "down": "down",
    "left": "left", "right": "right", "printscreen": "print_screen",
    "prtsc": "print_screen", "play": "media_play_pause",
    "pause": "media_play_pause", "playpause": "media_play_pause",
    "next": "media_next", "prev": "media_previous",
    "volumeup": "media_volume_up", "volup": "media_volume_up",
    "volumedown": "media_volume_down", "voldown": "media_volume_down",
    "mute": "media_volume_mute", "capslock": "caps_lock", "home": "home",
    "end": "end", "pageup": "page_up", "pagedown": "page_down",
    "insert": "insert",
}


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def key_from_name(name):
    from pynput import keyboard as pk

    n = name.lower()
    if n in SPECIAL_KEYS:
        return getattr(pk.Key, SPECIAL_KEYS[n])
    if len(n) == 1:
        return pk.KeyCode.from_char(n)
    if n.startswith("f") and n[1:].isdigit():
        key = getattr(pk.Key, "f" + n[1:], None)
        if key is not None:
            return key
    raise ValueError(f"неизвестная клавиша: {name}")


def do_action(action):
    if not action:
        return
    kind = action.get("type")

    try:
        if kind == "run":
            cmd = action.get("cmd", "")
            subprocess.Popen(cmd, shell=True)
            log(f"    -> запуск: {cmd}")

        elif kind == "hotkey":
            if KB is None:
                log("    -> pynput не установлен, hotkey пропущен")
                return
            keys = [key_from_name(k.strip())
                    for k in action.get("keys", "").split("+") if k.strip()]
            for k in keys:
                KB.press(k)
            for k in reversed(keys):
                KB.release(k)
            log(f"    -> хоткей: {action.get('keys')}")

        elif kind == "text":
            if KB is None:
                log("    -> pynput не установлен, text пропущен")
                return
            KB.type(action.get("text", ""))
            log(f"    -> ввод текста ({len(action.get('text', ''))} симв.)")

        elif kind == "url":
            webbrowser.open(action.get("url", ""))
            log(f"    -> открыть: {action.get('url')}")

        else:
            log(f"    -> неизвестный тип действия: {kind}")
    except Exception as exc:  # любая ошибка не должна ронять сервер
        log(f"    -> ошибка выполнения: {exc}")


def load_config():
    global CONFIG
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            CONFIG = json.load(f)
        pages = CONFIG.get("pages", [])[:MAX_PAGES]
        log(f"Конфигурация: {len(pages)} стр., кнопки: "
            + ", ".join(str(len(p.get('buttons', []))) for p in pages))
    except Exception as exc:
        log(f"ОШИБКА чтения config.json ({exc}) — использую пустую раскладку")
        CONFIG = {"pages": []}


def layout_wire():
    """Раскладка в текстовый протокол PSP."""
    lines = []
    pages = CONFIG.get("pages", [])[:MAX_PAGES]
    lines.append(f"LAYOUT {len(pages)}")
    for page in pages:
        name = str(page.get("name", "")).replace("\n", " ")[:31]
        lines.append(f"PAGE {name}")
        buttons = page.get("buttons", [])
        for i in range(BUTTONS_PER_PAGE):
            if i < len(buttons) and buttons[i]:
                b = buttons[i]
                label = str(b.get("label", "")).replace("\n", " ").strip()
                color = str(b.get("color", "#3a3f4a")).lstrip("#")
                if len(color) != 6 or any(
                        c not in "0123456789abcdefABCDEF" for c in color):
                    color = "3a3f4a"
                lines.append(f"BTN {color} {label if label else '-'}")
            else:
                lines.append("BTN 262a34 -")
    return ("\n".join(lines) + "\n").encode("utf-8")


def local_ip_for(peer_ip):
    """Свой IP в сети, через которую виден peer (без реального трафика)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((peer_ip, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def udp_discovery():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("", UDP_PORT))
    log(f"Discovery-служба на UDP {UDP_PORT} запущена")
    while True:
        try:
            data, addr = s.recvfrom(1024)
            if data.decode("utf-8", "ignore").strip() == DISCOVER_MSG:
                ip = local_ip_for(addr[0])
                reply = f"PSPDECK_SERVER {ip} {TCP_PORT}"
                s.sendto(reply.encode("utf-8"), addr)
                log(f"PSP найден: {addr[0]} -> отвечаю {ip}:{TCP_PORT}")
        except OSError as exc:
            log(f"UDP ошибка: {exc}")


def handle_client(conn, addr):
    log(f"Подключение: {addr[0]}:{addr[1]}")
    load_config()  # перечитываем конфиг на каждое новое подключение
    buf = b""
    try:
        conn.settimeout(15)
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                msg = raw.decode("utf-8", "ignore").strip()
                if not msg:
                    continue

                if msg.startswith("HELLO"):
                    conn.sendall(layout_wire())
                    log("Раскладка отправлена")
                elif msg.startswith("PRESS"):
                    parts = msg.split()
                    if len(parts) == 3:
                        try:
                            page, index = int(parts[1]), int(parts[2])
                            label = "?"
                            action = button_action_page(page, index)
                            if 0 <= page < len(CONFIG.get("pages", [])):
                                buttons = CONFIG["pages"][page].get("buttons", [])
                                if 0 <= index < len(buttons) and buttons[index]:
                                    label = buttons[index].get("label", "?")
                            log(f"НАЖАТИЕ: стр. {page + 1}, кнопка {index + 1} [{label}]")
                            do_action(action)
                            conn.sendall(b"OK\n")
                        except ValueError:
                            pass
                elif msg == "PING":
                    conn.sendall(b"PONG\n")
    except socket.timeout:
        log(f"{addr[0]}: таймаут (нет ping) — отключаю")
    except ConnectionError:
        pass
    finally:
        conn.close()
        log(f"Отключение: {addr[0]}")


def button_action_page(page, index):
    if 0 <= page < len(CONFIG.get("pages", [])):
        buttons = CONFIG["pages"][page].get("buttons", [])
        if 0 <= index < len(buttons) and buttons[index]:
            return buttons[index].get("action")
    return None


def main():
    try:  # корректная кириллица в консоли Windows
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    print("=" * 56)
    print("  PSP Deck — сервер")
    print("  Кнопки настраиваются в config.json (рядом со скриптом)")
    print("=" * 56)

    if KB is None:
        log("ВНИМАНИЕ: pynput не установлен -> типы действий hotkey/text")
        log("не работают. Установите: pip install pynput")

    load_config()

    threading.Thread(target=udp_discovery, daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", TCP_PORT))
    srv.listen(2)
    log(f"TCP-сервер на порту {TCP_PORT} запущен, жду PSP...")
    log("Окно можно свернуть. Остановка: Ctrl+C")

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_client, args=(conn, addr),
                         daemon=True).start()


if __name__ == "__main__":
    main()

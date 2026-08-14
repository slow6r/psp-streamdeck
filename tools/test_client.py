#!/usr/bin/env python3
"""Имитатор клиента PSP для локальной проверки сервера.

Полный цикл: discovery (UDP) -> TCP HELLO -> раскладка -> PRESS -> PING.
Запуск: python3 tools/test_client.py [ip_сервера]
Без аргумента используется 127.0.0.1.
"""
import socket
import sys
import time

SERVER = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
UDP_PORT = 50000


def recv_line(sock, timeout=3.0):
    sock.settimeout(timeout)
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(1024)
        if not chunk:
            raise ConnectionError("соединение закрыто")
        buf += chunk
    return buf.decode("utf-8").strip()


def main():
    ok = True

    # --- 1. discovery ---
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(2)
    udp.sendto(b"PSPDECK_DISCOVER", (SERVER, UDP_PORT))
    try:
        data, _ = udp.recvfrom(1024)
        reply = data.decode()
        print(f"[1] discovery OK: {reply!r}")
        parts = reply.split()
        assert parts[0] == "PSPDECK_SERVER", "неверный префикс ответа"
        ip, port = parts[1], int(parts[2])
    except Exception as exc:
        print(f"[1] discovery FAIL: {exc}")
        return 1
    finally:
        udp.close()

    # --- 2. TCP + HELLO + раскладка ---
    tcp = socket.create_connection((ip, port), timeout=5)
    try:
        tcp.sendall(b"HELLO PSPDECK 1\n")
        layout = recv_line(tcp, timeout=3)
        # раскладка приходит несколькими строками — дочитываем
        tcp.settimeout(0.5)
        try:
            while True:
                more = tcp.recv(4096)
                if not more:
                    break
                layout += "\n" + more.decode("utf-8")
        except socket.timeout:
            pass
        lines = layout.strip().split("\n")
        print(f"[2] раскладка OK: {len(lines)} строк")
        print("    " + "\n    ".join(lines[:6]) + ("\n    ..." if len(lines) > 6 else ""))
        assert lines[0].startswith("LAYOUT "), "нет строки LAYOUT"
        assert any(l.startswith("BTN ") for l in lines), "нет строк BTN"

        # --- 3. нажатие ---
        tcp.sendall(b"PRESS 0 0\n")
        resp = recv_line(tcp, timeout=3)
        print(f"[3] press OK: сервер ответил {resp!r}")
        assert resp == "OK"

        # --- 4. ping ---
        tcp.sendall(b"PING\n")
        resp = recv_line(tcp, timeout=3)
        print(f"[4] ping OK: сервер ответил {resp!r}")
        assert resp == "PONG"

        time.sleep(0.3)
    finally:
        tcp.close()

    print("\nВСЕ ПРОВЕРКИ ПРОЙДЕНЫ — протокол работает.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

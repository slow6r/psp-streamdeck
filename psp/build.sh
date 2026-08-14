#!/bin/sh
# Сборка EBOOT.PBP в контейнере pspdev/pspdev (требуется запущенный Docker).
# Использование: ./build.sh          — сборка
#                 ./build.sh clean    — очистка
set -e
cd "$(dirname "$0")"
IMAGE="pspdev/pspdev:latest"
exec docker run --rm -v "$PWD":/src -w /src "$IMAGE" make "$@"

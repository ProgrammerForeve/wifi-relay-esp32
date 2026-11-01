#!/usr/bin/env bash
#
# Сборка, прошивка и запуск монитора ESP32.
#
# Использование:
#   ./flash.sh                  # автоопределение последовательного порта
#   ./flash.sh /dev/ttyUSB0     # явный порт
#   ESPPORT=/dev/ttyUSB0 ./flash.sh
#
# Переменные окружения:
#   IDF_PATH     — путь к ESP-IDF (иначе ищется в ~/esp и ~/.espressif)
#   ESPPORT      — последовательный порт
#   FLASH_BAUD   — скорость прошивки, по умолчанию 460800
#   MONITOR_BAUD — скорость монитора, по умолчанию 115200

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

usage() {
    sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
esac

find_idf() {
    if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
        printf '%s\n' "$IDF_PATH"
        return 0
    fi

    for candidate in "$HOME/esp/esp-idf" "$HOME/.espressif"/*/esp-idf; do
        if [ -f "$candidate/export.sh" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

# Подхватываем окружение ESP-IDF, если idf.py ещё не доступен.
if ! command -v idf.py >/dev/null 2>&1; then
    IDF_DIR="$(find_idf || true)"
    if [ -z "$IDF_DIR" ]; then
        echo "Ошибка: ESP-IDF не найден. Укажите IDF_PATH или установите ESP-IDF." >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    . "$IDF_DIR/export.sh" >/dev/null
fi

PORT="${1:-${ESPPORT:-}}"
if [ -z "$PORT" ]; then
    PORT="$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n 1 || true)"
fi
if [ -z "$PORT" ]; then
    echo "Ошибка: последовательный порт не найден." >&2
    echo "Подключите плату или укажите порт: ./flash.sh /dev/ttyUSB0" >&2
    exit 1
fi

# Первый запуск: фиксируем целевой чип.
if [ ! -f sdkconfig ]; then
    echo "sdkconfig не найден, выбираю target esp32..."
    idf.py set-target esp32
fi

FLASH_BAUD="${FLASH_BAUD:-${BAUD:-460800}}"
MONITOR_BAUD="${MONITOR_BAUD:-115200}"

# Скорости прошивки и монитора разные: консоль ESP32 по умолчанию работает
# на 115200, а прошивка может идти быстрее.
idf.py -p "$PORT" -b "$FLASH_BAUD" flash

if [ -t 0 ]; then
    echo "Открываю монитор на $MONITOR_BAUD бод (выход — Ctrl+])."
    idf.py -p "$PORT" -b "$MONITOR_BAUD" monitor
else
    # Без TTY монитор не запустится: прошили и выходим.
    echo "stdin не терминал: монитор не запускаю."
fi

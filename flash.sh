#!/usr/bin/env bash
# Compila y flashea el inyector. Uso: ./flash.sh [puerto]
set -e
PORT="${1:-${ESPPORT:-/dev/cu.usbserial-110}}"
. "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null 2>&1
cd "$(dirname "$0")"
idf.py -p "$PORT" flash
echo
echo "Flasheado. Monitor:  idf.py -p $PORT monitor"

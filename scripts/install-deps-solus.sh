#!/bin/sh
set -eu

# Skript za enostavno namestitev odvisnosti na Solus Linux
# Namesti runtime in razvojne pakete potrebne za build in zagon baRadio

if ! command -v eopkg >/dev/null 2>&1; then
    echo "Ta skripta je namenjena Solus Linuxu in zahteva 'eopkg'." >&2
    exit 1
fi

echo "Namestitev osnovnih runtime paketov (zahteva sudo)..."
sudo eopkg install -y libgtk-3 libayatana-appindicator sqlite3 gstreamer-1.0-plugins-base gstreamer-1.0-plugins-good gstreamer-1.0-plugins-bad || true

echo "Namestitev razvojnih paketov za prevajanje (zahteva sudo)..."
sudo eopkg install -y -c system.devel libgtk-3-devel libayatana-appindicator-devel sqlite3-devel gstreamer-1.0-devel gstreamer-1.0-plugins-base-devel || true

echo "Namestitev končana. Sedaj lahko prevedete program z 'make'."

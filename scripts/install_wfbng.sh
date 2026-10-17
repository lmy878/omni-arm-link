#!/usr/bin/env bash
#
# Install the patched RTL8812EU driver and WFB-NG on a Debian/Ubuntu host
# (Raspberry Pi OS or Ubuntu on Jetson).
#
# Usage:
#   sudo scripts/install_wfbng.sh [8812eu|8812au]
#
# This script is intended for a clean host. Review it before running on a
# production machine.

set -euo pipefail

CHIPSET="${1:-8812eu}"
DRIVER_DIR="/usr/src"

if [[ "${EUID}" -ne 0 ]]; then
    echo "error: run as root (sudo)" >&2
    exit 1
fi

echo "==> installing build dependencies"
apt-get update
apt-get install -y \
    dkms git build-essential libpcap-dev libsodium-dev libevent-dev \
    python3 python3-pip python3-all python3-all-dev python3-venv \
    python3-pyroute2 python3-twisted python3-serial python3-msgpack \
    python3-setuptools libgstrtspserver-1.0-dev iw socat

echo "==> building patched WiFi driver (${CHIPSET})"
cd "${DRIVER_DIR}"
if [[ "${CHIPSET}" == "8812au" ]]; then
    if [[ ! -d rtl8812au ]]; then
        git clone -b v5.2.20 https://github.com/svpcom/rtl8812au.git
    fi
    cd rtl8812au
    ./dkms-install.sh
else
    if [[ ! -d rtl8812eu ]]; then
        git clone -b v5.15.0.1 https://github.com/svpcom/rtl8812eu.git
    fi
    cd rtl8812eu
    ./dkms-install.sh
fi

echo "==> installing wfb-ng from source"
if [[ ! -d /opt/wfb-ng ]]; then
    git clone https://github.com/svpcom/wfb-ng.git /opt/wfb-ng
fi
cd /opt/wfb-ng
git pull --ff-only || true
make -j"$(nproc)"
make install

echo "==> done"
echo "Verify the driver with: ethtool -i <wlan>"
echo "Expected driver name: rtl88xxau_wfb (8812au) or rtl8812eu (8812eu)"
echo "Then run: sudo scripts/setup_radio.sh <role> <wlan>"

#!/usr/bin/env bash
#
# Prepare a host as controller, robot or passive monitor.
#
# Usage:
#   sudo scripts/setup_radio.sh <controller|robot|monitor> <wlan> [--start]
#
# What it does:
#   1. installs the matching WFB-NG profile as /etc/wifibroadcast.cfg
#   2. installs the encryption keys required by that role
#   3. verifies the WiFi driver exposes monitor-mode support
#   4. tunes the interface (txqueuelen) and sets the regulatory domain
#   5. optionally starts WFB-NG (wfb-server or systemd profile)

set -euo pipefail

role="${1:-}"
wlan="${2:-}"
start="${3:-}"

if [[ -z "${role}" || -z "${wlan}" ]]; then
    echo "Usage: $0 <controller|robot|monitor> <wlan> [--start]" >&2
    exit 1
fi

case "${role}" in
    controller|robot|monitor) ;;
    *) echo "error: unknown role '${role}'" >&2; exit 1 ;;
esac

if [[ "${EUID}" -ne 0 ]]; then
    echo "error: run as root (sudo)" >&2
    exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "${script_dir}")"
config_src="${repo_root}/config/wifibroadcast-${role}.cfg"
keys_src="${repo_root}/config/keys"

echo "==> installing /etc/wifibroadcast.cfg for role '${role}'"
if [[ ! -f "${config_src}" ]]; then
    echo "error: missing ${config_src}" >&2
    exit 1
fi
[[ -f /etc/wifibroadcast.cfg ]] && cp /etc/wifibroadcast.cfg "/etc/wifibroadcast.cfg.bak.$(date +%s)"
cp "${config_src}" /etc/wifibroadcast.cfg

echo "==> installing encryption keys"
for key in drone.key gs.key; do
    if [[ -f "${keys_src}/${key}" ]]; then
        install -m 600 "${keys_src}/${key}" "/etc/${key}"
    elif [[ ! -f "/etc/${key}" ]]; then
        echo "warning: ${key} not found in ${keys_src} or /etc" >&2
    fi
done

if [[ "${role}" == "monitor" ]]; then
    echo "==> monitor role requires BOTH drone.key and gs.key in /etc"
    [[ -f /etc/drone.key && -f /etc/gs.key ]] || echo "warning: monitor is missing a key" >&2
fi

echo "==> verifying driver / monitor mode support for ${wlan}"
if ! ip link show "${wlan}" >/dev/null 2>&1; then
    echo "error: interface ${wlan} not found" >&2
    exit 1
fi
driver="$(ethtool -i "${wlan}" 2>/dev/null | awk -F': ' '/driver:/{print $2}')"
echo "    driver: ${driver:-unknown}"
case "${driver}" in
    rtl88xxau_wfb|rtl8812eu|ath9k) ;;
    *) echo "    warning: driver may not support packet injection" >&2 ;;
esac

echo "==> tuning interface ${wlan}"
ip link set "${wlan}" up
ip link set "${wlan}" mtu 2312 || true
ip link set "${wlan}" txqueuelen 1000 || true
iw reg set BO || true

if [[ "${start}" == "--start" ]]; then
    echo "==> starting WFB-NG profile '${role}'"
    if [[ -x "${script_dir}/wfb_server.sh" ]]; then
        "${script_dir}/wfb_server.sh" "${role}" "${wlan}" &
        echo "    wfb-server started in background (pid $!)"
    elif command -v wfb-server >/dev/null 2>&1; then
        wfb-server "${role}" "${wlan}" &
        echo "    wfb-server started in background (pid $!)"
    elif systemctl list-unit-files | grep -q "wifibroadcast@"; then
        systemctl enable --now "wifibroadcast@${role}"
        echo "    systemctl status wifibroadcast@${role}"
    else
        echo "error: neither wfb-server nor wifibroadcast@ unit found" >&2
        exit 1
    fi
fi

echo "==> done for role '${role}' on ${wlan}"
echo "    link statistics:  wfb-cli ${role}"

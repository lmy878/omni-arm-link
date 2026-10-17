#!/usr/bin/env bash
#
# Launch wfb-server as root, including user-local pip installs in PATH/PYTHONPATH.
#
# Usage:
#   sudo scripts/wfb_server.sh <profile> <wlan> [extra wfb-server arguments...]
#
# This avoids "sudo: wfb-server: command not found" when wfb-ng was installed
# with pip --user (~/.local/bin) instead of system-wide.

set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "error: run with sudo: sudo $0 $*" >&2
    exit 1
fi

if [[ $# -lt 2 ]]; then
    echo "Usage: sudo $0 <profile> <wlan> [extra wfb-server arguments...]" >&2
    exit 1
fi

target_user="${SUDO_USER:-}"
target_home=""
if [[ -n "${target_user}" && "${target_user}" != "root" ]]; then
    target_home="$(getent passwd "${target_user}" | cut -d: -f6)"
fi

wfb_server=""
if command -v wfb-server >/dev/null 2>&1; then
    wfb_server="$(command -v wfb-server)"
fi
if [[ -z "${wfb_server}" && -n "${target_home}" && -x "${target_home}/.local/bin/wfb-server" ]]; then
    wfb_server="${target_home}/.local/bin/wfb-server"
fi
for candidate in /usr/local/bin/wfb-server /usr/bin/wfb-server; do
    if [[ -z "${wfb_server}" && -x "${candidate}" ]]; then
        wfb_server="${candidate}"
    fi
done

if [[ -z "${wfb_server}" ]]; then
    echo "error: wfb-server not found (install wfb-ng first)" >&2
    exit 1
fi

env_path="${PATH}"
env_pythonpath="${PYTHONPATH:-}"
if [[ -n "${target_home}" ]]; then
    env_path="${target_home}/.local/bin:${env_path}"
    for site in "${target_home}"/.local/lib/python*/site-packages; do
        [[ -d "${site}" ]] || continue
        env_pythonpath="${site}${env_pythonpath:+:${env_pythonpath}}"
    done
fi

# Newer wfb-server releases use --profiles/--wlans; older ones take positionals.
if env PATH="${env_path}" PYTHONPATH="${env_pythonpath}" "${wfb_server}" --help 2>&1 | grep -q -- '--profiles'; then
    exec env PATH="${env_path}" PYTHONPATH="${env_pythonpath}" \
        "${wfb_server}" --profiles "$1" --wlans "$2" "${@:3}"
fi
exec env PATH="${env_path}" PYTHONPATH="${env_pythonpath}" "${wfb_server}" "$@"

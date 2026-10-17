#!/usr/bin/env bash
#
# Run the robot telemetry application.
# All arguments are forwarded to oal_robot (CLI overrides the config file).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "${script_dir}")"

bin="${OAL_ROBOT_BIN:-${repo_root}/build/oal_robot}"
config="${OAL_CONFIG:-/etc/omni-arm-link/link.conf}"
[[ -f "${config}" ]] || config="${repo_root}/config/link.conf"

if [[ ! -x "${bin}" ]]; then
    echo "error: ${bin} not found, build with: cmake -S . -B build && cmake --build build" >&2
    exit 1
fi

exec "${bin}" --config "${config}" "$@"

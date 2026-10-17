#!/usr/bin/env bash
#
# Generate the WFB-NG encryption keypair (drone.key + gs.key).
#
# Usage:
#   scripts/gen_keys.sh [output_dir] [password]
#
# If a password is provided the keypair is derived deterministically from it,
# which makes key rotation across a fleet easy. Otherwise a random keypair is
# generated.
#
# Key placement:
#   drone.key  -> robot         (and the monitor host)
#   gs.key     -> controller    (and the monitor host)
#
# The passive monitor needs BOTH keys to decode downlink and uplink traffic.

set -euo pipefail

OUT_DIR="${1:-./config/keys}"
PASSWORD="${2:-}"

if ! command -v wfb_keygen >/dev/null 2>&1; then
    echo "error: wfb_keygen not found, install wfb-ng first" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

if [[ -n "${PASSWORD}" ]]; then
    (cd "${OUT_DIR}" && wfb_keygen "${PASSWORD}")
else
    (cd "${OUT_DIR}" && wfb_keygen)
fi

chmod 600 "${OUT_DIR}"/drone.key "${OUT_DIR}"/gs.key
echo "keypair written to ${OUT_DIR} (drone.key, gs.key)"

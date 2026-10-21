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
#
# IMPORTANT: drone.key and gs.key are one matched pair. Generate them ONCE on a
# single host and distribute the same files everywhere. Running this script on
# each host generates a new random pair each time, so the nodes will not be able
# to decrypt each other. If separate generation is unavoidable, pass the same
# password on every host.

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
if command -v sha256sum >/dev/null 2>&1; then
    echo "fingerprints (must match on every host):"
    sha256sum "${OUT_DIR}"/drone.key "${OUT_DIR}"/gs.key
fi
echo "deploy the SAME drone.key to the robot and the SAME gs.key to the controller."
echo "do not run this script separately on each host: random keys will not match."

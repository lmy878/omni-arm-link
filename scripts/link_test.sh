#!/usr/bin/env bash
#
# Run the bidirectional link latency/loss test.
#
# Usage:
#   scripts/link_test.sh <controller|robot> [--rate 200] [--size 340] ...
#   scripts/link_test.sh report [--dir DIR]
#
# Run once on the controller and once on the robot (start the robot side first).
# Each run writes a report file to ${OAL_LINKTEST_REPORT_DIR:-/tmp/oal-linktest};
# print the combined report afterwards with:
#   scripts/link_test.sh report
#
# While the test runs, the local WFB-NG JSON stats API (api_port 8102 for the
# robot profile, 8103 for the controller profile) is sampled before and after
# so the report can show the FEC recovery rate. Set OAL_WFB_API_PORT=0 to
# disable this or OAL_WFB_API_PORT=<port> to override.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "${script_dir}")"

role="${1:-}"
case "${role}" in
    controller|robot) ;;
    report)
        shift
        exec "${script_dir}/link_report.sh" "$@"
        ;;
    *)
        echo "Usage: $0 <controller|robot> [options...]" >&2
        echo "       $0 report [--dir DIR]" >&2
        exit 1
        ;;
esac
shift

bin="${OAL_LINKTEST_BIN:-${repo_root}/build/oal_linktest}"
report_dir="${OAL_LINKTEST_REPORT_DIR:-/tmp/oal-linktest}"
mkdir -p "${report_dir}"

have_report=false
report_path=""
previous=""
for arg in "$@"; do
    case "${arg}" in
        --report) have_report=true ;;
        --report=*) have_report=true; report_path="${arg#*=}" ;;
    esac
    if [[ "${previous}" == "--report" ]]; then
        report_path="${arg}"
    fi
    previous="${arg}"
done
if [[ "${have_report}" == false ]]; then
    report_path="${report_dir}/${role}.report"
    set -- "$@" --report "${report_path}"
else
    [[ -n "${report_path}" ]] || { echo "error: --report requires a path" >&2; exit 1; }
    report_dir="$(dirname "${report_path}")"
    mkdir -p "${report_dir}"
fi
report_stem="${report_path%.report}"

api_port="${OAL_WFB_API_PORT:-}"
if [[ -z "${api_port}" ]]; then
    case "${role}" in
        robot) api_port=8102 ;;
        controller) api_port=8103 ;;
    esac
fi
service="${OAL_WFB_TEST_SERVICE:-}"
if [[ -z "${service}" ]]; then
    case "${role}" in
        robot) service=test_up ;;
        controller) service=test_down ;;
    esac
fi

fec_enabled=false
snapshot() {
    local phase="$1"
    local out="${report_stem}.fec_${phase}"
    rm -f "${out}"
    [[ "${api_port}" == "0" ]] && return 0
    command -v python3 >/dev/null 2>&1 || return 1
    python3 "${script_dir}/wfb_stats.py" \
        --host "${OAL_WFB_API_HOST:-127.0.0.1}" \
        --port "${api_port}" \
        --service "${service}" \
        --timeout "${OAL_WFB_STATS_TIMEOUT:-5}" \
        --out "${out}"
}

if snapshot start; then
    fec_enabled=true
else
    rm -f "${report_stem}.fec_start"
    echo "note: WFB-NG FEC stats unavailable on ${OAL_WFB_API_HOST:-127.0.0.1}:${api_port}" >&2
    echo "      (is wfb-server running for profile '${role}' with api_port enabled?); continuing without FEC metrics" >&2
fi

trap '' INT TERM
"${bin}" --role "${role}" "$@" &
child=$!
rc=0
wait "${child}" || rc=$?
trap - INT TERM

if [[ "${fec_enabled}" == true ]]; then
    if ! snapshot end; then
        rm -f "${report_stem}.fec_end"
        echo "note: failed to read WFB-NG FEC stats after the test" >&2
    fi
fi

exit "${rc}"

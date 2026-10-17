#!/usr/bin/env bash
#
# Print the combined report of a bidirectional oal_linktest run.
#
# Usage:
#   scripts/link_report.sh [--dir DIR]
#   scripts/link_report.sh ROBOT_REPORT CONTROLLER_REPORT
#
# By default the two report files are read from
# ${OAL_LINKTEST_REPORT_DIR:-/tmp/oal-linktest}/<role>.report, which is where
# scripts/link_test.sh writes them.
#
# Thresholds can be tuned with OAL_LINK_MAX_LOSS_PCT (default 5),
# OAL_LINK_MAX_P99_MS (default 100) and OAL_LINK_MIN_FEC_RECOVERY_PCT
# (default 0 = FEC recovery is reported but not enforced).

set -euo pipefail

report_dir="${OAL_LINKTEST_REPORT_DIR:-/tmp/oal-linktest}"
robot_file=""
controller_file=""

usage() {
    cat <<EOF
Usage: $0 [--dir DIR]
       $0 ROBOT_REPORT CONTROLLER_REPORT

Default report directory: ${report_dir}
EOF
}

case "${1:-}" in
    "" ) ;;
    -h|--help)
        usage
        exit 0
        ;;
    --dir)
        [[ $# -ge 2 ]] || { usage >&2; exit 2; }
        report_dir="$2"
        shift 2
        ;;
esac

if [[ $# -eq 0 ]]; then
    robot_file="${report_dir}/robot.report"
    controller_file="${report_dir}/controller.report"
elif [[ $# -eq 2 ]]; then
    robot_file="$1"
    controller_file="$2"
else
    usage >&2
    exit 2
fi

declare -A ROBOT=()
declare -A CONTROLLER=()
robot_ok=false
controller_ok=false

load_robot() {
    local key value
    while IFS='=' read -r key value; do
        [[ -z "${key}" || "${key}" == \#* ]] && continue
        ROBOT["${key}"]="${value}"
    done < "$1"
}

load_controller() {
    local key value
    while IFS='=' read -r key value; do
        [[ -z "${key}" || "${key}" == \#* ]] && continue
        CONTROLLER["${key}"]="${value}"
    done < "$1"
}

if [[ -f "${robot_file}" ]]; then
    load_robot "${robot_file}"
    robot_ok=true
fi
if [[ -f "${controller_file}" ]]; then
    load_controller "${controller_file}"
    controller_ok=true
fi

up_tx="${CONTROLLER[sent]:-0}"
up_rx="${ROBOT[received]:-0}"
down_tx="${ROBOT[sent]:-0}"
down_rx="${CONTROLLER[received]:-0}"

loss_pct() {
    awk -v tx="$1" -v rx="$2" 'BEGIN {
        if (tx <= 0) { print "n/a"; exit }
        loss = (tx - rx) * 100.0 / tx
        if (loss < 0) loss = 0
        printf "%.3f", loss
    }'
}

rate() {
    awk -v packets="$1" -v seconds="$2" 'BEGIN {
        if (seconds <= 0) { print "n/a"; exit }
        printf "%.1f", packets / seconds
    }'
}

duration_or_zero() {
    echo "${1:-0}"
}

format_number() {
    awk -v value="$1" 'BEGIN { printf "%g", value }'
}

field() {
    [[ -f "$1" ]] || { echo ""; return; }
    awk -F= -v key="$2" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' "$1"
}

counter_delta() {
    local start="$1" end="$2" key="$3" first last
    first="$(field "${start}" "${key}")"
    last="$(field "${end}" "${key}")"
    if [[ ! "${first}" =~ ^[0-9]+$ || ! "${last}" =~ ^[0-9]+$ ]]; then
        echo ""
        return
    fi
    if (( last < first )); then
        echo ""
        return
    fi
    echo $((last - first))
}

fec_rate() {
    awk -v recovered="$1" -v lost="$2" 'BEGIN {
        total = recovered + lost
        if (total <= 0) { print "n/a"; exit }
        printf "%.2f", recovered * 100.0 / total
    }'
}

robot_fec_start="${robot_file%.report}.fec_start"
robot_fec_end="${robot_file%.report}.fec_end"
controller_fec_start="${controller_file%.report}.fec_start"
controller_fec_end="${controller_file%.report}.fec_end"

up_fec_rec="$(counter_delta "${robot_fec_start}" "${robot_fec_end}" fec_rec)"
up_fec_lost="$(counter_delta "${robot_fec_start}" "${robot_fec_end}" lost)"
down_fec_rec="$(counter_delta "${controller_fec_start}" "${controller_fec_end}" fec_rec)"
down_fec_lost="$(counter_delta "${controller_fec_start}" "${controller_fec_end}" lost)"

up_fec_ok=false
down_fec_ok=false
[[ -n "${up_fec_rec}" && -n "${up_fec_lost}" ]] && up_fec_ok=true
[[ -n "${down_fec_rec}" && -n "${down_fec_lost}" ]] && down_fec_ok=true

up_fec_rate="n/a"
down_fec_rate="n/a"
if [[ ${up_fec_ok} == true ]]; then
    up_fec_rate="$(fec_rate "${up_fec_rec}" "${up_fec_lost}")"
fi
if [[ ${down_fec_ok} == true ]]; then
    down_fec_rate="$(fec_rate "${down_fec_rec}" "${down_fec_lost}")"
fi

echo "OAL bidirectional link test report"
echo "=================================="
printf 'robot report:       %s%s\n' "${robot_file}" \
    "$([[ ${robot_ok} == true ]] && echo "" || echo "  (missing)")"
printf 'controller report:  %s%s\n' "${controller_file}" \
    "$([[ ${controller_ok} == true ]] && echo "" || echo "  (missing)")"
if [[ ${controller_ok} == true ]]; then
    printf 'test:               %s Hz, %s B payload\n' \
        "$(format_number "${CONTROLLER[rate_hz]:-0}")" \
        "$(format_number "${CONTROLLER[size_bytes]:-0}")"
fi
printf 'duration:           robot %s s, controller %s s\n' \
    "${ROBOT[duration_s]:-n/a}" "${CONTROLLER[duration_s]:-n/a}"
echo

up_loss="$(loss_pct "${up_tx}" "${up_rx}")"
down_loss="$(loss_pct "${down_tx}" "${down_rx}")"
up_pps="$(rate "${up_rx}" "$(duration_or_zero "${ROBOT[duration_s]:-0}")")"
down_pps="$(rate "${down_rx}" "$(duration_or_zero "${CONTROLLER[duration_s]:-0}")")"

printf '%-20s %8s %8s %9s %8s %6s %8s %9s\n' \
    "Direction" "TX" "RX" "Loss %" "Lost" "Dup" "Reorder" "RX pps"
printf '%-20s %8s %8s %9s %8s %6s %8s %9s\n' \
    "controller->robot" "${up_tx}" "${up_rx}" "${up_loss}" \
    "${ROBOT[lost]:-0}" "${ROBOT[duplicates]:-0}" "${ROBOT[reordered]:-0}" "${up_pps}"
printf '%-20s %8s %8s %9s %8s %6s %8s %9s\n' \
    "robot->controller" "${down_tx}" "${down_rx}" "${down_loss}" \
    "${CONTROLLER[lost]:-0}" "${CONTROLLER[duplicates]:-0}" "${CONTROLLER[reordered]:-0}" "${down_pps}"
echo

rtt_samples="${CONTROLLER[rtt_samples]:-0}"
if awk -v n="${rtt_samples}" 'BEGIN { exit !(n > 0) }'; then
    printf 'Round-trip latency (controller->robot->controller):\n'
    printf '  samples=%s  min=%s ms  p50=%s ms  p95=%s ms  p99=%s ms  max=%s ms  jitter=%s ms\n' \
        "${rtt_samples}" "${CONTROLLER[rtt_min_ms]:-0}" "${CONTROLLER[rtt_p50_ms]:-0}" \
        "${CONTROLLER[rtt_p95_ms]:-0}" "${CONTROLLER[rtt_p99_ms]:-0}" \
        "${CONTROLLER[rtt_max_ms]:-0}" "${CONTROLLER[rtt_jitter_ms]:-0}"
else
    echo "Round-trip latency: no samples"
fi
echo

if [[ ${up_fec_ok} == true || ${down_fec_ok} == true ]]; then
    up_fec_k="$(field "${robot_fec_end}" fec_k)"
    up_fec_n="$(field "${robot_fec_end}" fec_n)"
    down_fec_k="$(field "${controller_fec_end}" fec_k)"
    down_fec_n="$(field "${controller_fec_end}" fec_n)"
    [[ -n "${up_fec_k}" && -n "${up_fec_n}" ]] || { up_fec_k="?"; up_fec_n="?"; }
    [[ -n "${down_fec_k}" && -n "${down_fec_n}" ]] || { down_fec_k="?"; down_fec_n="?"; }

    printf 'FEC recovery (WFB-NG rx counters during test):\n'
    printf '%-20s %7s %10s %8s %10s\n' "Direction" "K/N" "Recovered" "Lost" "Recovery %"
    printf '%-20s %7s %10s %8s %10s\n' "controller->robot" \
        "${up_fec_k}/${up_fec_n}" "${up_fec_rec:-n/a}" "${up_fec_lost:-n/a}" "${up_fec_rate}"
    printf '%-20s %7s %10s %8s %10s\n' "robot->controller" \
        "${down_fec_k}/${down_fec_n}" "${down_fec_rec:-n/a}" "${down_fec_lost:-n/a}" "${down_fec_rate}"
else
    echo "FEC recovery:      unavailable (check wfb-server api_port 8102/8103 and re-run the test)"
fi
echo

printf 'Scheduler missed:   robot=%s controller=%s\n' \
    "${ROBOT[scheduler_missed]:-n/a}" "${CONTROLLER[scheduler_missed]:-n/a}"

max_loss="${OAL_LINK_MAX_LOSS_PCT:-5}"
max_p99="${OAL_LINK_MAX_P99_MS:-100}"
min_fec="${OAL_LINK_MIN_FEC_RECOVERY_PCT:-0}"

fec_both=0
ufec_value=0
dfec_value=0
if [[ ${up_fec_ok} == true && ${down_fec_ok} == true ]]; then
    fec_both=1
    ufec_value="${up_fec_rate}"
    dfec_value="${down_fec_rate}"
fi

if [[ ${robot_ok} != true || ${controller_ok} != true ]]; then
    result="INCOMPLETE"
    reason="missing report file(s)"
else
    result="$(awk -v utx="${up_tx}" -v urx="${up_rx}" -v dtx="${down_tx}" -v drx="${down_rx}" \
        -v samples="${rtt_samples}" -v p99="${CONTROLLER[rtt_p99_ms]:-0}" \
        -v maxloss="${max_loss}" -v maxp99="${max_p99}" \
        -v fecok="${fec_both}" -v ufec="${ufec_value}" -v dfec="${dfec_value}" \
        -v minfec="${min_fec}" 'BEGIN {
            if (utx <= 0 || urx <= 0 || dtx <= 0 || drx <= 0) { print "FAIL no-traffic"; exit }
            uloss = (utx - urx) * 100.0 / utx
            dloss = (dtx - drx) * 100.0 / dtx
            if (uloss < 0) uloss = 0
            if (dloss < 0) dloss = 0
            if (uloss > maxloss || dloss > maxloss) { print "FAIL loss"; exit }
            if (samples <= 0) { print "FAIL no-rtt"; exit }
            if (p99 > maxp99) { print "FAIL latency"; exit }
            if (minfec > 0) {
                if (fecok < 1) { print "FAIL no-fec-data"; exit }
                if (ufec < minfec || dfec < minfec) { print "FAIL fec-recovery"; exit }
            }
            print "PASS"
        }')"
    reason="${result#* }"
    result="${result%% *}"
fi

if [[ "${result}" == "PASS" ]]; then
    if [[ "$(awk -v value="${min_fec}" 'BEGIN { print (value > 0) }')" == "1" ]]; then
        printf 'Result:             PASS (loss <= %s%%, p99 <= %s ms, FEC recovery >= %s%%)\n' \
            "${max_loss}" "${max_p99}" "${min_fec}"
    else
        printf 'Result:             PASS (loss <= %s%%, p99 <= %s ms)\n' "${max_loss}" "${max_p99}"
    fi
else
    printf 'Result:             %s (%s)\n' "${result}" "${reason}"
fi

[[ "${result}" == "PASS" ]]

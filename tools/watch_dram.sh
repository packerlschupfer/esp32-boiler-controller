#!/usr/bin/env bash
# Sample the boiler's DRAM figures into a CSV so the trend can be read later.
#
# WHY THIS EXISTS: heap_dram_min is the minimum free DRAM since boot - it only ever
# falls, and nothing on the device or in Home Assistant keeps a history of it. On
# 2026-09-18 it had fallen to 608 B after 8.6 h uptime with the health report reading
# "warning" and 8 memory errors recorded; a reboot reset it to 4424 B. Whether that is
# a slow consumer or just fragmentation under ordinary polling cannot be answered from
# a single reading, and every reflash destroys the evidence. This keeps it.
#
# Beware the Home Assistant label: HA shows heap_dram_min as "Min Block". It is the
# minimum FREE DRAM since boot, not a minimum block size - heap_dram_blk is the block
# figure, and it is the one MIN_DRAM_BLOCK_WARNING (2560) compares against.
#
# Usage:
#   tools/watch_dram.sh                 # append one sample
#   tools/watch_dram.sh --header        # print the CSV header (new file)
#   tools/watch_dram.sh --report        # summarise the log
#
# Install as a cron entry (every 15 min), from the project root:
#   (crontab -l 2>/dev/null; echo "*/15 * * * * cd $PWD && tools/watch_dram.sh >/dev/null 2>&1") | crontab -
# Remove with:  crontab -l | grep -v watch_dram.sh | crontab -
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
LOG="${DRAM_LOG:-tools/monitoring/dram_history.csv}"
CRED="credentials.ini"
BROKER="${MQTT_BROKER:-192.168.20.27}"
HEADER="iso_time,uptime_s,heap_dram,heap_dram_blk,heap_dram_min,heap_frag,stack_hwm,tasks"

if [[ "${1:-}" == "--header" ]]; then echo "$HEADER"; exit 0; fi

if [[ "${1:-}" == "--report" ]]; then
    [[ -f "$LOG" ]] || { echo "no log at $LOG yet"; exit 1; }
    # Reports the STEP-DOWNS, not just the minimum. The question this tool exists to answer
    # is "lazy one-off allocations that plateau, or a slow leak?", and that is visible in
    # whether heap_dram (current free, not the minimum) keeps stepping down after the first
    # half hour of boot churn. A reboot resets the baseline, so steps are only compared
    # within a run - uptime going backwards starts a new one.
    awk -F, 'NR>1 && $5 != "" {
        if (prev_up != "" && $2+0 < prev_up+0) { printf "  -- reboot --\n"; prev_dram="" }
        if (prev_dram != "" && $3+0 != prev_dram+0) {
            d = $3 - prev_dram
            printf "  %s  up=%-6s heap_dram %s -> %s (%+d)\n", substr($1,1,19), $2, prev_dram, $3, d
            if (d < 0) { lost += -d; steps++ }
        }
        n++; prev_dram=$3; prev_up=$2
        if (min == "" || $5+0 < min+0) { min = $5; mt = $1; mu = $2 }
        if ($4+0 < 2560) low++
        last = $0
    } END {
        if (!n) { print "no samples"; exit }
        printf "\nsamples: %d\n", n
        printf "lowest heap_dram_min: %s B  (at %s, uptime %ss)\n", min, mt, mu
        printf "heap_dram step-downs: %d, totalling %d B\n", steps+0, lost+0
        printf "samples with heap_dram_blk below MIN_DRAM_BLOCK_WARNING (2560): %d\n", low+0
        printf "latest: %s\n", last
        if (steps+0 > 0) printf "\nNOTE: a step-down AFTER the first ~30 min of boot churn is held memory,\nnot a transient - heap_dram is current free, not a minimum. Steps that stop\narriving = lazy allocation; steps that keep arriving = investigate.\n"
    }' "$LOG"
    exit 0
fi

# Credentials stay in credentials.ini (gitignored) and are never echoed.
MU=$(sed -nE 's/.*MQTT_USERNAME=\\?"([^"\\]*).*/\1/p' "$CRED" 2>/dev/null | head -1)
MP=$(sed -nE 's/.*MQTT_PASSWORD=\\?"([^"\\]*).*/\1/p' "$CRED" 2>/dev/null | head -1)
if [[ -z "$MU" || -z "$MP" ]]; then
    echo "watch_dram: could not read MQTT credentials from $CRED" >&2
    exit 1
fi

# boiler/status/health is published every MQTT_HEALTH_PUBLISH_INTERVAL_MS (60 s,
# SystemConstants.h) and is NOT retained, so the wait has to exceed one full interval
# or a sample lands in the gap and reports a false "device offline". 90 s gives margin
# for a missed publish under queue pressure.
PAYLOAD=$(timeout 90 mosquitto_sub -h "$BROKER" -u "$MU" -P "$MP" \
          -t 'boiler/status/health' -C 1 2>/dev/null)
if [[ -z "$PAYLOAD" ]]; then
    echo "watch_dram: no health payload within 90 s (device offline or broker unreachable)" >&2
    exit 1
fi

field() { printf '%s' "$PAYLOAD" | grep -oE "\"$1\":[0-9-]+" | head -1 | cut -d: -f2; }

mkdir -p "$(dirname "$LOG")"
[[ -f "$LOG" ]] || echo "$HEADER" > "$LOG"
printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(date -Iseconds)" "$(field uptime)" "$(field heap_dram)" "$(field heap_dram_blk)" \
    "$(field heap_dram_min)" "$(field heap_frag)" "$(field stack_hwm)" "$(field tasks)" \
    >> "$LOG"

#!/usr/bin/env bash

set -u

interval="${1:-2}"

find_ec_zone() {
  local zone

  for zone in /sys/class/thermal/thermal_zone*; do
    if [[ "$(<"$zone/type")" == "ec-temp1-thermal" ]]; then
      printf '%s\n' "$zone"
      return 0
    fi
  done

  return 1
}

zone="$(find_ec_zone)" || {
  echo "ec-temp1-thermal zone not found" >&2
  exit 1
}

trip="$(<"$zone/trip_point_0_temp")"

while :; do
  timestamp="$(date --iso-8601=seconds)"
  temperature="$(<"$zone/temp")"
  cooling=""

  for device in /sys/class/thermal/cooling_device*; do
    type="$(<"$device/type")"
    case "$type" in
      cpufreq-cpu0|cpufreq-cpu4)
        state="$(<"$device/cur_state")"
        max_state="$(<"$device/max_state")"
        cooling+=" ${type}=${state}/${max_state}"
        ;;
    esac
  done

  printf '%s temp_mC=%s trip_mC=%s%s\n' \
    "$timestamp" "$temperature" "$trip" "$cooling"
  sleep "$interval"
done

#!/bin/bash
for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq; do
  echo 2800000 > "$c"
done

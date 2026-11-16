#!/usr/bin/env bash
# Feed CPU/GPU temperature and cooler RPM from hwmon to the hid-valuetop driver.
#   valuetop-feed            run until stopped (systemd service)
#   INTERVAL=2 valuetop-feed update every 2 s (default 1)
#
# CPU temp: k10temp / coretemp.  GPU temp: amdgpu / nouveau / nvidia.
# Cooler RPM: first running fan on any other chip (usually the motherboard).
set -u
shopt -s nullglob
interval=${INTERVAL:-1}
drv=/sys/bus/hid/drivers/valuetop-display

is_cpu() { [[ $1 == k10temp || $1 == coretemp ]]; }
is_gpu() { [[ $1 == amdgpu || $1 == nouveau || $1 == nvidia ]]; }

# First readable value of $1 (a hwmon file name) among chips accepted by $2.
first_value() {
	local file=$1 want=$2 h name v
	for h in /sys/class/hwmon/hwmon*; do
		name=$(<"$h/name") || continue
		"$want" "$name" || continue
		for f in "$h"/$file; do
			read -r v 2>/dev/null <"$f" || continue
			[[ $v =~ ^[0-9]+$ && $v -gt 0 ]] && { echo "$v"; return 0; }
		done
	done
	return 1
}

not_cpu_or_gpu() { ! is_cpu "$1" && ! is_gpu "$1"; }

while :; do
	dev=$(compgen -G "$drv/0003:5131:2007.*" | head -n1)
	if [[ -z $dev ]]; then
		sleep 5
		continue
	fi

	t=$(first_value temp1_input is_cpu) && echo $((t / 1000)) >"$dev/cpu_temp"
	t=$(first_value temp1_input is_gpu) && echo $((t / 1000)) >"$dev/gpu_temp"
	r=$(first_value 'fan*_input' not_cpu_or_gpu) && echo "$r" >"$dev/cpu_fan_rpm"

	sleep "$interval"
done

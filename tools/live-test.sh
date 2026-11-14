#!/usr/bin/env bash
# Live check for hid-mk75 / hid-atk-vxe.  Run as root (needed to write the LED):
#   sudo tools/live-test.sh
# Checks every registered battery, that UPower sees it, and the keyboard
# backlight write path (change, read back, restore, then confirm on the device).
set -u
pass=0; fail=0
ok()  { echo "PASS  $*"; pass=$((pass+1)); }
bad() { echo "FAIL  $*"; fail=$((fail+1)); }

echo "=== batteries ==="
found=0
for p in /sys/class/power_supply/hid-*; do
	[ -e "$p/model_name" ] || continue
	m=$(cat $p/model_name)
	case $m in "Lingbao MK75"|"ATK VXE R1 NK") ;; *) continue;; esac
	found=1
	n=$(basename $p)
	cap=$(cat $p/capacity 2>&1); pres=$(cat $p/present); st=$(cat $p/status)
	echo "      $m: $cap% $st present=$pres"
	[ "$pres" = 1 ] && [[ $cap =~ ^[0-9]+$ ]] && [ "$cap" -le 100 ] \
		&& ok "$m battery readable" || bad "$m battery not present/readable (device asleep?)"
done
[ $found = 1 ] || bad "no MK75 / R1 NK battery registered"
echo "      UPower devices:"; upower -e | sed 's/^/        /'

echo "=== UPower keyboard backlight ==="
max=$(busctl --system call org.freedesktop.UPower /org/freedesktop/UPower/KbdBacklight org.freedesktop.UPower.KbdBacklight GetMaxBrightness 2>&1)
[ "$max" = "i 4" ] && ok "UPower KbdBacklight max brightness = 4" || bad "UPower KbdBacklight: $max"

echo "=== kbd_backlight write path ==="
[ "$(id -u)" = 0 ] || { echo "run as root for the LED test"; exit 1; }
led=$(ls -d /sys/class/leds/mk75::kbd_backlight* 2>/dev/null | head -1)
if [ -z "$led" ]; then bad "no mk75::kbd_backlight LED"; else
	read -rp "Keyboard awake (tap keys)? Enter... " _
	o=$(cat $led/brightness)
	t=$(( o == 1 ? 2 : 1 ))
	echo $t >$led/brightness 2>/tmp/lt_err && sleep 3 || bad "write $t failed: $(cat /tmp/lt_err)"
	[ "$(cat $led/brightness)" = "$t" ] && ok "$(basename $led): $o -> $t" || bad "readback != $t"
	echo 0 >$led/brightness && sleep 3 && ok "turned off (effect off)" || bad "off failed"
	echo $o >$led/brightness && sleep 3
	[ "$(cat $led/brightness)" = "$o" ] && ok "restored to $o" || bad "restore failed"
	echo "      waiting 35 s for the driver to re-read the device..."; sleep 35
	[ "$(cat $led/brightness)" = "$o" ] && ok "device-side brightness = $o" || bad "device-side brightness is $(cat $led/brightness), expected $o"
fi
echo; echo "RESULT: $pass passed, $fail failed"

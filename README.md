# hid-hwre: native Linux battery + backlight for two reverse-engineered devices

| Device | USB IDs | Module | Exposes |
|---|---|---|---|
| Lingbao MK75 keyboard (= AULA F75/AU75) | wired `258a:010c`, 2.4 GHz dongle `3554:fa09` | `hid-mk75` | battery, `kbd_backlight` |
| ATK VXE R1 NK mouse | wired `373b:124c`, NearLink dongle `373b:1216` | `hid-atk-vxe` | battery |

Protocols come from the `hw-re-docs` reverse-engineering notes. Bluetooth (MK75) is unsupported (protocol unsolved).

## Scope: only what existing software uses

* **Battery** -> `power_supply` (`/sys/class/power_supply/hid-*-battery`): UPower, GNOME, KDE.
  Capacity and charging/full/discharging state (mouse also voltage).
* **Keyboard backlight** -> `led_class` `mk75::kbd_backlight` (0-4): UPower's `KbdBacklight` D-Bus API.
  Fn-key brightness changes (wired) are reported back.

Nothing else is in the kernel on purpose: DPI, report rate, effects, colours, side light, keymaps, macros and
per-key RGB have no consumer in the kernel ecosystem. Use a userspace tool over hidraw (libratbag/Piper,
OpenRGB, VIA-style tools); the hidraw nodes stay available with these drivers bound.

Behaviours inherited from the hardware:
* MK75: a backlight write blanks the LEDs ~2 s; over the cable it returns `-EBUSY` while the keyboard is in
  2.4 GHz/BT mode; a sleeping keyboard's config is never written back.
* Shared USB IDs (`258a:010c`, `3554:fa09`, `373b:1216`): nothing is registered until the device answers like
  the documented one (MK75 config layout; R1 NK cid/mid 2/8). A sleeping device behind a dongle is retried.
* A dongle instance drops its battery and LED after ~2 min without an answer (asleep, or moved to the cable),
  so no stale duplicate `kbd_backlight` is left, and comes back on the next answer.
* UPower labels the mouse a "keyboard" under the dongle's USB name: it takes both from the USB parent.

## Tested live (kernel 6.18.63)

Both devices, via dongle and via cable: batteries show in UPower (charging, full and discharging seen),
`kbd_backlight` is picked up by UPower (max 4), and brightness writes read back and restore on the device.
Not exercised: long-idle behaviour, the Fn-key refresh and the `-EBUSY` guard (keyboard on cable in wireless mode).
Run `sudo tools/live-test.sh` to repeat the checks.

## Build / use (Nix)

```sh
nix build            # result/lib/modules/<ver>/updates/*.ko.xz (+ .cer signature digests, which NixOS needs)
nix develop          # shell with KDIR set;  cd src && make -C $KDIR M=$PWD modules
```

NixOS (flake input `hid-hwre`):

```nix
{ imports = [ inputs.hid-hwre.nixosModules.default ];
  hardware.hid-hwre.enable = true; }   # extraModulePackages + autoload + upower
```

Builds against `config.boot.kernelPackages`, so it follows your kernel. Manual test: `sudo insmod` **each**
`.ko` separately (a second path on one `insmod` line is parsed as a module parameter), then replug the device
so it rebinds from `hid-generic`. Do not hand-unbind interfaces: an unbound mouse interface means no cursor.

## ValueTop case display (separate, optional)

`src/valuetop/hid-valuetop.c` drives the ValueTop Tempest DF6 segmented LCD (`5131:2007`). It is **not** part
of the default package or `nixosModules.default`, so a host without the display (e.g. a laptop) never builds
or loads it.

| Piece | What it does |
|---|---|
| `hid-valuetop` | owns the device, sends the 64-byte report once a second, applies the RPM compensation for the display's firmware quirk |
| sysfs `cpu_temp`, `gpu_temp`, `cpu_fan_rpm` | the readings it displays (`/sys/bus/hid/devices/0003:5131:2007.*/`). The LCD has two temperature digits, so temperatures are shown modulo 100: 105 °C reads `05`, which stands out as abnormal |
| `tools/valuetop-feed.sh` | copies hwmon readings (k10temp/coretemp, amdgpu/nouveau/nvidia, first non-CPU/GPU fan) into those attributes |

If nothing has been written to the attributes for 5 seconds (feeder stopped or crashed) the driver sends all zeros, so a dead feeder can never leave frozen readings that look live; the next write brings the real values back. Which chip is "the CPU" or "the cooler" is policy, so it lives in the feeder, not the kernel. If no motherboard
fan is exposed by hwmon, RPM is left unset. Recent Gigabyte boards (ITE Super I/O) need the out-of-tree `it87`,
so the module also loads nixpkgs' `it87` with `ignore_resource_conflict=1` (`hardware.valuetop-display.it87.enable`,
default on; `it87.forceId = "0x8689"` if dmesg reports the chip as unsupported).

Only on a host that has the display:

```nix
{ imports = [ inputs.hid-hwre.nixosModules.valuetop-display ];
  hardware.valuetop-display.enable = true; }   # module + autoload + valuetop-feed service
```
Manual build: `cd src/valuetop && make -C $KDIR M=$PWD modules`. As with the other drivers, replug the display
(or reboot) so it rebinds from `hid-generic`.

## License

GPL-2.0-only (see `LICENSE`). Required for kernel modules that use GPL-only kernel interfaces.

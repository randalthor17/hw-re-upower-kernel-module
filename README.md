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
nix build            # result/lib/modules/<ver>/extra/*.ko
nix develop          # shell with KDIR set;  cd src && make
```

NixOS (flake input `hid-hwre`):

```nix
{ imports = [ inputs.hid-hwre.nixosModules.default ];
  hardware.hid-hwre.enable = true; }   # extraModulePackages + autoload + upower
```

Builds against `config.boot.kernelPackages`, so it follows your kernel. Manual test: `sudo insmod` **each**
`.ko` separately (a second path on one `insmod` line is parsed as a module parameter), then replug the device
so it rebinds from `hid-generic`. Do not hand-unbind interfaces: an unbound mouse interface means no cursor.

## License

GPL-2.0-only (see `LICENSE`). Required for kernel modules that use GPL-only kernel interfaces.

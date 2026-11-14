# NixOS module: boot.extraModulePackages + autoload + UPower.
self: { config, lib, pkgs, ... }:
let cfg = config.hardware.hid-hwre;
in {
  options.hardware.hid-hwre.enable =
    lib.mkEnableOption "kernel drivers for the Lingbao MK75 and ATK VXE R1 NK (battery in UPower, LEDs, settings)";

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [
      (config.boot.kernelPackages.callPackage ./package.nix { })
    ];
    boot.kernelModules = [ "hid-mk75" "hid-atk-vxe" ];
    services.upower.enable = lib.mkDefault true;
  };
}

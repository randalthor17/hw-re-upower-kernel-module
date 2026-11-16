# NixOS module for the ValueTop case display, separate from ./module.nix so a
# host without the display (a laptop) never builds or loads it:
#   imports = [ inputs.hid-hwre.nixosModules.valuetop-display ];
#   hardware.valuetop-display.enable = true;
self: { config, lib, pkgs, ... }:
let
  cfg = config.hardware.valuetop-display;
  feeder = self.packages.${pkgs.stdenv.hostPlatform.system}.valuetop-feed;
in {
  options.hardware.valuetop-display = {
    enable = lib.mkEnableOption "kernel driver and sensor feeder for the ValueTop Tempest DF6 case display";

    it87 = {
      enable = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = ''
          Also load the out-of-tree it87 hwmon driver (frankcrawford's fork).
          The display's cooler RPM comes from the motherboard's ITE Super I/O
          fan tachometer, which mainline kernels do not expose on recent
          Gigabyte boards.
        '';
      };
      forceId = lib.mkOption {
        type = lib.types.nullOr lib.types.str;
        default = null;
        example = "0x8689";
        description = ''
          Super I/O ID to force when the chip is not recognised by itself
          (passed as it87 `force_id=`). Leave null unless dmesg says the chip
          is unsupported.
        '';
      };
    };
  };

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [
      (config.boot.kernelPackages.callPackage ./valuetop-package.nix { })
    ] ++ lib.optional cfg.it87.enable config.boot.kernelPackages.it87;

    # loaded at boot (before the device is probed) so it wins over hid-generic
    boot.kernelModules = [ "hid-valuetop" ] ++ lib.optional cfg.it87.enable "it87";

    # Gigabyte's ACPI claims the Super I/O ports; ignore_resource_conflict is
    # the narrow alternative to the global acpi_enforce_resources=lax.
    boot.extraModprobeConfig = lib.mkIf cfg.it87.enable ''
      options it87 ignore_resource_conflict=1${
        lib.optionalString (cfg.it87.forceId != null) " force_id=${cfg.it87.forceId}"
      }
    '';

    systemd.services.valuetop-feed = {
      description = "Feed sensor readings to the ValueTop case display";
      wantedBy = [ "multi-user.target" ];
      # fan sensors appear once it87 has probed
      after = [ "systemd-modules-load.service" ];
      serviceConfig = {
        ExecStart = lib.getExe feeder;
        Restart = "on-failure";
        # sysfs attributes are root-writable, nothing else is needed
        ProtectHome = true;
        PrivateTmp = true;
        NoNewPrivileges = true;
      };
    };
  };
}

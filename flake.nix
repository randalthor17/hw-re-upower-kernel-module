{
  description = "Kernel drivers: Lingbao MK75 keyboard + ATK VXE R1 NK mouse (UPower, LEDs, sysfs)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      packages = forAll (pkgs: rec {
        # Built against the default kernel; for another one use
        #   config.boot.kernelPackages.callPackage ./nix/package.nix { }
        default = pkgs.linuxPackages.callPackage ./nix/package.nix { };
        hid-hwre = default;

        # The ValueTop case display is deliberately not part of `default`:
        # only hosts that have it import nixosModules.valuetop-display.
        hid-valuetop = pkgs.linuxPackages.callPackage ./nix/valuetop-package.nix { };
        valuetop-feed = pkgs.writeShellApplication {
          name = "valuetop-feed";
          runtimeInputs = [ pkgs.coreutils pkgs.gnugrep ];
          text = builtins.readFile ./tools/valuetop-feed.sh;
        };
      });

      nixosModules.default = import ./nix/module.nix self;
      nixosModules.valuetop-display = import ./nix/valuetop.nix self;

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = [ pkgs.bear ];
          KDIR = "${pkgs.linuxPackages.kernel.dev}/lib/modules/${pkgs.linuxPackages.kernel.modDirVersion}/build";
        };
      });
    };
}

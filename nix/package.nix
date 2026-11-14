{ lib, stdenv, kernel }:

stdenv.mkDerivation {
  pname = "hid-hwre";
  __structuredAttrs = true;
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ../src;
    fileset = lib.fileset.unions [ ../src/Makefile ../src/hid-mk75.c ../src/hid-atk-vxe.c ];
  };

  nativeBuildInputs = kernel.moduleBuildDependencies;
  makeFlags = (builtins.filter (f: !(lib.hasPrefix "O=" f)) kernel.makeFlags) ++ [ "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build" ];
  installPhase = ''
    runHook preInstall
    install -D -t $out/lib/modules/${kernel.modDirVersion}/extra hid-mk75.ko hid-atk-vxe.ko
    runHook postInstall
  '';

  meta = {
    description = "HID drivers: Lingbao MK75 keyboard and ATK VXE R1 NK mouse (battery, LEDs, settings)";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}

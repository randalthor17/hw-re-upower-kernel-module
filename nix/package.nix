{
  lib,
  stdenv,
  kernel,
  kernelModuleMakeFlags,
}:

stdenv.mkDerivation {
  pname = "hid-hwre";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ../src;
    fileset = lib.fileset.unions [
      ../src/Makefile
      ../src/hid-mk75.c
      ../src/hid-atk-vxe.c
    ];
  };

  nativeBuildInputs = kernel.moduleBuildDependencies;

  # The usual nixpkgs out-of-tree module recipe (see xpadneo, evdi).
  makeFlags = kernelModuleMakeFlags ++ [
    "-C"
    "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "M=$(PWD)"
  ];
  buildFlags = [ "modules" ];
  installFlags = [ "INSTALL_MOD_PATH=${placeholder "out"}" ];
  installTargets = [ "modules_install" ];

  meta = {
    description = "HID drivers: Lingbao MK75 keyboard and ATK VXE R1 NK mouse (battery, kbd backlight)";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}

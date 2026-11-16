{
  lib,
  stdenv,
  kernel,
  kernelModuleMakeFlags,
}:

stdenv.mkDerivation {
  pname = "hid-valuetop";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ../src/valuetop;
    fileset = lib.fileset.unions [
      ../src/valuetop/Makefile
      ../src/valuetop/hid-valuetop.c
    ];
  };

  nativeBuildInputs = kernel.moduleBuildDependencies;

  makeFlags = kernelModuleMakeFlags ++ [
    "-C"
    "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "M=$(PWD)"
  ];
  buildFlags = [ "modules" ];
  installFlags = [ "INSTALL_MOD_PATH=${placeholder "out"}" ];
  installTargets = [ "modules_install" ];

  meta = {
    description = "HID driver: ValueTop Tempest DF6 case display (CPU/GPU temperature, fan RPM)";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}

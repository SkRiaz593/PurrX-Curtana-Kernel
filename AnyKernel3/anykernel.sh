### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers
## PurrX — Xiaomi curtana / Redmi Note 9 Pro India
## A-only, boot header v2
## Full-stack flash: kernel + verified PurrX DTB + DTBO

### AnyKernel setup
# global properties
properties() { '
kernel.string=PurrX curtana r1 (KernelSU + SUSFS)
kernel.compiler=Clang/LLVM 18 (LLVM=1, no GCC)
kernel.made=SkRiaz593 / PurrX
kernel.version=4.14.357-openela
message.word=PurrX r1 compatibility build for Xiaomi curtana only.
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=curtana
supported.versions=
supported.patchlevels=
supported.vendorpatchlevels=
'; } # end properties


### AnyKernel install
## boot files attributes
boot_attributes() {
set_perm_recursive 0 0 755 644 $RAMDISK/*;
set_perm_recursive 0 0 750 750 $RAMDISK/init* $RAMDISK/sbin;
} # end attributes

# boot shell variables
BLOCK=/dev/block/bootdevice/by-name/boot;
IS_SLOT_DEVICE=0;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

# Import AnyKernel functions and initialize patching.
# Do not remove this line.
. tools/ak3-core.sh;

# Dump the existing boot image, install PurrX and repack it.
dump_boot;

# PurrX r1 includes the kernel-generated DTB and DTBO produced by the
# pinned curtana-compatible source tree. Device validation above prevents
# this package from being installed on other miatoll-family devices.

write_boot;
## end boot install

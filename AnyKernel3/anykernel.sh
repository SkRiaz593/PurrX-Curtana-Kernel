### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers
## PurrX — Xiaomi curtana / Redmi Note 9 Pro India
## A-only, boot header v2
## Full-stack flash: kernel + verified PurrX DTB + DTBO

### AnyKernel setup
# global properties
properties() { '
kernel.string=PurrX curtana Master (KernelSU + SUSFS)
kernel.compiler=Clang/LLVM 18 (LLVM=1, no GCC)
kernel.made=SkRiaz593 / PurrX
kernel.version=4.14.357-openela
message.word=PurrX Master build for Xiaomi curtana only.
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

# Write boot partition with kernel + curtana DTB + DTBO
write_boot;

# Install PurrX Master One-Shot KernelSU Boot Service
ui_print " ";
ui_print "Installing PurrX Master Performance & 4GB RAM Engine...";
mkdir -p /data/adb/service.d 2>/dev/null;
cat << 'EOF' > /data/adb/service.d/purrx-engine.sh
#!/system/bin/sh
# Runs once at boot completion, applies optimal kernel tunables, and exits cleanly (0 background overhead).
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 2
done

# 1. 4 GB RAM Optimization Engine (2 GiB LZ4 zRAM + MGLRU)
if [ -e /dev/block/zram0 ]; then
    swapoff /dev/block/zram0 2>/dev/null
    echo 1 > /sys/block/zram0/reset 2>/dev/null
    echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null
    echo 2147483648 > /sys/block/zram0/disksize 2>/dev/null
    mkswap /dev/block/zram0 2>/dev/null
    swapon /dev/block/zram0 -p 32767 2>/dev/null
fi

echo 160 > /proc/sys/vm/swappiness 2>/dev/null
echo 100 > /proc/sys/vm/vfs_cache_pressure 2>/dev/null
echo 0 > /proc/sys/vm/page-cluster 2>/dev/null
echo 10 > /proc/sys/vm/dirty_background_ratio 2>/dev/null
echo 20 > /proc/sys/vm/dirty_ratio 2>/dev/null

if [ -e /sys/kernel/mm/lru_gen/enabled ]; then
    echo y > /sys/kernel/mm/lru_gen/enabled 2>/dev/null || echo 7 > /sys/kernel/mm/lru_gen/enabled 2>/dev/null
fi

# 2. CPU Governor Frame-Pacing Calibration (Flat 16.67ms 60 FPS Lock)
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    if [ -d "$policy/schedutil" ]; then
        echo 500 > "$policy/schedutil/up_rate_limit_us" 2>/dev/null
        echo 20000 > "$policy/schedutil/down_rate_limit_us" 2>/dev/null
        echo 1 > "$policy/schedutil/iowait_boost_enable" 2>/dev/null
    fi
done

# 3. GPU Power Nap & Adreno 618 Scaling
if [ -e /sys/class/kgsl/kgsl-3d0/force_no_nap ]; then
    echo 0 > /sys/class/kgsl/kgsl-3d0/force_no_nap 2>/dev/null
fi

# 4. Low-Latency Network Engine (BBRplus + FQ-CoDel)
if grep -q "bbrplus" /proc/sys/net/ipv4/tcp_available_congestion_control 2>/dev/null; then
    echo bbrplus > /proc/sys/net/ipv4/tcp_congestion_control 2>/dev/null
fi
if grep -q "fq_codel" /proc/sys/net/core/default_qdisc 2>/dev/null; then
    echo fq_codel > /proc/sys/net/core/default_qdisc 2>/dev/null
fi
EOF

chmod 755 /data/adb/service.d/purrx-engine.sh 2>/dev/null;
ui_print "PurrX Master Engine successfully installed!";
ui_print " ";
## end boot install

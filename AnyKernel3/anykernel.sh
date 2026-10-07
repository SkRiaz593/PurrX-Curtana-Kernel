### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers
## PurrX r2 — Xiaomi curtana
## A-only, boot header v2
## Full-stack flash: kernel + verified PurrX DTB + DTBO

### AnyKernel setup
# global properties
properties() { '
kernel.string=PurrX curtana r2 (KernelSU + SUSFS)
kernel.compiler=Clang/LLVM 18 (LLVM=1, no GCC)
kernel.made=SkRiaz593 / PurrX
kernel.version=4.14.357-openela
message.word=PurrX r2 adaptive performance build for Xiaomi curtana only.
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

# Install PurrX r2 Adaptive Performance Service
ui_print " ";
ui_print "Installing PurrX r2 Adaptive Performance Service...";
mkdir -p /data/adb/service.d 2>/dev/null;

# ==============================================================================
# SCRIPT 1: purrx-engine.sh — One-shot boot baseline (Daily mode defaults)
# ==============================================================================
cat << 'EOF' > /data/adb/service.d/purrx-engine.sh
#!/system/bin/sh
# PurrX r2 Daily Baseline — runs once at boot, applies safe daily defaults, exits.

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done

# --- zRAM: preserve Android config, only init if truly uninitialized ---
if [ -e /proc/swaps ]; then
    zram_active=$(grep -E '/dev/block/zram0|/dev/zram0' /proc/swaps 2>/dev/null || true)
    if [ -z "$zram_active" ] && [ -e /dev/block/zram0 ]; then
        if grep -q "lz4" /sys/block/zram0/comp_algorithm 2>/dev/null; then
            echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null
        fi
        echo 2147483648 > /sys/block/zram0/disksize 2>/dev/null
        mkswap /dev/block/zram0 2>/dev/null
        swapon /dev/block/zram0 -p 32767 2>/dev/null
    fi
fi

# --- CPU: frame-pacing schedutil (applies to daily AND gaming) ---
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    if [ -d "$policy/schedutil" ]; then
        echo 500 > "$policy/schedutil/up_rate_limit_us" 2>/dev/null
        echo 20000 > "$policy/schedutil/down_rate_limit_us" 2>/dev/null
        echo 1 > "$policy/schedutil/iowait_boost_enable" 2>/dev/null
    fi
done

# --- GPU: ensure power-nap is active (daily efficiency) ---
if [ -e /sys/class/kgsl/kgsl-3d0/force_no_nap ]; then
    echo 0 > /sys/class/kgsl/kgsl-3d0/force_no_nap 2>/dev/null
fi

# --- Network: BBRplus + FQ-CoDel if supported ---
if [ -e /proc/sys/net/ipv4/tcp_available_congestion_control ]; then
    if grep -q "bbrplus" /proc/sys/net/ipv4/tcp_available_congestion_control 2>/dev/null; then
        echo bbrplus > /proc/sys/net/ipv4/tcp_congestion_control 2>/dev/null
    fi
fi
if [ -e /proc/sys/net/core/default_qdisc ]; then
    echo fq_codel > /proc/sys/net/core/default_qdisc 2>/dev/null || true
fi

# --- LMKD: protect foreground, aggressive background reclaim for 4GB ---
setprop ro.config.low_ram false
setprop ro.lmk.kill_heaviest_task true
setprop ro.lmk.kill_timeout_ms 100
setprop ro.lmk.psi_partial_stall_ms 70
setprop ro.lmk.psi_complete_stall_ms 300
setprop lmkd.reinit 1

# --- BFQ: low-latency mode for smoother game asset loading ---
for dev in /sys/block/sd*/queue/iosched/low_latency; do
    [ -e "$dev" ] && echo 1 > "$dev" 2>/dev/null
done
EOF

# ==============================================================================
# SCRIPT 2: purrx-gamewatch.sh — Adaptive mode switcher (runs in background)
# ==============================================================================
cat << 'EOF' > /data/adb/service.d/purrx-gamewatch.sh
#!/system/bin/sh
# PurrX r2 Game Watch — detects heavy games in foreground, switches profiles.
# Cost: ~0.1% CPU. One dumpsys call every 5 seconds.

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 5; done
sleep 20  # let system settle

# --- Heavy game package list (battle royale, FPS, racing) ---
HEAVY_GAMES="com.dts.freefireth|com.dts.freefiremax|com.tencent.ig|com.pubg.imobile|com.pubg.krmobile|com.activision.callofduty.shooter|com.garena.game.codm|com.ea.gp.fifamobile|com.gameloft.android.ANMP.GloftA9HM|com.gameloft.android.ANMP.GloftAGHM|com.mojang.minecraftpe|com.epicgames.fortnite"

CURRENT_MODE="daily"

# --- Save stock GPU min freq ---
STOCK_GPU_MIN=$(cat /sys/class/kgsl/kgsl-3d0/devfreq/min_freq 2>/dev/null)
[ -z "$STOCK_GPU_MIN" ] && STOCK_GPU_MIN=0

apply_gaming() {
    echo "PurrX: entering GAMING mode" > /dev/kmsg 2>/dev/null

    # GPU floor to 50% of max
    MAX=$(cat /sys/class/kgsl/kgsl-3d0/devfreq/max_freq 2>/dev/null)
    if [ -n "$MAX" ] && [ "$MAX" -gt 0 ]; then
        echo $((MAX / 2)) > /sys/class/kgsl/kgsl-3d0/devfreq/min_freq 2>/dev/null
    fi

    # SchedTune: boost top-app (the running game)
    if [ -d /dev/stune/top-app ]; then
        echo 20 > /dev/stune/top-app/schedtune.boost 2>/dev/null
        echo 1 > /dev/stune/top-app/schedtune.prefer_idle 2>/dev/null
    fi

    # Schedutil: faster ramp-up during gameplay
    for policy in /sys/devices/system/cpu/cpufreq/policy*; do
        if [ -d "$policy/schedutil" ]; then
            echo 500 > "$policy/schedutil/up_rate_limit_us" 2>/dev/null
            echo 10000 > "$policy/schedutil/down_rate_limit_us" 2>/dev/null
            echo 1 > "$policy/schedutil/iowait_boost_enable" 2>/dev/null
        fi
    done

    # GPU power-nap: disable during gaming
    [ -e /sys/class/kgsl/kgsl-3d0/force_no_nap ] && \
        echo 1 > /sys/class/kgsl/kgsl-3d0/force_no_nap 2>/dev/null

    # Re-assert LMKD
    setprop ro.lmk.kill_heaviest_task true
    setprop lmkd.reinit 1
}

apply_daily() {
    echo "PurrX: entering DAILY mode" > /dev/kmsg 2>/dev/null

    # Revert GPU floor to stock
    [ "$STOCK_GPU_MIN" -eq 0 ] && \
        echo "$STOCK_GPU_MIN" > /sys/class/kgsl/kgsl-3d0/devfreq/min_freq 2>/dev/null

    # Revert SchedTune
    if [ -d /dev/stune/top-app ]; then
        echo 0 > /dev/stune/top-app/schedtune.boost 2>/dev/null
        echo 0 > /dev/stune/top-app/schedtune.prefer_idle 2>/dev/null
    fi

    # Revert schedutil down_rate_limit
    for policy in /sys/devices/system/cpu/cpufreq/policy*; do
        if [ -d "$policy/schedutil" ]; then
            echo 20000 > "$policy/schedutil/down_rate_limit_us" 2>/dev/null
        fi
    done

    # GPU power-nap: re-enable
    [ -e /sys/class/kgsl/kgsl-3d0/force_no_nap ] && \
        echo 0 > /sys/class/kgsl/kgsl-3d0/force_no_nap 2>/dev/null
}

# --- Main watch loop ---
while true; do
    FG=$(dumpsys activity activities 2>/dev/null | grep -E 'ResumedActivity|mResumedActivity' | head -1)

    if echo "$FG" | grep -qE "$HEAVY_GAMES"; then
        [ "$CURRENT_MODE" != "gaming" ] && { apply_gaming; CURRENT_MODE="gaming"; }
    else
        [ "$CURRENT_MODE" != "daily" ] && { apply_daily; CURRENT_MODE="daily"; }
    fi

    sleep 5
done
EOF

chmod 755 /data/adb/service.d/purrx-engine.sh 2>/dev/null;
chmod 755 /data/adb/service.d/purrx-gamewatch.sh 2>/dev/null;
ui_print "PurrX r2 Adaptive Performance Service installed!";
ui_print " ";
## end boot install

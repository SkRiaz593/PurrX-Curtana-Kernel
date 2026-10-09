#!/system/bin/sh
# Daily Mode Enabler for Battery Efficiency
# Use this after gaming or for normal daily use

echo "[Daily Mode] Activating..."

# CPU Governor - Interactive (Battery Friendly)
echo "Setting CPU Governor to Interactive..."
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    echo "interactive" > $cpu/cpufreq/scaling_governor 2>/dev/null || true
done

# Reduce CPU Boost Duration
echo "Reducing boost duration to 100ms..."
echo 100 > /sys/module/cpu_boost/parameters/input_boost_ms 2>/dev/null || true

# Lower Boost Frequency to 1200MHz
echo "Reducing boost frequency for battery..."
echo 1200000 > /sys/module/cpu_boost/parameters/input_boost_freq_gb 2>/dev/null || true

# Re-enable Swap Pressure (Memory Efficiency)
echo "Enabling swap for memory efficiency..."
echo 60 > /proc/sys/vm/swappiness 2>/dev/null || true

# Enable MGLRU (Memory efficiency)
echo "Enabling MGLRU for memory management..."
echo 1 > /sys/kernel/mm/lru_gen/enabled 2>/dev/null || true

# Set I/O Scheduler to BFQ with fairness
echo "Keeping BFQ for fair I/O..."
for disk in /sys/block/mmcblk*/queue/scheduler; do
    if [ -f $disk ]; then
        echo "bfq" > $disk 2>/dev/null || true
    fi
done

# Return GPU to normal frequency
echo "Reducing GPU frequency for battery..."
echo 300000000 > /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq 2>/dev/null || true

# Lower thermal threshold for aggressive throttling
echo "Setting conservative thermal limits..."
echo 75000 > /sys/class/thermal/thermal_zone0/trip_point_0_temp 2>/dev/null || true

echo "[Daily Mode] ✅ ACTIVATED"
echo "Battery optimized for daily use"
echo "CPU: Interactive | Boost: 100ms @ 1.2GHz | MGLRU: Enabled | Thermal: 75°C"

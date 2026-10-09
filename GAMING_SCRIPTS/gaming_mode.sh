#!/system/bin/sh
# Gaming Mode Enabler for StormBreaker Kernel
# Use this before launching Free Fire for maximum performance

echo "[Gaming Mode] Activating..."

# CPU Governor - Performance
echo "Setting CPU Governor to Performance..."
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    echo "performance" > $cpu/cpufreq/scaling_governor 2>/dev/null || true
done

# Enable CPU Boost
echo "Enabling CPU Boost..."
echo 1 > /sys/module/cpu_boost/parameters/enabled 2>/dev/null || true

# Set CPU Boost Duration
echo "Setting boost duration to 500ms..."
echo 500 > /sys/module/cpu_boost/parameters/input_boost_ms 2>/dev/null || true

# Set Boost Frequency to 1500MHz
echo "Setting boost frequency..."
echo 1500000 > /sys/module/cpu_boost/parameters/input_boost_freq_gb 2>/dev/null || true

# Disable Swap Pressure
echo "Minimizing swap pressure..."
echo 0 > /proc/sys/vm/swappiness 2>/dev/null || true

# Drop unnecessary caches
echo "Clearing caches for faster response..."
sync
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true

# Set I/O Scheduler to BFQ (if available)
echo "Setting I/O scheduler to BFQ..."
for disk in /sys/block/mmcblk*/queue/scheduler; do
    if [ -f $disk ]; then
        echo "bfq" > $disk 2>/dev/null || true
    fi
done

# Disable MGLRU (if available) - reduce jitter
echo "Disabling MGLRU for stable FPS..."
echo 0 > /sys/kernel/mm/lru_gen/enabled 2>/dev/null || true

# Increase GPU frequency (Adreno)
echo "Boosting GPU frequency..."
echo 600000000 > /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq 2>/dev/null || true

# Set thermal threshold higher for sustained play
echo "Setting thermal headroom..."
echo 85000 > /sys/class/thermal/thermal_zone0/trip_point_0_temp 2>/dev/null || true

echo "[Gaming Mode] ✅ ACTIVATED"
echo "Ready for Free Fire esports!"
echo "CPU: Performance | Boost: 500ms @ 1.5GHz | I/O: BFQ | Thermal: 85°C"

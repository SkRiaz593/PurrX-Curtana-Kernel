#!/system/bin/sh
# Real-time Gaming Performance Monitor
# Shows CPU, GPU, Thermal, and FPS information

clear

echo "╔════════════════════════════════════════════════════════╗"
echo "║        StormBreaker Gaming Kernel Monitor              ║"
echo "║     Real-time Performance Metrics                      ║"
echo "╚════════════════════════════════════════════════════════╝"

echo ""
echo "[CPU Information]"
echo "─────────────────────────────────────────────────────────"

# Show CPU Frequencies
echo "CPU0 Frequency: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null | awk '{print $1/1000}') MHz"
echo "CPU1 Frequency: $(cat /sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq 2>/dev/null | awk '{print $1/1000}') MHz"
echo "CPU4 Frequency: $(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq 2>/dev/null | awk '{print $1/1000}') MHz (Big Core)"

# Show Governor
echo "Governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"

# Show Boost Status
echo "CPU Boost: $(cat /sys/module/cpu_boost/parameters/enabled 2>/dev/null)"

echo ""
echo "[Memory Information]"
echo "─────────────────────────────────────────────────────────"

# Show Memory Usage
echo "Total Memory: $(free -h | awk 'NR==2 {print $2}')"
echo "Used Memory: $(free -h | awk 'NR==2 {print $3}')"
echo "Available: $(free -h | awk 'NR==2 {print $7}')"
echo "Swappiness: $(cat /proc/sys/vm/swappiness 2>/dev/null)%"
echo "MGLRU Enabled: $(cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null)"

echo ""
echo "[Thermal Information]"
echo "─────────────────────────────────────────────────────────"

# Show Temperature
echo "CPU Temp: $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null | awk '{print $1/1000}')°C"
echo "Battery Temp: $(cat /sys/class/power_supply/battery/temp 2>/dev/null | awk '{print $1/10}')°C"
echo "Thermal Limit: $(cat /sys/class/thermal/thermal_zone0/trip_point_0_temp 2>/dev/null | awk '{print $1/1000}')°C"

echo ""
echo "[I/O Information]"
echo "─────────────────────────────────────────────────────────"

# Show I/O Scheduler
echo "I/O Scheduler: $(cat /sys/block/mmcblk0/queue/scheduler 2>/dev/null)"

echo ""
echo "[GPU Information]"
echo "─────────────────────────────────────────────────────────"

# Show GPU Frequency
echo "GPU Frequency: $(cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq 2>/dev/null | awk '{print $1/1000000}') MHz"

echo ""
echo "╔════════════════════════════════════════════════════════╗"
echo "║  Gaming Mode: gaming_mode.sh                           ║"
echo "║  Daily Mode:  daily_mode.sh                            ║"
echo "║  Refresh:     monitor.sh                               ║"
echo "╚════════════════════════════════════════════════════════╝"

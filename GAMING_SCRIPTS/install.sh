#!/system/bin/sh
# Install Gaming Mode Scripts
# Run this once to setup gaming/daily mode toggles

echo "[Install] Gaming Mode Script Setup"

# Create directory
mkdir -p /data/gaming_mode

# Copy scripts
cp gaming_mode.sh /data/gaming_mode/
cp daily_mode.sh /data/gaming_mode/
cp monitor.sh /data/gaming_mode/

# Make executable
chmod +x /data/gaming_mode/gaming_mode.sh
chmod +x /data/gaming_mode/daily_mode.sh
chmod +x /data/gaming_mode/monitor.sh

echo "[Install] ✅ Scripts installed to /data/gaming_mode/"
echo ""
echo "Usage:"
echo "  Before gaming: /data/gaming_mode/gaming_mode.sh"
echo "  After gaming:  /data/gaming_mode/daily_mode.sh"
echo "  Monitor:       /data/gaming_mode/monitor.sh"
echo ""
echo "Or with KernelSU:"
echo "  ksud cmd /data/gaming_mode/gaming_mode.sh"

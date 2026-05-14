#!/bin/bash

export BENCH='/home/ss22/Workspace/SRC/bench'
set -e  # Exit on any error
string=`cat /proc/cmdline | grep "i915.disable_display=0"`
echo "debug"
if [[ -z "$string"	 ]]; then
    echo "Please disable the display before running this script."
    exit 1
fi
echo "✓ Display is disabled"

# Set scaling governor to performance
echo "Setting CPU scaling governor to performance..."
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$cpu" ] && echo performance | sudo tee "$cpu" > /dev/null
done
echo "✓ Governor set to performance"

# Configure hugepages
echo "Configuring hugepages..."
sudo sysctl -w vm.nr_hugepages=64
echo "✓ Hugepages configured ($(cat /proc/sys/vm/nr_hugepages))"	

# Run benchmark
echo "Running benchmark..."
[ ! -f "$BENCH" ] && echo "ERROR: $BENCH not found" && exit 1

$BENCH --delay 10000 --size 32 --huge --perf --cpu 0 --auto --all --csv sustain_bw.csv


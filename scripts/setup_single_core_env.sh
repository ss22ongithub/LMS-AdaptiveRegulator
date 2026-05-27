#!/bin/bash

export MSR_WRITE_PERMISSION_PATH=/sys/module/msr/parameters/allow_writes
export MSR_REG_HW_PREFETECHER=0x1A4
export MASTER_CORE=0

# If SPEC Installation exists 
export SPEC=$SPEC

##############################################################################################################
execute_config() {
   start_msg=$1
   cmd=$2
   end_msg="DONE"
   
   echo "$start_msg..."   
   echo "$cmd" | bash
   echo $end_msg
}

init(){
   # Ensure to run as root
   if [ "$EUID" -ne 0 ]; then
     echo "Please run as root or use sudo"
     exit 1
   fi
}

stop_all_unwanted_services(){
   execute_config "Turning off Snapd..." "systemctl disable --now snapd.service snapd.socket"
   execute_config "Turn Off rootkit daemon" "systemctl stop rtkit-daemon"
   execute_config "Turn Off CUPS" "systemctl stop cups"
}

##############################################################################################################

main_setup(){
   # Detect total number of CPU cores
   TOTAL_CORES=$(nproc --all)

   # Turn on the permission for writing into msr registers
   execute_config "Turn on the permission for writing into msr" "echo on > $MSR_WRITE_PERMISSION_PATH"

   # Disable hardware prefetchers
   execute_config "Disable hardware prefetcher on all cores" "wrmsr -a $MSR_REG_HW_PREFETECHER 0x00000000000000F" 

   # Enable system wide and uncore event collection
   execute_config "Enable system wide and uncore event collection" "echo -1 > /proc/sys/kernel/perf_event_paranoid"

   # Disable Real-time throttling
   execute_config "Disable Real-time throttling" "echo -1 | tee /proc/sys/kernel/sched_rt_runtime_us"

   # Disable all cores except the master core
   for ((core=0; core<$TOTAL_CORES; core++)); do
      if [ $core -ne $MASTER_CORE ]; then
         # Check if the core can be disabled and is currently online
         if [ -f /sys/devices/system/cpu/cpu$core/online ]; then
            CORE_STATUS=$(cat /sys/devices/system/cpu/cpu$core/online)
            if [ "$CORE_STATUS" = "1" ]; then
               execute_config "Disable CPU core $core" "echo 0 > /sys/devices/system/cpu/cpu$core/online"
            fi
         fi
      fi
   done

   stop_all_unwanted_services

   execute_config "Display online CPUs " "lscpu -b --extended"
}

##############################################################################################################
# Main execution
##############################################################################################################

init
main_setup

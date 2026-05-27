#! /bin/bash


#if SPEC Installation exist
export SPEC=$SPEC
export CPU_CORE_C1=1
export CPU_CORE_C2=2
export CPU_CORE_C3=3
export CPU_CORE_C4=4


export CPUSET_C1="C1"
export CPUSET_C2="C2"
export CPUSET_C3="C3"
export CPUSET_C4="C4"

export BASE_DATA_PATH=/home/ss22/Workspace/data/
#export MEMG_PATH=without_regulation/  #default pat

export benchmarks_single=(
"503.bwaves_r"
)

export benchmarks_all=(
"500.perlbench_r"
"502.gcc_r"
"505.mcf_r"
"520.omnetpp_r"
"523.xalancbmk_r"
"525.x264_r"
"531.deepsjeng_r"
"541.leela_r"
"548.exchange2_r"
"557.xz_r"
"503.bwaves_r"
"507.cactuBSSN_r"
"508.namd_r"
"510.parest_r"
"511.povray_r"
"519.lbm_r"
"521.wrf_r"
"526.blender_r"
"527.cam4_r"
"538.imagick_r"
"544.nab_r"
"549.fotonik3d_r"
"554.roms_r"
"600.perlbench_s"
"602.gcc_s"
"605.mcf_s"
"620.omnetpp_s"
"623.xalancbmk_s"
"625.x264_s"
"631.deepsjeng_s"
"641.leela_s"
"648.exchange2_s"
"657.xz_s"
"603.bwaves_s"
"607.cactuBSSN_s"
"619.lbm_s"
"621.wrf_s"
"627.cam4_s"
"628.pop2_s"
"638.imagick_s"
"644.nab_s"
"649.fotonik3d_s"
"654.roms_s"
)


export benchmarks_intrate=(
"500.perlbench_r"
"502.gcc_r"
"505.mcf_r"
"520.omnetpp_r"
"523.xalancbmk_r"
"525.x264_r"
"531.deepsjeng_r"
"541.leela_r"
"548.exchange2_r"
"557.xz_r"
)

export benchmarks_intspeed=(
"600.perlbench_s"
"602.gcc_s"
"605.mcf_s"
"620.omnetpp_s"
"623.xalancbmk_s"
"625.x264_s"
"631.deepsjeng_s"
"641.leela_s"
"648.exchange2_s"
"657.xz_s"
)

export benchmarks_fprate=(
"503.bwaves_r"
"507.cactuBSSN_r"
"508.namd_r"
"510.parest_r"
"511.povray_r"
"519.lbm_r"
"521.wrf_r"
"526.blender_r"
"527.cam4_r"
"538.imagick_r"
"544.nab_r"
"549.fotonik3d_r"
"554.roms_r"
)

export benchmarks_fpspeed=(
"603.bwaves_s"
"607.cactuBSSN_s"
"619.lbm_s"
"621.wrf_s"
"627.cam4_s"
"628.pop2_s"
"638.imagick_s"
"644.nab_s"
"649.fotonik3d_s"
"654.roms_s"
)


function cleanup() {

#	LOG_FILEPATH=`ls -tr $SPEC/result/CPU2017*log | tail -1`
#	echo "Copying $LOG_FILEPATH to $BASE_DATA_PATH$MEMG_PATH$bm/"
#	cp "$LOG_FILEPATH" "$BASE_DATA_PATH$MEMG_PATH$bm/"

	echo "Stopping Perf($PERF_LLC_PID)"
	kill -9 $PERF_LLC_PID
}


# function exit() {
# 	echo "CLEANING UP ..."
# 	cleanup
# 	echo "EXITING..."
# 	exit
# }

 execute_config() {
    start_msg=$1
    cmd=$2
    end_msg="DONE"

    echo "$start_msg..."
    echo "sudo $cmd" | bash
    echo $end_msg
 }



 function preprocess_separate_load_store_misses(){
	if [ -n "$1" ]; then
    	FILEPATH=$1
    	grep "LLC-load-misses"	$FILEPATH > "$FILEPATH.lm"
    	grep "LLC-store-misses"	$FILEPATH > "$FILEPATH.sm"
 	fi
}

################# MAIN ##################################

function monitor_llc_cache_miss() {

# monitor the LLC cache misses
	PERF_LLC_FILEPATH=$1
	MONTOR_CORE=$2
	local INTERVAL_MSEC=1000

	perf stat -x, -e  LLC-load-misses,LLC-store-misses -C $MONTOR_CORE -I $INTERVAL_MSEC -o $PERF_LLC_FILEPATH &
	local PERF_LLC_PID=$!
	echo "PERF Recording LLC Misses $PERF_LLC_FILEPATH PID ($PERF_LLC_PID) on Core ($CPU_CORE_FOREGROUND)"
    sleep 1

    echo $PERF_LLC_PID
}

######## Step 1 : Ensure the setup is ready #############

#sudo bash -c isol_setup.sh

########Step 2: ###########################################
function run_benchmark() {
	dry_run=""
	export BENCHMARK_NAME=$1
	export rundt=$2
	local CPUSET_NAME=$3
	export COS=$4
	export PERF_TIMEOUT=20000

	local CPU_CORE=$CPU_CORE_FOREGROUND
	if [[ $CPUSET_NAME == $CPUSET_BACK_NAME ]] ; then
		CPU_CORE=$CPU_CORE_BACKGROUND
	fi


	export ITERATIONS=10000

	export dt=`date +"%Y-%m-%d-%H-%M-%S"`

	export RUN_DATA_PATH="$BASE_DATA_PATH$MEMG_PATH$BENCHMARK_NAME/RUN$COS-$rundt"


	RUN_EXE_PATH="$SPEC/benchspec/CPU/$BENCHMARK_NAME/run/run_base_test_all-suites-execution-times-m64.0000/"
	echo "RUNEXE_PATH = $RUN_EXE_PATH"

	execute_config "Creating $RUN_DATA_PATH" "mkdir -p $RUN_DATA_PATH ; sudo chmod 755 $RUN_DATA_PATH; sudo chown ss22 $RUN_DATA_PATH"

	# local PERF_LLC_PID =$(monitor_llc_cache_miss "$RUN_DATA_PATH/$BENCHMARK_NAME-llc$dry_run-$dt.csv" $CPU_CORE_FOREGROUND)

	#sudo bash -c "cd $SPEC; source shrc; cd -; \
	cset proc -s $CPUSET_NAME  -e  specinvoke -- -i $ITERATIONS  -d $RUN_EXE_PATH &
	export BENCHMARK_PID=$!
	echo "$BENCHMARK_NAME started on Core($CPUSET_NAME) PID ($BENCHMARK_PID)"

	export PERF_IPC_FILEPATH="$RUN_DATA_PATH/$BENCHMARK_NAME-IPC$dry_run-$dt.txt"
	echo "PERF Recording IPC in  $PERF_IPC_FILEPATH for $PERF_TIMEOUT ms"
	perf stat -e  instructions,cycles -o $PERF_IPC_FILEPATH  -p $BENCHMARK_PID --timeout $PERF_TIMEOUT

	# cleanup
	# Stop parent and all decendent processes
	echo "Stopping $BENCHMARK_NAME ($BENCHMARK_PID) ..."
	pstree -p $BENCHMARK_PID  |awk -F'[()]' '{for(i=2;i<=NF;i+=2)print $i}' |xargs kill -9 $BENCHMARK_PID

	# preprocess_separate_load_store_misses "$PERF_LLC_FILEPATH"

	IPC_VALUE=$(grep "insn per cycle" $PERF_IPC_FILEPATH | awk '{print $4}')
	echo "$BENCHMARK_NAME = $IPC_VALUE IPC"

}


################## Initialization function ##############################
function init(){
	execute_config "Switch off kernel buffer tracing! " "echo 0 >  /sys/kernel/tracing/tracing_on"

	execute_config "Setting CPU frequency governor = performance" "cpupower frequency-set -g performance"
	execute_config "Disable NMI watchdog" "echo 0 > /proc/sys/kernel/nmi_watchdog"

	if [ -d /sys/module/areg ]; then
	    export MEMG_PATH=with_ar/
	elif [ -d /sys/module/memguard ]; then
	    export MEMG_PATH=with_memguard/
	else
		 export MEMG_PATH=without_regulation/
	fi

}

#####################################################################################r###################
rundt=`date +"%Y-%m-%d-%H-%M-%S"`



###### Run Initializations #################

init

#############################################  MAIN #########################################r###################

echo "====================================================$benchmark START============================================"
#TASK_T1="519.lbm_r"
#TASK_T2="619.lbm_s"
#TASK_T3="621.wrf_s"
#TASK_T4="521.wrf_r"

#TASK_T1="519.lbm_r"
#TASK_T2="510.parest_r"
#TASK_T3="538.imagick_r"
#TASK_T4="544.nab_r"

#TASK_T1="541.leela_r"
#TASK_T2="600.perlbench_s"
#TASK_T3="511.povray_r"
#TASK_T4="544.nab_r"

TASK_T1="541.leela_r"
TASK_T2="603.bwaves_s"
TASK_T3="620.omnetpp_s"
TASK_T4="544.nab_r"

run_benchmark $TASK_T1 $rundt $CPUSET_C1 "-4CORE-COSCHED-$TASK_T1"&
pid_1=$!
run_benchmark $TASK_T2 $rundt $CPUSET_C2 "-4CORE-COSCHED-$TASK_T2"&
pid_2=$!
run_benchmark $TASK_T3 $rundt $CPUSET_C3 "-4CORE-COSCHED-$TASK_T3"&
pid_3=$!
run_benchmark $TASK_T4 $rundt $CPUSET_C4 "-4CORE-COSCHED-$TASK_T4"&
pid_4=$!

wait $pid_1
wait $pid_2
wait $pid_3
wait $pid_4
sleep 30
echo "====================================================$benchmark ENDS ============================================"

##############################################################################################


execute_config "Enable NMI watchdog" "echo 1 > /proc/sys/kernel/nmi_watchdog"


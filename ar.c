/**
 * Dynamic adaptive memory bandwidth controller for multi-core systems
 *
 *
 * This file is distributed under GPL v2 License. 
 * See LICENSE.TXT for details.
 *
 */


/**************************************************************************
 * Included Files
 **************************************************************************/
#include "kernel_headers.h"
#include "ar.h"
#include "master.h"
#include "ar_debugfs.h"
#include "ar_perfs.h"
#include "utils.h"
#include "model.h"

/**************************************************************************
 * Public Definitions
 **************************************************************************/







/**************************************************************************
 * Local Function Declarations
 **************************************************************************/

static void deinitialize_cpu_info(const u8 cpu_id);
static int  setup_cpu_info(const u8 cpu_id);
static void ar_handle_read_overflow(struct irq_work *entry);


/**************************************************************************
 * Module parameters
**************************************************************************/

static int g_read_counter_id = PMU_LLC_MISS_COUNTER_ID;
module_param(g_read_counter_id, hexint,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

static int g_write_counter_id = PMU_LLC_WB_COUNTER_ID;
module_param(g_write_counter_id, hexint,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);


//Default global pool is zero .It will be filled with unused bandwidths.
//Allow prefilling while loading module
ulong g_pool_bw_mb = 0;
module_param(g_pool_bw_mb, ulong, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

/**************************************************************************
 * Global Variables
 **************************************************************************/

u64 g_initial_bw_mb[MAX_NO_CPUS+1] = {0,500,500,500,500}; /*Pre-defined initial / min Bandwidth in MB/s */
const u64 g_percore_bw_limit_mb[MAX_NO_CPUS+1] = {0,1000,1000,1000,1000}; /*Pre-defined max Bandwidth per core in MB/s */

/**************************************************************************
 * Static Types
 ***************************************************************************/

static struct core_info all_cinfo[MAX_NO_CPUS + 1];

/**************************************************************************
 * Utils
 **************************************************************************/

struct core_info* get_core_info(u8 cpu_id){
    /* CPU 0 is reserved for master thread, not regulated */
    if (cpu_id == 0) {
        pr_err("Invalid CPU ID %u (CPU 0 is master, not regulated)!!!", cpu_id);
        return NULL;
    }
    
    /* Check if CPU ID is within valid range */
    if (cpu_id >= MAX_NO_CPUS) {
        pr_err("Invalid CPU ID %u (max supported: %d)!!!", cpu_id, MAX_NO_CPUS - 1);
        return NULL;
    }
    
    return &all_cinfo[cpu_id];
}




/* WARNING: This function should be kept strictly re-entrant */
void __throttle( void* cpu )
{
    /*Note : Ensure cinfo should always refer to the regulated cores */
    struct core_info* cinfo = (struct core_info*)cpu;
    bool t = atomic_read(&cinfo->throttler_task);

    AR_DEBUG("CPU(%d): throtle state = %d\n", cinfo->cpu_id, t);
    if ( t ) {
        AR_DEBUG("Already in throttled state");
        return;
    }
    atomic_set(&cinfo->throttler_task,true);
    wake_up_interruptible(&cinfo->throttle_evt);
}

/* WARNING: This function should be kept strictly re-entrant */
void __unthrottle( void* cpu) {
    /* Note : Ensure cinfo should always refer to the regulated cores */
    struct core_info* cinfo = (struct core_info*)cpu;
    atomic_set(&cinfo->throttler_task,false);
}


/**************************************************************************
 * Callbacks and Handlers
 **************************************************************************/


/**************************************************************************
 * Throttle Thread
 **************************************************************************/

static int throttler_task_func1(void * data){
    u8 cpu_id = (unsigned long)data;
    pr_info("%s: Enter CPU(%d)",__func__,cpu_id);

    struct core_info *cinfo = get_core_info(cpu_id);//per_cpu_ptr(core_info, cpunr);
    BUG_ON(cinfo == NULL);
    sched_set_fifo(current);

    while (!kthread_should_stop() && cpu_online(cpu_id)) {

        AR_DEBUG("CPU(%d):Waiting for Event\n", cpu_id);
        wait_event_interruptible(cinfo->throttle_evt,
                                 atomic_read(&cinfo->throttler_task)
                                 || kthread_should_stop() );
        AR_DEBUG("CPU(%d):Got Event\n", cpu_id);

        if (kthread_should_stop())
            break;

        AR_DEBUG("CPU(%d):Throttling with MWAIT...\n",cpu_id);
       
       /* Use x86 MWAIT instruction for power-efficient throttling */
       while (atomic_read(&cinfo->throttler_task) && !kthread_should_stop()) {
           if (boot_cpu_has(X86_FEATURE_MWAIT)) {
               /* MONITOR: Set up address to watch */
               __monitor(&cinfo->throttler_task, 0, 0);
               
               /* Recheck after MONITOR to avoid race */
               smp_mb();
               if (atomic_read(&cinfo->throttler_task) && !kthread_should_stop()) {
                   /* MWAIT: Enter low-power state until memory write */
                   __mwait(0, 0);
               }
           } else {
               /* Fallback if MWAIT not available */
               smp_mb();
               cpu_relax();
           }
       }
       
       AR_DEBUG("CPU(%d):Unthrottled\n",cpu_id);
    }

    pr_info("%s: Exit",__func__);
    return 0;
}

/* Callback when read counter exhuasts its budget*/
static void read_event_overflow_callback(struct perf_event *event,
                    struct perf_sample_data *data,
                    struct pt_regs *regs)
{
    u8 cpu_id = smp_processor_id();
    if (cpu_id == 0){
        AR_DEBUG("%s: CPU(%d) not expected here \n",__func__,cpu_id);
        return;
    }

    struct core_info *cinfo = get_core_info(cpu_id);
    BUG_ON(!cinfo);
    irq_work_queue(&cinfo->read_irq_work);
}

static void ar_handle_read_overflow(struct irq_work *entry)
{
    u8 cpu_id = smp_processor_id();
    if (cpu_id == 0){
        AR_DEBUG("%s: CPU(%d) not expected here\n",__func__,cpu_id);
        return;
    }
    BUG_ON(in_nmi() || !in_irq());

    struct core_info *cinfo = get_core_info(cpu_id);
    BUG_ON(!cinfo);
    u64 read_bw_used = cinfo->g_read_count_used;
    u64 reclaim_budget = 0;
    if (read_bw_used < g_pool_bw_mb )
    {
        reclaim_budget = read_bw_used/2;
        g_pool_bw_mb -=  reclaim_budget;
        local64_set(&cinfo->read_event->hw.period_left, reclaim_budget);
        AR_DEBUG("read_bw_used(%llu) < g_pool_bw_mb(%lu): reclaim_budget(%llu)", read_bw_used, g_pool_bw_mb, reclaim_budget);
        return;
    }

    //Activate Throttling
    atomic_set(&cinfo->throttler_task,true);
    wake_up_interruptible(&cinfo->throttle_evt);

}


/**************************************************************************
 * Other Utils
 **************************************************************************/

static int  setup_cpu_info(const u8 cpu_id){
    pr_info("%s: Enter CPU(%d)", __func__,cpu_id );
    struct core_info* cinfo = get_core_info(cpu_id);
    BUG_ON(cinfo==NULL);
    memset(cinfo, sizeof(struct core_info), 0);
    cinfo->cpu_id = cpu_id;


    cinfo->read_event =  init_counter(cinfo->cpu_id,
                                     convert_mb_to_events(g_initial_bw_mb[cpu_id]),
                                     g_read_counter_id,
                                     read_event_overflow_callback);
    if (cinfo->read_event == NULL){
        pr_err("Read_event %p did not allocate ", cinfo->read_event);
        return -1;
    }
    
    cinfo->write_event = init_counter(cinfo->cpu_id,
                                      convert_mb_to_events(g_initial_bw_mb[cpu_id]),
                                      PMU_LLC_WB_COUNTER_ID,
                                      NULL);  /* No callback for write - we don't throttle on write alone */
    if (cinfo->write_event == NULL){
        pr_err("Write_event %p did not allocate ", cinfo->write_event);
        return -1;
    }
    
    /* cycles_l3miss_event disabled
    cinfo->cycles_l3miss_event = init_counter(cinfo->cpu_id,6,
                                       PMU_STALL_L3_MISS_CYCLES_COUNTER_ID,
                                       NULL);

    if (cinfo->cycles_l3miss_event == NULL){
        pr_err("Cycles_l3miss_event %p did not allocate ", cinfo->cycles_l3miss_event);
        return -1;
    }
    */

    /* Initialize NMI irq_work_queue */
    init_irq_work(&cinfo->read_irq_work, ar_handle_read_overflow);

    /* Disable the throttle flag */
    atomic_set(&cinfo->throttler_task,false);   
    

    /* Initialize Wait queue for throttler */
    init_waitqueue_head(&cinfo->throttle_evt);

    /* TODO: Investigate kthread_run_on_cpu API which is  a convenience wrapper
     * for kthread_creat_on_node + kthread_bind + wake_up_process.
     * This has an issue the incorrect cpuid is displayed in the ps command.
     * This needs to be reconfirmed.
     * https://docs.kernel.org/next/driver-api/basics.html
     * */
#if defined(TODO)
    cinfo->throttler_thread = kthread_run_on_cpu(throttler_task_func1,
                                    (void *)((unsigned long)cpu_id),
                                    cpu_to_node(cpu_id),
                                    "areg_kthrottler/%u",cpu_id);

    BUG_ON(IS_ERR(cinfo->throttler_task));
#else
    cinfo->throttler_thread = kthread_create_on_node(throttler_task_func1,
                                    (void *)((unsigned long)cpu_id),
                                    cpu_to_node(cpu_id),
                                    "areg_kthrottler/%u",cpu_id);

    BUG_ON(IS_ERR(cinfo->throttler_thread));
    kthread_bind(cinfo->throttler_thread, cpu_id);
    wake_up_process(cinfo->throttler_thread);
#endif

    /***** Regulation to be started by setting
    /sys/kernel/debug/ar/enable_regulation to 1 ****/

    /* Initiialize weight matrix to predefined values */
    initialize_weight_matrix(cinfo, true);

    pr_info("%s: Exit", __func__ );
    return 0;
}

static void deinitialize_cpu_info( const u8 cpu_id){

    pr_info("%s:Enter CPU (%d)",__func__, cpu_id );
    
    struct core_info *cinfo = get_core_info(cpu_id);
    
    BUG_ON(cinfo == NULL);
        
    /* As much as possible keep the de-initialization in the reverse sequence of initialization
     * Refer: setup_cpu_info()
     * */
    
    //End the throttle thread
    if(cinfo->throttler_thread){
        atomic_set(&cinfo->throttler_task, false);  // Unthrottle first
        wake_up_interruptible(&cinfo->throttle_evt); // Wake up
        msleep(10);  // Give time to wake

        kthread_stop(cinfo->throttler_thread);
        atomic_set(&cinfo->throttler_task,false);
        cinfo->throttler_thread = NULL;
    }
    
    //Free the perf event counters
    if (cinfo->read_event) {
        disable_event(cinfo->read_event);
        cinfo->read_event = NULL;
    }
    
    if (cinfo->write_event) {
        disable_event(cinfo->write_event);
        cinfo->write_event = NULL;
    }
    
    /* cycles_l3miss_event disabled
    if (cinfo->cycles_l3miss_event) {
        disable_event(cinfo->cycles_l3miss_event);
        cinfo->cycles_l3miss_event = NULL;
    }
    */
    
    pr_info("%s:Exit",__func__ );
}



/* New wrapper function that coordinates regulation start */
void start_all_regulation(void){
    pr_info("%s: Starting regulation for all cores", __func__);
    
    /* Signal master thread to unthrottle cores and begin regulation */
    master_start_regulation();
    
    pr_info("%s: All cores regulation started", __func__);
}

void stop_all_regulation(void){
    pr_info("%s: Stopping regulation for all cores", __func__);
    
    /* Stop master thread regulation */
    master_stop_regulation();
    
    
    pr_info("%s: All cores regulation stopped", __func__);
}

/**************************************************************************************************************************
 * Module main
 **************************************************************************************************************************/

static int __init ar_init (void ){

    pr_info("Supported CPUs: %d, online_cpus: %d\n", NR_CPUS, num_online_cpus());
//    pr_info("FPU supported : %d",kernel_fpu_available());
#if defined(__x86_64__) || defined(__i386__)
    if (boot_cpu_has(X86_FEATURE_MWAIT)) {
        pr_info("MWAIT instruction supported - using hardware-efficient throttling\n");
    } else {
        pr_warn("MWAIT instruction NOT supported - using fallback throttling method\n");
    }
#else
    pr_info("Non-x86 architecture detected - MWAIT not available\n");
#endif

    //Initialise core infos
    memset(all_cinfo, 0, sizeof(all_cinfo));

    //Setup CPU info for all online CPUs (except CPU 0 - master)
    u8 cpu_id;
    u8 initialized_cpus[MAX_NO_CPUS] = {0};  // Track which CPUs were initialized
    u8 init_count = 0;
    int ret = 0;
    
    for_each_online_cpu(cpu_id) {
        if (cpu_id == 0)
            continue;  // Skip master CPU
        
        if (cpu_id >= MAX_NO_CPUS) {
            pr_warn("CPU %d exceeds MAX_NO_CPUS (%d), skipping\n", cpu_id, MAX_NO_CPUS);
            continue;
        }
        
        ret = setup_cpu_info(cpu_id);
        if (ret != 0) {
            pr_err("setup_cpu_info() failed for CPU %d\n", cpu_id);
            
            // Cleanup: deinitialize all previously initialized CPUs
            for (u8 i = 0; i < init_count; i++) {
                deinitialize_cpu_info(initialized_cpus[i]);
            }
            return -ENOMEM;
        }
        
        initialized_cpus[init_count++] = cpu_id;
        pr_info("CPU %d initialized successfully\n", cpu_id);
    }
    
    if (init_count == 0) {
        pr_err("No CPUs available for regulation (need at least 1 CPU besides CPU 0)\n");
        return -ENODEV;
    }
    
    pr_info("Initialized %d CPU(s) for regulation\n", init_count);

    /* Initialize the master thread - this will throttle all cores */
    initialize_master();

    /* Create entries in debugfs */
    ar_init_debugfs();

    pr_info("Module Initialized - All cores throttled, waiting for regulation start\n");
    return 0;

}


static void __exit ar_exit( void )
{
    u8 cpu_id;
    
    /* Keep the deinitializing sequence reverse of the allocation sequence seen in  __init function */
    ar_remove_debugfs();
    deinitialize_master();
    
    /* Deinitialize all regulated CPUs (except CPU 0 - master) */
    for_each_online_cpu(cpu_id) {
        if (cpu_id == 0)
            continue;  // Skip master CPU
        
        if (cpu_id >= MAX_NO_CPUS)
            continue;  // Skip invalid CPU IDs
        
        deinitialize_cpu_info(cpu_id);
        pr_info("CPU %d deinitialized\n", cpu_id);
    }

    pr_info("Module removed\n");
	return;
}

module_init(ar_init);
module_exit(ar_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sudarshan S <sudarshan.srinivasan@research.iiit.ac.in>");

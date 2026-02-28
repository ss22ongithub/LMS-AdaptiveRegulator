#include "kernel_headers.h"
#include "master.h"
#include "ar.h"
#include "ar_perfs.h"
#include "utils.h"
#include "model.h"

static struct task_struct* mthread = NULL;

/* Master thread state management */
static atomic_t master_state = ATOMIC_INIT(MASTER_STATE_INITIAL);
static wait_queue_head_t master_wait_queue;
/**/

/* External Functions */
extern u64 estimate(u64* feat, u8 feat_len, double *wm, u8 wm_len, u8 index);
extern void update_weight_matrix(s64 error,struct core_info* cinfo );

extern void __throttle( void* cpu );
extern void __unthrottle( void* cpu );

/* External Variables / constants */
extern u64 g_initial_bw_mb[MAX_NO_CPUS+1];/*Pre-defined initial / min Bandwidth in MB/s */
extern const u64 g_percore_bw_limit_mb[MAX_NO_CPUS+1]; /*Pre-defined max Bandwidth per core in MB/s */
extern ulong g_pool_bw_mb;


static int master_thread_func(void * data) {
    u8 cpu_id;
    pr_info("%s: Enter",__func__);

    /* Step 1: Immediately throttle all regulated cores */
    pr_info("%s: Throttling all regulated cores at startup", __func__);
    for_each_online_cpu(cpu_id) {
        if (cpu_id == 0)
            continue;  // Skip master CPU
        
        struct core_info* cinfo = get_core_info(cpu_id);
        if (cinfo) {
            __throttle((void*)cinfo);
            pr_info("%s: CPU(%d) throttled", __func__, cpu_id);
        }
    }

    /* Step 2: Wait in INITIAL state until regulation is enabled */
    pr_info("%s: Entering INITIAL state, waiting for regulation to start", __func__);
    wait_event_interruptible(master_wait_queue,
                             atomic_read(&master_state) == MASTER_STATE_RUNNING
                             || kthread_should_stop());

    if (kthread_should_stop()) {
	    pr_info("%s: Exit",__func__);
        return 0;
    }

    /* Step 3: Unthrottle all regulated cores when regulation begins */
    pr_info("%s: Regulation enabled, unthrottling all regulated cores", __func__);
    for_each_online_cpu(cpu_id) {
        if (cpu_id == 0)
            continue;  // Skip master CPU
        
        struct core_info* cinfo = get_core_info(cpu_id);
        if (cinfo) {
            __unthrottle((void*)cinfo);
            pr_info("%s: CPU(%d) unthrottled", __func__, cpu_id);
        }
    }


    /* Step 4: Begin normal regulation loop */
    pr_info("%s: Starting normal regulation loop", __func__);


    while (!kthread_should_stop() ) {
        
        /* Check if we should pause regulation */
        if (atomic_read(&master_state) != MASTER_STATE_RUNNING) {
            pr_info("%s: Regulation paused, waiting...", __func__);
            wait_event_interruptible(master_wait_queue,
                                     atomic_read(&master_state) == MASTER_STATE_RUNNING
                                     || kthread_should_stop());
            if (kthread_should_stop()) {
                break;
            }
            pr_info("%s: Regulation resumed", __func__);
        }

        if (kthread_should_stop()){
            pr_info("Stopping thread %s\n",__func__);
            break;
        }

        for_each_online_cpu(cpu_id){
            if (cpu_id == 0)
                continue;  // Skip master CPU
            
            s64 bw_total_req = 0;
            struct core_info* cinfo = get_core_info(cpu_id);
            
            if (!cinfo) {
                pr_warn_once("%s: No core_info for CPU %d\n", __func__, cpu_id);
                continue;
            }
            
            WARN_ON(cinfo->read_event == NULL);
            WARN_ON(cinfo->write_event == NULL);

            struct perf_event* read_event = cinfo->read_event;
            struct perf_event* write_event = cinfo->write_event;
            //struct perf_event* cycles_l3miss_event = cinfo->cycles_l3miss_event;

            /* Stop the counters */
            cinfo->read_event->pmu->stop(cinfo->read_event, PERF_EF_UPDATE);
            cinfo->write_event->pmu->stop(cinfo->write_event, PERF_EF_UPDATE);
            //cinfo->cycles_l3miss_event->pmu->stop(cinfo->cycles_l3miss_event,PERF_EF_UPDATE);

            /* Read bandwidth tracking */
            cinfo->g_read_count_old = cinfo->g_read_count_new;
            cinfo->g_read_count_new = convert_events_to_mb(perf_event_count(read_event));
            cinfo->g_read_count_used = cinfo->g_read_count_new - cinfo->g_read_count_old;

            /* Write bandwidth tracking */
            cinfo->g_write_count_old = cinfo->g_write_count_new;
            cinfo->g_write_count_new = convert_events_to_mb(perf_event_count(write_event));
            cinfo->g_write_count_used = cinfo->g_write_count_new - cinfo->g_write_count_old;

            /* Net bandwidth = read + write */
            u64 net_bandwidth_used = cinfo->g_read_count_used + cinfo->g_write_count_used;

            //u64 cycles_l3miss_count = perf_event_count(cycles_l3miss_event);

            bw_total_req += net_bandwidth_used;

            /* Use read bandwidth for ML prediction (history tracking) */
            cinfo->read_event_hist[cinfo->ri] = cinfo->g_read_count_used;
            cinfo->next_estimate = estimate( cinfo->read_event_hist,
                                             sizeof(cinfo->read_event_hist)/sizeof(cinfo->read_event_hist[0]),
                                             cinfo->weight_matrix,
                                             sizeof(cinfo->weight_matrix)/sizeof(cinfo->weight_matrix[0]),
                                             cinfo->ri) + g_initial_bw_mb[cpu_id];

            if(cinfo->next_estimate < 0){
                AR_DEBUG("CPU(%u): Negative Estimate=%lld \n",cpu_id,cinfo->next_estimate);
                //scale down the weights
                initialize_weight_matrix(cinfo, false);
                continue;
            }

            u64 allocation = g_percore_bw_limit_mb[cpu_id];
            if (cinfo->next_estimate < g_percore_bw_limit_mb[cpu_id])
            {
                allocation = cinfo->next_estimate;
            }
            g_pool_bw_mb += max(0, g_percore_bw_limit_mb[cpu_id] - allocation);

            /* Allocate budget to both read and write counters */
            local64_set(&cinfo->read_event->hw.period_left, convert_mb_to_events(allocation));
            local64_set(&cinfo->write_event->hw.period_left, convert_mb_to_events(allocation));

            /* Unthrottle if the core is in throttle state */
            atomic_set(&cinfo->throttler_task, false);

            /* Re-enable the counters */
            cinfo->read_event->pmu->start(cinfo->read_event, PERF_EF_RELOAD);
            cinfo->write_event->pmu->start(cinfo->write_event, PERF_EF_RELOAD);
            //cinfo->cycles_l3miss_event->pmu->start(cinfo->cycles_l3miss_event, PERF_EF_RELOAD);

            s64 error = cinfo->g_read_count_used - cinfo->prev_estimate;
            update_weight_matrix(error,cinfo);

#if defined(AR_DEBUG)
            char buf[HIST_SIZE][51]={0};
            for (u8 i = 0; i < HIST_SIZE; i++){
                kernel_fpu_begin();
                print_double(buf[i],cinfo->weight_matrix[i]);
                kernel_fpu_end();
            }
#endif
            (cinfo->ri)++;
            cinfo->ri = (cinfo->ri == HIST_SIZE)? 0:cinfo->ri;

            AR_DEBUG("CPU(%u):r_bw_used=%llu nxt_est=%lld err=%lld w0=%s w1=%s w2=%s w3=%s w4=%s treq=%lld \
            alloc=%llu Gpool=%lu net_bw_used=%llu w_bw_used=%llu \n",
                         cpu_id,
                         cinfo->g_read_count_used,
                         cinfo->next_estimate,
                         error,
                         buf[0],buf[1],buf[2],buf[3], buf[4],
                         bw_total_req, allocation,
                         g_pool_bw_mb,
                         net_bandwidth_used,
                         cinfo->g_write_count_used
							 );

            cinfo->prev_estimate=cinfo->next_estimate;
        }
       msleep(1);
    }

    pr_info("%s: Exit",__func__);
    return 0;
}


void initialize_master(void){

    const u8 cpu_id_zero  = 0; //CPU 0 reserved for master thread, all other online CPUs are regulated
    
    /* Initialize wait queue for master thread state changes */
    init_waitqueue_head(&master_wait_queue);
    
    /* Set initial state */
    atomic_set(&master_state, MASTER_STATE_INITIAL);
    
    mthread = kthread_create_on_node(master_thread_func,
                                       (void*)NULL,
                                       cpu_to_node(cpu_id_zero),
                                       "areg_master_thread/%d",cpu_id_zero);
    BUG_ON(IS_ERR(mthread));
    kthread_bind(mthread, cpu_id_zero);
    wake_up_process(mthread);

    pr_info("%s: Master thread initialized in INITIAL state", __func__);
}

void deinitialize_master(void){
    if (mthread){
        /* Wake up master thread if it's waiting */
        atomic_set(&master_state, MASTER_STATE_STOPPED);
        wake_up_interruptible(&master_wait_queue);
        
        kthread_stop(mthread);
        mthread = NULL;
    }
    pr_info("%s: Exit!",__func__ );
}

void start_regulation(u8 cpu_id){
    struct core_info* cinfo = get_core_info(cpu_id);
    BUG_ON(cinfo==NULL);
    cinfo->next_estimate=0;
    cinfo->prev_estimate=0;

    /* Enable perf events */
    enable_event(cinfo->read_event);
    enable_event(cinfo->write_event);
    //enable_event(cinfo->cycles_l3miss_event);

    /* Note: Timers are not used in this design.
     * Master thread controls regulation timer */

    pr_info("%s: Exit: (CPU %u)",__func__,cpu_id );
}

void stop_regulation(u8 cpu_id){
    struct core_info* cinfo = get_core_info(cpu_id);
    BUG_ON(cinfo==NULL);

    /* Disable perf events */
    perf_event_disable(cinfo->read_event);
    perf_event_disable(cinfo->write_event);
    //perf_event_disable(cinfo->cycles_l3miss_event);

    /* Note: Timers are not used in this design */
    pr_info("%s: Exit: (CPU %u)",__func__,cpu_id );
}

void master_start_regulation(void){
    u8 cpu_id;
    pr_info("%s: Starting regulation", __func__);
    
    /* Enable perf counters for all regulated cores */
    for_each_online_cpu(cpu_id) {
        if (cpu_id == 0)
            continue;  // Skip master CPU
        
        start_regulation(cpu_id);
    }
    msleep(100);

    atomic_set(&master_state, MASTER_STATE_RUNNING);
    wake_up_interruptible(&master_wait_queue);
}

void master_stop_regulation(void){
    u8 cpu_id;
    pr_info("%s: Stopping regulation", __func__);

    /* Disable perf counters for all regulated cores */
    for_each_online_cpu(cpu_id) {
        if (cpu_id == 0)
            continue;  // Skip master CPU
        
        stop_regulation(cpu_id);
    }

    atomic_set(&master_state, MASTER_STATE_INITIAL);
}

int master_get_state(void){
    return atomic_read(&master_state);
}

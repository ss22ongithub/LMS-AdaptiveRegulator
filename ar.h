#if !defined AR_H
#define AR_H

#define HIST_SIZE 5
#define MAX_NO_CPUS 4

extern ulong g_total_available_bw_mb;

/**************************************************************************
 * COUNTERS Format (Umask_code - EventCode) tools/perf/pmu-events/arch/x86/)
 **************************************************************************/
#if defined(__aarch64__) || defined(__arm__)
#  define PMU_LLC_MISS_COUNTER_ID 0x17   // LINE_REFILL
#  define PMU_LLC_WB_COUNTER_ID   0x18   // LINE_WB
#elif defined(__x86_64__) || defined(__i386__)
#  define PMU_LLC_MISS_COUNTER_ID 0x08b0 // OFFCORE_REQUESTS.ALL_DATA_RD
#  define PMU_LLC_WB_COUNTER_ID   0x40b0 // OFFCORE_REQUESTS.WB

//CYCLE_ACTIVITY.STALLS_L3_MISS
#  define PMU_STALL_L3_MISS_CYCLES_COUNTER_ID   0x06A3
/*
* Event: CYCLE_ACTIVITY.STALLS_L3_MISS
* EventSel=A3H, UMask=06H, CMask=06H
*/
#define PMU_STALL_L3_MISS_CYCLES_EVENTSEL 0xA3
#define PMU_STALL_L3_MISS_CYCLES_UMASK    0x06


#endif


/* Each CPU core's info */
struct core_info {
    bool thr;
    u64 g_read_count_new;
    u64 g_read_count_old;
    u64 g_read_count_used;
    u64 read_limit;

    u64 g_write_count_new;
    u64 g_write_count_old;
    u64 g_write_count_used;

  // History of count of LLC read misses occurred between the regulation intervals.
  // (Event: Read misses)
  u64 read_event_hist[HIST_SIZE];
  
  // History of count of LLC write-backs occurred between the regulation intervals.
  // (Event: Write-backs)
  u64 write_event_hist[HIST_SIZE];
  
  u8 ri;  // Ring buffer index (shared for both read and write history)

  u8 cpu_id;
  wait_queue_head_t throttle_evt;
  /* UPDATE: Currently this variable unused and atomic throttle variable is used
   * instead. True = core in throttled state  False = core not throttled */
  atomic_t throttler_task;

  /*
   * Pointer to the throttler kthread context.
   */
  struct task_struct *throttler_thread;

  //  Bandwidth utilization parameters
  // PMC events
  struct perf_event *read_event;
  struct perf_event *write_event;
  struct perf_event *cycles_l3miss_event;

  // Timer related
  struct hrtimer reg_timer;
  struct irq_work read_irq_work;

  // Memory Bandwidth Budget estimate for the core.
  // Computed by master core
  atomic64_t budget_est;
  /* Each core has an array of weights to generate the prediction */
  
  /* Weight matrices for bandwidth prediction */
  double weight_matrix[HIST_SIZE];         // Weights for read BW estimation (keep original name)
  double write_weight_matrix[HIST_SIZE];   // Weights for write BW estimation
  
  // Read bandwidth estimates (keep original names)
  s64 next_estimate;      // Next read bandwidth estimate
  s64 prev_estimate;      // Previous read bandwidth estimate
  
  // Write bandwidth estimates (NEW)
  s64 write_next_estimate;
  s64 write_prev_estimate;
  
  // Per-core bandwidth limits (for dynamic configuration)
  u64 initial_bw_mb;      // Initial/minimum bandwidth in MB/s
  u64 max_bw_limit_mb;    // Maximum bandwidth limit in MB/s
  
};

struct core_info *get_core_info(u8 cpu_id);
void start_regulation(u8 cpu_id);
void stop_regulation(u8 cpu_id);

/* New wrapper functions for coordinated regulation start/stop */
void start_all_regulation(void);
void stop_all_regulation(void);


struct bw_distribution {
  u32 time;
  u32 rd_avg_bw;
};

void __throttle( void* cpu );
void __unthrottle( void* cpu );
#endif

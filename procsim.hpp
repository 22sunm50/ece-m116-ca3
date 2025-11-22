#ifndef PROCSIM_HPP
#define PROCSIM_HPP

#include <cstdint>
#include <cstdio>

#define DEFAULT_K0 1
#define DEFAULT_K1 2
#define DEFAULT_K2 3
#define DEFAULT_R  8
#define DEFAULT_F  4

// One dynamic instruction in the pipeline
typedef struct _proc_inst_t
{
    // Trace fields
    uint32_t instruction_address;
    int32_t  op_code;          // FU type: 0,1,2 or -1 (we normalize -1 -> 1)
    int32_t  src_reg[2];
    int32_t  dest_reg;

    // --- Simulator fields ---

    // Identity / ordering
    uint64_t tag;              // 1,2,3,...

    // Stage entry cycles (for debugging / verification)
    uint64_t fetch_cycle;      // Fetch
    uint64_t disp_cycle;       // Dispatch
    uint64_t sched_cycle;      // Schedule (RS)
    uint64_t exec_cycle;       // Execute (issue)
    uint64_t state_cycle;      // State update

    // Register dependency tracking
    bool     src_ready[2];     // is this source operand ready?
    uint64_t src_tag[2];       // producer tag if not ready (0 if none)

    // Reservation station / FU state
    bool     in_dispatch;
    bool     in_rs;
    bool     issued;           // has been sent to FU
    bool     completed;        // FU latency done (but maybe not broadcast)
    bool     broadcast;        // result placed on a result bus
    bool     ready_to_retire;  // in state-update
    bool     retired;          // fully done, removed from RS

    int      fu_type;          // 0,1,2 after normalization
    int      fu_index;         // index into FU array (or -1 if none)

    uint64_t completion_cycle; // cycle when FU finished (for ordering)

} proc_inst_t;

typedef struct _proc_stats_t
{
    float         avg_inst_retired;
    float         avg_inst_fired;
    float         avg_disp_size;
    unsigned long max_disp_size;
    unsigned long retired_instruction;
    unsigned long cycle_count;
} proc_stats_t;

// Provided by procsim_driver.cpp
bool read_instruction(proc_inst_t* p_inst);

// Simulator API
void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f);
void run_proc(proc_stats_t* p_stats);
void complete_proc(proc_stats_t* p_stats);

#endif /* PROCSIM_HPP */

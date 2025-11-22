#include "procsim.hpp"

#include <deque>
#include <vector>
#include <algorithm>
#include <cstring>

// =============== DEBUG FLAG ===============
// Set this to false before submitting to Gradescope
// static const bool DEBUG_PRINT = false;

#define LOCAL_DEBUG 1   // uncomment when debugging locally

#ifdef LOCAL_DEBUG
static const bool DEBUG_PRINT = true;
#else
static const bool DEBUG_PRINT = false;
#endif

// =============== Global configuration ===============
static uint64_t g_R  = DEFAULT_R;
static uint64_t g_k0 = DEFAULT_K0;
static uint64_t g_k1 = DEFAULT_K1;
static uint64_t g_k2 = DEFAULT_K2;
static uint64_t g_F  = DEFAULT_F;

// Reservation station capacity = 2 * (k0 + k1 + k2)
static uint64_t g_rs_capacity = 0;

// =============== Global simulation state ===============
static uint64_t g_cycle    = 0;   // current cycle (1-based)
static uint64_t g_next_tag = 1;   // next tag to assign

// RS occupancy as seen by Dispatch (lags real frees by 2 cycles)
static uint64_t g_rs_occ_for_dispatch = 0;
// How many RS slots were freed in the last two cycles
static uint64_t g_rs_freed_delay[2] = {0, 0};

// Dispatch queue (unbounded)
static std::deque<proc_inst_t*> g_dispatch_q;

// Fetch latch: instructions fetched in previous cycle,
// will enter Dispatch in the next cycle.
static std::deque<proc_inst_t*> g_fetch_latch;

// Reservation Station (RS): centralized RS+ROB
static std::vector<proc_inst_t*> g_rs;

static std::vector<proc_inst_t*> g_all_insts;  // all dynamic insts, in tag order

// Per-register readiness (0..127)
struct RegState {
    bool     ready;
    uint64_t producer_tag;  // tag of last writer (0 if none)
};
static RegState g_reg[128];

// Functional Unit (FU) model
struct FuUnit {
    int          fu_type;          // 0,1,2
    bool         busy;
    proc_inst_t* inst;
    int          remaining_cycles; // latency counter (we use 1 cycle)
};
static std::vector<FuUnit> g_fu_units;

// For stats
static uint64_t g_total_disp_size = 0;   // sum over cycles
static uint64_t g_total_fired     = 0;   // total instructions issued to FUs

// =============== Helpers ===============

static inline void log_event(const char* op, uint64_t tag) {
    if (!DEBUG_PRINT) return;
    std::fprintf(stderr, "%lu\t%s\t%lu\n",
                 (unsigned long)g_cycle,
                 op,
                 (unsigned long)tag);
}

static proc_inst_t* alloc_inst(const proc_inst_t& base)
{
    proc_inst_t* p = new proc_inst_t;
    g_all_insts.push_back(p);

    // Copy fields read from trace
    p->instruction_address = base.instruction_address;
    p->op_code             = base.op_code;
    p->src_reg[0]          = base.src_reg[0];
    p->src_reg[1]          = base.src_reg[1];
    p->dest_reg            = base.dest_reg;

    // Normalize FU type: -1 means type 1 per spec
    if (p->op_code == -1) {
        p->fu_type = 1;
    } else {
        p->fu_type = p->op_code;
    }

    // Identity
    p->tag = g_next_tag++;

    // Stage cycles
    p->fetch_cycle  = 0;
    p->disp_cycle   = 0;
    p->sched_cycle  = 0;
    p->exec_cycle   = 0;
    p->state_cycle  = 0;

    // Dependency / RS state
    p->src_ready[0] = p->src_ready[1] = false;
    p->src_tag[0]   = p->src_tag[1]   = 0;

    p->in_dispatch    = false;
    p->in_rs          = false;
    p->issued         = false;
    p->completed      = false;
    p->broadcast      = false;
    p->ready_to_retire= false;
    p->retired        = false;

    p->fu_index        = -1;
    p->completion_cycle= 0;

    return p;
}

static int find_free_fu(int fu_type)
{
    for (size_t i = 0; i < g_fu_units.size(); ++i) {
        if (!g_fu_units[i].busy && g_fu_units[i].fu_type == fu_type) {
            return (int)i;
        }
    }
    return -1;
}

// =============== setup_proc ===============

void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f)
{
    g_R  = r;
    g_k0 = k0;
    g_k1 = k1;
    g_k2 = k2;
    g_F  = f;

    g_rs_capacity = 2 * (g_k0 + g_k1 + g_k2);

    g_cycle    = 0;
    g_next_tag = 1;

    g_dispatch_q.clear();
    g_fetch_latch.clear();
    g_rs.clear();
    g_fu_units.clear();

    g_rs_occ_for_dispatch = 0;
    g_rs_freed_delay[0] = 0;
    g_rs_freed_delay[1] = 0;


    // Initialize register file: all ready with no producer
    for (int i = 0; i < 128; ++i) {
        g_reg[i].ready        = true;
        g_reg[i].producer_tag = 0;
    }

    // Build FU array: first k0 of type 0, then k1 of type 1, then k2 of type 2
    for (uint64_t i = 0; i < g_k0; ++i) {
        FuUnit u;
        u.fu_type          = 0;
        u.busy             = false;
        u.inst             = nullptr;
        u.remaining_cycles = 0;
        g_fu_units.push_back(u);
    }
    for (uint64_t i = 0; i < g_k1; ++i) {
        FuUnit u;
        u.fu_type          = 1;
        u.busy             = false;
        u.inst             = nullptr;
        u.remaining_cycles = 0;
        g_fu_units.push_back(u);
    }
    for (uint64_t i = 0; i < g_k2; ++i) {
        FuUnit u;
        u.fu_type          = 2;
        u.busy             = false;
        u.inst             = nullptr;
        u.remaining_cycles = 0;
        g_fu_units.push_back(u);
    }

    g_total_disp_size = 0;
    g_total_fired     = 0;

    if (DEBUG_PRINT) {
        // Match style of provided log
        std::fprintf(stderr, "CYCLE\tOPERATION\tINSTRUCTION\n");
    }
}

// =============== run_proc ===============

void run_proc(proc_stats_t* p_stats)
{
    bool more_instructions = true;
    bool done              = false;

    p_stats->avg_inst_retired   = 0.0f;
    p_stats->avg_inst_fired     = 0.0f;
    p_stats->avg_disp_size      = 0.0f;
    p_stats->max_disp_size      = 0;
    p_stats->retired_instruction= 0;
    p_stats->cycle_count        = 0;

    while (!done) {
        g_cycle++;

        // Apply frees that became visible to Dispatch this cycle.
        // Freed RS slots are seen by Dispatch with a 2-cycle delay:
        // - we recorded them into g_rs_freed_delay[1] when they retired
        // - shift them down every cycle; subtract the ones that were retired 2 cycles ago
        g_rs_occ_for_dispatch -= g_rs_freed_delay[0];
        g_rs_freed_delay[0] = g_rs_freed_delay[1];
        g_rs_freed_delay[1] = 0;

        size_t fired_this_cycle   = 0;
        size_t retired_this_cycle = 0;

        // // =========================
        // // FIRST HALF OF CYCLE
        // // =========================
        
        // 1) Result buses (CDB) - first half
        //    Broadcast results, update register file, free FUs

        std::vector<proc_inst_t*> cdb_candidates;

        for (size_t i = 0; i < g_fu_units.size(); ++i) {
            FuUnit &fu = g_fu_units[i];
            if (fu.busy && fu.inst != nullptr &&
                fu.inst->completed && !fu.inst->broadcast) {
                cdb_candidates.push_back(fu.inst);
            }
        }

        std::sort(cdb_candidates.begin(), cdb_candidates.end(),
                  [](const proc_inst_t* a, const proc_inst_t* b) {
                      if (a->completion_cycle != b->completion_cycle)
                          return a->completion_cycle < b->completion_cycle;
                      return a->tag < b->tag;
                  });

        size_t buses_used = 0;
        std::vector<proc_inst_t*> broadcasted_this_cycle;

        for (proc_inst_t* inst : cdb_candidates) {
            if (buses_used >= g_R) break;
            buses_used++;

            inst->broadcast = true;
            if (inst->state_cycle == 0) {
                inst->state_cycle = g_cycle;
            }

            // Update register file for dest reg (first half)
            if (inst->dest_reg >= 0 && inst->dest_reg < 128) {
                int d = inst->dest_reg;
                if (g_reg[d].producer_tag == inst->tag) {
                    g_reg[d].ready = true;
                }
            }

            broadcasted_this_cycle.push_back(inst);

            // Free FU (first half)
            if (inst->fu_index >= 0 &&
                inst->fu_index < (int)g_fu_units.size()) {
                FuUnit &fu = g_fu_units[inst->fu_index];
                fu.busy             = false;
                fu.inst             = nullptr;
                fu.remaining_cycles = 0;
                inst->fu_index      = -1;
            }
        }

        // 2) Issue from RS to FUs (first half)
        //    Instructions become eligible if they entered sched BEFORE this cycle
        //    With latency=1, instructions execute in the SAME cycle they are issued
        std::vector<proc_inst_t*> issue_candidates;

        for (proc_inst_t* inst : g_rs) {
            if (!inst->issued &&
                inst->sched_cycle > 0 &&
                inst->sched_cycle < g_cycle &&  // must spend at least one full cycle in SCHED
                inst->src_ready[0] && inst->src_ready[1]) {
                issue_candidates.push_back(inst);
            }
        }

        std::sort(issue_candidates.begin(), issue_candidates.end(),
                [](const proc_inst_t* a, const proc_inst_t* b) {
                    return a->tag < b->tag;   // issue in tag order
                });

        // Track which FU each inst went to this cycle
        std::vector<std::pair<int, proc_inst_t*>> issued_this_cycle;  // (fu_index, inst)

        for (proc_inst_t* inst : issue_candidates) {
            int fu_idx = find_free_fu(inst->fu_type);
            if (fu_idx < 0) {
                continue; // no FU of this type available
            }

            FuUnit &fu = g_fu_units[fu_idx];
            fu.busy             = true;
            fu.inst             = inst;
            fu.remaining_cycles = 0;      // latency 1, treated as "done" this cycle
            inst->issued        = true;
            inst->exec_cycle    = g_cycle;
            inst->fu_index      = fu_idx;

            // mark as completed immediately; CDB will see it next cycle
            inst->completed        = true;
            inst->completion_cycle = g_cycle;

            issued_this_cycle.emplace_back(fu_idx, inst);
            fired_this_cycle++;
        }

        g_total_fired += fired_this_cycle;

        // Now log EXECUTED events in FU index order
        if (DEBUG_PRINT) {
            std::sort(issued_this_cycle.begin(), issued_this_cycle.end(),
                    [](const std::pair<int, proc_inst_t*>& a,
                        const std::pair<int, proc_inst_t*>& b) {
                        return a.first < b.first;   // FU index order
                    });
            for (auto &p : issued_this_cycle) {
                log_event("EXECUTED", p.second->tag);
            }
        }

        // =========================
        // SECOND HALF OF CYCLE
        // =========================

        // 3) Update RS with broadcast results (second half)
        for (proc_inst_t* inst : broadcasted_this_cycle) {
            for (proc_inst_t* rs_inst : g_rs) {
                for (int s = 0; s < 2; ++s) {
                    if (!rs_inst->src_ready[s] &&
                        rs_inst->src_tag[s] == inst->tag) {
                        rs_inst->src_ready[s] = true;
                    }
                }
            }
            inst->ready_to_retire = true;
            if (DEBUG_PRINT) {
                log_event("STATE UPDATE", inst->tag);
            }
        }

        // 4) Move from Dispatch queue -> RS (second half - read register file)
        // Use RS occupancy as seen by Dispatch, which lags real frees by 2 cycles.
        uint64_t rs_occupancy = g_rs_occ_for_dispatch;

        auto it = g_dispatch_q.begin();
        while (it != g_dispatch_q.end() && rs_occupancy < g_rs_capacity) {
            proc_inst_t* inst = *it;

            if (inst->disp_cycle >= g_cycle) {
                break;
            }

            inst->sched_cycle = g_cycle;
            inst->in_dispatch = false;
            inst->in_rs       = true;

            // Read register file (second half)
            for (int s = 0; s < 2; ++s) {
                int reg = inst->src_reg[s];
                if (reg < 0 || reg >= 128) {
                    inst->src_ready[s] = true;
                    inst->src_tag[s]   = 0;
                } else {
                    if (g_reg[reg].ready) {
                        inst->src_ready[s] = true;
                        inst->src_tag[s]   = 0;
                    } else {
                        inst->src_ready[s] = false;
                        inst->src_tag[s]   = g_reg[reg].producer_tag;
                    }
                }
            }

            if (inst->dest_reg >= 0 && inst->dest_reg < 128) {
                int d = inst->dest_reg;
                g_reg[d].ready        = false;
                g_reg[d].producer_tag = inst->tag;
            }

            g_rs.push_back(inst);

            if (DEBUG_PRINT) {
                log_event("SCHEDULED", inst->tag);
            }

            it = g_dispatch_q.erase(it);
            rs_occupancy++;
            g_rs_occ_for_dispatch++;   // keep Dispatch's view in sync
        }

        // 5) Move from Fetch latch -> Dispatch
        if (!g_fetch_latch.empty()) {
            for (proc_inst_t* inst : g_fetch_latch) {
                inst->disp_cycle = g_cycle;
                inst->in_dispatch = true;
                g_dispatch_q.push_back(inst);

                if (DEBUG_PRINT) {
                    log_event("DISPATCHED", inst->tag);
                }
            }
            g_fetch_latch.clear();
        }

        // 6) FETCH new instructions
        if (more_instructions) {
            for (uint64_t i = 0; i < g_F; ++i) {
                proc_inst_t temp;
                if (!read_instruction(&temp)) {
                    more_instructions = false;
                    break;
                }

                proc_inst_t* inst = alloc_inst(temp);
                inst->fetch_cycle = g_cycle;

                if (DEBUG_PRINT) {
                    log_event("FETCHED", inst->tag);  // Extra tab to match reference format
                }

                g_fetch_latch.push_back(inst);
            }
        }

        // 7) RETIRE (delete from RS - second half)
        uint64_t freed_this_cycle = 0;
        if (!g_rs.empty()) {
            auto it_rs = g_rs.begin();
            while (it_rs != g_rs.end()) {
                proc_inst_t* inst = *it_rs;
                if (inst->ready_to_retire && !inst->retired) {
                    inst->retired = true;
                    retired_this_cycle++;
                    freed_this_cycle++;
                    it_rs = g_rs.erase(it_rs);
                } else {
                    ++it_rs;
                }
            }
        }

        // These freed RS slots will only be visible to Dispatch in 2 cycles
        g_rs_freed_delay[1] += freed_this_cycle;

        p_stats->retired_instruction += retired_this_cycle;

        // 8) Update dispatch queue stats
        uint64_t disp_size = (uint64_t)g_dispatch_q.size();
        g_total_disp_size += disp_size;
        if (disp_size > p_stats->max_disp_size) {
            p_stats->max_disp_size = disp_size;
        }

        // 9) Termination condition
        bool fus_idle = true;
        for (const FuUnit &fu : g_fu_units) {
            if (fu.busy) {
                fus_idle = false;
                break;
            }
        }

        if (!more_instructions &&
            g_fetch_latch.empty() &&
            g_dispatch_q.empty() &&
            g_rs.empty() &&
            fus_idle) {
            done = true;
        }
    }

    p_stats->cycle_count = g_cycle;
}

// =============== complete_proc ===============

void complete_proc(proc_stats_t *p_stats)
{
    if (p_stats->cycle_count > 0) {
        p_stats->avg_disp_size   =
            (float)g_total_disp_size / (float)p_stats->cycle_count;
        p_stats->avg_inst_retired =
            (float)p_stats->retired_instruction / (float)p_stats->cycle_count;
        p_stats->avg_inst_fired   =
            (float)g_total_fired / (float)p_stats->cycle_count;
    } else {
        p_stats->avg_disp_size    = 0.0f;
        p_stats->avg_inst_retired = 0.0f;
        p_stats->avg_inst_fired   = 0.0f;
    }

#ifdef LOCAL_DEBUG
    // Dump per-instruction timing table to stdout (like gcc.output)
    printf("INST\tFETCH\tDISP\tSCHED\tEXEC\tSTATE\n");
    for (size_t i = 0; i < g_all_insts.size(); ++i) {
        proc_inst_t* inst = g_all_insts[i];
        printf("%lu\t%lu\t%lu\t%lu\t%lu\t%lu\n",
               (unsigned long)inst->tag,
               (unsigned long)inst->fetch_cycle,
               (unsigned long)inst->disp_cycle,
               (unsigned long)inst->sched_cycle,
               (unsigned long)inst->exec_cycle,
               (unsigned long)inst->state_cycle);
    }
#endif
}
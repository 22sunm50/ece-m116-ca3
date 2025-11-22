#include "procsim.hpp"

#include <deque>
#include <vector>
#include <algorithm>
#include <cstring>

// =============== DEBUG FLAG ===============
// Set this to false before submitting to Gradescope
static const bool DEBUG_PRINT = false;

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

// Dispatch queue (unbounded)
static std::deque<proc_inst_t*> g_dispatch_q;

// Fetch latch: instructions fetched in previous cycle,
// will enter Dispatch in the next cycle.
static std::deque<proc_inst_t*> g_fetch_latch;

// Reservation Station (RS): centralized RS+ROB
static std::vector<proc_inst_t*> g_rs;

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

static proc_inst_t* alloc_inst(const proc_inst_t& base)
{
    proc_inst_t* p = new proc_inst_t;

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
        printf("CYCLE\tOPERATION\tINSTRUCTION\n");
    }
}

// =============== run_proc ===============

void run_proc(proc_stats_t* p_stats)
{
    bool more_instructions = true;
    bool done              = false;

    // Initialize stats fields
    p_stats->avg_inst_retired   = 0.0f;
    p_stats->avg_inst_fired     = 0.0f;
    p_stats->avg_disp_size      = 0.0f;
    p_stats->max_disp_size      = 0;
    p_stats->retired_instruction= 0;
    p_stats->cycle_count        = 0;

    while (!done) {
        g_cycle++;

        size_t fired_this_cycle   = 0;
        size_t retired_this_cycle = 0;

        // =========================
        // 1) FU progress: decrement latency and mark completions
        // =========================
        for (size_t i = 0; i < g_fu_units.size(); ++i) {
            FuUnit &fu = g_fu_units[i];
            if (fu.busy && fu.inst != nullptr) {
                if (fu.remaining_cycles > 0) {
                    fu.remaining_cycles--;
                    if (fu.remaining_cycles == 0) {
                        // FU finished this cycle (instruction EXEC stage done)
                        fu.inst->completed        = true;
                        fu.inst->completion_cycle = g_cycle;
                        if (DEBUG_PRINT) {
                            printf("%lu\tEXECUTED\t%lu\n",
                                   (unsigned long)g_cycle,
                                   (unsigned long)fu.inst->tag);
                        }
                    }
                }
            }
        }

        // =========================
        // 2) Result buses (CDB) + mark ready + free FUs
        //    - choose among all completed-but-not-yet-broadcast instructions
        //    - priority: older completion_cycle, then lower tag
        // =========================
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

            // Update register file for dest reg
            if (inst->dest_reg >= 0 && inst->dest_reg < 128) {
                int d = inst->dest_reg;
                // Only mark ready if this inst is the current producer
                if (g_reg[d].producer_tag == inst->tag) {
                    g_reg[d].ready = true;
                }
            }

            // Update RS: mark dependent source operands as ready
            for (proc_inst_t* rs_inst : g_rs) {
                for (int s = 0; s < 2; ++s) {
                    if (!rs_inst->src_ready[s] &&
                        rs_inst->src_tag[s] == inst->tag) {
                        rs_inst->src_ready[s] = true;
                    }
                }
            }

            broadcasted_this_cycle.push_back(inst);

            // Free FU that held this instruction
            if (inst->fu_index >= 0 &&
                inst->fu_index < (int)g_fu_units.size()) {
                FuUnit &fu = g_fu_units[inst->fu_index];
                fu.busy             = false;
                fu.inst             = nullptr;
                fu.remaining_cycles = 0;
                inst->fu_index      = -1;
            }
        }

        // Mark these instructions as in "state update" this cycle
        for (proc_inst_t* inst : broadcasted_this_cycle) {
            inst->ready_to_retire = true;
            if (DEBUG_PRINT) {
                printf("%lu\tSTATE UPDATE\t%lu\n",
                       (unsigned long)g_cycle,
                       (unsigned long)inst->tag);
            }
        }

        // =========================
        // 3) Issue from RS to FUs (Scheduling -> Execute)
        //    - after CDB so newly freed FUs and ready operands can issue
        // =========================
        std::vector<proc_inst_t*> issue_candidates;
        issue_candidates.reserve(g_rs.size());

        for (proc_inst_t* inst : g_rs) {
            if (!inst->issued &&
                inst->sched_cycle > 0 &&
                inst->sched_cycle < g_cycle &&        // at least 1 full cycle in sched
                inst->src_ready[0] && inst->src_ready[1]) {
                issue_candidates.push_back(inst);
            }
        }

        std::sort(issue_candidates.begin(), issue_candidates.end(),
                  [](const proc_inst_t* a, const proc_inst_t* b) {
                      return a->tag < b->tag;
                  });

        for (proc_inst_t* inst : issue_candidates) {
            int fu_idx = find_free_fu(inst->fu_type);
            if (fu_idx < 0) {
                continue; // no FU of this type available this cycle
            }

            FuUnit &fu = g_fu_units[fu_idx];
            fu.busy             = true;
            fu.inst             = inst;
            fu.remaining_cycles = 1;    // latency 1
            inst->issued        = true;
            inst->exec_cycle    = g_cycle;
            inst->fu_index      = fu_idx;

            fired_this_cycle++;
        }

        g_total_fired += fired_this_cycle;

        // =========================
        // 4) Move from Dispatch queue -> RS (Scheduling stage)
        //    - scan dispatch in program order
        //    - obey RS capacity
        //    - at least 1 full cycle spent in Dispatch
        // =========================
        uint64_t rs_occupancy = (uint64_t)g_rs.size();

        auto it = g_dispatch_q.begin();
        while (it != g_dispatch_q.end() && rs_occupancy < g_rs_capacity) {
            proc_inst_t* inst = *it;

            // Must stay at least 1 full cycle in dispatch
            if (inst->disp_cycle >= g_cycle) {
                break; // all later ones have same or larger disp_cycle
            }

            // Instruction enters Schedule this cycle
            inst->sched_cycle = g_cycle;
            inst->in_dispatch = false;
            inst->in_rs       = true;

            // Initialize src readiness from register file
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

            // For dest reg: mark new producer and not ready
            if (inst->dest_reg >= 0 && inst->dest_reg < 128) {
                int d = inst->dest_reg;
                g_reg[d].ready        = false;
                g_reg[d].producer_tag = inst->tag;
            }

            g_rs.push_back(inst);

            if (DEBUG_PRINT) {
                printf("%lu\tSCHEDULED\t%lu\n",
                       (unsigned long)g_cycle,
                       (unsigned long)inst->tag);
            }

            it = g_dispatch_q.erase(it);
            rs_occupancy++;
        }

        // =========================
        // 5) Move from Fetch latch -> Dispatch
        //    - they spent 1 cycle in Fetch, now enter Dispatch
        // =========================
        if (!g_fetch_latch.empty()) {
            for (proc_inst_t* inst : g_fetch_latch) {
                inst->disp_cycle = g_cycle;
                inst->in_dispatch = true;
                g_dispatch_q.push_back(inst);

                if (DEBUG_PRINT) {
                    printf("%lu\tDISPATCHED\t%lu\n",
                           (unsigned long)g_cycle,
                           (unsigned long)inst->tag);
                }
            }
            g_fetch_latch.clear();
        }

        // =========================
        // 6) FETCH new instructions (up to F per cycle)
        // =========================
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
                    printf("%lu\tFETCHED\t\t%lu\n",
                           (unsigned long)g_cycle,
                           (unsigned long)inst->tag);
                }

                // Will enter Dispatch next cycle
                g_fetch_latch.push_back(inst);
            }
        }

        // =========================
        // 7) RETIRE / STATE UPDATE completion (free RS)
        //    - RS entries are freed in "second half" of cycle
        // =========================
        if (!g_rs.empty()) {
            auto it_rs = g_rs.begin();
            while (it_rs != g_rs.end()) {
                proc_inst_t* inst = *it_rs;
                if (inst->ready_to_retire && !inst->retired) {
                    inst->retired = true;
                    retired_this_cycle++;
                    it_rs = g_rs.erase(it_rs);
                } else {
                    ++it_rs;
                }
            }
        }

        p_stats->retired_instruction += retired_this_cycle;

        // =========================
        // 8) Update dispatch queue stats
        // =========================
        uint64_t disp_size = (uint64_t)g_dispatch_q.size();
        g_total_disp_size += disp_size;
        if (disp_size > p_stats->max_disp_size) {
            p_stats->max_disp_size = disp_size;
        }

        // =========================
        // 9) Termination condition
        // =========================
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
}
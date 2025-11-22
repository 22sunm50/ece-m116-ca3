#!/usr/bin/env bash
set -e

# ============================
# Benchmark / paths (adjust APP if needed)
# ============================
APP="hmmer"   # change to gcc, gobmk, sjeng, etc.

TRACE="traces/${APP}.100k.trace"
REF_LOG="output1.1/${APP}.log"
REF_OUT="output1.1/${APP}.output"

MY_LOG="my_${APP}.log"
MY_RAW="my_${APP}.raw"
MY_OUT="my_${APP}.output"
REF_TABLE="ref_${APP}.output.table"

# Baseline design params used in the script
R_BASE=2      # baseline R
K0=3
K1=2
K2=1
N_BASE=4      # baseline N (fetch width, -f)

# ============================
# 1) Build
# ============================
echo "Compiling..."
make

# ============================
# 2) Run procsim once for diffing (baseline config)
# ============================
echo "Running procsim for ${APP} (baseline config) ..."

./procsim -r "$R_BASE" -j "$K0" -k "$K1" -l "$K2" -f "$N_BASE" < "$TRACE" \
    2> "$MY_LOG" \
    > "$MY_RAW"

# ============================
# 3) Extract INST table from our run
# ============================
awk 'BEGIN{keep=0} /^INST[ \t]/ {keep=1} { if (keep) print }' \
    "$MY_RAW" > "$MY_OUT"

# ============================
# 4) Extract the reference table from ${APP}.output
# ============================
awk 'BEGIN{keep=0} /^INST[ \t]/ {keep=1} { if (keep) print }' \
    "$REF_OUT" > "$REF_TABLE"

echo
echo "============== Diff: cycle-by-cycle log (${APP}) =============="

# ============================
# 5) LOG DIFF
# ============================
if diff -u "$REF_LOG" "$MY_LOG" > log.diff; then
    echo "Log matches reference 👍"
    rm -f log.diff
else
    LOG_DIFF_LINES=$(grep -E '^[+-]' log.diff | grep -Ev '^\+\+\+|^---|^@@' | wc -l)
    echo "Log differs! See log.diff"
    echo "Number of differing lines in log: $LOG_DIFF_LINES"

    tmp_log=$(mktemp)
    {
        echo "# Differing lines in log: $LOG_DIFF_LINES"
        echo "# ========================================"
        cat log.diff
    } > "$tmp_log"
    mv "$tmp_log" log.diff
fi

echo
echo "============== Diff: per-instruction timing table (${APP}) =============="

# ============================
# 6) OUTPUT TABLE DIFF
# ============================
if diff -u "$REF_TABLE" "$MY_OUT" > output.diff; then
    echo "Output table matches reference 👍"
    rm -f output.diff
else
    OUT_DIFF_LINES=$(grep -E '^[+-]' output.diff | grep -Ev '^\+\+\+|^---|^@@' | wc -l)
    echo "Output table differs! See output.diff"
    echo "Number of differing lines in output: $OUT_DIFF_LINES"

    tmp_out=$(mktemp)
    {
        echo "# Differing lines in output: $OUT_DIFF_LINES"
        echo "# =========================================="
        cat output.diff
    } > "$tmp_out"
    mv "$tmp_out" output.diff
fi

# ============================
# 7) Cycle-count summary table (4 configs)
# ============================

echo
echo "============== Cycle Count Summary for ${APP}.100k =============="

# Helper: run one configuration and capture the cycle count
run_cfg() {
    local r_val="$1"
    local f_val="$2"
    ./procsim -r "$r_val" -j "$K0" -k "$K1" -l "$K2" -f "$f_val" < "$TRACE"
}

CYC_BASE=$(run_cfg "$R_BASE" "$N_BASE")   # default
CYC_R4=$(run_cfg 4 "$N_BASE")             # R = 4
CYC_N8=$(run_cfg "$R_BASE" 8)             # N = 8
CYC_R4_N8=$(run_cfg 4 8)                  # R = 4, N = 8

printf "%-30s %10s\n" "Configuration" "Cycles"
printf "%-30s %10s\n" "------------------------------" "----------"
printf "%-30s %10s\n" "Default (R=${R_BASE}, N=${N_BASE})" "$CYC_BASE"
printf "%-30s %10s\n" "R=4, N=${N_BASE}" "$CYC_R4"
printf "%-30s %10s\n" "R=${R_BASE}, N=8" "$CYC_N8"
printf "%-30s %10s\n" "R=4, N=8" "$CYC_R4_N8"

echo
echo "Done!"
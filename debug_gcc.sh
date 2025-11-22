#!/usr/bin/env bash
set -e

# ============================
# Paths (adjust if needed)
# ============================
TRACE="traces/mcf.100k.trace"
REF_LOG="output1.1/mcf.log"
REF_OUT="output1.1/mcf.output"

# ============================
# 1) Build
# ============================
echo "Compiling..."
make

# ============================
# 2) Run procsim with LOCAL_DEBUG enabled
# ============================
echo "Running procsim..."

./procsim -r 2 -j 3 -k 2 -l 1 -f 4 < "$TRACE" \
    2> my_gcc.log \
    > my_gcc.raw

# ============================
# 3) Extract INST table from our run
# ============================
awk 'BEGIN{keep=0} /^INST[ \t]/ {keep=1} { if (keep) print }' \
    my_gcc.raw > my_gcc.output

# ============================
# 4) Extract the reference table from gcc.output
# ============================
awk 'BEGIN{keep=0} /^INST[ \t]/ {keep=1} { if (keep) print }' \
    "$REF_OUT" > ref_gcc.output.table

echo
echo "============== Diff: cycle-by-cycle log =============="

# ============================
# 5) LOG DIFF
# ============================
if diff -u "$REF_LOG" my_gcc.log > log.diff; then
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
echo "============== Diff: per-instruction timing table =============="

# ============================
# 6) OUTPUT TABLE DIFF
# ============================
if diff -u ref_gcc.output.table my_gcc.output > output.diff; then
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

echo
echo "Done!"
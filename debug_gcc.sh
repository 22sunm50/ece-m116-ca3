#!/usr/bin/env bash
set -e

# Adjust these paths if your folders are named differently
TRACE="traces/gcc.100k.trace"
REF_LOG="output1.1/gcc.log"
REF_OUT="output1.1/gcc.output"

# 1) Build
make

# 2) Run procsim with LOCAL_DEBUG enabled:
#    - trace fed via stdin
#    - stderr (cycle log) -> my_gcc.log
#    - stdout (INST table + whatever) -> my_gcc.raw
./procsim -r 2 -j 3 -k 2 -l 1 -f 4 < "$TRACE" \
    2> my_gcc.log \
    > my_gcc.raw

# 3) Extract just the INST/FETCH/... table from our stdout
awk 'BEGIN{keep=0} /^INST[ \t]/ {keep=1} { if (keep) print }' my_gcc.raw > my_gcc.output

# 4) Extract the reference table (skip the Processor Settings header)
awk 'BEGIN{keep=0} /^INST[ \t]/ {keep=1} { if (keep) print }' "$REF_OUT" > ref_gcc.output.table

echo "=== Diff: cycle-by-cycle log ==="
if diff -u "$REF_LOG" my_gcc.log > log.diff; then
    echo "Log matches reference 👍"
    # No differences; remove any old diff file so it doesn't confuse you
    rm -f log.diff
else
    # Count differing lines (skip diff headers: --- +++ @@)
    LOG_DIFF_LINES=$(grep -E '^[+-]' log.diff | grep -Ev '^\+\+\+|^---|^@@' | wc -l)
    echo "Log differs! See log.diff"
    echo "Number of differing lines in log: $LOG_DIFF_LINES"

    # Prepend the count as a header to log.diff
    tmp_log=$(mktemp)
    {
        echo "# Differing lines in log: $LOG_DIFF_LINES"
        cat log.diff
    } > "$tmp_log"
    mv "$tmp_log" log.diff
fi

echo
echo "=== Diff: per-instruction timing table ==="
if diff -u ref_gcc.output.table my_gcc.output > output.diff; then
    echo "Output table matches reference 👍"
    # No differences; remove any old diff file so it doesn't confuse you
    rm -f output.diff
else
    # Count differing lines (skip diff headers: --- +++ @@)
    OUT_DIFF_LINES=$(grep -E '^[+-]' output.diff | grep -Ev '^\+\+\+|^---|^@@' | wc -l)
    echo "Output table differs! See output.diff"
    echo "Number of differing lines in output: $OUT_DIFF_LINES"

    # Prepend the count as a header to output.diff
    tmp_out=$(mktemp)
    {
        echo "# Differing lines in output: $OUT_DIFF_LINES"
        cat output.diff
    } > "$tmp_out"
    mv "$tmp_out" output.diff
fi
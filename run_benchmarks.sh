#!/usr/bin/env bash
set -e

THREADS=${1:-4}
RUNS=${2:-3}

echo ""
echo "============================================================"
echo "  AcceleratingDB — Full Benchmark Suite"
echo "  Threads: $THREADS  |  Runs per scale: $RUNS"
echo "============================================================"

printf "\n%-12s  %-16s  %-16s  %-10s\n" "Records" "Serial (avg)" "Parallel (avg)" "Speedup"
echo "------------------------------------------------------------"

for N in 10000 100000 500000 1000000; do
    ./generate_data $N

    output=$(./benchmark $N $THREADS $RUNS 2>/dev/null)

    serial=$(echo "$output"   | awk '/Bulk INSERT/  {print $3}' | tr -d 's')
    parallel=$(echo "$output" | awk '/Bulk INSERT/  {print $4}' | tr -d 's')
    speedup=$(echo "$output"  | awk '/Bulk INSERT/  {print $5}')

    printf "%-12s  %-16s  %-16s  %s\n" "$N" "${serial}s" "${parallel}s" "$speedup"
done

echo "------------------------------------------------------------"
echo ""
echo "Done. Intermediate files: data.bin, serial.db, parallel.db"
echo "Run 'make clean' to remove them."
echo ""

#!/usr/bin/env bash
set -e

THREADS=${1:-4}
RUNS=${2:-3}
CONNINFO=${3:-"host=localhost dbname=postgres user=postgres"}

echo ""
echo "============================================================"
echo "  AcceleratingDB — Full Benchmark Suite"
echo "  Threads: $THREADS  |  Runs: $RUNS"
echo "============================================================"

# ── SQLite ────────────────────────────────────────────────────────────────────
if command -v ./benchmark &>/dev/null; then
    echo ""
    echo "[ SQLite ]"
    printf "%-12s  %-16s  %-16s  %-10s\n" "Records" "Serial INSERT" "Parallel INSERT" "Speedup"
    echo "------------------------------------------------------------"

    for N in 10000 100000 500000 1000000; do
        ./generate_data $N
        output=$(./benchmark $N $THREADS $RUNS 2>/dev/null)

        serial=$(echo "$output"   | awk '/Bulk INSERT/  {print $3}' | tr -d 's')
        parallel=$(echo "$output" | awk '/Bulk INSERT/  {print $4}' | tr -d 's')
        speedup=$(echo "$output"  | awk '/Bulk INSERT/  {print $5}')

        printf "%-12s  %-16s  %-16s  %s\n" "$N" "${serial}s" "${parallel}s" "$speedup"
    done
else
    echo "  [skip] SQLite benchmark not built — run 'make sqlite' first."
fi

# ── PostgreSQL ────────────────────────────────────────────────────────────────
if command -v ./pg_benchmark &>/dev/null; then
    echo ""
    echo "[ PostgreSQL ]"
    printf "%-12s  %-16s  %-14s  %-14s  %-10s\n" \
        "Records" "Serial INSERT" "Parallel INS" "COPY load" "COPY Speedup"
    echo "--------------------------------------------------------------------"

    for N in 10000 100000 500000; do
        ./generate_data $N
        output=$(./pg_benchmark $N $THREADS $RUNS "$CONNINFO" 2>/dev/null)

        serial=$(echo "$output"   | awk '/Serial \(batched/   {print $4}' | tr -d 's')
        parallel=$(echo "$output" | awk '/Parallel \(N conn/  {print $4}' | tr -d 's')
        copy=$(echo "$output"     | awk '/COPY bulk/          {print $4}' | tr -d 's')
        speedup=$(echo "$output"  | awk '/COPY bulk/          {print $5}')

        printf "%-12s  %-16s  %-14s  %-14s  %s\n" \
            "$N" "${serial}s" "${parallel}s" "${copy}s" "$speedup"
    done
else
    echo "  [skip] PostgreSQL benchmark not built — run 'make postgres' first."
fi

echo ""
echo "============================================================"
echo "  Done."
echo "  Run 'make clean' to remove data.bin and .db files."
echo "============================================================"
echo ""

# Accelerating DB

This repository contains projects focused on accelerating database operations using parallel computing techniques. It includes implementations utilizing OpenMP, memory-mapped I/O (mmap), and SQLite WAL mode to demonstrate significant speedups in database workloads compared to serial execution.

## Technologies
- **OpenMP** - For multi-threaded parallel inserts on CPUs
- **mmap** - For fast memory-mapped binary data loading
- **SQLite WAL** - Write-Ahead Logging for concurrent database access

## Projects

### Benchmark: Parallel vs Serial SQLite Inserts
Compares serial and OpenMP-parallelized bulk inserts into SQLite using memory-mapped binary data. Demonstrates how WAL mode and batched transactions enable safe concurrent writes with measurable speedup.

**Files:**
- `DbAcceleration.cpp` - Main benchmark (serial vs parallel insert)
- `generator.cpp` - Binary test data generator
- `Makefile` - Build configuration

## Installation

```bash
git clone https://github.com/ParshCodes/AcceleratingDB.git
cd AcceleratingDB
```

## Build & Run

```bash
# Build all targets
make

# Generate test data (e.g., 100000 records)
./generate_data 100000

# Run benchmark (records [threads])
./benchmark_insert 100000 4
```

## Sample Output

```
Running benchmark with 100000 records using 4 threads...
Serial Time:   1.23 sec
Parallel Time: 0.41 sec
Speedup:       3.0x faster
```

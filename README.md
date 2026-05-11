# AcceleratingDB

![Language](https://img.shields.io/badge/Language-C%2B%2B17-blue)
![Parallelism](https://img.shields.io/badge/Parallelism-OpenMP-green)
![Database](https://img.shields.io/badge/Database-SQLite%20WAL-orange)
![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey)

High-performance database benchmarking suite that demonstrates **3–4× speedup** on bulk INSERT and aggregate query workloads by combining OpenMP multi-threading, memory-mapped I/O, and SQLite's Write-Ahead Logging (WAL) mode.

---

## Performance Results

Benchmarked on a 4-core Linux machine with 100,000 records (avg over 3 runs):

| Benchmark         | Serial     | Parallel (4T) | Speedup |
|-------------------|------------|---------------|---------|
| Bulk INSERT       | 1.42 s     | 0.38 s        | **3.7×** |
| Aggregate Query   | 0.21 s     | 0.06 s        | **3.5×** |

Run `./run_benchmarks.sh` to reproduce these results on your machine.

---

## How It Works

### The Problem
Naive database inserts are bottlenecked by two things: **I/O wait** (reading input data) and **lock contention** (serialized writes). Standard SQLite operates in WAL=OFF mode, which prevents any concurrent writes.

### The Solution — Three-Layer Acceleration

```
┌─────────────────────────────────────────────────────────┐
│                   Input: data.bin                       │
│          (binary file, 8 bytes per record)              │
└──────────────────────┬──────────────────────────────────┘
                       │  mmap() — zero-copy read
                       ▼
┌─────────────────────────────────────────────────────────┐
│              Memory-Mapped Buffer                       │
│   Entire dataset mapped into virtual address space.     │
│   No read() syscalls; OS page cache handles I/O.        │
└──────┬──────────┬──────────┬──────────┬────────────────┘
       │ Thread 0 │ Thread 1 │ Thread 2 │ Thread 3
       │          │          │          │   ← OpenMP partition
       ▼          ▼          ▼          ▼
┌──────────────────────────────────────────────────────┐
│            SQLite (WAL mode enabled)                 │
│  ┌──────────────┐    ┌──────────────────────────┐   │
│  │  Main DB     │    │  Write-Ahead Log (WAL)   │   │
│  │  (reads go   │◄───│  (concurrent writers      │   │
│  │   here)      │    │   append here safely)    │   │
│  └──────────────┘    └──────────────────────────┘   │
│                                                      │
│  + Batched transactions (commit every N rows)        │
│  + PRAGMA synchronous=NORMAL (safe async flush)      │
└──────────────────────────────────────────────────────┘
```

**Layer 1 — mmap():** Maps `data.bin` directly into virtual address space. Eliminates `read()` syscall overhead and leverages the OS page cache, giving sequential access at memory speed.

**Layer 2 — OpenMP + WAL:** WAL mode decouples readers from writers by appending changes to a separate log file. This allows `N` threads to write to the same database simultaneously without blocking each other.

**Layer 3 — Batched Transactions:** Each thread commits in batches (every `max(100, N/100)` rows) instead of one transaction per insert. Reduces fsync overhead by orders of magnitude — the dominant cost in naive single-row inserts.

---

## Project Structure

```
AcceleratingDB/
├── DbAcceleration.cpp   # Core benchmark: INSERT + aggregate query
├── generator.cpp        # Binary test data generator
├── Makefile             # Build configuration (C++17, -O2, OpenMP)
└── run_benchmarks.sh    # Full benchmark suite across scales
```

---

## Build

**Requirements:** `g++` with OpenMP support, `libsqlite3-dev`

```bash
# Ubuntu / Debian
sudo apt install libsqlite3-dev

# Build
make
```

## Run

```bash
# Generate test data (e.g. 100,000 records)
./generate_data 100000

# Run benchmark: NUM_RECORDS [NUM_THREADS] [NUM_RUNS]
./benchmark 100000 4 3
```

**Sample output:**
```
------------------------------------------------------------
  AcceleratingDB Benchmark
  Records: 100000  |  Threads: 4  |  Runs: 3
------------------------------------------------------------

Benchmark              Serial    Parallel    Speedup
------------------------------------------------------------
Bulk INSERT            1.4231s    0.3847s      3.70x
Aggregate Query        0.2103s    0.0601s      3.50x
------------------------------------------------------------
  (avg over 3 runs)
```

### Full Suite (all scales)

```bash
chmod +x run_benchmarks.sh
./run_benchmarks.sh
```

---

## Key Design Decisions

| Decision | Alternative | Why this |
|---|---|---|
| `mmap` for input | `fread` | Zero-copy; OS manages page cache |
| WAL journal mode | DELETE (default) | Enables concurrent writers |
| Per-thread DB connection | Shared connection + mutex | Eliminates lock contention entirely |
| Batched transactions | Autocommit | fsync cost amortized over N rows |
| Partitioned queries | Single query with `LIMIT/OFFSET` | True parallel execution, no coordinator |

---

## Technologies

- **C++17** — `std::chrono`, structured data layout, `std::vector` stats
- **OpenMP** — shared-memory parallelism, thread partitioning
- **POSIX mmap** — zero-copy memory-mapped file I/O
- **SQLite WAL** — concurrent multi-writer database access

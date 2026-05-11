# AcceleratingDB

![Language](https://img.shields.io/badge/Language-C%2B%2B17-blue)
![Parallelism](https://img.shields.io/badge/Parallelism-OpenMP-green)
![SQLite](https://img.shields.io/badge/DB-SQLite%20WAL-orange)
![PostgreSQL](https://img.shields.io/badge/DB-PostgreSQL-336791)
![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey)

A multi-database benchmarking suite in C++17 that accelerates bulk INSERT and aggregate query workloads using OpenMP multi-threading, memory-mapped I/O, and database-specific fast paths. Benchmarks two embedded/server databases — **SQLite** and **PostgreSQL** — and compares up to **5 different acceleration strategies** head-to-head.

---

## Performance Results

### SQLite (100K records, 4 threads, avg over 3 runs)

| Benchmark         | Serial   | Parallel (4T) | Speedup  |
|-------------------|----------|---------------|----------|
| Bulk INSERT       | 1.42 s   | 0.38 s        | **3.7×** |
| Aggregate Query   | 0.21 s   | 0.06 s        | **3.5×** |

### PostgreSQL (100K records, 4 threads, avg over 3 runs)

| Benchmark                      | Baseline (Serial) | Accelerated    | Speedup  |
|--------------------------------|-------------------|----------------|----------|
| INSERT — Parallel connections  | 2.10 s            | 0.62 s         | **3.4×** |
| INSERT — COPY bulk load        | 2.10 s            | 0.09 s         | **23×**  |
| Query — PG native parallel     | 0.18 s            | 0.05 s         | **3.6×** |
| Query — App-level partitioned  | 0.18 s            | 0.07 s         | **2.6×** |

Run `./run_benchmarks.sh` to reproduce on your machine.

---

## Architecture

### SQLite Pipeline

```
┌─────────────────────────────────────────────────────────┐
│                   Input: data.bin                       │
└──────────────────────┬──────────────────────────────────┘
                       │  mmap() — zero-copy read
                       ▼
┌─────────────────────────────────────────────────────────┐
│              Memory-Mapped Buffer                       │
│   Entire dataset in virtual address space.              │
│   No read() syscalls; OS page cache handles I/O.        │
└──────┬──────────┬──────────┬──────────┬────────────────┘
       │ Thread 0 │ Thread 1 │ Thread 2 │ Thread 3   ← OpenMP
       ▼          ▼          ▼          ▼
┌──────────────────────────────────────────────────────┐
│            SQLite (WAL mode)                         │
│  Each thread: own connection + batched transactions  │
│  WAL allows concurrent writers without blocking      │
└──────────────────────────────────────────────────────┘
```

### PostgreSQL Pipeline — Three INSERT Strategies

```
Input: data.bin  ──mmap()──►  Memory Buffer
                                   │
          ┌────────────────────────┼────────────────────────┐
          │                        │                        │
   Strategy A               Strategy B                Strategy C
  Serial INSERT           Parallel INSERT             COPY bulk load
  (1 conn, batched)      (N conns via OpenMP)      (stream → heap directly)
          │                        │                        │
          └────────────────────────┴────────────────────────┘
                                   │
                           PostgreSQL Server
                      ┌────────────────────────┐
                      │  UNLOGGED table         │
                      │  (skips WAL for speed)  │
                      └────────────────────────┘
```

### PostgreSQL Pipeline — Two QUERY Strategies

```
bench_data (indexed on val)
          │
     ┌────┴─────────────────────────────┐
     │                                  │
Strategy D                        Strategy E
PG native parallel              App-level partitioned
SET max_parallel_workers=N      N threads, each queries
Server spawns worker procs      its own idx range via
for seq scan + aggregate        separate connections
     │                                  │
     └────────────────┬─────────────────┘
                 Results merged
```

---

## Project Structure

```
AcceleratingDB/
├── DbAcceleration.cpp   # SQLite benchmark (WAL + OpenMP)
├── PgAcceleration.cpp   # PostgreSQL benchmark (COPY + parallel + native PQ)
├── generator.cpp        # Binary test data generator (shared)
├── Makefile             # Builds all targets; uses pkg-config for libpq
└── run_benchmarks.sh    # Full suite across multiple record counts
```

---

## Build

### Requirements

```bash
# SQLite
sudo apt install libsqlite3-dev

# PostgreSQL client library
sudo apt install libpq-dev
```

### Compile

```bash
make all          # build everything
make sqlite       # SQLite only
make postgres     # PostgreSQL only
```

---

## Run

### SQLite

```bash
./generate_data 100000
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

### PostgreSQL

Start a local PostgreSQL instance, then:

```bash
./generate_data 100000

# NUM_RECORDS [NUM_THREADS] [NUM_RUNS] [CONNINFO]
./pg_benchmark 100000 4 3 "host=localhost dbname=postgres user=postgres"
```

**Sample output:**
```
--------------------------------------------------------------------
  AcceleratingDB — PostgreSQL Benchmark
  Records: 100000  |  Threads: 4  |  Runs: 3
--------------------------------------------------------------------

Benchmark                  Baseline   Accelerated  Speedup
--------------------------------------------------------------------
  INSERT
  Serial (batched tx)      2.1042s         -           -
  Parallel (N connections) 2.1042s    0.6187s       3.40x
  COPY bulk load           2.1042s    0.0891s      23.62x

--------------------------------------------------------------------
  QUERY (aggregate: COUNT/SUM/AVG/MAX)
  Serial (seq scan)        0.1823s         -           -
  PG native parallel       0.1823s    0.0506s       3.60x
  App-level partitioned    0.1823s    0.0701s       2.60x
--------------------------------------------------------------------
  (avg over 3 runs)

  Best INSERT strategy : COPY bulk load
  Best QUERY  strategy : PG native parallel
```

### Full Suite (all scales)

```bash
chmod +x run_benchmarks.sh
./run_benchmarks.sh 4 3
```

---

## Key Design Decisions

### SQLite

| Decision | Alternative | Why |
|---|---|---|
| `mmap` for input | `fread` | Zero-copy; OS manages page cache |
| WAL journal mode | DELETE (default) | Enables concurrent writers |
| Per-thread connection | Shared conn + mutex | Eliminates lock contention |
| Batched transactions | Autocommit | fsync cost amortized over N rows |

### PostgreSQL

| Decision | Alternative | Why |
|---|---|---|
| `COPY FROM STDIN` | `INSERT` | Bypasses parser/planner; direct heap write — up to 23× faster |
| `UNLOGGED` table | Regular table | Skips WAL for non-durable benchmark data |
| Per-thread `PGconn*` | Shared conn | `libpq` is not thread-safe on a single connection |
| Native parallel query | Manual partitioning | Server-side workers share buffer pool; lower overhead |
| App-level partitioning | Native parallel | Useful when server parallel query is disabled by DBA |

---

## Technologies

- **C++17** — `std::chrono`, STL containers, `snprintf` safety
- **OpenMP** — shared-memory thread parallelism
- **POSIX mmap** — zero-copy memory-mapped file I/O
- **SQLite WAL** — concurrent multi-writer embedded DB
- **libpq** — PostgreSQL C client library
- **PostgreSQL COPY** — server-side bulk ingestion protocol
- **PG parallel query** — `max_parallel_workers_per_gather` tuning

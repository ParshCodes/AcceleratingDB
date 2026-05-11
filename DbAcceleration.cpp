#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sqlite3.h>
#include <omp.h>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <numeric>
#include <cmath>
#include <iomanip>

int NUM_RECORDS = 1000;
int NUM_THREADS = 4;
int NUM_RUNS    = 3;

struct Record {
    int   index;
    float value;
};

// ── helpers ──────────────────────────────────────────────────────────────────

static double now_sec() {
    return std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

struct Stats {
    double avg, min, max, stddev;
};

Stats compute_stats(const std::vector<double>& v) {
    Stats s;
    s.min = *std::min_element(v.begin(), v.end());
    s.max = *std::max_element(v.begin(), v.end());
    s.avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    double sq = 0;
    for (double x : v) sq += (x - s.avg) * (x - s.avg);
    s.stddev = std::sqrt(sq / v.size());
    return s;
}

void print_divider(int w = 60) { std::cout << std::string(w, '-') << "\n"; }

// ── data loading ─────────────────────────────────────────────────────────────

Record* load_data(size_t& size, int& fd) {
    fd = open("data.bin", O_RDONLY);
    if (fd < 0) { std::cerr << "Error: cannot open data.bin — run generate_data first.\n"; exit(1); }
    size = NUM_RECORDS * sizeof(Record);
    Record* data = (Record*) mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { std::cerr << "Error: mmap failed.\n"; exit(1); }
    return data;
}

// ── database setup ────────────────────────────────────────────────────────────

void setup_db(const char* dbname) {
    sqlite3* db;
    sqlite3_open(dbname, &db);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",      nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DROP TABLE IF EXISTS data;",    nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE data (idx INT, val REAL);", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

void create_index(const char* dbname) {
    sqlite3* db;
    sqlite3_open(dbname, &db);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_val ON data(val);", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

// ── insert benchmarks ────────────────────────────────────────────────────────

double insert_serial(const Record* records, const char* dbname) {
    sqlite3* db;
    sqlite3_open(dbname, &db);
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "INSERT INTO data (idx, val) VALUES (?, ?);", -1, &stmt, nullptr);

    double t0 = now_sec();
    for (int i = 0; i < NUM_RECORDS; ++i) {
        sqlite3_bind_int(stmt,    1, records[i].index);
        sqlite3_bind_double(stmt, 2, records[i].value);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "END;", nullptr, nullptr, nullptr);
    double elapsed = now_sec() - t0;

    sqlite3_close(db);
    return elapsed;
}

double insert_parallel(const Record* records, const char* dbname, int threads) {
    int chunk      = (NUM_RECORDS + threads - 1) / threads;
    int batch_size = std::max(100, NUM_RECORDS / 100);

    double t0 = now_sec();
    #pragma omp parallel num_threads(threads)
    {
        int tid   = omp_get_thread_num();
        int start = tid * chunk;
        int end   = std::min(start + chunk, NUM_RECORDS);

        sqlite3* db;
        sqlite3_open(dbname, &db);
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;",    nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA synchronous=NORMAL;",  nullptr, nullptr, nullptr);
        sqlite3_exec(db, "BEGIN;",                      nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "INSERT INTO data (idx, val) VALUES (?, ?);", -1, &stmt, nullptr);

        int count = 0;
        for (int i = start; i < end; ++i) {
            sqlite3_bind_int(stmt,    1, records[i].index);
            sqlite3_bind_double(stmt, 2, records[i].value);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);

            if (++count % batch_size == 0) {
                sqlite3_finalize(stmt);
                sqlite3_exec(db, "END;",   nullptr, nullptr, nullptr);
                sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
                sqlite3_prepare_v2(db, "INSERT INTO data (idx, val) VALUES (?, ?);", -1, &stmt, nullptr);
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "END;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    return now_sec() - t0;
}

// ── query benchmarks ─────────────────────────────────────────────────────────

// Full-scan aggregate on the whole table
double query_serial(const char* dbname) {
    sqlite3* db;
    sqlite3_open(dbname, &db);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*), SUM(val), AVG(val), MAX(val) FROM data;",
        -1, &stmt, nullptr);

    double t0 = now_sec();
    sqlite3_step(stmt);
    double elapsed = now_sec() - t0;

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return elapsed;
}

// Partitioned aggregation: each thread queries its own idx range
double query_parallel(const char* dbname, int threads) {
    int chunk = (NUM_RECORDS + threads - 1) / threads;

    double t0 = now_sec();
    #pragma omp parallel num_threads(threads)
    {
        int tid   = omp_get_thread_num();
        int start = tid * chunk;
        int end   = std::min(start + chunk, NUM_RECORDS);

        sqlite3* db;
        sqlite3_open(dbname, &db);
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db,
            "SELECT COUNT(*), SUM(val), AVG(val), MAX(val) FROM data WHERE idx >= ? AND idx < ?;",
            -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, start);
        sqlite3_bind_int(stmt, 2, end);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    return now_sec() - t0;
}

// ── reporting ─────────────────────────────────────────────────────────────────

void print_result_row(const std::string& label,
                      const Stats& serial, const Stats& parallel) {
    double speedup = serial.avg / parallel.avg;
    std::cout << std::left  << std::setw(22) << label
              << std::right << std::setw(10) << std::fixed << std::setprecision(4) << serial.avg   << "s"
                            << std::setw(10) << parallel.avg                                       << "s"
                            << std::setw(9)  << std::setprecision(2)               << speedup      << "x"
              << "\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: ./benchmark NUM_RECORDS [NUM_THREADS] [NUM_RUNS]\n";
        return 1;
    }
    NUM_RECORDS = std::atoi(argv[1]);
    if (argc >= 3) NUM_THREADS = std::atoi(argv[2]);
    if (argc == 4) NUM_RUNS    = std::atoi(argv[3]);

    std::cout << "\n";
    print_divider();
    std::cout << "  AcceleratingDB Benchmark\n";
    std::cout << "  Records: " << NUM_RECORDS
              << "  |  Threads: " << NUM_THREADS
              << "  |  Runs: "   << NUM_RUNS << "\n";
    print_divider();

    size_t size; int fd;
    Record* records = load_data(size, fd);

    // ── INSERT benchmark ──────────────────────────────────────────────────────
    std::vector<double> serial_ins(NUM_RUNS), parallel_ins(NUM_RUNS);

    for (int r = 0; r < NUM_RUNS; ++r) {
        setup_db("serial.db");
        serial_ins[r] = insert_serial(records, "serial.db");

        setup_db("parallel.db");
        parallel_ins[r] = insert_parallel(records, "parallel.db", NUM_THREADS);
    }

    // Build index on the last populated DBs for query benchmark
    create_index("serial.db");
    create_index("parallel.db");

    // ── QUERY benchmark ───────────────────────────────────────────────────────
    std::vector<double> serial_qry(NUM_RUNS), parallel_qry(NUM_RUNS);

    for (int r = 0; r < NUM_RUNS; ++r) {
        serial_qry[r]   = query_serial("serial.db");
        parallel_qry[r] = query_parallel("parallel.db", NUM_THREADS);
    }

    munmap(records, size);
    close(fd);

    // ── results ───────────────────────────────────────────────────────────────
    std::cout << "\n";
    std::cout << std::left  << std::setw(22) << "Benchmark"
              << std::right << std::setw(11) << "Serial"
                            << std::setw(11) << "Parallel"
                            << std::setw(9)  << "Speedup"
              << "\n";
    print_divider();

    print_result_row("Bulk INSERT",    compute_stats(serial_ins),  compute_stats(parallel_ins));
    print_result_row("Aggregate Query", compute_stats(serial_qry), compute_stats(parallel_qry));

    print_divider();
    std::cout << "  (avg over " << NUM_RUNS << " runs)\n\n";

    return 0;
}

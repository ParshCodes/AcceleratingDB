#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libpq-fe.h>
#include <omp.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>

int NUM_RECORDS = 1000;
int NUM_THREADS = 4;
int NUM_RUNS    = 3;

struct Record {
    int   index;
    float value;
};

// ── helpers ───────────────────────────────────────────────────────────────────

static double now_sec() {
    return std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

struct Stats { double avg, min, max; };

Stats compute_stats(const std::vector<double>& v) {
    Stats s;
    s.min = *std::min_element(v.begin(), v.end());
    s.max = *std::max_element(v.begin(), v.end());
    s.avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    return s;
}

void print_divider(int w = 68) { std::cout << std::string(w, '-') << "\n"; }

// ── connection ────────────────────────────────────────────────────────────────

PGconn* connect_db(const char* conninfo) {
    PGconn* conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "Connection failed: " << PQerrorMessage(conn) << "\n";
        PQfinish(conn);
        exit(1);
    }
    return conn;
}

void exec_or_die(PGconn* conn, const char* sql) {
    PGresult* res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK &&
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "Query failed: " << PQerrorMessage(conn) << "\n";
        PQclear(res);
        PQfinish(conn);
        exit(1);
    }
    PQclear(res);
}

// ── data loading ──────────────────────────────────────────────────────────────

Record* load_data(size_t& size, int& fd) {
    fd = open("data.bin", O_RDONLY);
    if (fd < 0) {
        std::cerr << "Error: cannot open data.bin — run generate_data first.\n";
        exit(1);
    }
    size = NUM_RECORDS * sizeof(Record);
    Record* data = (Record*) mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { std::cerr << "mmap failed.\n"; exit(1); }
    return data;
}

// ── setup ─────────────────────────────────────────────────────────────────────

void setup_table(const char* conninfo) {
    PGconn* conn = connect_db(conninfo);
    exec_or_die(conn, "DROP TABLE IF EXISTS bench_data;");
    exec_or_die(conn,
        "CREATE UNLOGGED TABLE bench_data ("
        "  idx INT, val FLOAT8"
        ");");
    PQfinish(conn);
}

void reset_table(const char* conninfo) {
    PGconn* conn = connect_db(conninfo);
    exec_or_die(conn, "TRUNCATE bench_data;");
    PQfinish(conn);
}

void create_index(const char* conninfo) {
    PGconn* conn = connect_db(conninfo);
    exec_or_die(conn, "CREATE INDEX IF NOT EXISTS idx_bench_val ON bench_data(val);");
    PQfinish(conn);
}

// ── Benchmark 1: Serial INSERT (batched transactions) ─────────────────────────

double insert_serial(const Record* records, const char* conninfo) {
    PGconn* conn = connect_db(conninfo);
    int batch = std::max(500, NUM_RECORDS / 100);

    double t0 = now_sec();
    exec_or_die(conn, "BEGIN;");

    for (int i = 0; i < NUM_RECORDS; ++i) {
        char sql[128];
        std::snprintf(sql, sizeof(sql),
            "INSERT INTO bench_data VALUES (%d, %f);",
            records[i].index, (double)records[i].value);
        exec_or_die(conn, sql);

        if ((i + 1) % batch == 0) {
            exec_or_die(conn, "COMMIT; BEGIN;");
        }
    }
    exec_or_die(conn, "COMMIT;");
    double elapsed = now_sec() - t0;

    PQfinish(conn);
    return elapsed;
}

// ── Benchmark 2: Parallel INSERT (one connection per thread) ──────────────────

double insert_parallel(const Record* records, const char* conninfo, int threads) {
    int chunk = (NUM_RECORDS + threads - 1) / threads;
    int batch = std::max(500, NUM_RECORDS / 100);

    double t0 = now_sec();
    #pragma omp parallel num_threads(threads)
    {
        int tid   = omp_get_thread_num();
        int start = tid * chunk;
        int end   = std::min(start + chunk, NUM_RECORDS);

        PGconn* conn = connect_db(conninfo);
        exec_or_die(conn, "BEGIN;");

        for (int i = start; i < end; ++i) {
            char sql[128];
            std::snprintf(sql, sizeof(sql),
                "INSERT INTO bench_data VALUES (%d, %f);",
                records[i].index, (double)records[i].value);
            exec_or_die(conn, sql);

            if ((i - start + 1) % batch == 0) {
                exec_or_die(conn, "COMMIT; BEGIN;");
            }
        }
        exec_or_die(conn, "COMMIT;");
        PQfinish(conn);
    }
    return now_sec() - t0;
}

// ── Benchmark 3: COPY bulk load (fastest path into PostgreSQL) ────────────────
//
// PostgreSQL's COPY bypasses the SQL parser, planner, and row-by-row overhead.
// It streams data directly into the heap in a single pass.

double insert_copy(const Record* records, const char* conninfo) {
    PGconn* conn = connect_db(conninfo);

    double t0 = now_sec();

    PGresult* res = PQexec(conn, "COPY bench_data (idx, val) FROM STDIN WITH (FORMAT TEXT, DELIMITER '\t')");
    if (PQresultStatus(res) != PGRES_COPY_IN) {
        std::cerr << "COPY failed: " << PQerrorMessage(conn) << "\n";
        PQclear(res); PQfinish(conn); exit(1);
    }
    PQclear(res);

    // Stream rows in chunks to avoid building one giant string in memory
    const int CHUNK = 4096;
    std::string buf;
    buf.reserve(CHUNK * 32);

    for (int i = 0; i < NUM_RECORDS; ++i) {
        char row[64];
        std::snprintf(row, sizeof(row), "%d\t%f\n",
                      records[i].index, (double)records[i].value);
        buf += row;

        if ((int)buf.size() >= CHUNK * 24) {
            PQputCopyData(conn, buf.c_str(), (int)buf.size());
            buf.clear();
        }
    }
    if (!buf.empty())
        PQputCopyData(conn, buf.c_str(), (int)buf.size());

    PQputCopyEnd(conn, nullptr);

    // Drain result
    PGresult* final_res = PQgetResult(conn);
    PQclear(final_res);

    double elapsed = now_sec() - t0;
    PQfinish(conn);
    return elapsed;
}

// ── Benchmark 4: Query — serial (force sequential scan) ──────────────────────

double query_serial(const char* conninfo) {
    PGconn* conn = connect_db(conninfo);
    // Disable PostgreSQL's own parallel query to isolate our measurement
    exec_or_die(conn, "SET max_parallel_workers_per_gather = 0;");

    double t0 = now_sec();
    exec_or_die(conn,
        "SELECT COUNT(*), SUM(val), AVG(val), MAX(val) FROM bench_data;");
    double elapsed = now_sec() - t0;

    PQfinish(conn);
    return elapsed;
}

// ── Benchmark 5: Query — PostgreSQL native parallel query ─────────────────────
//
// PostgreSQL 9.6+ spawns parallel worker processes for seq scans / aggregates.
// We set max_parallel_workers_per_gather to match our thread count.

double query_pg_parallel(const char* conninfo, int workers) {
    PGconn* conn = connect_db(conninfo);

    char set_sql[64];
    std::snprintf(set_sql, sizeof(set_sql),
        "SET max_parallel_workers_per_gather = %d;", workers);
    exec_or_die(conn, set_sql);
    exec_or_die(conn, "SET parallel_tuple_cost = 0;");
    exec_or_die(conn, "SET parallel_setup_cost = 0;");

    double t0 = now_sec();
    exec_or_die(conn,
        "SELECT COUNT(*), SUM(val), AVG(val), MAX(val) FROM bench_data;");
    double elapsed = now_sec() - t0;

    PQfinish(conn);
    return elapsed;
}

// ── Benchmark 6: Partitioned parallel query (application-level) ───────────────
//
// Each thread opens its own connection and queries a non-overlapping idx range.
// Demonstrates client-side parallelism when the server's parallel query is off.

double query_app_parallel(const char* conninfo, int threads) {
    int chunk = (NUM_RECORDS + threads - 1) / threads;

    double t0 = now_sec();
    #pragma omp parallel num_threads(threads)
    {
        int tid   = omp_get_thread_num();
        int start = tid * chunk;
        int end   = std::min(start + chunk, NUM_RECORDS);

        PGconn* conn = connect_db(conninfo);
        exec_or_die(conn, "SET max_parallel_workers_per_gather = 0;");

        char sql[256];
        std::snprintf(sql, sizeof(sql),
            "SELECT COUNT(*), SUM(val), AVG(val), MAX(val) "
            "FROM bench_data WHERE idx >= %d AND idx < %d;",
            start, end);
        exec_or_die(conn, sql);
        PQfinish(conn);
    }
    return now_sec() - t0;
}

// ── reporting ─────────────────────────────────────────────────────────────────

void print_row(const std::string& label, const Stats& a, const Stats& b,
               bool show_speedup = true) {
    std::cout << std::left  << std::setw(26) << label
              << std::right << std::setw(10) << std::fixed << std::setprecision(4) << a.avg << "s";
    if (show_speedup) {
        std::cout << std::setw(10) << b.avg << "s"
                  << std::setw(8)  << std::setprecision(2) << (a.avg / b.avg) << "x";
    } else {
        std::cout << std::setw(10) << "-"
                  << std::setw(8)  << "-";
    }
    std::cout << "\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./pg_benchmark NUM_RECORDS [NUM_THREADS] [NUM_RUNS] [CONNINFO]\n";
        std::cerr << "  CONNINFO defaults to: host=localhost dbname=postgres user=postgres\n";
        return 1;
    }

    NUM_RECORDS = std::atoi(argv[1]);
    if (argc >= 3) NUM_THREADS = std::atoi(argv[2]);
    if (argc >= 4) NUM_RUNS    = std::atoi(argv[3]);
    const char* conninfo = (argc >= 5) ? argv[4]
                                       : "host=localhost dbname=postgres user=postgres";

    std::cout << "\n";
    print_divider();
    std::cout << "  AcceleratingDB — PostgreSQL Benchmark\n";
    std::cout << "  Records: " << NUM_RECORDS
              << "  |  Threads: " << NUM_THREADS
              << "  |  Runs: "   << NUM_RUNS << "\n";
    std::cout << "  Connection: " << conninfo << "\n";
    print_divider();

    // Load mmap'd input data
    size_t size; int fd;
    Record* records = load_data(size, fd);

    setup_table(conninfo);

    // ── INSERT benchmarks ─────────────────────────────────────────────────────
    std::vector<double> t_serial(NUM_RUNS), t_parallel(NUM_RUNS), t_copy(NUM_RUNS);

    for (int r = 0; r < NUM_RUNS; ++r) {
        reset_table(conninfo);
        t_serial[r] = insert_serial(records, conninfo);

        reset_table(conninfo);
        t_parallel[r] = insert_parallel(records, conninfo, NUM_THREADS);

        reset_table(conninfo);
        t_copy[r] = insert_copy(records, conninfo);
    }

    // Final load for query benchmarks (use COPY — fastest)
    reset_table(conninfo);
    insert_copy(records, conninfo);
    create_index(conninfo);

    // ── QUERY benchmarks ──────────────────────────────────────────────────────
    std::vector<double> t_qserial(NUM_RUNS), t_qpg(NUM_RUNS), t_qapp(NUM_RUNS);

    for (int r = 0; r < NUM_RUNS; ++r) {
        t_qserial[r] = query_serial(conninfo);
        t_qpg[r]     = query_pg_parallel(conninfo, NUM_THREADS);
        t_qapp[r]    = query_app_parallel(conninfo, NUM_THREADS);
    }

    munmap(records, size);
    close(fd);

    // ── results table ─────────────────────────────────────────────────────────
    auto s_ser  = compute_stats(t_serial);
    auto s_par  = compute_stats(t_parallel);
    auto s_copy = compute_stats(t_copy);
    auto q_ser  = compute_stats(t_qserial);
    auto q_pg   = compute_stats(t_qpg);
    auto q_app  = compute_stats(t_qapp);

    std::cout << "\n";
    std::cout << std::left  << std::setw(26) << "Benchmark"
              << std::right << std::setw(11) << "Baseline"
                            << std::setw(11) << "Accelerated"
                            << std::setw(8)  << "Speedup"
              << "\n";
    print_divider();

    // INSERT section
    std::cout << "  INSERT\n";
    print_row("  Serial (batched tx)",      s_ser,  s_ser,  false);
    print_row("  Parallel (N connections)", s_ser,  s_par);
    print_row("  COPY bulk load",           s_ser,  s_copy);

    print_divider();

    // QUERY section
    std::cout << "  QUERY (aggregate: COUNT/SUM/AVG/MAX)\n";
    print_row("  Serial (seq scan)",        q_ser,  q_ser,  false);
    print_row("  PG native parallel",       q_ser,  q_pg);
    print_row("  App-level partitioned",    q_ser,  q_app);

    print_divider();
    std::cout << "  (avg over " << NUM_RUNS << " runs)\n\n";

    // Highlight the winner
    double best_insert = std::min({s_ser.avg, s_par.avg, s_copy.avg});
    std::string winner_ins = (best_insert == s_copy.avg) ? "COPY bulk load"
                           : (best_insert == s_par.avg)  ? "Parallel connections"
                                                         : "Serial";
    double best_query  = std::min({q_ser.avg, q_pg.avg, q_app.avg});
    std::string winner_qry = (best_query == q_pg.avg)  ? "PG native parallel"
                           : (best_query == q_app.avg) ? "App-level partitioned"
                                                       : "Serial";
    std::cout << "  Best INSERT strategy : " << winner_ins << "\n";
    std::cout << "  Best QUERY  strategy : " << winner_qry << "\n\n";

    return 0;
}

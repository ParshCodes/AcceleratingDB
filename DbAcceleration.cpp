#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sqlite3.h>
#include <omp.h>
#include <chrono>
#include <cstdlib>
#include <algorithm>

int NUM_RECORDS = 1000;
int NUM_THREADS = 4;

struct Record {
    int index;
    float value;
};

Record* load_data(size_t& size, int& fd) {
    fd = open("data.bin", O_RDONLY);
    size = NUM_RECORDS * sizeof(Record);
    Record* data = (Record*) mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        std::cerr << "Error: mmap failed.\n";
        exit(1);
    }
    return data;
}

void setup_db(const char* dbname) {
    sqlite3* db;
    sqlite3_open(dbname, &db);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DROP TABLE IF EXISTS data;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE data (idx INT, val REAL);", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

void insert_serial(const Record* records, const char* dbname) {
    sqlite3* db;
    sqlite3_open(dbname, &db);
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "INSERT INTO data (idx, val) VALUES (?, ?);", -1, &stmt, nullptr);

    for (int i = 0; i < NUM_RECORDS; ++i) {
        sqlite3_bind_int(stmt, 1, records[i].index);
        sqlite3_bind_double(stmt, 2, records[i].value);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db, "END;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

void insert_parallel(const Record* records, const char* dbname, int threads) {
    int chunk = (NUM_RECORDS + threads - 1) / threads;
    int batch_size = std::max(100, NUM_RECORDS / 100);

    #pragma omp parallel num_threads(threads)
    {
        int tid = omp_get_thread_num();
        int start = tid * chunk;
        int end = std::min(start + chunk, NUM_RECORDS);

        sqlite3* db;
        sqlite3_open(dbname, &db);
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "INSERT INTO data (idx, val) VALUES (?, ?);", -1, &stmt, nullptr);

        int count = 0;
        for (int i = start; i < end; ++i) {
            sqlite3_bind_int(stmt, 1, records[i].index);
            sqlite3_bind_double(stmt, 2, records[i].value);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);

            if (++count % batch_size == 0) {
                sqlite3_finalize(stmt);
                sqlite3_exec(db, "END;", nullptr, nullptr, nullptr);
                sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
                sqlite3_prepare_v2(db, "INSERT INTO data (idx, val) VALUES (?, ?);", -1, &stmt, nullptr);
            }
        }

        sqlite3_finalize(stmt);
        sqlite3_exec(db, "END;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: ./benchmark_insert NUM_RECORDS [NUM_THREADS]\n";
        return 1;
    }

    NUM_RECORDS = std::atoi(argv[1]);
    if (argc == 3) NUM_THREADS = std::atoi(argv[2]);

    std::cout << "Running benchmark with " << NUM_RECORDS << " records using " << NUM_THREADS << " threads...\n";

    size_t size;
    int fd;
    Record* records = load_data(size, fd);

    setup_db("cpu.db");
    auto t1 = std::chrono::high_resolution_clock::now();
    insert_serial(records, "cpu.db");
    auto t2 = std::chrono::high_resolution_clock::now();
    double cpu_time = std::chrono::duration<double>(t2 - t1).count();

    setup_db("omp.db");
    t1 = std::chrono::high_resolution_clock::now();
    insert_parallel(records, "omp.db", NUM_THREADS);
    t2 = std::chrono::high_resolution_clock::now();
    double omp_time = std::chrono::duration<double>(t2 - t1).count();

    std::cout << "Serial Time:   " << cpu_time << " sec\n";
    std::cout << "Parallel Time: " << omp_time << " sec\n";
    std::cout << "Speedup:       " << cpu_time / omp_time << "x faster\n";

    munmap(records, size);
    close(fd);
    return 0;
}

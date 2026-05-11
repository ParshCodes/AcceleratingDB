CXX      = g++
CXXFLAGS = -std=c++17 -O2 -fopenmp
SQLITE_LIBS = -lsqlite3
PG_LIBS     = $(shell pkg-config --libs libpq 2>/dev/null || echo -lpq)
PG_CFLAGS   = $(shell pkg-config --cflags libpq 2>/dev/null)

all: generate_data benchmark pg_benchmark

generate_data: generator.cpp
	$(CXX) $(CXXFLAGS) generator.cpp -o generate_data

benchmark: DbAcceleration.cpp
	$(CXX) $(CXXFLAGS) DbAcceleration.cpp $(SQLITE_LIBS) -o benchmark

pg_benchmark: PgAcceleration.cpp
	$(CXX) $(CXXFLAGS) $(PG_CFLAGS) PgAcceleration.cpp $(PG_LIBS) -o pg_benchmark

sqlite: generate_data benchmark

postgres: generate_data pg_benchmark

clean:
	rm -f generate_data benchmark pg_benchmark data.bin serial.db parallel.db

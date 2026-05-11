CXX=g++
CXXFLAGS=-std=c++11 -fopenmp
SQLITE_LIBS=-lsqlite3

all: generate_data benchmark_insert

generate_data: generator.cpp
	$(CXX) $(CXXFLAGS) generator.cpp -o generate_data

benchmark_insert: DbAcceleration.cpp
	$(CXX) $(CXXFLAGS) DbAcceleration.cpp $(SQLITE_LIBS) -o benchmark_insert

clean:
	rm -f generate_data benchmark_insert data.bin cpu.db omp.db

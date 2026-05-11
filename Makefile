CXX      = g++
CXXFLAGS = -std=c++17 -O2 -fopenmp
LIBS     = -lsqlite3

all: generate_data benchmark

generate_data: generator.cpp
	$(CXX) $(CXXFLAGS) generator.cpp -o generate_data

benchmark: DbAcceleration.cpp
	$(CXX) $(CXXFLAGS) DbAcceleration.cpp $(LIBS) -o benchmark

clean:
	rm -f generate_data benchmark data.bin serial.db parallel.db

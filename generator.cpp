#include <fstream>
#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: ./generate_data NUM_RECORDS\n";
        return 1;
    }

    int num_records = std::atoi(argv[1]);
    std::ofstream out("data.bin", std::ios::binary);
    for (int i = 0; i < num_records; ++i) {
        int idx = i;
        float val = i * 1.1f;
        out.write((char*)&idx, sizeof(int));
        out.write((char*)&val, sizeof(float));
    }
    return 0;
}

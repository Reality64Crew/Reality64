#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <fstream>

class MemoryBus {
public:
    bool loadROM(const std::string& filepath) {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return false;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(size);
        return static_cast<bool>(file.read(buffer.data(), size));
    }
};

class VR4300 {
private:
    MemoryBus& bus;
    uint64_t pc;

public:
    VR4300(MemoryBus& memoryBus) : bus(memoryBus), pc(0xA4000040) {}

    void reset() {
        pc = 0xA4000040;
    }

    void step() {
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return 1;
    }

    std::string romPath = argv[1];

    auto bus = std::make_unique<MemoryBus>();
    auto cpu = std::make_unique<VR4300>(*bus);

    if (!bus->loadROM(romPath)) {
        return 1;
    }

    cpu->reset();

    bool isRunning = true;
    while (isRunning) {
        cpu->step();
        isRunning = false;
    }

    return 0;
}

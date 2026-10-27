#include "TestSupport.h"

#include <cstring>
#include <exception>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <test-name>\n", argv[0]);
        return 2;
    }
    for (const auto& entry : test::cases()) {
        if (std::strcmp(entry.name, argv[1]) != 0) {
            continue;
        }
        try {
            entry.run();
            std::printf("PASS %s\n", entry.name);
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL %s: %s\n", entry.name, error.what());
            return 1;
        } catch (...) {
            std::fprintf(stderr, "FAIL %s: unknown exception\n", entry.name);
            return 1;
        }
    }
    std::fprintf(stderr, "unknown test: %s\n", argv[1]);
    return 2;
}

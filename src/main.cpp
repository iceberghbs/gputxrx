#include <cstdio>
#include <exception>

#include "gputxrx/cli.hpp"

int wmain(int argc, wchar_t** argv) {
    try {
        return gputxrx::cli::run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "gputxrx: error: %s\n", e.what());
        return 1;
    }
}

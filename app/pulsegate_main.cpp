#include "pulsegate/core/version.h"

#include <iostream>
#include <string_view>

namespace {

void printUsage(std::ostream& output) {
    output << "PulseGate " << pulsegate::core::version() << '\n'
           << "Usage: pulsegate [--help | --version]\n\n"
           << "Chapter 5 project skeleton is ready.\n"
           << "Network serving is introduced in Chapter 6.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 1) {
        printUsage(std::cout);
        return 0;
    }

    const std::string_view argument{argv[1]};

    if (argc == 2 && argument == "--version") {
        std::cout << pulsegate::core::version() << '\n';
        return 0;
    }

    if (argc == 2 && (argument == "--help" || argument == "-h")) {
        printUsage(std::cout);
        return 0;
    }

    std::cerr << "Unknown arguments. Run pulsegate --help for usage.\n";
    return 2;
}

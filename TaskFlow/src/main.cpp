#include "c_test.h"
#include "cpp_test.hpp"
#include "availability_management.hpp"
#include "task_management.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int runModuleChecks(bool announce)
{
    if (taskflow_c_module_test() != 0 || !taskflow_cpp_module_test()) {
        std::cerr << "A preserved module check failed.\n";
        return 1;
    }
    if (announce) {
        std::cout << "PASS regression: preserved C11 and C++17 module checks\n";
    }
    return 0;
}

void printUsage()
{
    std::cout << "TaskFlow - Task and Availability Management\n"
              << "Usage:\n"
              << "  TaskFlow.exe                 Run the interactive manager\n"
              << "  TaskFlow.exe --demo sector-01\n"
              << "  TaskFlow.exe --demo sector-02\n"
              << "  TaskFlow.exe --test [test-data-directory]\n";
}

} // namespace

int main(int argc, char* argv[])
{
    const bool testMode = argc >= 2 && std::string(argv[1]) == "--test";
    if (runModuleChecks(testMode) != 0) return 1;

    if (argc == 1) {
        return taskflow::runInteractiveTaskManager(
            "data/tasks.csv", "data/availability.csv");
    }

    const std::string command = argv[1];
    if (command == "--demo" && argc == 3
        && std::string(argv[2]) == "sector-01") {
        return taskflow::runSector01Demo("build/sector-01-demo-tasks.csv");
    }
    if (command == "--demo" && argc == 3
        && std::string(argv[2]) == "sector-02") {
        return taskflow::runSector02Demo(
            "build/sector-02-demo-availability.csv");
    }
    if (command == "--test" && argc <= 3) {
        const std::filesystem::path directory = argc == 3
            ? std::filesystem::path(argv[2])
            : std::filesystem::path("build/test-data");
        const int taskTestResult =
            taskflow::runTaskManagementTests(directory / "sector-01");
        if (taskTestResult != 0) return taskTestResult;
        return taskflow::runAvailabilityTests(directory / "sector-02");
    }
    if (command == "--help" || command == "-h") {
        printUsage();
        return 0;
    }

    printUsage();
    return 2;
}

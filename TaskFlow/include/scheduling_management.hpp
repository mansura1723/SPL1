#ifndef TASKFLOW_SCHEDULING_MANAGEMENT_HPP
#define TASKFLOW_SCHEDULING_MANAGEMENT_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "availability_management.hpp"
#include "dependency_management.hpp"
#include "task_management.hpp"

namespace taskflow {

enum class SchedulingStrategy {
    Priority,
    EDF,
    SJF,
    LST,
    Custom
};

using SchedulingTimePoint = std::chrono::system_clock::time_point;

struct CommittedInterval {
    SchedulingTimePoint start;
    SchedulingTimePoint end;
};

struct SchedulingContext {
    SchedulingTimePoint now;
    std::vector<CommittedInterval> commitments;
    std::map<std::string, int> customRanks;
};

struct TaskScheduleMetrics {
    std::int64_t slackMinutes = 0;
    std::size_t downstreamImpact = 0;
    int remainingDurationMinutes = 0;
};

struct BlockedTask {
    Task task;
    std::vector<std::string> blockingPrerequisites;
};

struct ExcludedTask {
    Task task;
    std::string reason;
};

struct SchedulingResult {
    bool valid = false;
    std::string error;
    std::vector<Task> orderedTasks;
    std::vector<BlockedTask> blockedTasks;
    std::vector<Task> completedTasks;
    std::vector<ExcludedTask> excludedTasks;
    std::map<std::string, TaskScheduleMetrics> metrics;
};

bool parseSchedulingStrategy(const std::string& value,
    SchedulingStrategy& strategy);
std::string toString(SchedulingStrategy strategy);
SchedulingResult generateSchedule(SchedulingStrategy strategy,
    const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies,
    const DependencyManager& dependencyValidator,
    const SchedulingContext& context);
int runSchedulingMenu(const std::vector<Task>& tasks,
    const AvailabilityManager& availability,
    const DependencyManager& dependencies);
int runSector04Demo();
int runSchedulingTests(const std::filesystem::path& testDirectory);

} // namespace taskflow

#endif

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

struct ScheduleEntry {
    std::string taskId;
    std::string taskName;
    SchedulingTimePoint start;
    SchedulingTimePoint end;
    std::int64_t durationMinutes = 0;
    SchedulingStrategy strategy = SchedulingStrategy::Priority;
    std::string availabilityWindowId;
    std::string deadline;
    std::string dependencyState;
    std::string selectionExplanation;
};

struct ScheduleMetrics {
    std::size_t totalTasks = 0;
    std::size_t scheduledTasks = 0;
    std::size_t unscheduledTasks = 0;
    std::size_t deadlineViolations = 0;
    std::int64_t totalScheduledMinutes = 0;
    std::int64_t totalAvailableMinutes = 0;
    std::int64_t totalIdleMinutes = 0;
    double completionPercentage = 0.0;
};

struct UnscheduledTask {
    Task task;
    std::string reason;
    std::int64_t requiredMinutes = 0;
    std::int64_t availableMinutes = 0;
    SchedulingTimePoint deadline;
};

struct GeneratedSchedule {
    bool valid = false;
    std::string error;
    SchedulingStrategy strategy = SchedulingStrategy::Priority;
    std::vector<ScheduleEntry> entries;
    std::vector<UnscheduledTask> unscheduledTasks;
    ScheduleMetrics metrics;
};

class ScheduleGenerator {
public:
    GeneratedSchedule generate(SchedulingStrategy strategy,
        const std::vector<Task>& tasks,
        const std::vector<AvailabilityWindow>& availability,
        const std::vector<Dependency>& dependencies,
        const DependencyManager& dependencyValidator,
        const SchedulingContext& context) const;

    std::map<SchedulingStrategy, GeneratedSchedule> compareAll(
        const std::vector<Task>& tasks,
        const std::vector<AvailabilityWindow>& availability,
        const std::vector<Dependency>& dependencies,
        const DependencyManager& dependencyValidator,
        const SchedulingContext& context) const;
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
int runSector05Demo();
int runSchedulingTests(const std::filesystem::path& testDirectory);

} // namespace taskflow

#endif

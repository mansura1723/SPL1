#ifndef TASKFLOW_REPLANNING_MANAGEMENT_HPP
#define TASKFLOW_REPLANNING_MANAGEMENT_HPP

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "scheduling_management.hpp"

namespace taskflow {

struct AvailabilityUpdate {
    enum class Type {
        Add,
        Update,
        Remove
    };

    Type type = Type::Add;
    std::string id;
    AvailabilityWindow window;
};

struct ImpactEntry {
    std::string taskId;
    std::string reason;
    std::string group;
};

struct ImpactAnalysis {
    std::vector<std::string> directlyAffected;
    std::vector<std::string> downstreamAffected;
    std::vector<std::string> deadlineRisks;
    std::vector<std::string> unaffected;
    std::vector<ImpactEntry> entries;
};

struct ScenarioResult {
    bool valid = false;
    bool originalDataModified = false;
    std::string summary;
    std::vector<Task> originalTasks;
    std::vector<Task> simulatedTasks;
    std::vector<AvailabilityWindow> originalAvailability;
    std::vector<AvailabilityWindow> simulatedAvailability;
    std::vector<Dependency> originalDependencies;
    std::vector<Dependency> simulatedDependencies;
    GeneratedSchedule originalSchedule;
    GeneratedSchedule simulatedSchedule;
};

struct ScheduleDelta {
    std::string taskId;
    std::string action;
    std::string details;
};

struct ScenarioEvaluation {
    bool valid = false;
    std::string summary;
    ScenarioResult scenario;
    std::vector<ScheduleDelta> deltas;
};

struct CommitRequest {
    std::vector<Task> currentTasks;
    std::vector<AvailabilityWindow> currentAvailability;
    std::vector<Dependency> currentDependencies;
    GeneratedSchedule previousSchedule;
    GeneratedSchedule candidateSchedule;
    std::string baselineFingerprint;
    std::string scheduleVersion;
};

struct CommitResult {
    bool valid = false;
    std::string error;
    std::string summary;
    GeneratedSchedule previousSchedule;
    GeneratedSchedule committedSchedule;
    std::vector<std::string> validationErrors;
};

struct CarryoverResult {
    bool valid = false;
    std::vector<Task> carriedTasks;
    std::vector<Task> remainingTasks;
    std::vector<std::string> notes;
    std::map<std::string, int> remainingMinutesByTask;
};

struct ReplanningResult {
    bool valid = false;
    std::string error;
    GeneratedSchedule newSchedule;
    std::vector<std::string> affectedTasks;
    std::vector<std::string> unaffectedTasks;
    ImpactAnalysis impact;
    std::vector<Task> carriedTasks;
    std::vector<std::string> warnings;
    std::map<std::string, int> remainingMinutesByTask;
    std::map<SchedulingStrategy, GeneratedSchedule> alternativeSchedules;
};

struct InProgressAllocation {
    std::string taskId;
    ScheduleEntry allocation;
    int elapsedMinutes = 0;

    bool remainingDurationMinutes(const Task& task,
        int& remainingMinutes, std::string& error) const;
};

std::vector<AvailabilityWindow> applyAvailabilityUpdate(
    const std::vector<AvailabilityWindow>& current,
    const AvailabilityUpdate& update);

bool validateInProgressAllocations(const std::vector<Task>& tasks,
    const std::vector<InProgressAllocation>& allocations,
    std::map<std::string, int>& remainingMinutesByTask,
    std::string& error);

bool validateGeneratedSchedule(const GeneratedSchedule& schedule,
    const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies,
    const SchedulingContext& context,
    std::string& error);

std::string scheduleFingerprint(const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies);

bool isEquivalentSchedule(const GeneratedSchedule& left,
    const GeneratedSchedule& right);

ScenarioEvaluation evaluateWhatIfScenario(
    const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies,
    const std::vector<Task>& scenarioTasks,
    const std::vector<AvailabilityWindow>& scenarioAvailability,
    const std::vector<Dependency>& scenarioDependencies,
    const DependencyManager& validator,
    const SchedulingContext& context,
    SchedulingStrategy strategy);

CommitResult commitSchedule(const CommitRequest& request,
    const DependencyManager& validator,
    const SchedulingContext& context);

class CarryoverManager {
public:
    CarryoverResult applyCarryover(const std::vector<Task>& tasks,
        const std::vector<Dependency>& dependencies,
        const std::vector<AvailabilityWindow>& availability,
        const SchedulingContext& context,
        const GeneratedSchedule& currentSchedule) const;

private:
    static std::string shiftDeadlineNextDay(const std::string& deadline);
};

class ImpactAnalyzer {
public:
    ImpactAnalysis analyze(const std::vector<Task>& tasks,
        const std::vector<Dependency>& dependencies,
        const std::vector<AvailabilityWindow>& availability,
        const std::vector<std::string>& changedTaskIds,
        const std::vector<std::string>& changedAvailabilityIds) const;
};

class ScenarioSimulator {
public:
    ScenarioResult simulate(const std::vector<Task>& tasks,
        const std::vector<AvailabilityWindow>& availability,
        const std::vector<Dependency>& dependencies,
        const std::vector<Task>& scenarioTasks,
        const std::vector<AvailabilityWindow>& scenarioAvailability,
        const std::vector<Dependency>& scenarioDependencies,
        const DependencyManager& validator,
        const SchedulingContext& context,
        SchedulingStrategy strategy) const;
};

class ReplanningEngine {
public:
    ReplanningResult replan(const std::vector<Task>& tasks,
        const std::vector<AvailabilityWindow>& availability,
        const std::vector<Dependency>& dependencies,
        const std::vector<Task>& insertedTasks,
        const std::vector<Task>& updatedTasks,
        const std::vector<std::string>& removedTaskIds,
        const std::vector<AvailabilityUpdate>& availabilityUpdates,
        const std::vector<Dependency>& dependencyUpdates,
        const DependencyManager& validator,
        const SchedulingContext& context,
        SchedulingStrategy strategy) const;

    ReplanningResult replan(const std::vector<Task>& tasks,
        const std::vector<AvailabilityWindow>& availability,
        const std::vector<Dependency>& dependencies,
        const std::vector<Task>& insertedTasks,
        const std::vector<Task>& updatedTasks,
        const std::vector<std::string>& removedTaskIds,
        const std::vector<AvailabilityUpdate>& availabilityUpdates,
        const std::vector<Dependency>& dependencyUpdates,
        const std::vector<InProgressAllocation>& inProgressAllocations,
        const DependencyManager& validator,
        const SchedulingContext& context,
        SchedulingStrategy strategy) const;
};

int runSector06Demo();
int runReplanningTests(const std::filesystem::path& testDirectory);

} // namespace taskflow

#endif

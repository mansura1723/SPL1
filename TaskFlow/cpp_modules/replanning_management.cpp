#include "replanning_management.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace taskflow {
namespace {

bool localTimeFromTimePoint(const SchedulingTimePoint& timePoint,
    std::tm& output)
{
    const std::time_t raw = std::chrono::system_clock::to_time_t(timePoint);
#ifdef _WIN32
    return localtime_s(&output, &raw) == 0;
#else
    return localtime_r(&output, &raw) != nullptr;
#endif
}

std::string formatLocalTimePoint(const SchedulingTimePoint& timePoint)
{
    std::tm local = {};
    if (!localTimeFromTimePoint(timePoint, local)) {
        return "unknown";
    }
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M");
    return stream.str();
}

std::string futureDeadlineString(int daysAhead, int hour, int minute)
{
    const SchedulingTimePoint now = std::chrono::system_clock::now();
    const auto offset = std::chrono::hours(24 * daysAhead + hour);
    const SchedulingTimePoint point = now + offset + std::chrono::minutes(minute);
    std::tm local = {};
    if (!localTimeFromTimePoint(point, local)) {
        return "2099-01-01T09:00";
    }
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%dT%H:%M");
    return stream.str();
}

bool sameTaskList(const std::vector<Task>& left,
    const std::vector<Task>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const Task& leftTask = left[index];
        const Task& rightTask = right[index];
        if (leftTask.id != rightTask.id || leftTask.name != rightTask.name
            || leftTask.description != rightTask.description
            || leftTask.priority != rightTask.priority
            || leftTask.durationMinutes != rightTask.durationMinutes
            || leftTask.deadline != rightTask.deadline
            || leftTask.type != rightTask.type
            || leftTask.status != rightTask.status) {
            return false;
        }
    }
    return true;
}

bool sameAvailabilityList(const std::vector<AvailabilityWindow>& left,
    const std::vector<AvailabilityWindow>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const AvailabilityWindow& leftWindow = left[index];
        const AvailabilityWindow& rightWindow = right[index];
        if (leftWindow.id != rightWindow.id || leftWindow.day != rightWindow.day
            || leftWindow.startMinute != rightWindow.startMinute
            || leftWindow.endMinute != rightWindow.endMinute
            || leftWindow.label != rightWindow.label) {
            return false;
        }
    }
    return true;
}

bool sameDependencyList(const std::vector<Dependency>& left,
    const std::vector<Dependency>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const Dependency& leftDependency = left[index];
        const Dependency& rightDependency = right[index];
        if (leftDependency.dependentId != rightDependency.dependentId
            || leftDependency.prerequisiteId != rightDependency.prerequisiteId) {
            return false;
        }
    }
    return true;
}

std::string normalizeScheduleEntryKey(const ScheduleEntry& entry)
{
    std::ostringstream stream;
    stream << entry.taskId << "|" << entry.taskName << "|"
           << entry.durationMinutes << "|" << entry.deadline << "|"
           << entry.start.time_since_epoch().count() << "|"
           << entry.end.time_since_epoch().count();
    return stream.str();
}

bool hasTaskId(const std::vector<Task>& tasks, const std::string& taskId)
{
    return std::any_of(tasks.begin(), tasks.end(),
        [&](const Task& task) { return task.id == taskId; });
}

const Task* findTaskById(const std::vector<Task>& tasks,
    const std::string& taskId)
{
    const auto iterator = std::find_if(tasks.begin(), tasks.end(),
        [&](const Task& task) { return task.id == taskId; });
    return iterator == tasks.end() ? nullptr : &(*iterator);
}

std::vector<std::string> collectTaskIds(const std::vector<Task>& tasks)
{
    std::vector<std::string> ids;
    ids.reserve(tasks.size());
    for (const Task& task : tasks) {
        ids.push_back(task.id);
    }
    return ids;
}

std::vector<std::string> pruneDuplicates(std::vector<std::string> values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::vector<std::string> downstreamDependents(const std::string& taskId,
    const std::vector<Dependency>& dependencies)
{
    std::vector<std::string> ids;
    std::set<std::string> visited;
    std::vector<std::string> queue = {taskId};

    while (!queue.empty()) {
        const std::string current = queue.back();
        queue.pop_back();
        if (!visited.insert(current).second) continue;
        for (const Dependency& dependency : dependencies) {
            if (dependency.prerequisiteId == current) {
                const std::string dependentId = dependency.dependentId;
                if (visited.find(dependentId) == visited.end()) {
                    queue.push_back(dependentId);
                    ids.push_back(dependentId);
                }
            }
        }
    }
    return pruneDuplicates(ids);
}

std::vector<std::string> prerequisiteTasksFor(const std::string& taskId,
    const std::vector<Dependency>& dependencies)
{
    std::vector<std::string> ids;
    for (const Dependency& dependency : dependencies) {
        if (dependency.dependentId == taskId) {
            ids.push_back(dependency.prerequisiteId);
        }
    }
    return ids;
}

std::vector<Task> mergeInsertedAndUpdatedTasks(const std::vector<Task>& tasks,
    const std::vector<Task>& insertedTasks,
    const std::vector<Task>& updatedTasks,
    const std::vector<std::string>& removedTaskIds)
{
    std::vector<Task> working = tasks;
    std::set<std::string> removed;
    for (const std::string& id : removedTaskIds) {
        removed.insert(id);
    }

    working.erase(std::remove_if(working.begin(), working.end(),
        [&](const Task& task) {
            return removed.count(task.id) != 0
                && task.status != Status::Completed;
        }),
        working.end());

    for (const Task& inserted : insertedTasks) {
        if (std::find_if(working.begin(), working.end(),
            [&](const Task& task) { return task.id == inserted.id; })
            == working.end()) {
            working.push_back(inserted);
        }
    }

    for (const Task& updated : updatedTasks) {
        const auto iterator = std::find_if(working.begin(), working.end(),
            [&](const Task& task) { return task.id == updated.id; });
        if (iterator != working.end()) {
            if (iterator->status != Status::Completed) {
                *iterator = updated;
            }
        } else {
            working.push_back(updated);
        }
    }

    return working;
}

std::vector<Dependency> mergeDependenciesWithUpdates(
    const std::vector<Dependency>& existing,
    const std::vector<Dependency>& dependencyUpdates)
{
    std::vector<Dependency> merged = existing;
    for (const Dependency& dependency : dependencyUpdates) {
        const bool alreadyExists = std::any_of(merged.begin(), merged.end(),
            [&](const Dependency& candidate) {
                return candidate.dependentId == dependency.dependentId
                    && candidate.prerequisiteId == dependency.prerequisiteId;
            });
        if (!alreadyExists) {
            merged.push_back(dependency);
        }
    }
    return merged;
}

std::vector<AvailabilityWindow> mergeAvailabilityWithUpdates(
    const std::vector<AvailabilityWindow>& current,
    const std::vector<AvailabilityUpdate>& updates)
{
    std::vector<AvailabilityWindow> working = current;
    for (const AvailabilityUpdate& update : updates) {
        if (update.type == AvailabilityUpdate::Type::Remove) {
            working.erase(std::remove_if(working.begin(), working.end(),
                [&](const AvailabilityWindow& window) {
                    return window.id == update.id;
                }), working.end());
            continue;
        }

        auto iterator = std::find_if(working.begin(), working.end(),
            [&](const AvailabilityWindow& window) { return window.id == update.id; });
        if (iterator != working.end()) {
            if (update.type == AvailabilityUpdate::Type::Update) {
                *iterator = update.window;
            }
        } else if (update.type == AvailabilityUpdate::Type::Add) {
            working.push_back(update.window);
        } else if (update.type == AvailabilityUpdate::Type::Update) {
            working.push_back(update.window);
        }
    }
    return working;
}

bool doesScheduleOverlap(const ScheduleEntry& left,
    const ScheduleEntry& right)
{
    return left.start < right.end && right.start < left.end;
}

bool sameFixedAllocation(const ScheduleEntry& left,
    const ScheduleEntry& right)
{
    return left.taskId == right.taskId
        && left.taskName == right.taskName
        && left.start == right.start
        && left.end == right.end
        && left.durationMinutes == right.durationMinutes
        && left.strategy == right.strategy
        && left.availabilityWindowId == right.availabilityWindowId
        && left.deadline == right.deadline
        && left.dependencyState == right.dependencyState
        && left.selectionExplanation == right.selectionExplanation;
}

bool windowContainsInterval(const AvailabilityWindow& window,
    const SchedulingTimePoint& start,
    const SchedulingTimePoint& end)
{
    if (start >= end || window.startMinute < 0
        || window.startMinute >= 1440 || window.endMinute <= window.startMinute
        || window.endMinute > 1440) {
        return false;
    }

    std::tm startDate = {};
    std::tm endDate = {};
    if (!localTimeFromTimePoint(start, startDate)
        || !localTimeFromTimePoint(end - std::chrono::seconds(1), endDate)
        || startDate.tm_year != endDate.tm_year
        || startDate.tm_yday != endDate.tm_yday
        || (startDate.tm_wday + 6) % 7 != static_cast<int>(window.day)) {
        return false;
    }

    std::tm windowStart = startDate;
    windowStart.tm_hour = window.startMinute / 60;
    windowStart.tm_min = window.startMinute % 60;
    windowStart.tm_sec = 0;
    windowStart.tm_isdst = -1;
    std::tm windowEnd = startDate;
    windowEnd.tm_hour = window.endMinute / 60;
    windowEnd.tm_min = window.endMinute % 60;
    windowEnd.tm_sec = 0;
    windowEnd.tm_isdst = -1;
    const std::time_t windowStartTime = std::mktime(&windowStart);
    const std::time_t windowEndTime = std::mktime(&windowEnd);
    if (windowStartTime == static_cast<std::time_t>(-1)
        || windowEndTime == static_cast<std::time_t>(-1)) {
        return false;
    }
    return start >= std::chrono::system_clock::from_time_t(windowStartTime)
        && end <= std::chrono::system_clock::from_time_t(windowEndTime);
}

bool validateFixedAllocationWindows(
    const std::vector<InProgressAllocation>& allocations,
    const std::vector<AvailabilityWindow>& availability,
    const SchedulingContext& context,
    std::string& error)
{
    for (const InProgressAllocation& allocation : allocations) {
        const ScheduleEntry& entry = allocation.allocation;
        if (entry.start > context.now) {
            error = "In-progress allocation for task " + allocation.taskId
                + " cannot start in the future.";
            return false;
        }
        const auto window = std::find_if(availability.begin(), availability.end(),
            [&](const AvailabilityWindow& candidate) {
                return candidate.id == entry.availabilityWindowId;
            });
        if (window == availability.end()
            || !windowContainsInterval(*window, entry.start, entry.end)) {
            error = "Fixed allocation for task " + allocation.taskId
                + " is not contained in its configured availability window.";
            return false;
        }
        for (const CommittedInterval& commitment : context.commitments) {
            const bool sameInterval = commitment.start == entry.start
                && commitment.end == entry.end;
            if (!sameInterval && entry.start < commitment.end
                && commitment.start < entry.end) {
                error = "Fixed allocation for task " + allocation.taskId
                    + " overlaps an existing committed interval.";
                return false;
            }
        }
    }

    for (std::size_t index = 0; index < allocations.size(); ++index) {
        for (std::size_t other = index + 1; other < allocations.size(); ++other) {
            if (doesScheduleOverlap(allocations[index].allocation,
                    allocations[other].allocation)) {
                error = "Fixed in-progress allocations overlap for tasks "
                    + allocations[index].taskId + " and "
                    + allocations[other].taskId + ".";
                return false;
            }
        }
    }
    return true;
}

void addFixedAllocations(GeneratedSchedule& schedule,
    const std::vector<InProgressAllocation>& allocations)
{
    for (const InProgressAllocation& allocation : allocations) {
        schedule.entries.push_back(allocation.allocation);
        schedule.metrics.totalScheduledMinutes +=
            allocation.allocation.durationMinutes;
    }
    std::sort(schedule.entries.begin(), schedule.entries.end(),
        [](const ScheduleEntry& left, const ScheduleEntry& right) {
            if (left.start != right.start) return left.start < right.start;
            return left.taskId < right.taskId;
        });
    schedule.metrics.scheduledTasks = schedule.entries.size();
    schedule.metrics.unscheduledTasks = schedule.unscheduledTasks.size();
    schedule.metrics.totalIdleMinutes = std::max<std::int64_t>(0,
        schedule.metrics.totalAvailableMinutes
            - schedule.metrics.totalScheduledMinutes);
    if (schedule.metrics.totalTasks > 0) {
        schedule.metrics.completionPercentage =
            100.0 * schedule.metrics.scheduledTasks
            / static_cast<double>(schedule.metrics.totalTasks);
    }
}

std::string explainTaskSelection(const Task& task,
    SchedulingStrategy strategy,
    const TaskScheduleMetrics& metrics)
{
    switch (strategy) {
    case SchedulingStrategy::Priority:
        return "Priority ranked task " + task.id + " based on the selected strategy.";
    case SchedulingStrategy::EDF:
        return "EDF selected task " + task.id + " using earliest deadline and "
            + std::to_string(metrics.slackMinutes) + " minutes of slack.";
    case SchedulingStrategy::SJF:
        return "SJF selected task " + task.id + " based on shortest job duration.";
    case SchedulingStrategy::LST:
        return "LST selected task " + task.id + " based on least slack time.";
    case SchedulingStrategy::Custom:
        return "Custom strategy selected task " + task.id + " according to user ranking.";
    }
    return "Strategy selected task " + task.id + ".";
}

struct ReplanningTestRun {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& description)
    {
        if (condition) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "FAIL sector-06: " << description << '\n';
        }
    }
};

} // namespace

bool InProgressAllocation::remainingDurationMinutes(const Task& task,
    int& remainingMinutes, std::string& error) const
{
    remainingMinutes = 0;
    error.clear();

    if (taskId.empty() || task.id != taskId
        || allocation.taskId != taskId) {
        error = "In-progress allocation task ID does not match the task and schedule entry.";
        return false;
    }
    if (task.status == Status::Completed) {
        error = "Completed task " + task.id
            + " cannot be supplied as an in-progress allocation.";
        return false;
    }
    if (task.status != Status::InProgress) {
        error = "Task " + task.id
            + " must have InProgress status for an in-progress allocation.";
        return false;
    }
    if (task.durationMinutes <= 0) {
        error = "Task " + task.id + " must have a positive original duration.";
        return false;
    }
    if (elapsedMinutes < 0) {
        error = "Elapsed work for task " + task.id + " cannot be negative.";
        return false;
    }
    if (elapsedMinutes > task.durationMinutes) {
        error = "Elapsed work for task " + task.id
            + " cannot exceed its original duration.";
        return false;
    }
    if (allocation.start >= allocation.end
        || allocation.durationMinutes <= 0
        || allocation.end - allocation.start
            != std::chrono::minutes(allocation.durationMinutes)) {
        error = "Existing allocation for task " + task.id
            + " has an invalid time range or duration.";
        return false;
    }

    remainingMinutes = task.durationMinutes - elapsedMinutes;
    return true;
}

bool validateInProgressAllocations(const std::vector<Task>& tasks,
    const std::vector<InProgressAllocation>& allocations,
    std::map<std::string, int>& remainingMinutesByTask,
    std::string& error)
{
    remainingMinutesByTask.clear();
    error.clear();

    for (const InProgressAllocation& allocation : allocations) {
        const auto task = std::find_if(tasks.begin(), tasks.end(),
            [&](const Task& item) { return item.id == allocation.taskId; });
        if (task == tasks.end()) {
            error = "In-progress allocation references unknown task ID: "
                + allocation.taskId;
            remainingMinutesByTask.clear();
            return false;
        }
        if (remainingMinutesByTask.find(allocation.taskId)
            != remainingMinutesByTask.end()) {
            error = "Duplicate in-progress allocation for task ID: "
                + allocation.taskId;
            remainingMinutesByTask.clear();
            return false;
        }

        int remainingMinutes = 0;
        if (!allocation.remainingDurationMinutes(*task, remainingMinutes,
                error)) {
            remainingMinutesByTask.clear();
            return false;
        }
        remainingMinutesByTask.emplace(allocation.taskId, remainingMinutes);
    }

    return true;
}

std::vector<AvailabilityWindow> applyAvailabilityUpdate(
    const std::vector<AvailabilityWindow>& current,
    const AvailabilityUpdate& update)
{
    std::vector<AvailabilityWindow> merged = current;
    if (update.type == AvailabilityUpdate::Type::Remove) {
        merged.erase(std::remove_if(merged.begin(), merged.end(),
            [&](const AvailabilityWindow& window) {
                return window.id == update.id;
            }), merged.end());
        return merged;
    }

    auto iterator = std::find_if(merged.begin(), merged.end(),
        [&](const AvailabilityWindow& window) {
            return window.id == update.id;
        });
    if (iterator != merged.end()) {
        if (update.type == AvailabilityUpdate::Type::Update) {
            *iterator = update.window;
        }
        return merged;
    }

    if (update.type == AvailabilityUpdate::Type::Add
        || update.type == AvailabilityUpdate::Type::Update) {
        merged.push_back(update.window);
    }
    return merged;
}

std::string scheduleFingerprint(const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies)
{
    std::ostringstream stream;
    for (const Task& task : tasks) {
        stream << "task:" << task.id << ":" << toString(task.status) << ":"
               << task.durationMinutes << ":" << task.deadline << ";";
    }
    for (const AvailabilityWindow& window : availability) {
        stream << "window:" << window.id << ":" << static_cast<int>(window.day)
               << ":" << window.startMinute << ":" << window.endMinute << ";";
    }
    for (const Dependency& dependency : dependencies) {
        stream << "dep:" << dependency.dependentId << "->"
               << dependency.prerequisiteId << ";";
    }
    return stream.str();
}

bool isEquivalentSchedule(const GeneratedSchedule& left,
    const GeneratedSchedule& right)
{
    if (left.entries.size() != right.entries.size()) return false;
    std::vector<std::string> leftKeys;
    leftKeys.reserve(left.entries.size());
    for (const ScheduleEntry& entry : left.entries) {
        leftKeys.push_back(normalizeScheduleEntryKey(entry));
    }
    std::sort(leftKeys.begin(), leftKeys.end());

    std::vector<std::string> rightKeys;
    rightKeys.reserve(right.entries.size());
    for (const ScheduleEntry& entry : right.entries) {
        rightKeys.push_back(normalizeScheduleEntryKey(entry));
    }
    std::sort(rightKeys.begin(), rightKeys.end());
    return leftKeys == rightKeys;
}

bool validateGeneratedSchedule(const GeneratedSchedule& schedule,
    const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies,
    const SchedulingContext& context,
    std::string& error)
{
    error.clear();
    if (schedule.error.empty() == false) {
        error = schedule.error;
        return false;
    }

    std::set<std::string> seenTasks;
    std::vector<ScheduleEntry> sortedEntries = schedule.entries;
    std::sort(sortedEntries.begin(), sortedEntries.end(),
        [](const ScheduleEntry& left, const ScheduleEntry& right) {
            return left.start < right.start;
        });

    for (std::size_t index = 0; index < sortedEntries.size(); ++index) {
        const ScheduleEntry& entry = sortedEntries[index];
        const Task* task = findTaskById(tasks, entry.taskId);
        if (task == nullptr) {
            error = "Schedule includes an unknown task: " + entry.taskId;
            return false;
        }
        if (entry.start >= entry.end) {
            error = "Schedule entry for task " + entry.taskId + " has an invalid time range.";
            return false;
        }
        if (seenTasks.find(entry.taskId) != seenTasks.end()) {
            error = "Task " + entry.taskId + " is scheduled multiple times.";
            return false;
        }
        seenTasks.insert(entry.taskId);

        std::tm deadlineTime = {};
        std::time_t deadlineRaw = 0;
        const std::string deadlineText = task->deadline;
        if (!deadlineText.empty()) {
            std::tm value = {};
            std::istringstream stream(deadlineText);
            if (deadlineText.size() == 5 && deadlineText[2] == ':') {
                // Local daily time format, e.g. HH:MM.
                std::tm nowTime = {};
                const std::time_t nowRaw = std::chrono::system_clock::to_time_t(context.now);
#ifdef _WIN32
                localtime_s(&nowTime, &nowRaw);
#else
                localtime_r(&nowTime, &nowRaw);
#endif
                value = nowTime;
                int hour = 0;
                int minute = 0;
                std::sscanf(deadlineText.c_str(), "%2d:%2d", &hour, &minute);
                value.tm_hour = hour;
                value.tm_min = minute;
                value.tm_sec = 0;
                deadlineRaw = std::mktime(&value);
            } else {
                stream >> std::get_time(&value, "%Y-%m-%d");
                if (stream.fail()) {
                    stream.clear();
                    stream.str(deadlineText);
                    stream >> std::get_time(&value, "%Y-%m-%dT%H:%M");
                }
                if (!stream.fail()) {
                    if (deadlineText.size() == 16 || deadlineText.size() == 19) {
                        value.tm_sec = 0;
                    }
                    deadlineRaw = std::mktime(&value);
                }
            }
            if (deadlineRaw == -1 || stream.fail()) {
                error = "Task " + task->id + " has a malformed deadline.";
                return false;
            }
            const std::chrono::system_clock::time_point deadlinePoint =
                std::chrono::system_clock::from_time_t(deadlineRaw);
            if (entry.end > deadlinePoint) {
                error = "Task " + task->id + " violates its deadline.";
                return false;
            }
        }

        const bool hasWindowReference = !entry.availabilityWindowId.empty();
        const bool inAvailability = hasWindowReference
            && std::any_of(availability.begin(), availability.end(),
                [&](const AvailabilityWindow& window) {
                    return window.id == entry.availabilityWindowId;
                });
        if (!inAvailability && !availability.empty() && hasWindowReference) {
            error = "Task " + task->id + " references an unavailable window.";
            return false;
        }

        for (std::size_t other = index + 1; other < sortedEntries.size(); ++other) {
            const ScheduleEntry& otherEntry = sortedEntries[other];
            if (doesScheduleOverlap(entry, otherEntry)) {
                error = "Scheduled tasks overlap: " + entry.taskId + " and " + otherEntry.taskId;
                return false;
            }
        }
    }

    for (const Dependency& dependency : dependencies) {
        const ScheduleEntry* dependent = nullptr;
        const ScheduleEntry* prerequisite = nullptr;
        for (const ScheduleEntry& entry : sortedEntries) {
            if (entry.taskId == dependency.dependentId) {
                dependent = &entry;
            }
            if (entry.taskId == dependency.prerequisiteId) {
                prerequisite = &entry;
            }
        }
        if (dependent != nullptr && prerequisite != nullptr
            && dependent->start < prerequisite->end) {
            error = "Dependency ordering violated for " + dependency.dependentId
                + " -> " + dependency.prerequisiteId + ".";
            return false;
        }
    }

    return true;
}

CarryoverResult CarryoverManager::applyCarryover(
    const std::vector<Task>& tasks,
    const std::vector<Dependency>& dependencies,
    const std::vector<AvailabilityWindow>& availability,
    const SchedulingContext& context,
    const GeneratedSchedule& currentSchedule) const
{
    CarryoverResult result;
    result.valid = true;
    (void)dependencies;
    (void)availability;
    (void)context;

    std::set<std::string> scheduledIds;
    for (const ScheduleEntry& entry : currentSchedule.entries) {
        scheduledIds.insert(entry.taskId);
    }

    for (const Task& task : tasks) {
        if (task.status == Status::Completed) {
            continue;
        }
        if (scheduledIds.find(task.id) != scheduledIds.end()) {
            continue;
        }
        result.carriedTasks.push_back(task);
        result.notes.push_back("Task " + task.id + " remains incomplete and is carried forward.");
    }

    for (const Task& task : tasks) {
        if (task.status == Status::Completed) continue;
        const std::vector<std::string> prerequisites = prerequisiteTasksFor(task.id, dependencies);
        if (prerequisites.empty()) continue;
        bool allReady = true;
        for (const std::string& prerequisiteId : prerequisites) {
            const Task* prerequisite = findTaskById(tasks, prerequisiteId);
            if (prerequisite != nullptr && prerequisite->status != Status::Completed) {
                allReady = false;
                break;
            }
        }
        if (!allReady) {
            result.remainingTasks.push_back(task);
        }
    }

    return result;
}

std::string CarryoverManager::shiftDeadlineNextDay(const std::string& deadline)
{
    if (deadline.empty()) {
        return deadline;
    }
    std::tm value = {};
    std::istringstream stream(deadline);
    stream >> std::get_time(&value, "%Y-%m-%d");
    if (stream.fail()) {
        return deadline;
    }
    value.tm_mday += 1;
    std::time_t raw = std::mktime(&value);
    if (raw == -1) {
        return deadline;
    }
    std::tm local = {};
#ifdef _WIN32
    localtime_s(&local, &raw);
#else
    localtime_r(&local, &raw);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d");
    return output.str();
}

ImpactAnalysis ImpactAnalyzer::analyze(
    const std::vector<Task>& tasks,
    const std::vector<Dependency>& dependencies,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<std::string>& changedTaskIds,
    const std::vector<std::string>& changedAvailabilityIds) const
{
    ImpactAnalysis analysis;
    std::set<std::string> impacted;
    std::set<std::string> risky;
    std::set<std::string> visited;

    for (const std::string& taskId : changedTaskIds) {
        impacted.insert(taskId);
        for (const std::string& dependentId : downstreamDependents(taskId, dependencies)) {
            impacted.insert(dependentId);
        }
    }

    for (const std::string& availabilityId : changedAvailabilityIds) {
        for (const Task& task : tasks) {
            if (task.status == Status::Completed) continue;
            const std::string windowId = availabilityId;
            if (std::any_of(availability.begin(), availability.end(),
                [&](const AvailabilityWindow& window) {
                    return window.id == windowId && (task.deadline == window.label || windowId == task.id);
                })) {
                impacted.insert(task.id);
            }
        }
    }

    for (const std::string& taskId : impacted) {
        std::vector<std::string> prerequisiteIds = prerequisiteTasksFor(taskId, dependencies);
        if (!prerequisiteIds.empty()) {
            analysis.downstreamAffected.push_back(taskId);
        }
        analysis.entries.push_back({taskId, "affected by dependency or availability change", "replan"});
    }

    const std::vector<std::string> allIds = collectTaskIds(tasks);
    for (const std::string& taskId : allIds) {
        if (impacted.find(taskId) == impacted.end()) {
            analysis.unaffected.push_back(taskId);
        }
    }

    const std::set<std::string> impactedSet(impacted.begin(), impacted.end());
    for (const Task& task : tasks) {
        if (task.status == Status::Completed) continue;
        if (impactedSet.find(task.id) != impactedSet.end()) {
            analysis.directlyAffected.push_back(task.id);
        }
        if (task.deadline.size() >= 5 && task.deadline[2] == ':') {
            risky.insert(task.id);
        }
    }

    std::set<std::string> finalAffected;
    for (const std::string& id : analysis.directlyAffected) {
        finalAffected.insert(id);
    }
    for (const std::string& id : analysis.downstreamAffected) {
        finalAffected.insert(id);
    }
    analysis.directlyAffected.clear();
    analysis.directlyAffected.assign(finalAffected.begin(), finalAffected.end());

    for (const std::string& id : finalAffected) {
        analysis.entries.push_back({id, "impact analysis", "affected"});
    }

    return analysis;
}

ScenarioResult ScenarioSimulator::simulate(
    const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies,
    const std::vector<Task>& scenarioTasks,
    const std::vector<AvailabilityWindow>& scenarioAvailability,
    const std::vector<Dependency>& scenarioDependencies,
    const DependencyManager& validator,
    const SchedulingContext& context,
    SchedulingStrategy strategy) const
{
    ScenarioResult result;
    result.originalTasks = tasks;
    result.originalAvailability = availability;
    result.originalDependencies = dependencies;
    result.simulatedTasks = scenarioTasks.empty() ? tasks : scenarioTasks;
    result.simulatedAvailability = scenarioAvailability.empty() ? availability : scenarioAvailability;
    result.simulatedDependencies = scenarioDependencies.empty() ? dependencies : scenarioDependencies;

    result.originalDataModified = !sameTaskList(tasks, result.simulatedTasks)
        || !sameAvailabilityList(availability, result.simulatedAvailability)
        || !sameDependencyList(dependencies, result.simulatedDependencies);

    ScheduleGenerator generator;
    const GeneratedSchedule generated = generator.generate(strategy,
        result.simulatedTasks,
        result.simulatedAvailability,
        result.simulatedDependencies,
        validator,
        context);

    if (!generated.valid) {
        result.summary = generated.error;
        return result;
    }

    result.valid = true;
    result.simulatedSchedule = generated;
    result.originalSchedule = generator.generate(strategy, tasks, availability,
        dependencies, validator, context);
    result.summary = "Scenario simulation created a valid schedule for "
        + std::to_string(generated.metrics.scheduledTasks) + " tasks.";
    return result;
}

ScenarioEvaluation evaluateWhatIfScenario(
    const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies,
    const std::vector<Task>& scenarioTasks,
    const std::vector<AvailabilityWindow>& scenarioAvailability,
    const std::vector<Dependency>& scenarioDependencies,
    const DependencyManager& validator,
    const SchedulingContext& context,
    SchedulingStrategy strategy)
{
    ScenarioEvaluation evaluation;
    ScenarioSimulator simulator;
    evaluation.scenario = simulator.simulate(tasks, availability, dependencies,
        scenarioTasks, scenarioAvailability, scenarioDependencies,
        validator, context, strategy);
    evaluation.valid = evaluation.scenario.valid;
    evaluation.summary = evaluation.scenario.summary;
    if (!evaluation.valid) {
        return evaluation;
    }

    std::map<std::string, std::string> originalByTask;
    for (const ScheduleEntry& entry : evaluation.scenario.originalSchedule.entries) {
        originalByTask[entry.taskId] = normalizeScheduleEntryKey(entry);
    }
    std::map<std::string, std::string> simulatedByTask;
    for (const ScheduleEntry& entry : evaluation.scenario.simulatedSchedule.entries) {
        simulatedByTask[entry.taskId] = normalizeScheduleEntryKey(entry);
    }

    std::set<std::string> seen;
    for (const auto& entry : simulatedByTask) {
        seen.insert(entry.first);
        if (originalByTask.find(entry.first) == originalByTask.end()) {
            evaluation.deltas.push_back({entry.first, "added",
                "Task was added to the simulated schedule."});
        } else if (originalByTask[entry.first] != entry.second) {
            evaluation.deltas.push_back({entry.first, "moved",
                "Allocation changed to a different time window."});
        } else {
            evaluation.deltas.push_back({entry.first, "unchanged",
                "Allocation preserved in the simulated scenario."});
        }
    }
    for (const auto& entry : originalByTask) {
        if (seen.find(entry.first) == seen.end()) {
            evaluation.deltas.push_back({entry.first, "removed",
                "Task was removed from the simulated schedule."});
        }
    }

    if (evaluation.deltas.empty()) {
        evaluation.summary = "No schedule changes were detected in the scenario comparison.";
    }
    return evaluation;
}

CommitResult commitSchedule(const CommitRequest& request,
    const DependencyManager& validator,
    const SchedulingContext& context)
{
    CommitResult result;
    result.previousSchedule = request.previousSchedule;
    result.committedSchedule = request.previousSchedule;

    std::string currentFingerprint = scheduleFingerprint(request.currentTasks,
        request.currentAvailability, request.currentDependencies);
    if (!request.baselineFingerprint.empty()
        && request.baselineFingerprint != currentFingerprint) {
        result.error = "Schedule commit rejected: the task, availability, or dependency state changed since this candidate was generated.";
        result.validationErrors.push_back(result.error);
        result.summary = result.error;
        return result;
    }
    if (!request.candidateSchedule.valid) {
        result.error = "Schedule commit rejected: the candidate schedule is not valid.";
        result.validationErrors.push_back(result.error);
        result.summary = result.error;
        return result;
    }
    if (!request.candidateSchedule.error.empty()) {
        result.error = "Schedule commit rejected: candidate validation reported "
            + request.candidateSchedule.error;
        result.validationErrors.push_back(result.error);
        result.summary = result.error;
        return result;
    }

    for (const Task& task : request.currentTasks) {
        if (task.status != Status::InProgress) continue;

        if (!request.previousSchedule.valid) {
            result.error = "Schedule commit rejected: an authoritative valid "
                "previous schedule is required to preserve in-progress task "
                + task.id + ".";
            result.validationErrors.push_back(result.error);
            result.summary = result.error;
            return result;
        }

        const auto previousCount = std::count_if(
            request.previousSchedule.entries.begin(),
            request.previousSchedule.entries.end(),
            [&](const ScheduleEntry& entry) {
                return entry.taskId == task.id;
            });
        if (previousCount != 1) {
            result.error = "Schedule commit rejected: authoritative previous "
                "schedule must contain exactly one fixed allocation for "
                "in-progress task " + task.id + ".";
            result.validationErrors.push_back(result.error);
            result.summary = result.error;
            return result;
        }
        const auto previousEntry = std::find_if(
            request.previousSchedule.entries.begin(),
            request.previousSchedule.entries.end(),
            [&](const ScheduleEntry& entry) {
                return entry.taskId == task.id;
            });

        const auto candidateCount = std::count_if(
            request.candidateSchedule.entries.begin(),
            request.candidateSchedule.entries.end(),
            [&](const ScheduleEntry& entry) {
                return entry.taskId == task.id;
            });
        if (candidateCount != 1) {
            result.error = "Schedule commit rejected: candidate must contain "
                "exactly one fixed allocation for in-progress task "
                + task.id + ".";
            result.validationErrors.push_back(result.error);
            result.summary = result.error;
            return result;
        }
        const auto candidateEntry = std::find_if(
            request.candidateSchedule.entries.begin(),
            request.candidateSchedule.entries.end(),
            [&](const ScheduleEntry& entry) {
                return entry.taskId == task.id;
            });
        if (!sameFixedAllocation(*previousEntry, *candidateEntry)) {
            result.error = "Schedule commit rejected: fixed allocation for "
                "in-progress task " + task.id
                + " was moved or modified.";
            result.validationErrors.push_back(result.error);
            result.summary = result.error;
            return result;
        }
    }

    std::string validationError;
    if (!validateGeneratedSchedule(request.candidateSchedule,
            request.currentTasks, request.currentAvailability,
            request.currentDependencies, context, validationError)) {
        result.error = "Schedule commit rejected: " + validationError;
        result.validationErrors.push_back(result.error);
        result.summary = result.error;
        return result;
    }

    for (const Task& task : request.currentTasks) {
        if (task.status == Status::Completed) {
            const auto scheduled = std::find_if(request.candidateSchedule.entries.begin(),
                request.candidateSchedule.entries.end(),
                [&](const ScheduleEntry& entry) { return entry.taskId == task.id; });
            if (scheduled != request.candidateSchedule.entries.end()) {
                result.error = "Schedule commit rejected: completed task " + task.id
                    + " must never be rescheduled.";
                result.validationErrors.push_back(result.error);
                result.summary = result.error;
                return result;
            }
        }
    }

    std::set<std::string> currentTaskIds;
    for (const Task& task : request.currentTasks) {
        currentTaskIds.insert(task.id);
    }
    for (const ScheduleEntry& entry : request.candidateSchedule.entries) {
        if (currentTaskIds.find(entry.taskId) == currentTaskIds.end()) {
            result.error = "Schedule commit rejected: candidate contains unknown task "
                + entry.taskId + ".";
            result.validationErrors.push_back(result.error);
            result.summary = result.error;
            return result;
        }
    }

    if (!request.scheduleVersion.empty()
        && request.previousSchedule.error.empty() == false) {
        result.error = "Schedule commit rejected: previous schedule snapshot is missing for this version.";
        result.validationErrors.push_back(result.error);
        result.summary = result.error;
        return result;
    }

    result.committedSchedule = request.candidateSchedule;
    for (const Task& task : request.currentTasks) {
        if (task.status != Status::Completed) continue;
        const auto previousEntry = std::find_if(
            request.previousSchedule.entries.begin(),
            request.previousSchedule.entries.end(),
            [&](const ScheduleEntry& entry) {
                return entry.taskId == task.id;
            });
        if (previousEntry != request.previousSchedule.entries.end()) {
            result.committedSchedule.entries.push_back(*previousEntry);
        }
    }
    std::sort(result.committedSchedule.entries.begin(),
        result.committedSchedule.entries.end(),
        [](const ScheduleEntry& left, const ScheduleEntry& right) {
            if (left.start != right.start) return left.start < right.start;
            return left.taskId < right.taskId;
        });
    result.valid = true;
    result.summary = "Candidate schedule passed validation and was accepted for commit.";
    return result;
}

ReplanningResult ReplanningEngine::replan(
    const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies,
    const std::vector<Task>& insertedTasks,
    const std::vector<Task>& updatedTasks,
    const std::vector<std::string>& removedTaskIds,
    const std::vector<AvailabilityUpdate>& availabilityUpdates,
    const std::vector<Dependency>& dependencyUpdates,
    const DependencyManager& validator,
    const SchedulingContext& context,
    SchedulingStrategy strategy) const
{
    ReplanningResult result;
    std::vector<Task> currentTasks = mergeInsertedAndUpdatedTasks(
        tasks, insertedTasks, updatedTasks, removedTaskIds);
    std::vector<Dependency> currentDependencies = mergeDependenciesWithUpdates(
        dependencies, dependencyUpdates);
    std::vector<AvailabilityWindow> currentAvailability = mergeAvailabilityWithUpdates(
        availability, availabilityUpdates);

    std::vector<std::string> changedTaskIds;
    for (const Task& task : insertedTasks) {
        changedTaskIds.push_back(task.id);
    }
    for (const Task& task : updatedTasks) {
        changedTaskIds.push_back(task.id);
    }
    for (const std::string& taskId : removedTaskIds) {
        changedTaskIds.push_back(taskId);
    }
    changedTaskIds = pruneDuplicates(changedTaskIds);

    std::vector<std::string> changedAvailabilityIds;
    for (const AvailabilityUpdate& update : availabilityUpdates) {
        if (!update.id.empty()) {
            changedAvailabilityIds.push_back(update.id);
        }
    }

    ImpactAnalyzer analyzer;
    result.impact = analyzer.analyze(currentTasks, currentDependencies,
        currentAvailability, changedTaskIds, changedAvailabilityIds);

    const std::vector<std::string> allTaskIds = collectTaskIds(currentTasks);
    std::set<std::string> affectedSet(result.impact.directlyAffected.begin(),
        result.impact.directlyAffected.end());
    for (const std::string& id : result.impact.downstreamAffected) {
        affectedSet.insert(id);
    }

    result.affectedTasks.assign(affectedSet.begin(), affectedSet.end());
    std::sort(result.affectedTasks.begin(), result.affectedTasks.end());
    for (const std::string& id : allTaskIds) {
        if (affectedSet.find(id) == affectedSet.end()) {
            result.unaffectedTasks.push_back(id);
        }
    }
    std::sort(result.unaffectedTasks.begin(), result.unaffectedTasks.end());

    CarryoverManager carryoverManager;
    result.carriedTasks = carryoverManager.applyCarryover(currentTasks,
        currentDependencies, currentAvailability, context,
        GeneratedSchedule{}).carriedTasks;

    if (!result.carriedTasks.empty()) {
        result.warnings.push_back("One or more incomplete tasks will be carried forward.");
    }

    std::set<std::string> withheldTaskIds;
    for (const Task& task : currentTasks) {
        if (task.status == Status::InProgress) {
            withheldTaskIds.insert(task.id);
        }
    }
    bool addedDependent = true;
    while (addedDependent) {
        addedDependent = false;
        for (const Dependency& dependency : currentDependencies) {
            if (withheldTaskIds.count(dependency.prerequisiteId) == 0) continue;
            const Task* dependent = findTaskById(currentTasks,
                dependency.dependentId);
            if (dependent != nullptr && dependent->status != Status::Completed
                && withheldTaskIds.insert(dependent->id).second) {
                addedDependent = true;
            }
        }
    }

    std::vector<Task> schedulableTasks;
    std::vector<Task> preservedInProgressTasks;
    std::vector<Task> blockedByInProgressTasks;
    for (const Task& task : currentTasks) {
        if (task.status == Status::InProgress) {
            preservedInProgressTasks.push_back(task);
        } else if (task.status != Status::Completed
            && withheldTaskIds.count(task.id) != 0) {
            blockedByInProgressTasks.push_back(task);
        } else {
            schedulableTasks.push_back(task);
        }
    }
    std::vector<Dependency> schedulableDependencies;
    for (const Dependency& dependency : currentDependencies) {
        if (withheldTaskIds.count(dependency.dependentId) == 0
            && withheldTaskIds.count(dependency.prerequisiteId) == 0) {
            schedulableDependencies.push_back(dependency);
        }
    }

    ScheduleGenerator generator;
    const auto comparison = generator.compareAll(schedulableTasks,
        currentAvailability,
        schedulableDependencies,
        validator,
        context);

    const GeneratedSchedule* bestSchedule = nullptr;
    std::string candidateFailure;
    result.alternativeSchedules = comparison;
    for (const auto& entry : comparison) {
        const GeneratedSchedule& candidate = entry.second;
        if (!candidate.valid) {
            if (candidateFailure.empty()) {
                candidateFailure = toString(entry.first) + ": " + candidate.error;
            }
            continue;
        }
        std::string validationError;
        if (!validateGeneratedSchedule(candidate, currentTasks,
                currentAvailability, currentDependencies, context,
                validationError)) {
            if (candidateFailure.empty()) {
                candidateFailure = toString(entry.first) + ": " + validationError;
            }
            continue;
        }
        if (bestSchedule == nullptr
            || candidate.metrics.scheduledTasks > bestSchedule->metrics.scheduledTasks
            || (candidate.metrics.scheduledTasks == bestSchedule->metrics.scheduledTasks
                && candidate.metrics.totalIdleMinutes < bestSchedule->metrics.totalIdleMinutes)) {
            bestSchedule = &candidate;
        }
    }

    if (bestSchedule != nullptr) {
        result.newSchedule = *bestSchedule;
        for (const Task& task : preservedInProgressTasks) {
            UnscheduledTask item;
            item.task = task;
            item.reason = "In-progress task preserved; existing allocation is not available to the replanner.";
            item.requiredMinutes = task.durationMinutes;
            result.newSchedule.unscheduledTasks.push_back(item);
        }
        for (const Task& task : blockedByInProgressTasks) {
            UnscheduledTask item;
            item.task = task;
            item.reason = "Dependency blocked: prerequisite is in progress and has no completed allocation in the replanning input.";
            item.requiredMinutes = task.durationMinutes;
            result.newSchedule.unscheduledTasks.push_back(item);
        }
        result.newSchedule.metrics.totalTasks = currentTasks.size();
        result.newSchedule.metrics.unscheduledTasks =
            result.newSchedule.unscheduledTasks.size();

        const bool hasIncompleteTask = std::any_of(currentTasks.begin(),
            currentTasks.end(), [](const Task& task) {
                return task.status != Status::Completed;
            });
        if (result.newSchedule.entries.empty() && hasIncompleteTask) {
            result.newSchedule.valid = false;
            result.newSchedule.error = "No incomplete task could be allocated in a valid availability window.";
            result.valid = false;
            result.error = result.newSchedule.error;
            for (const UnscheduledTask& unscheduled :
                    result.newSchedule.unscheduledTasks) {
                result.error += " " + unscheduled.task.id + ": "
                    + unscheduled.reason;
            }
        } else {
            result.valid = true;
            result.error.clear();
        }
        return result;
    }

    result.valid = false;
    result.newSchedule.valid = false;
    result.newSchedule.strategy = strategy;
    result.newSchedule.metrics.totalTasks = currentTasks.size();
    result.newSchedule.error = "No scheduling strategy produced a candidate that passed validation.";
    result.error = result.newSchedule.error;
    if (!candidateFailure.empty()) {
        result.error += " " + candidateFailure;
    }

    const GeneratedSchedule diagnostic = generator.generate(strategy,
        schedulableTasks, currentAvailability, schedulableDependencies, validator,
        context);
    result.newSchedule.unscheduledTasks = diagnostic.unscheduledTasks;
    for (const Task& task : schedulableTasks) {
        if (task.status == Status::Completed
            || std::any_of(result.newSchedule.unscheduledTasks.begin(),
                result.newSchedule.unscheduledTasks.end(),
                [&](const UnscheduledTask& item) {
                    return item.task.id == task.id;
                })) {
            continue;
        }
        UnscheduledTask item;
        item.task = task;
        item.requiredMinutes = task.durationMinutes;
        if (currentAvailability.empty()) {
            item.reason = "No available window: no recurring availability windows are configured.";
        } else if (candidateFailure.find("deadline") != std::string::npos) {
            item.reason = "Deadline exceeded: no feasible allocation meets the task deadline.";
        } else if (candidateFailure.find("Dependency graph validation failed")
            != std::string::npos) {
            item.reason = "Invalid task data: dependency graph validation rejected the replanning input.";
        } else {
            item.reason = "No valid availability slot satisfies the task duration and deadline.";
        }
        result.newSchedule.unscheduledTasks.push_back(item);
    }
    for (const Task& task : preservedInProgressTasks) {
        UnscheduledTask item;
        item.task = task;
        item.reason = "In-progress task preserved; existing allocation is not available to the replanner.";
        item.requiredMinutes = task.durationMinutes;
        result.newSchedule.unscheduledTasks.push_back(item);
    }
    for (const Task& task : blockedByInProgressTasks) {
        UnscheduledTask item;
        item.task = task;
        item.reason = "Dependency blocked: prerequisite is in progress and has no completed allocation in the replanning input.";
        item.requiredMinutes = task.durationMinutes;
        result.newSchedule.unscheduledTasks.push_back(item);
    }
    result.newSchedule.metrics.unscheduledTasks =
        result.newSchedule.unscheduledTasks.size();
    for (const UnscheduledTask& unscheduled : result.newSchedule.unscheduledTasks) {
        result.error += " " + unscheduled.task.id + ": " + unscheduled.reason;
    }
    result.newSchedule.error = result.error;
    return result;
}

ReplanningResult ReplanningEngine::replan(
    const std::vector<Task>& tasks,
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
    SchedulingStrategy strategy) const
{
    const std::vector<Task> effectiveTasks = mergeInsertedAndUpdatedTasks(
        tasks, insertedTasks, updatedTasks, removedTaskIds);
    const std::vector<Dependency> effectiveDependencies =
        mergeDependenciesWithUpdates(dependencies, dependencyUpdates);
    const std::vector<AvailabilityWindow> effectiveAvailability =
        mergeAvailabilityWithUpdates(availability, availabilityUpdates);

    std::map<std::string, int> remainingMinutesByTask;
    std::string validationError;
    if (!validateInProgressAllocations(effectiveTasks, inProgressAllocations,
            remainingMinutesByTask, validationError)) {
        ReplanningResult result;
        result.error = validationError;
        result.newSchedule.strategy = strategy;
        result.newSchedule.error = validationError;
        return result;
    }

    std::set<std::string> allocatedTaskIds;
    for (const InProgressAllocation& allocation : inProgressAllocations) {
        allocatedTaskIds.insert(allocation.taskId);
    }
    for (const Task& task : effectiveTasks) {
        if (task.status == Status::InProgress
            && allocatedTaskIds.count(task.id) == 0) {
            ReplanningResult result;
            result.error = "In-progress task " + task.id
                + " has no fixed allocation.";
            result.newSchedule.strategy = strategy;
            result.newSchedule.error = result.error;
            return result;
        }
    }

    ReplanningResult result;
    result.newSchedule.strategy = strategy;
    if (!validateFixedAllocationWindows(inProgressAllocations,
            effectiveAvailability, context, validationError)) {
        result.error = validationError;
        result.newSchedule.error = validationError;
        return result;
    }

    SchedulingContext fixedContext = context;
    std::vector<Task> schedulingTasks = effectiveTasks;
    for (const InProgressAllocation& allocation : inProgressAllocations) {
        fixedContext.commitments.push_back({
            allocation.allocation.start, allocation.allocation.end});
        fixedContext.fixedTaskEndTimes[allocation.taskId] =
            allocation.allocation.end;
        const auto task = std::find_if(schedulingTasks.begin(),
            schedulingTasks.end(), [&](const Task& item) {
                return item.id == allocation.taskId;
            });
        if (task != schedulingTasks.end()) {
            task->status = Status::Completed;
        }
    }

    result = replan(schedulingTasks, effectiveAvailability,
        effectiveDependencies, {}, {}, {}, {}, {}, validator, fixedContext,
        strategy);
    result.remainingMinutesByTask = std::move(remainingMinutesByTask);
    for (auto& alternative : result.alternativeSchedules) {
        addFixedAllocations(alternative.second, inProgressAllocations);
        alternative.second.metrics.totalTasks = effectiveTasks.size();
    }
    if (!result.newSchedule.entries.empty()
        || !inProgressAllocations.empty()) {
        addFixedAllocations(result.newSchedule, inProgressAllocations);
    }
    result.newSchedule.metrics.totalTasks = effectiveTasks.size();

    if (result.valid) {
        const auto unscheduled = std::find_if(
            result.newSchedule.unscheduledTasks.begin(),
            result.newSchedule.unscheduledTasks.end(),
            [](const UnscheduledTask& item) {
                return item.task.status != Status::Completed;
            });
        if (unscheduled != result.newSchedule.unscheduledTasks.end()) {
            result.valid = false;
            result.error = "Replanning is infeasible: task "
                + unscheduled->task.id + " could not be allocated. "
                + unscheduled->reason;
            result.newSchedule.valid = false;
            result.newSchedule.error = result.error;
        } else {
            std::string scheduleError;
            if (!validateGeneratedSchedule(result.newSchedule, effectiveTasks,
                    effectiveAvailability, effectiveDependencies, context,
                    scheduleError)) {
                result.valid = false;
                result.error = "Fixed-allocation schedule validation failed: "
                    + scheduleError;
                result.newSchedule.valid = false;
                result.newSchedule.error = result.error;
            }
        }
    }
    if (!inProgressAllocations.empty()) {
        result.warnings.push_back(
            "Fixed allocations were preserved; elapsed work was subtracted "
            "from each task's remaining duration.");
    }
    if (!result.valid && result.newSchedule.error.empty()) {
        result.newSchedule.valid = false;
        result.newSchedule.error = result.error;
    }
    return result;
}

int runSector06Demo()
{
    std::cout << "=== TaskFlow Sector 06: Replanning & Impact Analysis ===\n";

    DependencyManager validator("unused-sector-06-demo.csv");
    SchedulingContext context;
    std::tm demoNow = {};
    demoNow.tm_year = 2028 - 1900;
    demoNow.tm_mon = 0;
    demoNow.tm_mday = 3;
    demoNow.tm_hour = 9;
    demoNow.tm_min = 0;
    demoNow.tm_sec = 0;
    demoNow.tm_isdst = -1;
    const std::time_t demoEpoch = std::mktime(&demoNow);
    context.now = std::chrono::system_clock::from_time_t(demoEpoch);

    std::vector<Task> initialTasks = {
        {"TSK-060200", "Requirements", "Collect requirements", Priority::High, 90, "2028-01-05T12:00", "Project", Status::Pending},
        {"TSK-060201", "Design draft", "Draft the design", Priority::Medium, 60, "2028-01-05T14:00", "Project", Status::Pending},
        {"TSK-060202", "Review design", "Review and approve the design", Priority::Low, 45, "2028-01-06T10:00", "Project", Status::Pending},
        {"TSK-060203", "Status update", "Prepare the progress update", Priority::Critical, 30, "2028-01-06T15:00", "Project", Status::InProgress}
    };
    std::vector<Dependency> initialDependencies = {
        {"TSK-060201", "TSK-060200"},
        {"TSK-060202", "TSK-060201"}
    };
    std::vector<AvailabilityWindow> initialAvailability = {
        {"AV-MON", DayOfWeek::Monday, 9 * 60, 10 * 60, "Monday availability"},
        {"AV-TUE", DayOfWeek::Tuesday, 9 * 60, 17 * 60, "Tuesday availability"},
        {"AV-WED", DayOfWeek::Wednesday, 9 * 60, 17 * 60, "Wednesday availability"}
    };
    ScheduleEntry progressEntry;
    progressEntry.taskId = initialTasks[3].id;
    progressEntry.taskName = initialTasks[3].name;
    progressEntry.start = context.now;
    progressEntry.end = context.now + std::chrono::minutes(
        initialTasks[3].durationMinutes);
    progressEntry.durationMinutes = initialTasks[3].durationMinutes;
    progressEntry.strategy = SchedulingStrategy::EDF;
    progressEntry.availabilityWindowId = "AV-MON";
    progressEntry.deadline = initialTasks[3].deadline;
    progressEntry.dependencyState = "In Progress";
    progressEntry.selectionExplanation = "Existing fixed in-progress allocation.";
    const InProgressAllocation progressAllocation{
        initialTasks[3].id, progressEntry, 10};
    const std::vector<InProgressAllocation> fixedAllocations = {
        progressAllocation
    };

    ReplanningEngine engine;
    std::cout << "Initial schedule:\n";
    const ReplanningResult initialResult = engine.replan(initialTasks,
        initialAvailability, initialDependencies, {}, {}, {}, {}, {},
        fixedAllocations, validator, context, SchedulingStrategy::EDF);
    if (initialResult.valid && !initialResult.newSchedule.entries.empty()) {
        for (const ScheduleEntry& entry : initialResult.newSchedule.entries) {
            std::cout << "  " << entry.taskId << " | "
                      << formatLocalTimePoint(entry.start) << " -> "
                      << formatLocalTimePoint(entry.end) << " | "
                      << entry.selectionExplanation << "\n";
        }
    } else {
        std::cout << "  No valid initial schedule was produced.\n";
    }

    std::vector<AvailabilityUpdate> availabilityChanges = {
        {AvailabilityUpdate::Type::Update, "AV-TUE",
         {"AV-TUE", DayOfWeek::Tuesday, 9 * 60, 15 * 60, "Reduced Tuesday availability"}}
    };
    std::vector<Task> updatedTasks = initialTasks;
    updatedTasks[0].durationMinutes = 120;
    updatedTasks[0].deadline = "2028-01-05T13:00";
    updatedTasks[2].priority = Priority::High;
    updatedTasks[3].status = Status::InProgress;

    std::cout << "\nTriggering change: Tuesday availability reduced and task TSK-060200 duration changed.\n";
    const ReplanningResult replanned = engine.replan(initialTasks,
        initialAvailability,
        initialDependencies,
        {},
        updatedTasks,
        {},
        availabilityChanges,
        {},
        fixedAllocations,
        validator,
        context,
        SchedulingStrategy::Priority);
    if (replanned.valid) {
        std::cout << "Replanning result: valid replacement schedule generated.\n";
        for (const ScheduleEntry& entry : replanned.newSchedule.entries) {
            std::cout << "  " << entry.taskId << " | "
                      << formatLocalTimePoint(entry.start) << " -> "
                      << formatLocalTimePoint(entry.end) << " | "
                      << entry.selectionExplanation << "\n";
        }
        std::cout << "Affected tasks: ";
        for (const std::string& id : replanned.affectedTasks) {
            std::cout << id << ' ';
        }
        std::cout << "\n";
        for (const auto& pair : replanned.remainingMinutesByTask) {
            std::cout << "  Remaining minutes for " << pair.first << ": "
                      << pair.second << "\n";
        }
        std::cout << "Alternative schedules:\n";
        std::set<std::string> printedAlternatives;
        for (const auto& entry : replanned.alternativeSchedules) {
            const std::string key = toString(entry.first) + ":"
                + std::to_string(entry.second.entries.size()) + ":"
                + std::to_string(entry.second.valid);
            if (printedAlternatives.insert(key).second) {
                std::cout << "  " << toString(entry.first) << " | valid="
                          << (entry.second.valid ? "yes" : "no")
                          << " entries=" << entry.second.entries.size()
                          << " unscheduled=" << entry.second.unscheduledTasks.size()
                          << "\n";
            }
        }
    } else {
        std::cout << "Replanning result: rejected.\n";
        std::cout << "  " << replanned.error << "\n";
    }

    std::vector<Task> whatIfTasks = initialTasks;
    whatIfTasks[0].durationMinutes = 120;
    const std::vector<AvailabilityWindow> whatIfAvailability = {
        {"AV-PLAN", DayOfWeek::Tuesday, 9 * 60, 16 * 60, "Tuesday plan window"}
    };
    const ScenarioEvaluation scenario = evaluateWhatIfScenario(initialTasks,
        initialAvailability, initialDependencies, whatIfTasks,
        whatIfAvailability, {}, validator, context, SchedulingStrategy::EDF);
    std::cout << "\nWhat-if simulation: " << scenario.summary << "\n";
    for (const ScheduleDelta& delta : scenario.deltas) {
        std::cout << "  " << delta.taskId << " | " << delta.action
                  << " | " << delta.details << "\n";
    }

    const std::vector<Task> impossibleTasks = {
        {"TSK-060300", "Impossible task", "Cannot finish before deadline", Priority::Critical, 180, "2028-01-03T10:00", "Project", Status::Pending}
    };
    const std::vector<AvailabilityWindow> impossibleAvailability = {
        {"AV-IMP", DayOfWeek::Tuesday, 9 * 60, 10 * 60, "Too short to fit"}
    };
    const ReplanningResult impossible = engine.replan(impossibleTasks,
        impossibleAvailability, {}, {}, {}, {}, {}, {}, validator, context,
        SchedulingStrategy::Priority);
    std::cout << "\nInfeasible scenario: ";
    if (impossible.valid) {
        std::cout << "unexpectedly scheduled\n";
    } else {
        std::cout << "rejected as expected\n";
        std::cout << "  " << impossible.error << "\n";
    }

    std::cout << "=== Sector 06 demo complete ===\n";
    return 0;
}

int runReplanningTests(const std::filesystem::path& testDirectory)
{
    (void)testDirectory;
    ReplanningTestRun tests;
    DependencyManager validator("unused-sector-06-tests.csv");
    std::tm testDate = {};
    testDate.tm_year = 2028 - 1900;
    testDate.tm_mon = 0;
    testDate.tm_mday = 3;
    testDate.tm_hour = 16;
    testDate.tm_isdst = -1;
    const std::time_t testRaw = std::mktime(&testDate);
    if (testRaw == static_cast<std::time_t>(-1)) {
        std::cerr << "Sector 06 tests could not create a deterministic local time.\n";
        return 1;
    }
    SchedulingContext context;
    context.now = std::chrono::system_clock::from_time_t(testRaw);
    context.customRanks = {{"TSK-060001", 1}};

    const std::vector<Task> tasks = {
        {"TSK-060001", "Task A", "", Priority::High, 60, "20:00", "Core", Status::Pending},
        {"TSK-060002", "Task B", "Depends on A", Priority::Medium, 45, "20:00", "Core", Status::Pending},
        {"TSK-060003", "Task C", "Depends on B", Priority::Low, 30, "20:00", "Core", Status::Pending}
    };
    const std::vector<Dependency> dependencies = {
        {"TSK-060002", "TSK-060001"},
        {"TSK-060003", "TSK-060002"}
    };
    const std::vector<AvailabilityWindow> availability = {
        {"AV-1", DayOfWeek::Monday, 16 * 60, 20 * 60, "Available window"}
    };

    const std::vector<AvailabilityWindow> updated = applyAvailabilityUpdate(availability,
        {AvailabilityUpdate::Type::Add, "AV-2", {"AV-2", DayOfWeek::Tuesday, 9 * 60, 12 * 60, "Extra availability"}});
    tests.check(updated.size() == 2, "availability add retains existing windows");

    ImpactAnalyzer analyzer;
    const ImpactAnalysis impact = analyzer.analyze(tasks, dependencies, availability,
        {"TSK-060001"}, {});
    tests.check(!impact.directlyAffected.empty() && !impact.entries.empty(),
        "impact analysis reports a changed task");

    ReplanningEngine engine;
    const auto replan = [&](const std::vector<Task>& caseTasks,
                            const std::vector<Dependency>& caseDependencies,
                            const std::vector<AvailabilityWindow>& caseAvailability,
                            const SchedulingContext& caseContext) {
        return engine.replan(caseTasks, caseAvailability, caseDependencies,
            {}, {}, {}, {}, {}, validator, caseContext,
            SchedulingStrategy::Priority);
    };
    const auto hasUnscheduledReason = [](const GeneratedSchedule& schedule,
                                         const std::string& taskId,
                                         const std::string& reasonText) {
        return std::any_of(schedule.unscheduledTasks.begin(),
            schedule.unscheduledTasks.end(),
            [&](const UnscheduledTask& item) {
                return item.task.id == taskId
                    && item.reason.find(reasonText) != std::string::npos;
            });
    };
    const auto describeSchedule = [](const GeneratedSchedule& schedule) {
        std::string description = "valid=" + std::to_string(schedule.valid)
            + ", error=" + schedule.error + ", entries="
            + std::to_string(schedule.entries.size());
        for (const UnscheduledTask& item : schedule.unscheduledTasks) {
            description += "; " + item.task.id + ": " + item.reason;
        }
        return description;
    };
    const auto testTask = [](const std::string& id, int duration,
                             const std::string& deadline,
                             Status status = Status::Pending) {
        return Task{id, id, "", Priority::High, duration, deadline, "Test", status};
    };
    const Task progressTask = testTask("TSK-060020", 60, "20:00",
        Status::InProgress);
    const auto progressAllocation = [&](const Task& task, int elapsedMinutes) {
        ScheduleEntry entry;
        entry.taskId = task.id;
        entry.taskName = task.name;
        entry.start = context.now;
        entry.end = context.now + std::chrono::minutes(task.durationMinutes);
        entry.durationMinutes = task.durationMinutes;
        entry.availabilityWindowId = "AV-1";
        return InProgressAllocation{task.id, entry, elapsedMinutes};
    };
    const auto fixedPlan = [&](const std::vector<Task>& caseTasks,
                               const std::vector<Dependency>& caseDependencies,
                               const std::vector<AvailabilityWindow>& caseAvailability,
                               const std::vector<InProgressAllocation>& allocations) {
        return engine.replan(caseTasks, caseAvailability, caseDependencies,
            {}, {}, {}, {}, {}, allocations, validator, context,
            SchedulingStrategy::Priority);
    };

    std::map<std::string, int> remaining;
    std::string progressError;
    tests.check(validateInProgressAllocations({progressTask},
            {progressAllocation(progressTask, 20)}, remaining, progressError)
            && remaining.at(progressTask.id) == 40,
        "elapsed work subtracts to the expected remaining duration");

    tests.check(validateInProgressAllocations({progressTask},
            {progressAllocation(progressTask, 0)}, remaining, progressError)
            && remaining.at(progressTask.id) == 60,
        "zero elapsed work leaves the full original duration remaining");

    const Task fixedPendingTask = testTask("TSK-060023", 30, "20:00");
    const InProgressAllocation fixedAllocation =
        progressAllocation(progressTask, 20);
    const ReplanningResult fixedPlanResult = fixedPlan(
        {progressTask, fixedPendingTask}, {}, availability, {fixedAllocation});
    const auto preservedEntry = std::find_if(
        fixedPlanResult.newSchedule.entries.begin(),
        fixedPlanResult.newSchedule.entries.end(),
        [&](const ScheduleEntry& entry) {
            return entry.taskId == progressTask.id;
        });
    const auto replannedPendingEntry = std::find_if(
        fixedPlanResult.newSchedule.entries.begin(),
        fixedPlanResult.newSchedule.entries.end(),
        [&](const ScheduleEntry& entry) {
            return entry.taskId == fixedPendingTask.id;
        });
    tests.check(fixedPlanResult.valid
            && preservedEntry != fixedPlanResult.newSchedule.entries.end()
            && preservedEntry->taskName == fixedAllocation.allocation.taskName
            && preservedEntry->start == fixedAllocation.allocation.start
            && preservedEntry->end == fixedAllocation.allocation.end
            && preservedEntry->durationMinutes
                == fixedAllocation.allocation.durationMinutes
            && preservedEntry->availabilityWindowId
                == fixedAllocation.allocation.availabilityWindowId,
        "fixed in-progress allocation is preserved without changing its boundaries");
    tests.check(fixedPlanResult.valid
            && fixedPlanResult.remainingMinutesByTask.at(progressTask.id) == 40
            && replannedPendingEntry != fixedPlanResult.newSchedule.entries.end()
            && replannedPendingEntry->start >= fixedAllocation.allocation.end,
        "elapsed work is excluded and pending work is placed after the fixed allocation");

    const Task fixedDependentTask = testTask("TSK-060024", 20, "20:00");
    const ReplanningResult fixedDependencyPlan = fixedPlan(
        {progressTask, fixedDependentTask},
        {{fixedDependentTask.id, progressTask.id}}, availability,
        {fixedAllocation});
    const auto dependentEntry = std::find_if(
        fixedDependencyPlan.newSchedule.entries.begin(),
        fixedDependencyPlan.newSchedule.entries.end(),
        [&](const ScheduleEntry& entry) {
            return entry.taskId == fixedDependentTask.id;
        });
    tests.check(fixedDependencyPlan.valid
            && dependentEntry != fixedDependencyPlan.newSchedule.entries.end()
            && dependentEntry->start >= fixedAllocation.allocation.end,
        "dependent pending work starts after the in-progress prerequisite ("
            + describeSchedule(fixedDependencyPlan.newSchedule) + ")");

    ScheduleEntry invalidFixedEntry = fixedAllocation.allocation;
    invalidFixedEntry.availabilityWindowId = "AV-MISSING";
    const ReplanningResult invalidFixedWindow = fixedPlan(
        {progressTask}, {}, availability,
        {{progressTask.id, invalidFixedEntry, 20}});
    tests.check(!invalidFixedWindow.valid
            && invalidFixedWindow.error.find("availability window")
                != std::string::npos,
        "fixed allocations outside configured availability are rejected");

    const std::vector<AvailabilityWindow> fullFixedWindow = {
        {"AV-1", DayOfWeek::Monday, 16 * 60, 17 * 60, "Fixed interval only"}
    };
    const Task insufficientPendingTask = testTask("TSK-060025", 15, "20:00");
    const ReplanningResult infeasibleFixedPlan = fixedPlan(
        {progressTask, insufficientPendingTask}, {}, fullFixedWindow,
        {fixedAllocation});
    tests.check(!infeasibleFixedPlan.valid
            && hasUnscheduledReason(infeasibleFixedPlan.newSchedule,
                insufficientPendingTask.id, "availability"),
        "pending work that cannot fit remaining availability is rejected");

    const Task secondProgressTask = testTask("TSK-060026", 60, "20:00",
        Status::InProgress);
    ScheduleEntry earlierFixedEntry = progressAllocation(
        secondProgressTask, 60).allocation;
    earlierFixedEntry.start = context.now - std::chrono::hours(1);
    earlierFixedEntry.end = context.now;
    ScheduleEntry currentFixedEntry = progressAllocation(
        progressTask, 20).allocation;
    const std::vector<AvailabilityWindow> multiFixedWindow = {
        {"AV-1", DayOfWeek::Monday, 15 * 60, 19 * 60, "Multiple fixed intervals"}
    };
    const ReplanningResult multipleFixedPlan = fixedPlan(
        {secondProgressTask, progressTask}, {}, multiFixedWindow,
        {{secondProgressTask.id, earlierFixedEntry, 60},
         {progressTask.id, currentFixedEntry, 20}});
    tests.check(multipleFixedPlan.valid
            && multipleFixedPlan.newSchedule.entries.size() == 2
            && multipleFixedPlan.remainingMinutesByTask.at(secondProgressTask.id) == 0
            && multipleFixedPlan.remainingMinutesByTask.at(progressTask.id) == 40,
        "multiple non-overlapping in-progress allocations are preserved");

    tests.check(validateInProgressAllocations({progressTask},
            {progressAllocation(progressTask, 60)}, remaining, progressError)
            && remaining.at(progressTask.id) == 0,
        "elapsed work equal to task duration leaves zero remaining");

    tests.check(!validateInProgressAllocations({progressTask},
            {progressAllocation(progressTask, -1)}, remaining, progressError)
            && !progressError.empty(),
        "negative elapsed work is rejected with an error");

    tests.check(!validateInProgressAllocations({progressTask},
            {progressAllocation(progressTask, 61)}, remaining, progressError)
            && !progressError.empty(),
        "elapsed work greater than task duration is rejected");

    tests.check(!validateInProgressAllocations({progressTask},
            {progressAllocation(testTask("TSK-060099", 30, "20:00",
                Status::InProgress), 5)}, remaining, progressError)
            && progressError.find("unknown task ID") != std::string::npos,
        "allocation for an unknown task ID is rejected");

    tests.check(!validateInProgressAllocations({progressTask},
            {progressAllocation(progressTask, 5),
             progressAllocation(progressTask, 10)}, remaining, progressError)
            && progressError.find("Duplicate") != std::string::npos,
        "duplicate allocations for one task are rejected");

    const Task completedProgressTask = testTask("TSK-060021", 60, "20:00",
        Status::Completed);
    tests.check(!validateInProgressAllocations({completedProgressTask},
            {progressAllocation(completedProgressTask, 10)}, remaining,
            progressError)
            && progressError.find("Completed") != std::string::npos,
        "completed tasks cannot be supplied as in-progress allocations");

    const auto noWindow = replan(
        {testTask("TSK-060010", 30, "18:00")}, {}, {}, context);
    tests.check(!noWindow.valid && noWindow.newSchedule.entries.empty()
            && hasUnscheduledReason(noWindow.newSchedule, "TSK-060010",
                "No available window"),
        "task with no availability remains unscheduled with an explicit reason ("
            + describeSchedule(noWindow.newSchedule) + ")");

    const auto impossibleDeadline = replan(
        {testTask("TSK-060011", 60, "16:30")}, {}, availability,
        context);
    tests.check(!impossibleDeadline.valid
            && hasUnscheduledReason(impossibleDeadline.newSchedule,
                "TSK-060011", "Deadline exceeded"),
        "impossible deadline does not produce a valid candidate");

    const std::vector<Task> blockedTasks = {
        testTask("TSK-060012", 60, "16:30"),
        testTask("TSK-060013", 30, "18:00")
    };
    const auto blockedDependency = replan(blockedTasks,
        {{"TSK-060013", "TSK-060012"}}, availability, context);
    tests.check(!blockedDependency.valid
            && hasUnscheduledReason(blockedDependency.newSchedule,
                "TSK-060013", "Dependency blocked"),
        "dependent task remains blocked when its prerequisite cannot be scheduled");

    const std::vector<Task> whatIfBaseTasks = {
        testTask("TSK-060040", 30, "20:00"),
        testTask("TSK-060041", 30, "20:00")
    };
    const auto whatIfIsolation = evaluateWhatIfScenario(whatIfBaseTasks,
        availability, {}, whatIfBaseTasks, availability, {},
        validator, context, SchedulingStrategy::Priority);
    tests.check(whatIfIsolation.valid && !whatIfIsolation.scenario.originalDataModified,
        "what-if evaluation reports unchanged inputs without mutating original state");

    const auto completedTask = replan(
        {testTask("TSK-060014", 30, "18:00", Status::Completed)},
        {}, availability, context);
    tests.check(completedTask.valid && completedTask.newSchedule.entries.empty()
            && std::none_of(completedTask.newSchedule.entries.begin(),
                completedTask.newSchedule.entries.end(),
                [](const ScheduleEntry& entry) {
                    return entry.taskId == "TSK-060014";
                }),
        "completed task is not reallocated");

    Task attemptedCompletedUpdate = testTask("TSK-060014", 30, "18:00");
    attemptedCompletedUpdate.name = "Changed completed task";
    const auto completedUpdate = engine.replan(
        {testTask("TSK-060014", 30, "18:00", Status::Completed)},
        availability, {}, {}, {attemptedCompletedUpdate},
        {"TSK-060014"}, {}, {}, validator, context,
        SchedulingStrategy::Priority);
    tests.check(completedUpdate.valid
            && completedUpdate.newSchedule.entries.empty(),
        "completed task updates and removals do not reschedule completed work");

    const std::vector<AvailabilityWindow> oneHourWindow = {
        {"AV-VALID", DayOfWeek::Monday, 16 * 60, 17 * 60, "One hour"}
    };
    const auto validWindow = replan(
        {testTask("TSK-060015", 30, "17:00")}, {},
        oneHourWindow, context);
    tests.check(validWindow.valid && validWindow.newSchedule.entries.size() == 1
            && validWindow.newSchedule.entries.front().start >= context.now
            && validWindow.newSchedule.entries.front().end
                <= context.now + std::chrono::hours(1)
            && validWindow.newSchedule.entries.front().durationMinutes == 30,
        "task fits entirely within a real availability window ("
            + describeSchedule(validWindow.newSchedule) + ")");

    SchedulingContext allocatedContext = context;
    allocatedContext.commitments.push_back({
        context.now, context.now + std::chrono::minutes(30)});
    const auto existingAllocation = replan(
        {testTask("TSK-060016", 30, "18:00",
            Status::InProgress)}, {}, availability, allocatedContext);
    tests.check(!existingAllocation.valid
            && std::none_of(existingAllocation.newSchedule.entries.begin(),
                existingAllocation.newSchedule.entries.end(),
                [](const ScheduleEntry& entry) {
                    return entry.taskId == "TSK-060016";
                })
            && hasUnscheduledReason(existingAllocation.newSchedule,
                "TSK-060016", "In-progress task preserved"),
        "existing in-progress allocation is not generated a second time");

    const auto validReplan = replan(tasks, dependencies, availability, context);
    tests.check(validReplan.valid && !validReplan.newSchedule.entries.empty(),
        "legacy replanning signature remains source-compatible");

    const std::vector<Task> extendedTasks = {
        testTask("TSK-060022", 30, "20:00"), progressTask
    };
    const ReplanningResult extendedApiResult = engine.replan(
        extendedTasks,
        availability, {}, {}, {}, {}, {}, {},
        {progressAllocation(progressTask, 15)}, validator, context,
        SchedulingStrategy::Priority);
    tests.check(extendedApiResult.valid
            && extendedApiResult.remainingMinutesByTask.at(progressTask.id) == 45
            && !extendedApiResult.warnings.empty()
            && std::any_of(extendedApiResult.newSchedule.entries.begin(),
                extendedApiResult.newSchedule.entries.end(),
                [&](const ScheduleEntry& entry) {
                    return entry.taskId == progressTask.id;
                }),
        "extended API preserves fixed allocations and reports remaining work");

    GeneratedSchedule authoritativeProgressSchedule;
    authoritativeProgressSchedule.valid = true;
    authoritativeProgressSchedule.entries.push_back(
        progressAllocation(progressTask, 15).allocation);
    GeneratedSchedule fixedOnlyCandidate;
    fixedOnlyCandidate.valid = true;
    fixedOnlyCandidate.entries.push_back(
        progressAllocation(progressTask, 15).allocation);
    const std::string progressFingerprint = scheduleFingerprint(
        {progressTask}, availability, {});
    const auto commitFixedCandidate = [&](const GeneratedSchedule& candidate) {
        return commitSchedule({
                {progressTask}, availability, {},
                authoritativeProgressSchedule, candidate,
                progressFingerprint, ""},
            validator, context);
    };
    const auto failedCommitPreservesPrevious = [&](
        const CommitResult& commit) {
        return !commit.valid
            && commit.committedSchedule.entries.size()
                == authoritativeProgressSchedule.entries.size()
            && sameFixedAllocation(commit.committedSchedule.entries.front(),
                authoritativeProgressSchedule.entries.front());
    };

    const CommitResult unchangedFixedCommit =
        commitFixedCandidate(fixedOnlyCandidate);
    tests.check(unchangedFixedCommit.valid
            && unchangedFixedCommit.committedSchedule.entries.size() == 1
            && sameFixedAllocation(
                unchangedFixedCommit.committedSchedule.entries.front(),
                authoritativeProgressSchedule.entries.front()),
        "commit accepts an unchanged authoritative in-progress allocation");

    GeneratedSchedule missingFixedCandidate = fixedOnlyCandidate;
    missingFixedCandidate.entries.clear();
    const CommitResult missingFixedCommit =
        commitFixedCandidate(missingFixedCandidate);
    tests.check(failedCommitPreservesPrevious(missingFixedCommit)
            && missingFixedCommit.error.find("exactly one fixed allocation")
                != std::string::npos,
        "commit rejects a missing in-progress allocation and preserves prior schedule");

    GeneratedSchedule movedFixedCandidate = fixedOnlyCandidate;
    movedFixedCandidate.entries.front().start += std::chrono::minutes(1);
    movedFixedCandidate.entries.front().end += std::chrono::minutes(1);
    const CommitResult movedFixedCommit =
        commitFixedCandidate(movedFixedCandidate);
    tests.check(failedCommitPreservesPrevious(movedFixedCommit)
            && movedFixedCommit.error.find("moved or modified")
                != std::string::npos,
        "commit rejects a moved in-progress allocation and preserves prior schedule");

    GeneratedSchedule changedEndCandidate = fixedOnlyCandidate;
    changedEndCandidate.entries.front().end -= std::chrono::minutes(1);
    const CommitResult changedEndCommit =
        commitFixedCandidate(changedEndCandidate);
    tests.check(failedCommitPreservesPrevious(changedEndCommit)
            && changedEndCommit.error.find("moved or modified")
                != std::string::npos,
        "commit rejects a modified allocation end and preserves prior schedule");

    GeneratedSchedule changedDurationCandidate = fixedOnlyCandidate;
    --changedDurationCandidate.entries.front().durationMinutes;
    const CommitResult changedDurationCommit =
        commitFixedCandidate(changedDurationCandidate);
    tests.check(failedCommitPreservesPrevious(changedDurationCommit)
            && changedDurationCommit.error.find("moved or modified")
                != std::string::npos,
        "commit rejects a modified allocation duration and preserves prior schedule");

    GeneratedSchedule duplicateFixedCandidate = fixedOnlyCandidate;
    duplicateFixedCandidate.entries.push_back(
        duplicateFixedCandidate.entries.front());
    const CommitResult duplicateFixedCommit =
        commitFixedCandidate(duplicateFixedCandidate);
    tests.check(failedCommitPreservesPrevious(duplicateFixedCommit)
            && duplicateFixedCommit.error.find("exactly one fixed allocation")
                != std::string::npos,
        "commit rejects duplicate in-progress allocations and preserves prior schedule");

    GeneratedSchedule missingAuthorityCandidate = fixedOnlyCandidate;
    const CommitResult missingAuthorityCommit = commitSchedule({
            {progressTask}, availability, {}, GeneratedSchedule{},
            missingAuthorityCandidate, progressFingerprint, ""},
        validator, context);
    tests.check(!missingAuthorityCommit.valid
            && missingAuthorityCommit.committedSchedule.entries.empty()
            && missingAuthorityCommit.error.find("authoritative valid previous")
                != std::string::npos,
        "direct commit rejects in-progress allocation without authoritative previous schedule");

    const GeneratedSchedule selectedCandidate = extendedApiResult.newSchedule;
    const CommitRequest commitRequest = {
        extendedTasks,
        availability,
        {},
        authoritativeProgressSchedule,
        selectedCandidate,
        scheduleFingerprint(extendedTasks, availability, {}),
        "version-1"};
    const CommitResult commitResult = commitSchedule(commitRequest, validator, context);
    const auto committedProgressEntry = std::find_if(
        commitResult.committedSchedule.entries.begin(),
        commitResult.committedSchedule.entries.end(),
        [&](const ScheduleEntry& entry) {
            return entry.taskId == progressTask.id;
        });
    tests.check(commitResult.valid
            && commitResult.summary.find("accepted") != std::string::npos
            && committedProgressEntry
                != commitResult.committedSchedule.entries.end()
            && sameFixedAllocation(*committedProgressEntry,
                authoritativeProgressSchedule.entries.front()),
        "transactional commit accepts a valid candidate containing its fixed allocation");

    GeneratedSchedule previousSnapshot;
    previousSnapshot.valid = true;
    previousSnapshot.entries.push_back(fixedAllocation.allocation);
    const CommitRequest failedCommitRequest = {
        {progressTask, insufficientPendingTask},
        fullFixedWindow,
        {},
        previousSnapshot,
        infeasibleFixedPlan.newSchedule,
        scheduleFingerprint({progressTask, insufficientPendingTask},
            fullFixedWindow, {}),
        "version-failed"};
    const CommitResult failedCommit = commitSchedule(failedCommitRequest,
        validator, context);
    tests.check(!failedCommit.valid
            && failedCommit.committedSchedule.entries.size()
                == previousSnapshot.entries.size()
            && sameFixedAllocation(failedCommit.committedSchedule.entries.front(),
                previousSnapshot.entries.front()),
        "failed commit retains the previous schedule without partial changes");

    const Task completedForCommit = testTask("TSK-060027", 20, "20:00",
        Status::Completed);
    const Task pendingForCommit = testTask("TSK-060028", 20, "20:00");
    ScheduleEntry previousCompletedEntry;
    previousCompletedEntry.taskId = completedForCommit.id;
    previousCompletedEntry.taskName = completedForCommit.name;
    previousCompletedEntry.start = context.now - std::chrono::minutes(20);
    previousCompletedEntry.end = context.now;
    previousCompletedEntry.durationMinutes = 20;
    previousCompletedEntry.availabilityWindowId = "AV-1";
    GeneratedSchedule completedSnapshot;
    completedSnapshot.valid = true;
    completedSnapshot.entries.push_back(previousCompletedEntry);
    const std::vector<Task> completedCommitTasks = {
        completedForCommit, pendingForCommit
    };
    const ReplanningResult completedCandidate = replan(completedCommitTasks,
        {}, availability, context);
    const CommitResult completedCommit = commitSchedule({
            completedCommitTasks, availability, {}, completedSnapshot,
            completedCandidate.newSchedule,
            scheduleFingerprint(completedCommitTasks, availability, {}), ""},
        validator, context);
    const auto retainedCompletedEntry = std::find_if(
        completedCommit.committedSchedule.entries.begin(),
        completedCommit.committedSchedule.entries.end(),
        [&](const ScheduleEntry& entry) {
            return entry.taskId == completedForCommit.id;
        });
    tests.check(completedCommit.valid
            && retainedCompletedEntry
                != completedCommit.committedSchedule.entries.end()
            && retainedCompletedEntry->start == previousCompletedEntry.start
            && retainedCompletedEntry->end == previousCompletedEntry.end
            && retainedCompletedEntry->durationMinutes
                == previousCompletedEntry.durationMinutes,
        "successful commit retains completed task allocations unchanged");

    GeneratedSchedule invalidSimulatedCandidate =
        extendedApiResult.newSchedule;
    const auto simulatedFixedEntry = std::find_if(
        invalidSimulatedCandidate.entries.begin(),
        invalidSimulatedCandidate.entries.end(),
        [&](const ScheduleEntry& entry) {
            return entry.taskId == progressTask.id;
        });
    if (simulatedFixedEntry != invalidSimulatedCandidate.entries.end()) {
        simulatedFixedEntry->end += std::chrono::minutes(1);
    }
    const CommitResult invalidDirectCommit = commitSchedule({
            extendedTasks, availability, {}, authoritativeProgressSchedule,
            invalidSimulatedCandidate,
            scheduleFingerprint(extendedTasks, availability, {}), ""},
        validator, context);
    tests.check(extendedApiResult.valid
            && !invalidDirectCommit.valid
            && invalidDirectCommit.committedSchedule.entries.size()
                == authoritativeProgressSchedule.entries.size()
            && sameFixedAllocation(
                invalidDirectCommit.committedSchedule.entries.front(),
                authoritativeProgressSchedule.entries.front()),
        "invalid direct commit after successful simulation is rejected without changing prior schedule");

    const ReplanningResult pendingOnlyCandidate = replan(
        {pendingForCommit}, {}, availability, context);
    const CommitResult pendingOnlyCommit = commitSchedule({
            {pendingForCommit}, availability, {}, GeneratedSchedule{},
            pendingOnlyCandidate.newSchedule,
            scheduleFingerprint({pendingForCommit}, availability, {}), ""},
        validator, context);
    tests.check(pendingOnlyCandidate.valid && pendingOnlyCommit.valid
            && pendingOnlyCommit.committedSchedule.entries.size() == 1
            && pendingOnlyCommit.committedSchedule.entries.front().taskId
                == pendingForCommit.id,
        "valid pending-task allocation remains committable");

    const InProgressAllocation unknownProgress =
        progressAllocation(testTask("TSK-060099", 30, "20:00",
            Status::InProgress), 5);
    const ReplanningResult invalidProgressApiResult = engine.replan(
        {progressTask}, availability, {}, {}, {}, {}, {}, {},
        {unknownProgress}, validator, context, SchedulingStrategy::Priority);
    tests.check(!invalidProgressApiResult.valid
            && invalidProgressApiResult.error.find("unknown task ID")
                != std::string::npos,
        "extended API rejects unknown task IDs with a clear error");

    const CommitRequest staleRequest = {
        extendedTasks,
        availability,
        {},
        authoritativeProgressSchedule,
        selectedCandidate,
        "stale-fingerprint",
        "version-stale"};
    const CommitResult staleCommit = commitSchedule(staleRequest, validator, context);
    tests.check(!staleCommit.valid && !staleCommit.error.empty(),
        "stale candidate rejection preserves current state and prior schedule");

    std::cout << "Sector 06 tests: " << tests.passed << " passed, "
              << tests.failed << " failed.\n";
    return tests.failed == 0 ? 0 : 1;
}

} // namespace taskflow

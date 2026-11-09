#include "scheduling_management.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace taskflow {
namespace {

struct Candidate {
    const Task* task = nullptr;
    SchedulingTimePoint deadline;
    TaskScheduleMetrics metrics;
    int priorityRank = 0;
    int customRank = 0;
};

bool localTime(std::time_t value, std::tm& result)
{
#ifdef _WIN32
    return localtime_s(&result, &value) == 0;
#else
    return localtime_r(&value, &result) != nullptr;
#endif
}

bool makeLocalTime(int year, int month, int day, int hour, int minute,
    std::time_t& result)
{
    std::tm value = {};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_isdst = -1;
    const std::tm expected = value;
    result = std::mktime(&value);
    if (result == static_cast<std::time_t>(-1)) return false;
    std::tm actual = {};
    return localTime(result, actual)
        && actual.tm_year == expected.tm_year
        && actual.tm_mon == expected.tm_mon
        && actual.tm_mday == expected.tm_mday
        && actual.tm_hour == expected.tm_hour
        && actual.tm_min == expected.tm_min;
}

bool parseDigits(const std::string& text, std::size_t position,
    std::size_t count, int& value)
{
    if (position + count > text.size()) return false;
    value = 0;
    for (std::size_t index = position; index < position + count; ++index) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if (!std::isdigit(ch)) return false;
        value = value * 10 + (ch - '0');
    }
    return true;
}

bool parseDeadline(const std::string& text, SchedulingTimePoint now,
    SchedulingTimePoint& deadline)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 23;
    int minute = 59;
    if (text.size() == 5 && text[2] == ':') {
        std::tm current = {};
        const std::time_t nowValue = std::chrono::system_clock::to_time_t(now);
        if (!localTime(nowValue, current)
            || !parseDigits(text, 0, 2, hour)
            || !parseDigits(text, 3, 2, minute)) return false;
        year = current.tm_year + 1900;
        month = current.tm_mon + 1;
        day = current.tm_mday;
    } else {
        if (text.size() != 10 && text.size() != 16) return false;
        if (text[4] != '-' || text[7] != '-') return false;
        if (!parseDigits(text, 0, 4, year)
            || !parseDigits(text, 5, 2, month)
            || !parseDigits(text, 8, 2, day)) return false;
        if (text.size() == 16) {
            if ((text[10] != 'T' && text[10] != ' ')
                || text[13] != ':'
                || !parseDigits(text, 11, 2, hour)
                || !parseDigits(text, 14, 2, minute)) return false;
        }
    }
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31
        || hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    std::time_t converted = 0;
    if (!makeLocalTime(year, month, day, hour, minute, converted)) return false;
    deadline = std::chrono::system_clock::from_time_t(converted);
    return true;
}

int weekdayIndex(const std::tm& value)
{
    return (value.tm_wday + 6) % 7;
}

bool validAvailability(const std::vector<AvailabilityWindow>& windows,
    std::string& error)
{
    if (windows.empty()) {
        error = "No availability windows are configured; slack cannot be calculated.";
        return false;
    }
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const AvailabilityWindow& window = windows[index];
        const int day = static_cast<int>(window.day);
        if (day < 0 || day > 6 || window.startMinute < 0
            || window.startMinute >= 1440 || window.endMinute <= window.startMinute
            || window.endMinute > 1440) {
            error = "Availability window " + window.id + " has invalid day or times.";
            return false;
        }
        for (std::size_t other = 0; other < index; ++other) {
            const AvailabilityWindow& previous = windows[other];
            if (previous.day == window.day
                && previous.startMinute < window.endMinute
                && window.startMinute < previous.endMinute) {
                error = "Availability windows overlap on "
                    + toString(window.day) + ".";
                return false;
            }
        }
    }
    return true;
}

bool validCommitments(const std::vector<CommittedInterval>& intervals,
    std::string& error)
{
    for (const CommittedInterval& interval : intervals) {
        if (interval.start >= interval.end) {
            error = "A committed interval has an invalid or empty time range.";
            return false;
        }
    }
    return true;
}

std::int64_t minuteValue(SchedulingTimePoint time)
{
    return std::chrono::duration_cast<std::chrono::minutes>(
        time.time_since_epoch()).count();
}

std::int64_t ceilMinuteValue(SchedulingTimePoint time)
{
    return std::chrono::ceil<std::chrono::minutes>(
        time.time_since_epoch()).count();
}

std::int64_t clippedWorkMinutes(std::int64_t start, std::int64_t end,
    const std::vector<CommittedInterval>& commitments)
{
    if (start >= end) return 0;
    std::vector<std::pair<std::int64_t, std::int64_t>> busy;
    for (const CommittedInterval& interval : commitments) {
        const std::int64_t busyStart = std::max(start, minuteValue(interval.start));
        const std::int64_t busyEnd = std::min(end, minuteValue(interval.end));
        if (busyStart < busyEnd) busy.emplace_back(busyStart, busyEnd);
    }
    std::sort(busy.begin(), busy.end());

    std::int64_t available = end - start;
    std::int64_t mergedStart = 0;
    std::int64_t mergedEnd = 0;
    bool haveRange = false;
    for (const auto& range : busy) {
        if (!haveRange) {
            mergedStart = range.first;
            mergedEnd = range.second;
            haveRange = true;
        } else if (range.first <= mergedEnd) {
            mergedEnd = std::max(mergedEnd, range.second);
        } else {
            available -= mergedEnd - mergedStart;
            mergedStart = range.first;
            mergedEnd = range.second;
        }
    }
    if (haveRange) available -= mergedEnd - mergedStart;
    return available;
}

bool availableWorkMinutes(SchedulingTimePoint startTime,
    SchedulingTimePoint endTime,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<CommittedInterval>& commitments,
    std::int64_t& total, std::string& error)
{
    total = 0;
    const std::int64_t firstMinute = ceilMinuteValue(startTime);
    const std::int64_t lastMinute = minuteValue(endTime);
    if (lastMinute <= firstMinute) return true;

    std::time_t cursor = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::time_point(std::chrono::minutes(firstMinute)));
    std::tm date = {};
    if (!localTime(cursor, date)) {
        error = "Could not convert the scheduling time to local time.";
        return false;
    }
    date.tm_hour = 0;
    date.tm_min = 0;
    date.tm_sec = 0;
    date.tm_isdst = -1;
    cursor = std::mktime(&date);
    if (cursor == static_cast<std::time_t>(-1)) {
        error = "Could not determine the local calendar day for availability.";
        return false;
    }

    std::size_t daysVisited = 0;
    while (true) {
        if (++daysVisited > 366000) {
            error = "Availability calculation exceeds its 1000-year safety limit.";
            return false;
        }
        std::tm currentDate = {};
        if (!localTime(cursor, currentDate)) {
            error = "Could not convert an availability date to local time.";
            return false;
        }
        const std::int64_t midnight = minuteValue(
            std::chrono::system_clock::from_time_t(cursor));
        const int day = weekdayIndex(currentDate);
        for (const AvailabilityWindow& window : availability) {
            if (static_cast<int>(window.day) != day) continue;
            std::tm begin = currentDate;
            begin.tm_hour = window.startMinute / 60;
            begin.tm_min = window.startMinute % 60;
            begin.tm_sec = 0;
            begin.tm_isdst = -1;
            std::tm finish = currentDate;
            finish.tm_hour = window.endMinute / 60;
            finish.tm_min = window.endMinute % 60;
            finish.tm_sec = 0;
            finish.tm_isdst = -1;
            const std::time_t beginTime = std::mktime(&begin);
            const std::time_t endTime = std::mktime(&finish);
            if (beginTime == static_cast<std::time_t>(-1)
                || endTime == static_cast<std::time_t>(-1)) {
                error = "Could not convert an availability window to local time.";
                return false;
            }
            const std::int64_t segmentStart = std::max(firstMinute,
                minuteValue(std::chrono::system_clock::from_time_t(beginTime)));
            const std::int64_t segmentEnd = std::min(lastMinute,
                minuteValue(std::chrono::system_clock::from_time_t(endTime)));
            total += clippedWorkMinutes(segmentStart, segmentEnd, commitments);
        }

        if (midnight >= lastMinute) break;
        currentDate.tm_mday += 1;
        currentDate.tm_hour = 0;
        currentDate.tm_min = 0;
        currentDate.tm_sec = 0;
        currentDate.tm_isdst = -1;
        cursor = std::mktime(&currentDate);
        if (cursor == static_cast<std::time_t>(-1)) {
            error = "Could not advance to the next local availability day.";
            return false;
        }
    }
    error.clear();
    return true;
}

std::map<std::string, std::size_t> downstreamImpacts(
    const std::vector<Task>& tasks, const std::vector<Dependency>& dependencies)
{
    std::unordered_set<std::string> unfinished;
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    for (const Task& task : tasks) {
        if (task.status != Status::Completed) unfinished.insert(task.id);
    }
    for (const Dependency& dependency : dependencies) {
        dependents[dependency.prerequisiteId].push_back(dependency.dependentId);
    }
    for (auto& entry : dependents) std::sort(entry.second.begin(), entry.second.end());

    std::map<std::string, std::size_t> result;
    for (const Task& task : tasks) {
        std::unordered_set<std::string> visited;
        std::vector<std::string> pending;
        const auto first = dependents.find(task.id);
        if (first != dependents.end()) pending = first->second;
        while (!pending.empty()) {
            std::string current = std::move(pending.back());
            pending.pop_back();
            if (current == task.id || !visited.insert(current).second) continue;
            const auto children = dependents.find(current);
            if (children != dependents.end()) {
                pending.insert(pending.end(), children->second.begin(),
                    children->second.end());
            }
        }
        std::size_t count = 0;
        for (const std::string& id : visited) {
            if (unfinished.count(id)) ++count;
        }
        result[task.id] = count;
    }
    return result;
}

std::unordered_map<std::string, std::vector<std::string>> incompleteByTask(
    const std::vector<Task>& tasks, const std::vector<Dependency>& dependencies)
{
    std::unordered_map<std::string, const Task*> taskById;
    for (const Task& task : tasks) taskById.emplace(task.id, &task);
    std::unordered_map<std::string, std::vector<std::string>> result;
    for (const Dependency& dependency : dependencies) {
        const auto prerequisite = taskById.find(dependency.prerequisiteId);
        if (prerequisite == taskById.end()
            || prerequisite->second->status != Status::Completed) {
            result[dependency.dependentId].push_back(dependency.prerequisiteId);
        }
    }
    for (auto& entry : result) {
        std::sort(entry.second.begin(), entry.second.end());
    }
    return result;
}

bool precedes(const Candidate& left, const Candidate& right,
    SchedulingStrategy strategy)
{
    switch (strategy) {
    case SchedulingStrategy::Priority:
        if (left.priorityRank != right.priorityRank)
            return left.priorityRank > right.priorityRank;
        break;
    case SchedulingStrategy::EDF:
        if (left.deadline != right.deadline) return left.deadline < right.deadline;
        break;
    case SchedulingStrategy::SJF:
        if (left.metrics.remainingDurationMinutes
            != right.metrics.remainingDurationMinutes) {
            return left.metrics.remainingDurationMinutes
                < right.metrics.remainingDurationMinutes;
        }
        break;
    case SchedulingStrategy::LST:
        if (left.metrics.slackMinutes != right.metrics.slackMinutes)
            return left.metrics.slackMinutes < right.metrics.slackMinutes;
        break;
    case SchedulingStrategy::Custom:
        if (left.customRank != right.customRank)
            return left.customRank < right.customRank;
        break;
    }

    if (strategy != SchedulingStrategy::EDF
        && left.deadline != right.deadline) return left.deadline < right.deadline;
    if (strategy != SchedulingStrategy::LST
        && left.metrics.slackMinutes != right.metrics.slackMinutes) {
        return left.metrics.slackMinutes < right.metrics.slackMinutes;
    }
    if (left.metrics.downstreamImpact != right.metrics.downstreamImpact) {
        return left.metrics.downstreamImpact > right.metrics.downstreamImpact;
    }
    if (strategy != SchedulingStrategy::SJF
        && left.metrics.remainingDurationMinutes
            != right.metrics.remainingDurationMinutes) {
        return left.metrics.remainingDurationMinutes
            < right.metrics.remainingDurationMinutes;
    }
    return left.task->id < right.task->id;
}

void printResult(SchedulingStrategy strategy, const SchedulingResult& result)
{
    std::cout << "\nStrategy: " << toString(strategy) << "\n";
    if (!result.valid) {
        std::cout << "Validation: FAILED - " << result.error << '\n';
        return;
    }
    std::cout << "Validation: PASS\nEligible scheduling order:\n";
    for (std::size_t index = 0; index < result.orderedTasks.size(); ++index) {
        const Task& task = result.orderedTasks[index];
        const TaskScheduleMetrics& metric = result.metrics.at(task.id);
        std::cout << "  " << index + 1 << ". " << task.id << " | " << task.name
                  << " | slack " << metric.slackMinutes << " min"
                  << (metric.slackMinutes < 0 ? " (INFEASIBLE)" : "")
                  << " | downstream impact " << metric.downstreamImpact << '\n';
    }
    std::cout << "Blocked tasks:\n";
    if (result.blockedTasks.empty()) std::cout << "  (none)\n";
    for (const BlockedTask& blocked : result.blockedTasks) {
        std::cout << "  " << blocked.task.id << " | " << blocked.task.name
                  << " | waiting for ";
        for (std::size_t index = 0;
             index < blocked.blockingPrerequisites.size(); ++index) {
            if (index != 0) std::cout << ", ";
            std::cout << blocked.blockingPrerequisites[index];
        }
        std::cout << '\n';
    }
    std::cout << "Completed tasks:\n";
    if (result.completedTasks.empty()) std::cout << "  (none)\n";
    for (const Task& task : result.completedTasks) {
        std::cout << "  " << task.id << " | " << task.name << '\n';
    }
    std::cout << "Excluded tasks:\n";
    if (result.excludedTasks.empty()) std::cout << "  (none)\n";
    for (const ExcludedTask& excluded : result.excludedTasks) {
        std::cout << "  " << excluded.task.id << " | " << excluded.task.name
                  << " | " << excluded.reason << '\n';
    }
    std::cout << "Completed/excluded tasks: " << result.completedTasks.size()
              << " completed, " << result.excludedTasks.size() << " excluded\n"
              << "Total eligible tasks: " << result.orderedTasks.size() << '\n';
}

bool readRank(const Task& task, int& rank)
{
    std::cout << "Rank for " << task.id << " (" << task.name
              << ", lower is sooner): ";
    std::string input;
    if (!std::getline(std::cin, input)) return false;
    try {
        std::size_t parsed = 0;
        rank = std::stoi(input, &parsed);
        return parsed == input.size() && rank > 0;
    } catch (const std::exception&) {
        return false;
    }
}

SchedulingTimePoint localPoint(int year, int month, int day, int hour, int minute)
{
    std::time_t value = 0;
    if (!makeLocalTime(year, month, day, hour, minute, value)) {
        throw std::runtime_error("Demo/test time is not valid in the local timezone.");
    }
    return std::chrono::system_clock::from_time_t(value);
}

} // namespace

bool parseSchedulingStrategy(const std::string& value,
    SchedulingStrategy& strategy)
{
    if (value == "1" || value == "priority") strategy = SchedulingStrategy::Priority;
    else if (value == "2" || value == "edf") strategy = SchedulingStrategy::EDF;
    else if (value == "3" || value == "sjf") strategy = SchedulingStrategy::SJF;
    else if (value == "4" || value == "lst") strategy = SchedulingStrategy::LST;
    else if (value == "5" || value == "custom") strategy = SchedulingStrategy::Custom;
    else return false;
    return true;
}

std::string toString(SchedulingStrategy strategy)
{
    switch (strategy) {
    case SchedulingStrategy::Priority: return "Priority Based";
    case SchedulingStrategy::EDF: return "Earliest Deadline First (EDF)";
    case SchedulingStrategy::SJF: return "Shortest Job First (SJF)";
    case SchedulingStrategy::LST: return "Least Slack Time First (LST)";
    case SchedulingStrategy::Custom: return "User Defined Ranking";
    }
    return "Invalid";
}

SchedulingResult generateSchedule(SchedulingStrategy strategy,
    const std::vector<Task>& tasks,
    const std::vector<AvailabilityWindow>& availability,
    const std::vector<Dependency>& dependencies,
    const DependencyManager& dependencyValidator,
    const SchedulingContext& context)
{
    SchedulingResult result;
    const int strategyValue = static_cast<int>(strategy);
    if (strategyValue < static_cast<int>(SchedulingStrategy::Priority)
        || strategyValue > static_cast<int>(SchedulingStrategy::Custom)) {
        result.error = "Invalid scheduling strategy.";
        return result;
    }
    std::string error;
    if (!dependencyValidator.validateGraph(tasks, dependencies, error)) {
        result.error = "Dependency graph validation failed: " + error;
        return result;
    }
    if (!validAvailability(availability, error)
        || !validCommitments(context.commitments, error)) {
        result.error = error;
        return result;
    }
    if (context.now == SchedulingTimePoint{}) {
        result.error = "Scheduling context must provide a valid current time.";
        return result;
    }

    const auto incomplete = incompleteByTask(tasks, dependencies);
    std::vector<Candidate> candidates;
    for (const Task& task : tasks) {
        switch (task.status) {
        case Status::Pending:
        case Status::Ready:
        case Status::InProgress:
        case Status::Completed:
        case Status::Overdue:
            break;
        default:
            result.error = "Task " + task.id + " has an invalid status.";
            return result;
        }
        if (task.status == Status::Completed) {
            result.completedTasks.push_back(task);
            continue;
        }
        const auto prerequisites = incomplete.find(task.id);
        if (prerequisites != incomplete.end() && !prerequisites->second.empty()) {
            result.blockedTasks.push_back({task, prerequisites->second});
            continue;
        }
        if (task.status != Status::Pending && task.status != Status::InProgress) {
            result.excludedTasks.push_back({task,
                "Only Pending and In Progress tasks are schedulable; status is "
                    + toString(task.status) + "."});
            continue;
        }
        if (task.durationMinutes < 1 || task.durationMinutes > 1000000) {
            result.error = "Task " + task.id + " has an invalid duration.";
            return result;
        }
        SchedulingTimePoint deadline;
        if (!parseDeadline(task.deadline, context.now, deadline)) {
            result.error = "Task " + task.id
                + " has an invalid deadline; use YYYY-MM-DD, YYYY-MM-DDTHH:MM, "
                  "or HH:MM.";
            return result;
        }
        int priority = 0;
        switch (task.priority) {
        case Priority::Low: priority = 0; break;
        case Priority::Medium: priority = 1; break;
        case Priority::High: priority = 2; break;
        case Priority::Critical: priority = 3; break;
        default:
            result.error = "Task " + task.id + " has an invalid priority.";
            return result;
        }
        int rank = 0;
        if (strategy == SchedulingStrategy::Custom) {
            const auto custom = context.customRanks.find(task.id);
            if (custom == context.customRanks.end()) {
                result.error = "No custom rank was provided for eligible task "
                    + task.id + ".";
                return result;
            }
            if (custom->second < 1) {
                result.error = "Custom ranks must be positive integers.";
                return result;
            }
            rank = custom->second;
        }
        candidates.push_back({&task, deadline, {}, priority, rank});
    }

    const std::map<std::string, std::size_t> impacts =
        downstreamImpacts(tasks, dependencies);
    for (Candidate& candidate : candidates) {
        std::int64_t workMinutes = 0;
        if (!availableWorkMinutes(context.now, candidate.deadline, availability,
                context.commitments, workMinutes, error)) {
            result.error = error;
            return result;
        }
        candidate.metrics.slackMinutes =
            workMinutes - candidate.task->durationMinutes;
        candidate.metrics.downstreamImpact = impacts.at(candidate.task->id);
        candidate.metrics.remainingDurationMinutes = candidate.task->durationMinutes;
        result.metrics.emplace(candidate.task->id, candidate.metrics);
    }
    std::sort(candidates.begin(), candidates.end(),
        [strategy](const Candidate& left, const Candidate& right) {
            return precedes(left, right, strategy);
        });
    for (const Candidate& candidate : candidates) {
        result.orderedTasks.push_back(*candidate.task);
    }
    std::sort(result.completedTasks.begin(), result.completedTasks.end(),
        [](const Task& left, const Task& right) { return left.id < right.id; });
    std::sort(result.blockedTasks.begin(), result.blockedTasks.end(),
        [](const BlockedTask& left, const BlockedTask& right) {
            return left.task.id < right.task.id;
        });
    std::sort(result.excludedTasks.begin(), result.excludedTasks.end(),
        [](const ExcludedTask& left, const ExcludedTask& right) {
            return left.task.id < right.task.id;
        });
    result.valid = true;
    return result;
}

int runSchedulingMenu(const std::vector<Task>& tasks,
    const AvailabilityManager& availability,
    const DependencyManager& dependencies)
{
    for (;;) {
        std::cout << "\n========== SCHEDULING STRATEGIES ==========\n"
                  << "1. Priority Based\n"
                  << "2. Earliest Deadline First (EDF)\n"
                  << "3. Shortest Job First (SJF)\n"
                  << "4. Least Slack Time First (LST)\n"
                  << "5. User Defined Ranking\n"
                  << "6. Back\nSelect Strategy: ";
        std::string choice;
        if (!std::getline(std::cin, choice)) return 0;
        if (choice == "6") return 0;
        SchedulingStrategy strategy;
        if (!parseSchedulingStrategy(choice, strategy)) {
            std::cerr << "Invalid strategy selection.\n";
            continue;
        }
        std::string validationError;
        if (!dependencies.validateGraph(tasks, validationError)) {
            std::cerr << "Dependency graph validation failed: "
                      << validationError << '\n';
            continue;
        }

        SchedulingContext context;
        context.now = std::chrono::system_clock::now();
        if (strategy == SchedulingStrategy::Custom) {
            bool inputValid = true;
            for (const Task& task : tasks) {
                if (task.status != Status::Pending
                    && task.status != Status::InProgress) continue;
                if (!dependencies.incompletePrerequisites(task.id, tasks).empty())
                    continue;
                int rank = 0;
                if (!readRank(task, rank)) {
                    if (std::cin.eof()) return 0;
                    std::cerr << "Rank must be a positive whole number.\n";
                    inputValid = false;
                    break;
                }
                context.customRanks[task.id] = rank;
            }
            if (!inputValid) continue;
        }

        const SchedulingResult result = generateSchedule(strategy, tasks,
            availability.all(), dependencies.all(), dependencies, context);
        printResult(strategy, result);
    }
}

int runSector04Demo()
{
    std::cout << "=== TaskFlow Sector 04: Scheduling Algorithms Demo ===\n";
    const std::vector<Task> tasks = {
        {"TSK-000001", "Task A", "", Priority::Critical, 120, "20:00",
            "Demo", Status::Pending},
        {"TSK-000002", "Task B", "", Priority::High, 60, "18:00",
            "Demo", Status::Pending},
        {"TSK-000003", "Task C", "", Priority::Critical, 30, "19:00",
            "Demo", Status::Pending},
        {"TSK-000004", "Task D", "", Priority::Medium, 45, "21:00",
            "Demo", Status::Pending},
        {"TSK-000005", "Blocked dependent of A", "", Priority::Low, 25,
            "21:30", "Demo", Status::Pending},
        {"TSK-000006", "Already completed", "", Priority::Low, 10,
            "2028-01-03", "Demo", Status::Completed}
    };
    const std::vector<AvailabilityWindow> windows = {
        {"DEMO-MON", DayOfWeek::Monday, 16 * 60, 22 * 60, "Demo availability"}
    };
    const std::vector<Dependency> dependencies = {
        {"TSK-000005", "TSK-000001"}
    };
    DependencyManager validator("unused-sector-04-demo.csv");
    SchedulingContext context;
    context.now = localPoint(2028, 1, 3, 16, 0);
    context.customRanks = {
        {"TSK-000001", 4}, {"TSK-000002", 2},
        {"TSK-000003", 3}, {"TSK-000004", 1}
    };

    std::cout << "Scheduling context: Monday, 2028-01-03 at 16:00; "
              << "availability 16:00-22:00; deadlines are local time.\n"
              << "Tie-break case: Task A and Task C share Critical priority; "
              << "Task C has the earlier deadline.\n";
    const SchedulingStrategy strategies[] = {
        SchedulingStrategy::Priority, SchedulingStrategy::EDF,
        SchedulingStrategy::SJF, SchedulingStrategy::LST,
        SchedulingStrategy::Custom
    };
    for (SchedulingStrategy strategy : strategies) {
        const SchedulingResult result = generateSchedule(strategy, tasks, windows,
            dependencies, validator, context);
        printResult(strategy, result);
        if (!result.valid) return 1;
        std::vector<std::string> order;
        for (const Task& task : result.orderedTasks) order.push_back(task.id);
        const SchedulingResult repeated = generateSchedule(strategy, tasks, windows,
            dependencies, validator, context);
        std::vector<std::string> repeatedOrder;
        for (const Task& task : repeated.orderedTasks)
            repeatedOrder.push_back(task.id);
        if (!repeated.valid || repeatedOrder != order) {
            std::cerr << "Determinism demonstration failed.\n";
            return 1;
        }
        std::cout << "Determinism check: PASS\n";
    }

    SchedulingContext missingRank = context;
    missingRank.customRanks.erase("TSK-000001");
    const SchedulingResult invalidInput = generateSchedule(
        SchedulingStrategy::Custom, tasks, windows, dependencies, validator,
        missingRank);
    std::cout << "\nInvalid-input check: "
              << (!invalidInput.valid ? "PASS - " + invalidInput.error
                                      : "FAIL - missing rank accepted") << '\n';
    if (invalidInput.valid) return 1;
    return 0;
}

} // namespace taskflow

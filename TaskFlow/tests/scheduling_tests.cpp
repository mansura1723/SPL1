#include "scheduling_management.hpp"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace taskflow {
namespace {

struct TestRun {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& description)
    {
        if (condition) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "FAIL sector-04: " << description << '\n';
        }
    }
};

SchedulingTimePoint point(int year, int month, int day, int hour, int minute)
{
    std::tm value = {};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_isdst = -1;
    const std::time_t converted = std::mktime(&value);
    if (converted == static_cast<std::time_t>(-1)) return {};
    return std::chrono::system_clock::from_time_t(converted);
}

Task task(const std::string& id, const std::string& deadline, int duration,
    Priority priority = Priority::High, Status status = Status::Pending)
{
    return {id, id, "", priority, duration, deadline, "Test", status};
}

std::vector<std::string> ids(const std::vector<Task>& tasks)
{
    std::vector<std::string> result;
    for (const Task& item : tasks) result.push_back(item.id);
    return result;
}

std::string describe(const SchedulingResult& result)
{
    if (!result.valid) return "invalid: " + result.error;
    std::string value = "order:";
    for (const std::string& id : ids(result.orderedTasks)) value += " " + id;
    value += "; blocked:";
    for (const BlockedTask& blocked : result.blockedTasks)
        value += " " + blocked.task.id;
    return value;
}

std::vector<AvailabilityWindow> mondayWindow()
{
    return {{"MON", DayOfWeek::Monday, 16 * 60, 22 * 60, "Monday work"}};
}

SchedulingContext baseContext()
{
    SchedulingContext context;
    context.now = point(2028, 1, 3, 16, 0);
    return context;
}

} // namespace

int runSchedulingTests(const std::filesystem::path&)
{
    TestRun tests;
    DependencyManager validator("unused-sector-04-tests.csv");
    const std::vector<Task> sample = {
        task("TSK-000001", "20:00", 120, Priority::Critical),
        task("TSK-000002", "18:00", 60, Priority::High),
        task("TSK-000003", "19:00", 30, Priority::Critical),
        task("TSK-000004", "21:00", 45, Priority::Medium)
    };
    const auto windows = mondayWindow();
    const auto context = baseContext();
    const std::vector<Dependency> noDependencies;
    const std::vector<std::pair<SchedulingStrategy, std::vector<std::string>>> expected = {
        {SchedulingStrategy::Priority,
            {"TSK-000003", "TSK-000001", "TSK-000002", "TSK-000004"}},
        {SchedulingStrategy::EDF,
            {"TSK-000002", "TSK-000003", "TSK-000001", "TSK-000004"}},
        {SchedulingStrategy::SJF,
            {"TSK-000003", "TSK-000004", "TSK-000002", "TSK-000001"}},
        {SchedulingStrategy::LST,
            {"TSK-000002", "TSK-000001", "TSK-000003", "TSK-000004"}}
    };
    for (const auto& test : expected) {
        const SchedulingResult result = generateSchedule(test.first, sample,
            windows, noDependencies, validator, context);
        tests.check(result.valid && ids(result.orderedTasks) == test.second,
            toString(test.first) + " returns the required order");
    }
    const SchedulingResult edfDateResult = generateSchedule(SchedulingStrategy::EDF,
        {task("TSK-000001", "2028-01-04T09:00", 30),
         task("TSK-000002", "2028-01-03T20:00", 30)},
        windows, noDependencies, validator, context);
    tests.check(edfDateResult.valid && ids(edfDateResult.orderedTasks)
            == std::vector<std::string>{"TSK-000002", "TSK-000001"},
        "EDF compares full local date-times across dates");

    SchedulingContext custom = context;
    custom.customRanks = {
        {"TSK-000001", 4}, {"TSK-000002", 2},
        {"TSK-000003", 3}, {"TSK-000004", 1}
    };
    const SchedulingResult customResult = generateSchedule(
        SchedulingStrategy::Custom, sample, windows, noDependencies,
        validator, custom);
    tests.check(customResult.valid
            && ids(customResult.orderedTasks)
                == std::vector<std::string>{"TSK-000004", "TSK-000002",
                    "TSK-000003", "TSK-000001"},
        "custom ranking returns the required order");

    const std::vector<Task> equalPriority = {
        task("TSK-000002", "20:00", 30),
        task("TSK-000001", "18:00", 30)
    };
    SchedulingResult result = generateSchedule(SchedulingStrategy::Priority,
        equalPriority, windows, noDependencies, validator, context);
    tests.check(result.valid && ids(result.orderedTasks)
            == std::vector<std::string>{"TSK-000001", "TSK-000002"},
        "equal priority uses earlier deadline");

    const std::vector<Task> slackTie = {
        task("TSK-000001", "20:00", 120),
        task("TSK-000002", "20:00", 60)
    };
    result = generateSchedule(SchedulingStrategy::Priority, slackTie, windows,
        noDependencies, validator, context);
    tests.check(result.valid && ids(result.orderedTasks)
            == std::vector<std::string>{"TSK-000001", "TSK-000002"},
        "equal priority and deadline use lower slack");

    const std::vector<Task> impactTie = {
        task("TSK-000001", "18:00", 60),
        task("TSK-000002", "18:00", 60),
        task("TSK-000003", "19:00", 30)
    };
    const std::vector<Dependency> impactDependencies = {
        {"TSK-000003", "TSK-000002"}
    };
    result = generateSchedule(SchedulingStrategy::Priority, impactTie, windows,
        impactDependencies, validator, context);
    tests.check(result.valid && ids(result.orderedTasks)
            == std::vector<std::string>{"TSK-000002", "TSK-000001"},
        "equal prior criteria use higher unfinished downstream impact ("
            + describe(result) + ")");
    tests.check(result.valid
            && result.metrics.at("TSK-000002").downstreamImpact == 1
            && result.metrics.at("TSK-000001").downstreamImpact == 0,
        "impact counts a unique unfinished descendant");

    const std::vector<Task> idTie = {
        task("TSK-000002", "18:00", 60),
        task("TSK-000001", "18:00", 60)
    };
    result = generateSchedule(SchedulingStrategy::Priority, idTie, windows,
        noDependencies, validator, context);
    tests.check(result.valid && ids(result.orderedTasks)
            == std::vector<std::string>{"TSK-000001", "TSK-000002"},
        "all equal criteria finish in Task ID order");

    const std::vector<Task> dependencyTasks = {
        task("TSK-000001", "20:00", 20),
        task("TSK-000002", "20:00", 20),
        task("TSK-000003", "20:00", 20),
        task("TSK-000004", "20:00", 20, Priority::High, Status::Completed),
        task("TSK-000005", "20:00", 20),
        task("TSK-000006", "20:00", 20)
    };
    const std::vector<Dependency> chain = {
        {"TSK-000002", "TSK-000001"},
        {"TSK-000003", "TSK-000002"},
        {"TSK-000005", "TSK-000001"},
        {"TSK-000005", "TSK-000004"},
        {"TSK-000006", "TSK-000004"}
    };
    result = generateSchedule(SchedulingStrategy::Priority, dependencyTasks,
        windows, chain, validator, context);
    tests.check(result.valid && ids(result.orderedTasks)
            == std::vector<std::string>{"TSK-000001", "TSK-000006"},
        "only ready tasks are eligible and a completed prerequisite is satisfied ("
            + describe(result) + ")");
    tests.check(result.valid && result.blockedTasks.size() == 3
            && result.blockedTasks[0].task.id == "TSK-000002"
            && result.blockedTasks[1].task.id == "TSK-000003"
            && result.blockedTasks[2].task.id == "TSK-000005",
        "dependency chain and multiple prerequisites produce blocking reasons ("
            + describe(result) + ")");
    tests.check(result.valid && result.completedTasks.size() == 1
            && result.completedTasks[0].id == "TSK-000004",
        "completed tasks are reported separately");
    std::vector<Task> afterPrerequisite = dependencyTasks;
    afterPrerequisite[0].status = Status::Completed;
    result = generateSchedule(SchedulingStrategy::Priority, afterPrerequisite,
        windows, chain, validator, context);
    const std::vector<std::string> afterPrerequisiteIds = ids(result.orderedTasks);
    tests.check(result.valid
            && std::find(afterPrerequisiteIds.begin(), afterPrerequisiteIds.end(),
                "TSK-000002") != afterPrerequisiteIds.end()
            && std::none_of(result.blockedTasks.begin(), result.blockedTasks.end(),
                [](const BlockedTask& blocked) {
                    return blocked.task.id == "TSK-000002";
                })
            && dependencyTasks[0].status == Status::Pending,
        "completing a prerequisite unlocks its dependent without mutating input");

    const std::vector<Dependency> cycle = {
        {"TSK-000001", "TSK-000002"},
        {"TSK-000002", "TSK-000001"}
    };
    result = generateSchedule(SchedulingStrategy::Priority,
        {task("TSK-000001", "18:00", 10), task("TSK-000002", "18:00", 10)},
        windows, cycle, validator, context);
    tests.check(!result.valid
            && result.error.find("Dependency graph validation failed")
                != std::string::npos,
        "dependency cycle rejects scheduling");

    const std::vector<AvailabilityWindow> multipleWindows = {
        {"MON-1", DayOfWeek::Monday, 16 * 60, 18 * 60, "First"},
        {"MON-2", DayOfWeek::Monday, 19 * 60, 22 * 60, "Second"},
        {"TUE", DayOfWeek::Tuesday, 9 * 60, 12 * 60, "Tuesday"}
    };
    const std::vector<Task> oneTask = {task("TSK-000001", "17:00", 30)};
    result = generateSchedule(SchedulingStrategy::LST, oneTask, windows,
        noDependencies, validator, context);
    tests.check(result.valid && result.metrics.at("TSK-000001").slackMinutes == 30,
        "same-day deadline uses the matching availability window");

    SchedulingContext committed = context;
    committed.commitments.push_back({point(2028, 1, 3, 17, 0),
        point(2028, 1, 3, 17, 30)});
    result = generateSchedule(SchedulingStrategy::LST,
        {task("TSK-000001", "18:00", 60)}, windows, noDependencies,
        validator, committed);
    tests.check(result.valid && result.metrics.at("TSK-000001").slackMinutes == 30,
        "commitments reduce available work by the committed interval");

    result = generateSchedule(SchedulingStrategy::LST,
        {task("TSK-000001", "2028-01-04T11:00", 420)},
        multipleWindows, noDependencies, validator, context);
    tests.check(result.valid && result.metrics.at("TSK-000001").slackMinutes == 0,
        "multiple weekly windows and a cross-date deadline calculate zero slack");

    result = generateSchedule(SchedulingStrategy::LST,
        {task("TSK-000001", "17:00", 60)}, windows, noDependencies,
        validator, context);
    tests.check(result.valid && result.metrics.at("TSK-000001").slackMinutes == 0,
        "zero slack is preserved");
    result = generateSchedule(SchedulingStrategy::LST,
        {task("TSK-000001", "17:00", 61)}, windows, noDependencies,
        validator, context);
    tests.check(result.valid && result.metrics.at("TSK-000001").slackMinutes == -1,
        "negative slack is not treated as feasible");

    result = generateSchedule(SchedulingStrategy::Priority, {}, windows,
        noDependencies, validator, context);
    tests.check(result.valid && result.orderedTasks.empty(),
        "empty task input produces an empty valid schedule");
    result = generateSchedule(SchedulingStrategy::Priority,
        {task("TSK-000001", "18:00", 10)}, windows, noDependencies,
        validator, context);
    tests.check(result.valid && result.orderedTasks.size() == 1,
        "single-task schedule succeeds");
    result = generateSchedule(SchedulingStrategy::Priority,
        {task("TSK-000001", "18:00", 10), task("TSK-000001", "18:00", 20)},
        windows, noDependencies, validator, context);
    tests.check(!result.valid, "duplicate task IDs are rejected");
    result = generateSchedule(SchedulingStrategy::Priority,
        {task("TSK-000001", "not-a-deadline", 10)}, windows,
        noDependencies, validator, context);
    tests.check(!result.valid && result.error.find("invalid deadline")
            != std::string::npos,
        "invalid deadline is rejected");
    result = generateSchedule(SchedulingStrategy::Priority,
        {task("TSK-000001", "18:00", 0)}, windows, noDependencies,
        validator, context);
    tests.check(!result.valid, "invalid duration is rejected");
    result = generateSchedule(SchedulingStrategy::Priority, oneTask, {},
        noDependencies, validator, context);
    tests.check(!result.valid && result.error.find("No availability")
            != std::string::npos,
        "missing availability fails explicitly");
    result = generateSchedule(SchedulingStrategy::Priority, oneTask,
        {{"BAD", DayOfWeek::Monday, 18 * 60, 16 * 60, "Invalid"}},
        noDependencies, validator, context);
    tests.check(!result.valid, "invalid availability is rejected");
    SchedulingContext invalidCommitment = context;
    invalidCommitment.commitments.push_back({context.now, context.now});
    result = generateSchedule(SchedulingStrategy::Priority, oneTask, windows,
        noDependencies, validator, invalidCommitment);
    tests.check(!result.valid, "empty committed interval is rejected");

    result = generateSchedule(SchedulingStrategy::Custom, oneTask, windows,
        noDependencies, validator, context);
    tests.check(!result.valid && result.error.find("No custom rank")
            != std::string::npos,
        "missing custom rank is rejected");
    SchedulingContext invalidRank = context;
    invalidRank.customRanks = {{"TSK-000001", 0}};
    result = generateSchedule(SchedulingStrategy::Custom, oneTask, windows,
        noDependencies, validator, invalidRank);
    tests.check(!result.valid, "non-positive custom rank is rejected");
    SchedulingContext equalRanks = context;
    equalRanks.customRanks = {{"TSK-000001", 1}};
    result = generateSchedule(SchedulingStrategy::Custom, idTie, windows,
        noDependencies, validator, equalRanks);
    tests.check(!result.valid, "custom ranking does not invent missing ranks");
    equalRanks.customRanks = {{"TSK-000001", 2}, {"TSK-000002", 2}};
    result = generateSchedule(SchedulingStrategy::Custom, idTie, windows,
        noDependencies, validator, equalRanks);
    tests.check(result.valid && ids(result.orderedTasks)
            == std::vector<std::string>{"TSK-000001", "TSK-000002"},
        "equal custom ranks use deterministic common tie-breakers");
    result = generateSchedule(static_cast<SchedulingStrategy>(99), oneTask,
        windows, noDependencies, validator, context);
    tests.check(!result.valid, "invalid strategy enum is rejected");
    SchedulingStrategy parsedStrategy = SchedulingStrategy::Priority;
    tests.check(!parseSchedulingStrategy("99", parsedStrategy),
        "invalid menu strategy selection is rejected");

    const std::vector<Task> statusTasks = {
        task("TSK-000001", "18:00", 10, Priority::High, Status::InProgress),
        task("TSK-000002", "18:00", 10, Priority::High, Status::Ready),
        task("TSK-000003", "18:00", 10, Priority::High, Status::Overdue),
        task("TSK-000004", "18:00", 10, Priority::High, Status::Completed)
    };
    result = generateSchedule(SchedulingStrategy::Priority, statusTasks,
        windows, noDependencies, validator, context);
    tests.check(result.valid && ids(result.orderedTasks)
            == std::vector<std::string>{"TSK-000001"}
            && result.excludedTasks.size() == 2
            && result.completedTasks.size() == 1,
        "In Progress is eligible while Ready/Overdue are excluded and Completed is separated");

    std::vector<Task> shuffled = sample;
    std::mt19937 generator(1723);
    std::shuffle(shuffled.begin(), shuffled.end(), generator);
    for (const auto& test : expected) {
        const auto first = generateSchedule(test.first, sample, windows,
            noDependencies, validator, context);
        const auto second = generateSchedule(test.first, shuffled, windows,
            noDependencies, validator, context);
        tests.check(first.valid && second.valid
                && ids(first.orderedTasks) == ids(second.orderedTasks),
            toString(test.first) + " is deterministic across input order");
    }

    std::vector<Task> large;
    for (int index = 1; index <= 300; ++index) {
        std::string id = "TSK-";
        const std::string digits = std::to_string(index);
        id.append(6 - digits.size(), '0');
        id += digits;
        large.push_back(task(id, "18:00", 1));
    }
    result = generateSchedule(SchedulingStrategy::EDF, large, windows,
        noDependencies, validator, context);
    const std::vector<std::string> largeOrder = ids(result.orderedTasks);
    tests.check(result.valid && result.orderedTasks.size() == large.size()
            && std::is_sorted(largeOrder.begin(), largeOrder.end()),
        "large task lists sort deterministically");

    std::cout << "Sector 04 tests: " << tests.passed << " passed, "
              << tests.failed << " failed.\n";
    return tests.failed == 0 ? 0 : 1;
}

} // namespace taskflow

#include "task_management.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace taskflow {
namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

Task sampleTask(const std::string& name, Priority priority = Priority::Medium,
    int duration = 30, const std::string& deadline = "2027-05-20",
    const std::string& type = "Development", Status status = Status::Pending)
{
    return Task{"", name, "Task description", priority, duration,
        deadline, type, status};
}

std::filesystem::path taskFile(const std::filesystem::path& directory,
    const std::string& name)
{
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    if (error) throw std::runtime_error("Cannot prepare test directory: "
        + error.message());
    std::filesystem::create_directories(directory);
    return directory / name;
}

void validationTests(const std::filesystem::path& directory)
{
    Task valid = sampleTask("Check dates");
    std::string error;
    require(validateTask(valid, error), "A valid task was rejected.");
    require(isValidDate("2024-02-29"), "Leap day was rejected.");
    require(!isValidDate("2025-02-29"), "Invalid leap day was accepted.");
    require(!isValidDate("2027-13-01"), "Out-of-range month was accepted.");
    require(isValidDeadline("2028-02-29T15:30"),
        "A valid local date-time deadline was rejected.");
    require(isValidDeadline("2028-02-29"),
        "A valid date-only deadline was rejected.");
    require(!isValidDeadline("2028-02-29T25:30"),
        "An invalid date-time deadline was accepted.");
    valid.deadline = "2028-02-29T15:30";
    require(validateTask(valid, error),
        "A task with a local date-time deadline was rejected.");
    valid.deadline = "2027-02-30";
    require(!validateTask(valid, error), "Invalid calendar date was accepted.");
    valid = sampleTask("Bad duration", Priority::Low, 0);
    require(!validateTask(valid, error), "Zero duration was accepted.");
    valid = sampleTask("");
    require(!validateTask(valid, error), "Empty task name was accepted.");
    valid = sampleTask("Minimum duration", Priority::Low, 1);
    require(validateTask(valid, error), "Minimum duration was rejected.");
    valid = sampleTask(std::string(120, 'N'), Priority::Critical, 1000000);
    require(validateTask(valid, error),
        "Maximum name length or duration was rejected.");
    valid = sampleTask(std::string(121, 'N'));
    require(!validateTask(valid, error), "Overlong task name was accepted.");
    valid = sampleTask("Over maximum duration", Priority::Low, 1000001);
    require(!validateTask(valid, error), "Overlong duration was accepted.");

    Priority priority;
    Status status;
    require(!parsePriority("Urgent", priority), "Unknown priority was accepted.");
    require(!parseStatus("Started", status), "Unknown status was accepted.");

    TaskManager manager(taskFile(directory, "validation.csv"));
    Task created;
    valid = sampleTask("Valid task");
    valid.deadline = "2027-02-30";
    require(!manager.create(valid, created, error),
        "Invalid task was accepted by create.");
    require(manager.all().empty(), "Invalid create changed task data.");
}

void crudTests(const std::filesystem::path& directory)
{
    TaskManager manager(taskFile(directory, "crud.csv"));
    std::string error;
    require(manager.load(error), error);
    require(manager.all().empty(), "A missing file should load as an empty list.");

    Task created;
    require(manager.create(sampleTask("Initial"), created, error), error);
    require(created.id == "TSK-000001", "First generated task ID is incorrect.");
    Task second;
    require(manager.create(sampleTask("Second"), second, error), error);
    require(created.id != second.id, "Generated task IDs are not unique.");

    Task replacement = sampleTask("Edited", Priority::Critical, 90,
        "2027-06-01", "Testing", Status::InProgress);
    require(manager.update(created.id, replacement, error), error);
    require(manager.all().front().id == created.id
        && manager.all().front().name == "Edited",
        "Edit did not preserve the ID and update the task.");

    replacement.deadline = "not-a-date";
    require(!manager.update(created.id, replacement, error),
        "Invalid edit was accepted.");
    require(manager.all().front().name == "Edited",
        "Invalid edit changed the original task.");
    require(!manager.erase("TSK-999999", error),
        "Deleting a missing task unexpectedly succeeded.");
    require(manager.all().size() == 2,
        "Failed deletion changed the task list.");
    require(manager.erase(created.id, error), error);
    require(manager.all().size() == 1 && manager.all().front().id == second.id,
        "Successful deletion removed the wrong task.");
    require(manager.erase(second.id, error), error);
    require(manager.all().empty(), "Deleting all tasks failed.");
    require(!manager.erase(second.id, error),
        "Deletion from an empty list unexpectedly succeeded.");
}

void queryTests(const std::filesystem::path& directory)
{
    TaskManager manager(taskFile(directory, "queries.csv"));
    std::string error;
    Task created;
    require(manager.create(sampleTask("API migration", Priority::High, 60,
        "2027-08-03", "Development", Status::Ready), created, error), error);
    require(manager.create(sampleTask("Write API guide", Priority::Low, 20,
        "2027-08-01", "Documentation", Status::Pending), created, error), error);
    require(manager.create(sampleTask("Build test harness", Priority::Critical, 40,
        "2027-08-02", "Testing", Status::InProgress), created, error), error);

    require(manager.search("aPi").size() == 2,
        "Case-insensitive search returned the wrong number of tasks.");
    require(manager.search("no match").empty(),
        "Search returned unexpected matches.");
    TaskFilter filter;
    filter.priority = Priority::High;
    filter.type = "development";
    require(manager.filter(filter).size() == 1,
        "Combined priority/type filter failed.");
    filter = TaskFilter{};
    filter.status = Status::Overdue;
    require(manager.filter(filter).empty(),
        "Status filter returned unexpected matches.");

    const std::vector<Task> byDuration = manager.sorted(SortField::Duration);
    require(byDuration.front().durationMinutes == 20,
        "Duration sort is not ascending.");
    const std::vector<Task> byPriority = manager.sorted(SortField::Priority);
    require(byPriority.front().priority == Priority::Critical,
        "Priority sort does not put highest priority first.");
    const std::vector<Task> byDeadline = manager.sorted(SortField::Deadline);
    require(byDeadline.front().deadline == "2027-08-01",
        "Deadline sort is not ascending.");
    Task timed = sampleTask("Timed deadline", Priority::High, 10,
        "2027-08-01T10:00", "Development", Status::Pending);
    require(manager.create(timed, created, error), error);
    const std::vector<Task> mixedDeadlines = manager.sorted(SortField::Deadline);
    require(mixedDeadlines.front().deadline == "2027-08-01T10:00",
        "Date-time sorting did not precede the same date's end-of-day deadline.");
}

void persistenceTests(const std::filesystem::path& directory)
{
    const std::filesystem::path path = taskFile(directory, "persistence.csv");
    TaskManager manager(path);
    std::string error;
    Task task = sampleTask("Comma, and \"quotes\"",
        Priority::Medium, 1, "2028-02-29", "Research, Planning",
        Status::Completed);
    task.description = "Keep commas, quotes \"and\" punctuation.";
    Task created;
    require(manager.create(task, created, error), error);

    TaskManager reloaded(path);
    require(reloaded.load(error), error);
    require(reloaded.all().size() == 1, "Saved task did not reload.");
    const Task& restored = reloaded.all().front();
    require(restored.name == task.name && restored.description == task.description
        && restored.type == task.type && restored.deadline == task.deadline,
        "CSV escaping did not preserve task text.");
    require(restored.id == created.id && restored.status == Status::Completed,
        "Task ID or status did not persist.");

    Task next;
    require(reloaded.create(sampleTask("After reload"), next, error), error);
    require(next.id == "TSK-000002",
        "ID generation did not continue after loading saved tasks.");
}

void malformedFileTests(const std::filesystem::path& directory)
{
    const std::filesystem::path path = taskFile(directory, "malformed.csv");
    TaskManager manager(path);
    std::string error;
    Task created;
    require(manager.create(sampleTask("Kept in memory"), created, error), error);
    {
        std::ofstream output(path, std::ios::trunc);
        output << "id,name,description,priority,duration_minutes,deadline,type,status\n"
               << "TSK-000001,Incomplete record\n";
    }
    require(!manager.load(error), "Malformed task records were accepted.");
    require(manager.all().size() == 1
        && manager.all().front().name == "Kept in memory",
        "Malformed load replaced the existing in-memory task list.");
    {
        std::ofstream output(path, std::ios::trunc);
        output << "id,name,description,priority,duration_minutes,deadline,type,status\n"
               << "TSK-000001,First,Description,Low,10,2027-05-20,Test,Pending\n"
               << "TSK-000001,Duplicate,Description,Low,10,2027-05-20,Test,Pending\n";
    }
    require(!manager.load(error), "Duplicate task IDs in storage were accepted.");
    require(manager.all().size() == 1
        && manager.all().front().name == "Kept in memory",
        "Failed load replaced the existing in-memory task list.");
}

void failedSaveTests(const std::filesystem::path& directory)
{
    std::error_code fileError;
    std::filesystem::remove_all(directory, fileError);
    if (fileError) throw std::runtime_error("Cannot prepare failed-save test: "
        + fileError.message());
    std::filesystem::create_directories(directory);
    const std::filesystem::path blockedParent = directory / "not-a-directory";
    {
        std::ofstream sentinel(blockedParent);
        sentinel << "preserve this file";
    }

    TaskManager manager(blockedParent / "tasks.csv");
    std::string error;
    Task created;
    require(!manager.create(sampleTask("Must not be lost"), created, error),
        "Create unexpectedly succeeded when persistence was unavailable.");
    require(!error.empty(), "Failed save did not return an error message.");
    require(manager.all().empty(),
        "A task was kept in memory after its save failed.");
    std::ifstream sentinel(blockedParent);
    std::string contents;
    std::getline(sentinel, contents);
    require(contents == "preserve this file",
        "Failed save overwrote the pre-existing file.");
}

void endToEndTests(const std::filesystem::path& directory)
{
    const std::filesystem::path path = taskFile(directory, "integration.csv");
    TaskManager manager(path);
    std::string error;
    require(manager.load(error), error);

    Task created;
    require(manager.create(sampleTask("Integration task", Priority::High),
        created, error), error);
    Task replacement = sampleTask("Integrated and edited", Priority::Critical,
        75, "2027-09-10", "Testing", Status::Ready);
    require(manager.update(created.id, replacement, error), error);
    require(manager.search("edited").size() == 1,
        "Integration search did not find the edited task.");
    TaskFilter filter;
    filter.status = Status::Ready;
    require(manager.filter(filter).size() == 1,
        "Integration filter did not find the edited task.");
    require(manager.sorted(SortField::Name).front().name
        == "Integrated and edited", "Integration sort failed.");

    TaskManager reloaded(path);
    require(reloaded.load(error), error);
    require(reloaded.all().size() == 1
        && reloaded.all().front().name == "Integrated and edited",
        "Integration persistence check failed.");
    require(reloaded.erase(created.id, error), error);
    require(reloaded.all().empty(), "Integration delete failed.");
}

} // namespace

int runTaskManagementTests(const std::filesystem::path& testDirectory)
{
    using Test = std::pair<const char*, void (*)(const std::filesystem::path&)>;
    const Test tests[] = {
        {"unit: input validation and edge cases", validationTests},
        {"unit: create, edit, delete, and empty lists", crudTests},
        {"unit: search, filter, and sort", queryTests},
        {"integration: CSV persistence and ID continuity", persistenceTests},
        {"integration: malformed data is rejected safely", malformedFileTests},
        {"integration: failed save preserves existing data", failedSaveTests},
        {"integration: full task-management workflow", endToEndTests}
    };

    try {
        std::filesystem::create_directories(testDirectory);
        for (std::size_t index = 0; index < sizeof(tests) / sizeof(tests[0]);
             ++index) {
            const Test& test = tests[index];
            test.second(testDirectory / ("case-" + std::to_string(index + 1)));
            std::cout << "PASS " << test.first << '\n';
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL " << exception.what() << '\n';
        return 1;
    }
    std::cout << "All " << (sizeof(tests) / sizeof(tests[0]))
              << " Task Management tests passed.\n";
    return 0;
}

} // namespace taskflow

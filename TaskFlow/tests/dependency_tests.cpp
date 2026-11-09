#include "dependency_management.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace taskflow {
namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path prepareDirectory(
    const std::filesystem::path& directory)
{
    std::error_code fileError;
    std::filesystem::remove_all(directory, fileError);
    if (fileError) {
        throw std::runtime_error("Cannot prepare dependency test directory: "
            + fileError.message());
    }
    std::filesystem::create_directories(directory);
    return directory;
}

std::vector<Task> sampleTasks()
{
    return {
        {"TSK-000001", "Complete Requirements", "", Priority::High, 60,
            "2028-01-01", "Planning", Status::Completed},
        {"TSK-000002", "Implement Code", "", Priority::High, 120,
            "2028-01-02", "Development", Status::Pending},
        {"TSK-000003", "Test Application", "", Priority::Medium, 45,
            "2028-01-03", "Testing", Status::Pending},
        {"TSK-000004", "Review Code", "", Priority::Medium, 30,
            "2028-01-04", "Review", Status::Pending},
        {"TSK-000005", "Completed Independent Task", "", Priority::Low, 10,
            "2028-01-05", "Admin", Status::Completed},
        {"TSK-000006", "In Progress Independent Task", "", Priority::Low, 10,
            "2028-01-06", "Admin", Status::InProgress},
        {"TSK-000007", "Overdue Independent Task", "", Priority::Low, 10,
            "2028-01-07", "Admin", Status::Overdue}
    };
}

void addAndCycleTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    const std::vector<Task> tasks = sampleTasks();
    DependencyManager manager(directory / "dependencies.csv");
    std::string error;
    require(manager.load(error), error);

    require(manager.addDependency("TSK-000002", "TSK-000001", tasks, error),
        "Valid dependency was rejected: " + error);
    require(manager.all().size() == 1, "Valid dependency was not added.");
    require(manager.prerequisitesForTask("TSK-000002").size() == 1
            && manager.prerequisitesForTask("TSK-000002")[0].prerequisiteId
                == "TSK-000001",
        "Prerequisite query returned the wrong dependency.");
    require(manager.dependentsForTask("TSK-000001").size() == 1
            && manager.dependentsForTask("TSK-000001")[0].dependentId
                == "TSK-000002",
        "Dependent query returned the wrong dependency.");
    require(manager.search("TSK-000001").size() == 1,
        "Dependency search did not match a task ID.");
    require(!manager.addDependency("TSK-000002", "TSK-000001", tasks, error),
        "Duplicate dependency was accepted.");
    require(error.find("Duplicate") != std::string::npos,
        "Duplicate rejection did not explain the error.");
    require(!manager.addDependency("TSK-000001", "TSK-000001", tasks, error),
        "Self-dependency was accepted.");
    require(!manager.addDependency("TSK-invalid", "TSK-000001", tasks, error),
        "Invalid task ID format was accepted.");
    require(!manager.addDependency("TSK-000002", "TSK-999999", tasks, error),
        "Missing task reference was accepted.");

    require(manager.addDependency("TSK-000003", "TSK-000002", tasks, error),
        "Second edge in indirect-cycle test was rejected: " + error);
    require(!manager.addDependency("TSK-000001", "TSK-000002", tasks, error),
        "Direct cycle was accepted.");
    require(error.find("Circular dependency detected:") != std::string::npos,
        "Direct cycle rejection did not identify a cycle.");
    require(!manager.addDependency("TSK-000001", "TSK-000003", tasks, error),
        "Indirect cycle was accepted.");
    require(error.find("Circular dependency detected:") != std::string::npos
            && error.find("TSK-000001") != std::string::npos
            && error.find("TSK-000003") != std::string::npos,
        "Indirect cycle rejection did not identify involved tasks.");

    std::vector<std::string> cycle;
    cycle.push_back("stale cycle entry");
    require(!manager.detectCycle(cycle, error),
        "Valid acyclic dependency graph was reported cyclic.");
    require(cycle.empty(),
        "A successful cycle check retained a stale cycle path.");
    require(manager.taskHasReferences("TSK-000001")
            && manager.taskHasReferences("TSK-000002")
            && !manager.taskHasReferences("TSK-000007"),
        "Dependency-reference protection does not reflect both edge endpoints.");

    const std::vector<Dependency> duplicates = {
        {"TSK-000002", "TSK-000001"},
        {"TSK-000002", "TSK-000001"}
    };
    require(!manager.validateGraph(tasks, duplicates, error),
        "Graph validation accepted duplicate dependency records.");
    require(!manager.validateGraph({}, {{"TSK-000002", "TSK-000001"}}, error),
        "Graph validation accepted missing task references with an empty task list.");
    require(manager.validateGraph(tasks, error),
        "Valid dependency graph failed validation: " + error);
    const std::vector<Dependency> cycleSet = {
        {"TSK-000002", "TSK-000001"},
        {"TSK-000003", "TSK-000002"},
        {"TSK-000001", "TSK-000003"}
    };
    require(!manager.validateGraph(tasks, cycleSet, error)
            && error.find("Circular dependency detected:") != std::string::npos,
        "Cycle detection did not reject a cyclic graph supplied for validation.");
}

void graphReadinessAndImpactTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    const std::vector<Task> tasks = sampleTasks();
    DependencyManager manager(directory / "dependencies.csv");
    std::string error;
    require(manager.load(error), error);
    require(manager.addDependencies({
            {"TSK-000002", "TSK-000001"},
            {"TSK-000003", "TSK-000002"},
            {"TSK-000003", "TSK-000004"}
        }, tasks, error), "Valid multi-prerequisite batch failed: " + error);

    const std::vector<std::string> blocked = manager.blockedTasks(tasks);
    require(blocked == std::vector<std::string>{"TSK-000003"},
        "Blocked-task detection included the wrong tasks.");
    require(manager.incompletePrerequisites("TSK-000003", tasks)
            == std::vector<std::string>({"TSK-000002", "TSK-000004"}),
        "Blocked task did not report every incomplete prerequisite.");
    const std::vector<std::string> ready = manager.readyTasks(tasks);
    require(std::find(ready.begin(), ready.end(), "TSK-000002") != ready.end(),
        "Task with completed prerequisites was not identified as ready.");
    require(std::find(ready.begin(), ready.end(), "TSK-000001") == ready.end()
            && std::find(ready.begin(), ready.end(), "TSK-000005") == ready.end(),
        "Completed tasks were incorrectly identified as ready.");
    require(std::find(ready.begin(), ready.end(), "TSK-000006") != ready.end()
            && std::find(ready.begin(), ready.end(), "TSK-000007") != ready.end(),
        "Uncompleted task statuses were not preserved in readiness calculation.");

    require(manager.topologicalOrder(tasks)
            == std::vector<std::string>({
                "TSK-000001", "TSK-000002", "TSK-000004", "TSK-000003",
                "TSK-000005", "TSK-000006", "TSK-000007"}),
        "Kahn ordering was not valid and deterministic for independent tasks.");

    DependencyImpact impact;
    require(manager.analyzeImpact("TSK-000001", tasks, impact, error), error);
    require(impact.directDependentCount == 1
            && impact.downstreamTaskIds
                == std::vector<std::string>({"TSK-000002", "TSK-000003"})
            && impact.importanceScore == 2,
        "Downstream impact counts or documented score are incorrect.");
    require(!manager.analyzeImpact("TSK-999999", tasks, impact, error),
        "Impact analysis accepted an unknown task.");

    const std::vector<Task> independentTasks = {
        tasks[3], tasks[1], tasks[2]
    };
    DependencyManager emptyManager(directory / "empty.csv");
    require(emptyManager.load(error), error);
    require(emptyManager.validateGraph(independentTasks, error),
        "Empty dependency graph should validate.");
    require(emptyManager.topologicalOrder(independentTasks)
            == std::vector<std::string>({
                "TSK-000002", "TSK-000003", "TSK-000004"}),
        "Independent tasks did not get a deterministic topological ordering.");
}

void persistenceAndRemovalTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    const std::filesystem::path path = directory / "dependencies.csv";
    const std::vector<Task> tasks = sampleTasks();
    std::string error;
    DependencyManager manager(path);
    require(manager.load(error), error);
    require(manager.addDependency("TSK-000002", "TSK-000001", tasks, error),
        error);

    DependencyManager reloaded(path);
    require(reloaded.load(error), error);
    require(reloaded.all().size() == 1
            && reloaded.hasDependency("TSK-000002", "TSK-000001"),
        "Dependency persistence or reload failed.");
    require(!reloaded.removeDependency("TSK-000002", "TSK-000003", error),
        "Removal of a nonexistent dependency succeeded.");
    require(reloaded.removeDependency("TSK-000002", "TSK-000001", error),
        "Removal of an existing dependency failed: " + error);
    DependencyManager afterRemoval(path);
    require(afterRemoval.load(error), error);
    require(afterRemoval.all().empty(),
        "Removed dependency remained in persisted storage.");

    const std::filesystem::path malformed = directory / "malformed.csv";
    {
        std::ofstream output(malformed, std::ios::trunc);
        output << "dependent_id,prerequisite_id\n"
               << "TSK-000002,TSK-000001,extra\n";
    }
    DependencyManager malformedManager(malformed);
    require(!malformedManager.load(error),
        "Malformed dependency record was accepted.");

    {
        std::ofstream output(malformed, std::ios::trunc);
        output << "dependent_id,prerequisite_id\n"
               << "TSK-000001,TSK-000002\n"
               << "TSK-000002,TSK-000001\n";
    }
    require(!malformedManager.load(error),
        "Circular dependency records were accepted during load.");
    require(error.find("Circular dependency detected:") != std::string::npos,
        "Cycle in persisted data was not explained.");

    const std::filesystem::path blockedParent = directory / "not-a-directory";
    {
        std::ofstream sentinel(blockedParent);
        sentinel << "preserve this file";
    }
    DependencyManager failedSave(blockedParent / "dependencies.csv");
    require(failedSave.load(error), error);
    require(!failedSave.addDependency("TSK-000002", "TSK-000001", tasks, error),
        "Dependency add succeeded when persistence was unavailable.");
    require(failedSave.all().empty(),
        "Failed dependency save changed the in-memory graph.");
    std::ifstream sentinel(blockedParent);
    std::string contents;
    std::getline(sentinel, contents);
    require(contents == "preserve this file",
        "Failed dependency save overwrote pre-existing data.");

    const std::filesystem::path targetDirectory =
        directory / "dependency-directory.csv";
    std::filesystem::create_directories(targetDirectory);
    {
        std::ofstream sentinel(targetDirectory / "preserve.txt");
        sentinel << "preserve directory contents";
    }
    DependencyManager directoryTarget(targetDirectory);
    require(!directoryTarget.addDependency("TSK-000002", "TSK-000001",
                tasks, error),
        "Saving to a directory unexpectedly succeeded.");
    require(std::filesystem::is_directory(targetDirectory),
        "Saving to a directory moved or replaced the original directory.");
    std::ifstream directorySentinel(targetDirectory / "preserve.txt");
    std::getline(directorySentinel, contents);
    require(contents == "preserve directory contents",
        "Saving to a directory modified its existing contents.");

    DependencyManager missing(directory / "missing.csv");
    require(missing.load(error) && missing.all().empty(),
        "A missing dependency file did not initialize as an empty graph.");
}

void invalidBatchIsAtomicTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    const std::filesystem::path path = directory / "dependencies.csv";
    const std::vector<Task> tasks = sampleTasks();
    DependencyManager manager(path);
    std::string error;
    require(manager.load(error), error);
    require(manager.addDependency("TSK-000002", "TSK-000001", tasks, error),
        "Could not create existing dependency for batch test: " + error);

    const std::vector<Dependency> before = manager.all();
    const std::vector<Dependency> invalidBatch = {
        {"TSK-000003", "TSK-000002"},
        {"TSK-000004", "TSK-999999"}
    };
    require(!manager.addDependencies(invalidBatch, tasks, error),
        "Batch containing an unknown task was accepted.");
    require(manager.all().size() == before.size()
            && manager.all()[0].dependentId == before[0].dependentId
            && manager.all()[0].prerequisiteId == before[0].prerequisiteId,
        "Invalid batch partially changed the in-memory dependencies.");

    DependencyManager reloaded(path);
    require(reloaded.load(error), error);
    require(reloaded.all().size() == before.size()
            && reloaded.hasDependency("TSK-000002", "TSK-000001"),
        "Invalid batch partially changed the persisted dependencies.");
}

void readinessUpdatesAfterCompletionTests(
    const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    TaskManager tasks(directory / "tasks.csv");
    std::string error;
    require(tasks.load(error), error);

    Task prerequisite{"", "Prerequisite", "", Priority::Medium, 30,
        "2028-01-01", "Test", Status::Pending};
    Task dependent{"", "Dependent", "", Priority::Medium, 30,
        "2028-01-02", "Test", Status::Pending};
    Task createdPrerequisite;
    Task createdDependent;
    require(tasks.create(prerequisite, createdPrerequisite, error), error);
    require(tasks.create(dependent, createdDependent, error), error);

    DependencyManager dependencies(directory / "dependencies.csv");
    require(dependencies.load(error), error);
    require(dependencies.addDependency(createdDependent.id,
            createdPrerequisite.id, tasks.all(), error), error);
    require(dependencies.blockedTasks(tasks.all())
                == std::vector<std::string>{createdDependent.id},
        "Dependent task was not blocked before its prerequisite completed.");
    require(!dependencies.isTaskReady(createdDependent.id, tasks.all(), error),
        "A task was ready before its prerequisite was completed.");

    prerequisite.status = Status::Completed;
    require(tasks.update(createdPrerequisite.id, prerequisite, error), error);
    require(dependencies.isTaskReady(createdDependent.id, tasks.all(), error),
        "Readiness did not update after its prerequisite was completed: " + error);
    require(dependencies.blockedTasks(tasks.all()).empty(),
        "Blocked-task results did not update after prerequisite completion.");
}

void branchingImpactDeduplicatesTests(
    const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    const std::vector<Task> tasks = sampleTasks();
    DependencyManager manager(directory / "dependencies.csv");
    std::string error;
    require(manager.load(error), error);
    require(manager.addDependencies({
            {"TSK-000002", "TSK-000001"},
            {"TSK-000003", "TSK-000001"},
            {"TSK-000004", "TSK-000002"},
            {"TSK-000004", "TSK-000003"}
        }, tasks, error), "Branching dependency batch failed: " + error);

    DependencyImpact impact;
    require(manager.analyzeImpact("TSK-000001", tasks, impact, error), error);
    require(impact.directDependentCount == 2
            && impact.downstreamTaskIds
                == std::vector<std::string>({
                    "TSK-000002", "TSK-000003", "TSK-000004"})
            && impact.importanceScore == 3,
        "Branching impact counted a shared downstream task more than once.");
}

void emptyAndDeleteProtectionTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    DependencyManager manager(directory / "dependencies.csv");
    std::string error;
    require(manager.load(error), error);
    require(manager.validateGraph({}, error),
        "An empty graph should be valid.");
    require(manager.topologicalOrder({}).empty(),
        "Empty graph unexpectedly produced an execution order.");

    const std::vector<Task> tasks = sampleTasks();
    require(manager.addDependency("TSK-000002", "TSK-000001", tasks, error),
        error);
    require(manager.taskHasReferences("TSK-000001")
            && manager.taskHasReferences("TSK-000002"),
        "Task deletion protection did not catch dependency endpoints.");
    require(manager.removeDependency("TSK-000002", "TSK-000001", error), error);
    require(!manager.taskHasReferences("TSK-000001")
            && !manager.taskHasReferences("TSK-000002"),
        "Removing the dependency did not release task deletion protection.");
}

void interactiveDeletionProtectionTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    const std::filesystem::path taskPath = directory / "tasks.csv";
    const std::filesystem::path availabilityPath = directory / "availability.csv";
    const std::filesystem::path dependencyPath = directory / "dependencies.csv";
    TaskManager tasks(taskPath);
    std::string error;
    require(tasks.load(error), error);

    const Task first{"", "Prerequisite", "", Priority::Medium, 30,
        "2028-01-01", "Test", Status::Pending};
    const Task second{"", "Dependent", "", Priority::Medium, 30,
        "2028-01-02", "Test", Status::Pending};
    const Task independent{"", "Independent", "", Priority::Medium, 30,
        "2028-01-03", "Test", Status::Pending};
    Task createdFirst;
    Task createdSecond;
    Task createdIndependent;
    require(tasks.create(first, createdFirst, error), error);
    require(tasks.create(second, createdSecond, error), error);
    require(tasks.create(independent, createdIndependent, error), error);

    DependencyManager dependencies(dependencyPath);
    require(dependencies.load(error), error);
    require(dependencies.addDependency(createdSecond.id, createdFirst.id,
            tasks.all(), error), error);

    std::istringstream scriptedInput(
        "10\n"
        "6\nTSK-000002\n"
        "7\nTSK-000001\n"
        "14\nTSK-000001\n"
        "15\n"
        "11\n"
        "17\n"
        "3\nTSK-000001\n3\nTSK-000003\nyes\n0\n");
    std::ostringstream capturedOutput;
    std::ostringstream capturedError;
    std::streambuf* oldInput = std::cin.rdbuf(scriptedInput.rdbuf());
    std::streambuf* oldOutput = std::cout.rdbuf(capturedOutput.rdbuf());
    std::streambuf* oldError = std::cerr.rdbuf(capturedError.rdbuf());
    int appResult = 1;
    try {
        appResult = runInteractiveTaskManager(taskPath, availabilityPath,
            dependencyPath);
    } catch (...) {
        std::cin.rdbuf(oldInput);
        std::cout.rdbuf(oldOutput);
        std::cerr.rdbuf(oldError);
        throw;
    }
    std::cin.rdbuf(oldInput);
    std::cout.rdbuf(oldOutput);
    std::cerr.rdbuf(oldError);

    require(appResult == 0, "Interactive task manager did not exit successfully.");
    require(capturedError.str().find("Invalid menu choice") == std::string::npos,
        "Interactive deletion test supplied an invalid menu choice.");
    require(capturedOutput.str().find("Cannot delete task TSK-000001")
            != std::string::npos
            && capturedOutput.str().find("Deleted TSK-000003")
                != std::string::npos,
        "Task deletion protection did not block referenced tasks while allowing an unreferenced deletion.");
    require(capturedOutput.str().find("Prerequisites for TSK-000002")
                != std::string::npos
            && capturedOutput.str().find("Tasks depending on TSK-000001")
                != std::string::npos
            && capturedOutput.str().find("Dependencies referencing TSK-000001")
                != std::string::npos,
        "Interactive prerequisite, dependent, or search view did not run.");
    require(capturedOutput.str().find("TSK-000003: Independent")
                != std::string::npos
            && capturedOutput.str().find("Topological order:")
                != std::string::npos,
        "Interactive graph omitted an isolated task or topological order.");

    TaskManager reloaded(taskPath);
    require(reloaded.load(error), error);
    require(reloaded.all().size() == 2,
        "The interactive deletion guard deleted a referenced task or retained the independent task.");
    DependencyManager reloadedDependencies(dependencyPath);
    require(reloadedDependencies.load(error), error);
    require(reloadedDependencies.hasDependency("TSK-000002", "TSK-000001"),
        "Task deletion modified its dependency record.");
}

} // namespace

int runDependencyTests(const std::filesystem::path& testDirectory)
{
    using Test = std::pair<const char*, void (*)(const std::filesystem::path&)>;
    const Test tests[] = {
        {"add, duplicate, self, ID, and cycle validation", addAndCycleTests},
        {"topological order, readiness, blockers, and impact", graphReadinessAndImpactTests},
        {"remove, persistence, malformed records, and failed saves", persistenceAndRemovalTests},
        {"invalid batch atomicity", invalidBatchIsAtomicTests},
        {"readiness after prerequisite completion", readinessUpdatesAfterCompletionTests},
        {"unique downstream impact in a branching graph", branchingImpactDeduplicatesTests},
        {"empty graph and task deletion protection", emptyAndDeleteProtectionTests},
        {"interactive deletion protection and Sector 01 deletion regression", interactiveDeletionProtectionTests}
    };

    try {
        std::filesystem::create_directories(testDirectory);
        for (std::size_t index = 0; index < sizeof(tests) / sizeof(tests[0]);
             ++index) {
            tests[index].second(
                testDirectory / ("dependency-case-" + std::to_string(index + 1)));
            std::cout << "PASS Sector 03 " << tests[index].first << '\n';
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL Sector 03: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "All " << (sizeof(tests) / sizeof(tests[0]))
              << " Sector 03 Dependency tests passed.\n";
    return 0;
}

} // namespace taskflow

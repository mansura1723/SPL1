#include "dependency_management.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace taskflow {
namespace {

const char* const fileHeader = "dependent_id,prerequisite_id";

std::string trim(const std::string& value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool hasLineBreak(const std::string& value)
{
    return value.find('\n') != std::string::npos
        || value.find('\r') != std::string::npos;
}

bool isTaskIdFormat(const std::string& id)
{
    if (id.size() < 5 || id.compare(0, 4, "TSK-") != 0) return false;
    const std::string digits = id.substr(4);
    if (digits.empty()) return false;
    return std::all_of(digits.begin(), digits.end(),
        [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool parseRecord(const std::string& line, std::vector<std::string>& fields)
{
    fields.clear();
    std::string field;
    bool quoted = false;
    bool closedQuote = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (quoted) {
            if (ch == '"') {
                if (index + 1 < line.size() && line[index + 1] == '"') {
                    field += '"';
                    ++index;
                } else {
                    quoted = false;
                    closedQuote = true;
                }
            } else {
                field += ch;
            }
        } else if (ch == ',') {
            fields.push_back(field);
            field.clear();
            closedQuote = false;
        } else if (ch == '"' && field.empty() && !closedQuote) {
            quoted = true;
        } else if (closedQuote) {
            return false;
        } else {
            field += ch;
        }
    }

    if (quoted) return false;
    fields.push_back(field);
    return true;
}

void writeField(std::ostream& output, const std::string& field)
{
    if (field.find_first_of(",\"\r\n") == std::string::npos) {
        output << field;
        return;
    }
    output << '"';
    for (char ch : field) {
        if (ch == '"') output << '"';
        output << ch;
    }
    output << '"';
}

std::string formatCycle(const std::vector<std::string>& cycle)
{
    if (cycle.empty()) return "cycle";
    std::ostringstream output;
    for (std::size_t index = 0; index < cycle.size(); ++index) {
        if (index != 0) output << " -> ";
        output << cycle[index];
    }
    return output.str();
}

bool taskExists(const std::string& id, const std::vector<Task>& tasks)
{
    return std::any_of(tasks.begin(), tasks.end(),
        [&id](const Task& task) { return task.id == id; });
}

bool dependencyExists(const std::vector<Dependency>& dependencies,
    const std::string& dependentId, const std::string& prerequisiteId)
{
    return std::any_of(dependencies.begin(), dependencies.end(),
        [&dependentId, &prerequisiteId](const Dependency& dependency) {
            return dependency.dependentId == dependentId
                && dependency.prerequisiteId == prerequisiteId;
        });
}

bool parseQuickDependency(const std::string& line, Dependency& dependency,
    std::string& error)
{
    const std::string cleaned = trim(line);
    if (cleaned.empty()) {
        error = "Input is empty.";
        return false;
    }
    const std::size_t separator = cleaned.find('|');
    if (separator == std::string::npos || cleaned.find('|', separator + 1) != std::string::npos) {
        error = "Expected DEPENDENT_TASK_ID|PREREQUISITE_TASK_ID.";
        return false;
    }

    dependency.dependentId = trim(cleaned.substr(0, separator));
    dependency.prerequisiteId = trim(cleaned.substr(separator + 1));
    if (dependency.dependentId.empty() || dependency.prerequisiteId.empty()) {
        error = "Both dependent and prerequisite task IDs are required.";
        return false;
    }
    error.clear();
    return true;
}

bool containsTaskId(const std::vector<std::string>& ids, const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool detectCycleForSet(const std::vector<Dependency>& dependencySet,
    const std::vector<Task>& tasks,
    std::vector<std::string>& cycle, std::string& error)
{
    std::set<std::string> nodes;
    for (const Task& task : tasks) nodes.insert(task.id);
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    std::unordered_set<std::string> seen;

    for (const Dependency& dependency : dependencySet) {
        if (!nodes.count(dependency.dependentId) || !nodes.count(dependency.prerequisiteId)) {
            continue;
        }
        if (dependency.dependentId == dependency.prerequisiteId) {
            cycle = {dependency.dependentId, dependency.prerequisiteId};
            error = "A task cannot depend on itself: " + dependency.dependentId;
            return true;
        }
        if (seen.insert(dependency.dependentId + "|" + dependency.prerequisiteId).second) {
            adjacency[dependency.prerequisiteId].push_back(dependency.dependentId);
            std::sort(adjacency[dependency.prerequisiteId].begin(),
                adjacency[dependency.prerequisiteId].end());
        }
        nodes.insert(dependency.dependentId);
        nodes.insert(dependency.prerequisiteId);
    }

    std::map<std::string, int> state;
    std::vector<std::string> stack;
    std::function<bool(const std::string&)> dfs = [&](const std::string& node) -> bool {
        state[node] = 1;
        stack.push_back(node);
        for (const std::string& neighbor : adjacency[node]) {
            if (!state.count(neighbor)) {
                if (dfs(neighbor)) return true;
            } else if (state[neighbor] == 1) {
                auto found = std::find(stack.begin(), stack.end(), neighbor);
                if (found != stack.end()) {
                    cycle.assign(found, stack.end());
                    cycle.push_back(neighbor);
                } else {
                    cycle = {neighbor, node};
                }
                return true;
            }
        }
        stack.pop_back();
        state[node] = 2;
        return false;
    };

    for (const std::string& node : nodes) {
        if (!state.count(node) && dfs(node)) {
            error = "Circular dependency detected: " + formatCycle(cycle);
            return true;
        }
    }

    error.clear();
    return false;
}

std::vector<Dependency> resolveGraph(const std::vector<Task>& tasks,
    const std::vector<Dependency>& dependencySet)
{
    std::vector<Dependency> result;
    for (const Dependency& dependency : dependencySet) {
        if (taskExists(dependency.dependentId, tasks)
            && taskExists(dependency.prerequisiteId, tasks)) {
            result.push_back(dependency);
        }
    }
    return result;
}

} // namespace

DependencyManager::DependencyManager(std::filesystem::path storagePath)
    : storagePath_(std::move(storagePath))
{
}

bool DependencyManager::load(std::string& error)
{
    std::error_code fileError;
    const bool exists = std::filesystem::exists(storagePath_, fileError);
    if (fileError) {
        error = "Cannot inspect dependency file: " + fileError.message();
        return false;
    }
    if (!exists) {
        dependencies_.clear();
        error.clear();
        return true;
    }

    std::ifstream input(storagePath_);
    if (!input) {
        error = "Cannot open dependency file for reading: " + storagePath_.string();
        return false;
    }

    std::string line;
    if (!std::getline(input, line)) {
        dependencies_.clear();
        error.clear();
        return true;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != fileHeader) {
        error = "Dependency file has an invalid or unsupported header.";
        return false;
    }

    std::vector<Dependency> loaded;
    std::unordered_set<std::string> seen;
    std::size_t lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;

        std::vector<std::string> fields;
        if (!parseRecord(line, fields) || fields.size() != 2) {
            error = "Dependency file line " + std::to_string(lineNumber)
                + " is malformed; expected 2 CSV fields.";
            return false;
        }

        Dependency dependency{trim(fields[0]), trim(fields[1])};
        if (dependency.dependentId.empty() || dependency.prerequisiteId.empty()) {
            error = "Dependency file line " + std::to_string(lineNumber)
                + " contains an empty task ID.";
            return false;
        }
        if (!isTaskIdFormat(dependency.dependentId)
            || !isTaskIdFormat(dependency.prerequisiteId)) {
            error = "Dependency file line " + std::to_string(lineNumber)
                + " contains an invalid task ID.";
            return false;
        }
        const std::string key = dependency.dependentId + "|" + dependency.prerequisiteId;
        if (seen.count(key) != 0U) {
            error = "Duplicate dependency in file: " + dependency.dependentId
                + " depends on " + dependency.prerequisiteId;
            return false;
        }
        if (dependency.dependentId == dependency.prerequisiteId) {
            error = "A task cannot depend on itself in dependency file: "
                + dependency.dependentId;
            return false;
        }
        seen.insert(key);
        loaded.push_back(dependency);
    }

    dependencies_ = std::move(loaded);
    error.clear();
    return true;
}

bool DependencyManager::save(std::string& error) const
{
    return saveDependencies(dependencies_, error);
}

bool DependencyManager::addDependency(const std::string& dependentId,
    const std::string& prerequisiteId, std::string& error)
{
    return addDependency(dependentId, prerequisiteId, std::vector<Task>{}, error);
}

bool DependencyManager::addDependency(const std::string& dependentId,
    const std::string& prerequisiteId, const std::vector<Task>& tasks,
    std::string& error)
{
    if (trim(dependentId).empty() || trim(prerequisiteId).empty()) {
        error = "Both dependent and prerequisite task IDs are required.";
        return false;
    }
    const std::string dependent = trim(dependentId);
    const std::string prerequisite = trim(prerequisiteId);
    if (dependent == prerequisite) {
        error = "A task cannot depend on itself: " + dependent;
        return false;
    }
    if (!isTaskIdFormat(dependent) || !isTaskIdFormat(prerequisite)) {
        error = "Task IDs must use the TSK-##### format.";
        return false;
    }
    if (!tasks.empty() && (!taskExists(dependent, tasks) || !taskExists(prerequisite, tasks))) {
        error = "Dependency references a task ID that does not exist.";
        return false;
    }
    if (containsDependency(dependent, prerequisite)) {
        error = "Duplicate dependency record: " + dependent + " depends on "
            + prerequisite;
        return false;
    }

    std::vector<Dependency> candidate = dependencies_;
    candidate.push_back({dependent, prerequisite});
    std::string validationError;
    if (!validateGraph(tasks, candidate, validationError)) {
        error = validationError.empty() ? "Dependency validation failed." : validationError;
        return false;
    }

    if (!saveDependencies(candidate, error)) return false;
    dependencies_ = std::move(candidate);
    error.clear();
    return true;
}

bool DependencyManager::addDependencies(const std::vector<Dependency>& batch,
    const std::vector<Task>& tasks, std::string& error)
{
    if (batch.empty()) {
        error = "No dependency records were provided.";
        return false;
    }

    std::vector<Dependency> combined = dependencies_;
    combined.insert(combined.end(), batch.begin(), batch.end());
    std::string validationError;
    if (!validateGraph(tasks, combined, validationError)) {
        error = validationError.empty() ? "The proposed promotion batch is invalid." : validationError;
        return false;
    }

    if (!saveDependencies(combined, error)) return false;
    dependencies_ = std::move(combined);
    error.clear();
    return true;
}

bool DependencyManager::removeDependency(const std::string& dependentId,
    const std::string& prerequisiteId, std::string& error)
{
    const std::string dependent = trim(dependentId);
    const std::string prerequisite = trim(prerequisiteId);
    if (dependent.empty() || prerequisite.empty()) {
        error = "Both task IDs are required to remove a dependency.";
        return false;
    }
    const auto found = std::find_if(dependencies_.begin(), dependencies_.end(),
        [&dependent, &prerequisite](const Dependency& dependency) {
            return dependency.dependentId == dependent
                && dependency.prerequisiteId == prerequisite;
        });
    if (found == dependencies_.end()) {
        error = "Dependency not found: " + dependent + " depends on " + prerequisite;
        return false;
    }

    std::vector<Dependency> candidate = dependencies_;
    candidate.erase(found);
    if (!saveDependencies(candidate, error)) return false;
    dependencies_ = std::move(candidate);
    error.clear();
    return true;
}

const std::vector<Dependency>& DependencyManager::all() const
{
    return dependencies_;
}

std::vector<Dependency> DependencyManager::prerequisitesForTask(
    const std::string& taskId) const
{
    std::vector<Dependency> matches;
    for (const Dependency& dependency : dependencies_) {
        if (dependency.dependentId == taskId) matches.push_back(dependency);
    }
    std::sort(matches.begin(), matches.end(),
        [](const Dependency& left, const Dependency& right) {
            if (left.prerequisiteId == right.prerequisiteId) return left.dependentId < right.dependentId;
            return left.prerequisiteId < right.prerequisiteId;
        });
    return matches;
}

std::vector<Dependency> DependencyManager::dependentsForTask(
    const std::string& taskId) const
{
    std::vector<Dependency> matches;
    for (const Dependency& dependency : dependencies_) {
        if (dependency.prerequisiteId == taskId) matches.push_back(dependency);
    }
    std::sort(matches.begin(), matches.end(),
        [](const Dependency& left, const Dependency& right) {
            if (left.dependentId == right.dependentId) return left.prerequisiteId < right.prerequisiteId;
            return left.dependentId < right.dependentId;
        });
    return matches;
}

std::vector<Dependency> DependencyManager::search(const std::string& taskId) const
{
    const std::string normalized = trim(taskId);
    if (normalized.empty()) return {};

    std::vector<Dependency> matches;
    for (const Dependency& dependency : dependencies_) {
        if (dependency.dependentId == normalized || dependency.prerequisiteId == normalized) {
            matches.push_back(dependency);
        }
    }
    std::sort(matches.begin(), matches.end(),
        [](const Dependency& left, const Dependency& right) {
            if (left.dependentId == right.dependentId) return left.prerequisiteId < right.prerequisiteId;
            return left.dependentId < right.dependentId;
        });
    return matches;
}

bool DependencyManager::validateGraph(const std::vector<Task>& tasks,
    std::string& error) const
{
    return validateGraph(tasks, dependencies_, error);
}

bool DependencyManager::validateGraph(const std::vector<Task>& tasks,
    const std::vector<Dependency>& dependencySet, std::string& error) const
{
    std::set<std::string> knownTasks;
    for (const Task& task : tasks) knownTasks.insert(task.id);
    std::set<std::pair<std::string, std::string>> seen;

    for (const Dependency& dependency : dependencySet) {
        if (dependency.dependentId.empty() || dependency.prerequisiteId.empty()) {
            error = "Dependency record contains an empty task ID.";
            return false;
        }
        if (dependency.dependentId == dependency.prerequisiteId) {
            error = "A task cannot depend on itself: " + dependency.dependentId;
            return false;
        }
        if (!isTaskIdFormat(dependency.dependentId)
            || !isTaskIdFormat(dependency.prerequisiteId)) {
            error = "Dependency references an invalid task ID format.";
            return false;
        }
        if (!knownTasks.empty() && (!knownTasks.count(dependency.dependentId)
                || !knownTasks.count(dependency.prerequisiteId))) {
            error = "Dependency references a task ID that does not exist: "
                + dependency.dependentId + " -> " + dependency.prerequisiteId;
            return false;
        }
        const std::pair<std::string, std::string> key(dependency.dependentId,
            dependency.prerequisiteId);
        if (!seen.insert(key).second) {
            error = "Duplicate dependency record: " + dependency.dependentId
                + " depends on " + dependency.prerequisiteId;
            return false;
        }
    }

    std::vector<std::string> cycle;
    if (detectCycleForSet(dependencySet, tasks, cycle, error)) {
        return false;
    }
    error.clear();
    return true;
}

bool DependencyManager::validateBatch(const std::vector<Dependency>& batch,
    const std::vector<Task>& tasks, std::string& error) const
{
    if (batch.empty()) {
        error = "No dependency records were provided.";
        return false;
    }
    std::vector<Dependency> combined = dependencies_;
    combined.insert(combined.end(), batch.begin(), batch.end());
    return validateGraph(tasks, combined, error);
}

bool DependencyManager::detectCycle(std::vector<std::string>& cycle,
    std::string& error) const
{
    std::vector<std::string> nodes;
    for (const Dependency& dependency : dependencies_) {
        if (!containsTaskId(nodes, dependency.dependentId)) nodes.push_back(dependency.dependentId);
        if (!containsTaskId(nodes, dependency.prerequisiteId)) nodes.push_back(dependency.prerequisiteId);
    }
    std::vector<Dependency> unique;
    for (const Dependency& dependency : dependencies_) {
        if (!dependencyExists(unique, dependency.dependentId, dependency.prerequisiteId)) {
            unique.push_back(dependency);
        }
    }
    std::vector<Task> tasks; 
    for (const std::string& id : nodes) {
        tasks.push_back(Task{id, "", "", Priority::Medium, 1, "2028-01-01", "General", Status::Pending});
    }
    if (detectCycleForSet(unique, tasks, cycle, error)) return true;
    error.clear();
    return false;
}

bool DependencyManager::detectCircularDependencies(std::vector<std::string>& cycle,
    std::string& error) const
{
    return detectCycle(cycle, error);
}

std::vector<std::string> DependencyManager::topologicalOrder(
    const std::vector<Task>& tasks) const
{
    return topologicalOrder(tasks, dependencies_);
}

std::vector<std::string> DependencyManager::topologicalOrder(
    const std::vector<Task>& tasks,
    const std::vector<Dependency>& dependencySet) const
{
    std::set<std::string> nodes;
    for (const Task& task : tasks) nodes.insert(task.id);
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    std::map<std::string, int> indegree;
    for (const std::string& node : nodes) indegree[node] = 0;

    std::unordered_set<std::string> seen;
    for (const Dependency& dependency : dependencySet) {
        if (!nodes.count(dependency.dependentId) || !nodes.count(dependency.prerequisiteId)) {
            continue;
        }
        const std::string edgeKey = dependency.prerequisiteId + "|" + dependency.dependentId;
        if (!seen.insert(edgeKey).second) continue;
        adjacency[dependency.prerequisiteId].push_back(dependency.dependentId);
        std::sort(adjacency[dependency.prerequisiteId].begin(), adjacency[dependency.prerequisiteId].end());
        ++indegree[dependency.dependentId];
    }

    std::vector<std::string> ready;
    for (const auto& [node, count] : indegree) {
        if (count == 0) ready.push_back(node);
    }
    std::sort(ready.begin(), ready.end());

    std::vector<std::string> order;
    while (!ready.empty()) {
        const std::string current = ready.front();
        ready.erase(ready.begin());
        order.push_back(current);
        for (const std::string& neighbor : adjacency[current]) {
            --indegree[neighbor];
            if (indegree[neighbor] == 0) {
                ready.push_back(neighbor);
                std::sort(ready.begin(), ready.end());
            }
        }
    }

    if (order.size() != nodes.size()) {
        return {};
    }
    return order;
}

std::vector<std::string> DependencyManager::blockedTasks(
    const std::vector<Task>& tasks) const
{
    std::vector<std::string> blocked;
    for (const Task& task : tasks) {
        const std::vector<Dependency> prerequisites = prerequisitesForTask(task.id);
        if (prerequisites.empty()) continue;
        bool blockedTask = false;
        for (const Dependency& prerequisite : prerequisites) {
            const auto found = std::find_if(tasks.begin(), tasks.end(),
                [&prerequisite](const Task& candidate) {
                    return candidate.id == prerequisite.prerequisiteId;
                });
            if (found == tasks.end()) {
                blockedTask = true;
                break;
            }
            if (found->status != Status::Completed) {
                blockedTask = true;
                break;
            }
        }
        if (blockedTask) blocked.push_back(task.id);
    }
    std::sort(blocked.begin(), blocked.end());
    return blocked;
}

std::vector<std::string> DependencyManager::readyTasks(
    const std::vector<Task>& tasks) const
{
    std::vector<std::string> ready;
    for (const Task& task : tasks) {
        std::string error;
        if (isTaskReady(task.id, tasks, error)) ready.push_back(task.id);
    }
    std::sort(ready.begin(), ready.end());
    return ready;
}

bool DependencyManager::isTaskReady(const std::string& taskId,
    const std::vector<Task>& tasks, std::string& error) const
{
    const std::vector<Dependency> prerequisites = prerequisitesForTask(taskId);
    if (prerequisites.empty()) {
        error.clear();
        return true;
    }
    for (const Dependency& dependency : prerequisites) {
        const auto found = std::find_if(tasks.begin(), tasks.end(),
            [&dependency](const Task& task) { return task.id == dependency.prerequisiteId; });
        if (found == tasks.end()) {
            error = "Missing prerequisite task ID: " + dependency.prerequisiteId;
            return false;
        }
        if (found->status != Status::Completed) {
            error = "Prerequisite " + dependency.prerequisiteId
                + " is not marked as Completed.";
            return false;
        }
    }
    error.clear();
    return true;
}

bool DependencyManager::taskHasDependents(const std::string& taskId) const
{
    return !dependentsForTask(taskId).empty();
}

bool DependencyManager::hasDependency(const std::string& dependentId,
    const std::string& prerequisiteId) const
{
    return containsDependency(dependentId, prerequisiteId);
}

bool DependencyManager::saveDependencies(const std::vector<Dependency>& dependencies,
    std::string& error) const
{
    if (!storagePath_.parent_path().empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(storagePath_.parent_path(), directoryError);
        if (directoryError) {
            error = "Cannot create dependency data directory: " + directoryError.message();
            return false;
        }
    }

    const std::filesystem::path tempPath = storagePath_;
    const std::filesystem::path backupPath = storagePath_;
    std::filesystem::path temporaryPath = storagePath_;
    temporaryPath += ".tmp";
    std::filesystem::path backup = storagePath_;
    backup += ".bak";

    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) {
        error = "Cannot open temporary dependency file for writing: "
            + temporaryPath.string();
        return false;
    }
    output << fileHeader << '\n';
    for (const Dependency& dependency : dependencies) {
        writeField(output, dependency.dependentId); output << ',';
        writeField(output, dependency.prerequisiteId); output << '\n';
    }
    output.flush();
    if (!output) {
        output.close();
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        error = "Failed while writing temporary dependency file.";
        return false;
    }

    output.close();
    if (!output) {
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        error = "Failed to close temporary dependency file.";
        return false;
    }

    std::error_code fileError;
    const bool targetExists = std::filesystem::exists(storagePath_, fileError);
    if (fileError) {
        std::filesystem::remove(temporaryPath, fileError);
        error = "Cannot inspect existing dependency file: " + fileError.message();
        return false;
    }

    bool backupCreated = false;
    if (targetExists) {
        std::error_code backupError;
        if (std::filesystem::exists(backup, backupError) || backupError) {
            std::filesystem::remove(temporaryPath, backupError);
            error = "Cannot safely save dependencies because a backup already exists: "
                + backup.string();
            return false;
        }
        std::filesystem::rename(storagePath_, backup, fileError);
        if (fileError) {
            std::filesystem::remove(temporaryPath, fileError);
            error = "Cannot preserve the previous dependency file: " + fileError.message();
            return false;
        }
        backupCreated = true;
    }

    std::filesystem::rename(temporaryPath, storagePath_, fileError);
    if (fileError) {
        if (backupCreated) {
            std::error_code restoreError;
            std::filesystem::rename(backup, storagePath_, restoreError);
            if (restoreError) {
                error = "Saving failed (" + fileError.message()
                    + "); restoring the previous dependency file also failed ("
                    + restoreError.message() + ").";
                return false;
            }
        }
        std::filesystem::remove(temporaryPath, fileError);
        error = "Cannot install the new dependency file: " + fileError.message();
        return false;
    }

    if (backupCreated) {
        std::error_code removeBackupError;
        std::filesystem::remove(backup, removeBackupError);
        if (removeBackupError) {
            error = "Dependencies were saved, but the previous-file backup could not be removed.";
            return true;
        }
    }

    error.clear();
    return true;
}

bool DependencyManager::containsDependency(const std::string& dependentId,
    const std::string& prerequisiteId) const
{
    return std::any_of(dependencies_.begin(), dependencies_.end(),
        [&dependentId, &prerequisiteId](const Dependency& dependency) {
            return dependency.dependentId == dependentId
                && dependency.prerequisiteId == prerequisiteId;
        });
}

bool DependencyManager::isTaskKnown(const std::string& taskId,
    const std::vector<Task>& tasks) const
{
    return taskExists(taskId, tasks);
}

int runSector03Demo(const std::filesystem::path& storagePath)
{
    TaskManager taskManager("data/tasks.csv");
    std::string error;
    if (!taskManager.load(error)) {
        std::cerr << "Could not load tasks for dependency demo: " << error << '\n';
        return 1;
    }

    DependencyManager dependencyManager(storagePath);
    if (!dependencyManager.load(error)) {
        std::cerr << "Could not load dependencies for demo: " << error << '\n';
        return 1;
    }

    std::cout << "=== Sector 03: Dependency Management Demo ===\n";
    if (taskManager.all().empty()) {
        std::cout << "No task data found. Create tasks in Sector 01 before demoing dependencies.\n";
        return 0;
    }

    const std::vector<Task> tasks = taskManager.all();
    const std::vector<std::string> ready = dependencyManager.readyTasks(tasks);
    std::cout << "Ready tasks: " << (ready.empty() ? "none" : ready[0]) << '\n';
    std::cout << "Dependency records on disk: " << dependencyManager.all().size() << '\n';
    return 0;
}

int runDependencyMenu(TaskManager& manager, DependencyManager& dependencyManager)
{
    for (;;) {
        std::cout << "\nDEPENDENCY MANAGEMENT\n"
                  << "1. Add Dependency (Guided Input)\n"
                  << "2. Add Dependency (Quick Input)\n"
                  << "3. Add Multiple Dependencies (Bulk Input)\n"
                  << "4. Remove Dependency\n"
                  << "5. View All Dependencies\n"
                  << "6. View Task Prerequisites\n"
                  << "7. View Task Dependents\n"
                  << "8. Check Task Readiness\n"
                  << "9. View Blocked Tasks\n"
                  << "10. View Ready Tasks\n"
                  << "11. Generate Topological Order\n"
                  << "12. Validate Dependency Graph\n"
                  << "13. Save Dependencies\n"
                  << "14. Back to Main Menu\n"
                  << "Choice: ";

        std::string choice;
        if (!std::getline(std::cin, choice)) return 0;

        if (choice == "14") return 0;

        if (choice == "1") {
            std::string dependentId;
            std::cout << "Task that depends on (Dependent Task ID): ";
            if (!std::getline(std::cin, dependentId)) return 0;
            std::string prerequisiteId;
            std::cout << "Prerequisite Task ID: ";
            if (!std::getline(std::cin, prerequisiteId)) return 0;

            std::string error;
            if (!dependencyManager.addDependency(dependentId, prerequisiteId,
                    manager.all(), error)) {
                std::cerr << error << '\n';
                continue;
            }
            std::cout << "Added dependency: " << dependentId << " depends on "
                      << prerequisiteId << ".\n";
        } else if (choice == "2") {
            std::string line;
            std::cout << "Dependent Task ID|Prerequisite Task ID: ";
            if (!std::getline(std::cin, line)) return 0;
            Dependency dependency;
            std::string error;
            if (!parseQuickDependency(line, dependency, error)) {
                std::cerr << error << '\n';
                continue;
            }
            if (!dependencyManager.addDependency(dependency.dependentId,
                    dependency.prerequisiteId, manager.all(), error)) {
                std::cerr << error << '\n';
                continue;
            }
            std::cout << "Added quick dependency: " << dependency.dependentId
                      << " depends on " << dependency.prerequisiteId << ".\n";
        } else if (choice == "3") {
            std::vector<Dependency> proposed;
            std::vector<std::string> lines;
            std::cout << "Paste dependency records one per line; type END when done.\n";
            for (;;) {
                std::string line;
                if (!std::getline(std::cin, line)) return 0;
                if (trim(line) == "END") break;
                lines.push_back(line);
            }

            std::vector<std::string> errors;
            std::vector<Dependency> validEntries;
            for (std::size_t index = 0; index < lines.size(); ++index) {
                Dependency dependency;
                std::string error;
                if (!parseQuickDependency(lines[index], dependency, error)) {
                    errors.push_back("line " + std::to_string(index + 1) + ": " + error);
                    continue;
                }
                validEntries.push_back(dependency);
            }

            std::vector<Dependency> combined = dependencyManager.all();
            combined.insert(combined.end(), validEntries.begin(), validEntries.end());
            std::string validationError;
            if (!dependencyManager.validateGraph(manager.all(), combined, validationError)) {
                std::cerr << "Bulk dependency validation failed: " << validationError << '\n';
                std::cout << "Preview of invalid rows:\n";
                for (const std::string& lineError : errors) std::cout << "  " << lineError << '\n';
                if (validationError.find("Duplicate") != std::string::npos
                    || validationError.find("A task cannot depend") != std::string::npos
                    || validationError.find("Circular") != std::string::npos) {
                    std::cout << "  Graph error: " << validationError << '\n';
                }
                continue;
            }

            std::cout << "Bulk dependency preview:\n";
            for (const Dependency& dependency : validEntries) {
                std::cout << "  " << dependency.dependentId << " depends on "
                          << dependency.prerequisiteId << '\n';
            }
            std::cout << "Confirm saving the entire batch? (Y/N): ";
            std::string confirmation;
            if (!std::getline(std::cin, confirmation)) return 0;
            const std::string response = lower(trim(confirmation));
            if (response != "y" && response != "yes") {
                std::cout << "Bulk dependency save cancelled.\n";
                continue;
            }

            std::string error;
            if (!dependencyManager.addDependencies(validEntries, manager.all(), error)) {
                std::cerr << error << '\n';
                continue;
            }
            std::cout << "Saved " << validEntries.size() << " dependency record(s).\n";
        } else if (choice == "4") {
            std::string dependentId;
            std::cout << "Dependent Task ID: ";
            if (!std::getline(std::cin, dependentId)) return 0;
            std::string prerequisiteId;
            std::cout << "Prerequisite Task ID: ";
            if (!std::getline(std::cin, prerequisiteId)) return 0;
            std::string error;
            if (!dependencyManager.removeDependency(dependentId, prerequisiteId, error)) {
                std::cerr << error << '\n';
                continue;
            }
            std::cout << "Removed dependency: " << dependentId << " depends on "
                      << prerequisiteId << ".\n";
        } else if (choice == "5") {
            const std::vector<Dependency>& dependencies = dependencyManager.all();
            if (dependencies.empty()) {
                std::cout << "No dependency records found.\n";
                continue;
            }
            std::cout << "All dependencies:\n";
            for (const Dependency& dependency : dependencies) {
                std::cout << "  " << dependency.dependentId << " depends on "
                          << dependency.prerequisiteId << '\n';
            }
        } else if (choice == "6") {
            std::string taskId;
            std::cout << "Task ID: ";
            if (!std::getline(std::cin, taskId)) return 0;
            const std::vector<Dependency> prereqs = dependencyManager.prerequisitesForTask(taskId);
            if (prereqs.empty()) {
                std::cout << "Task " << taskId << " has no prerequisites.\n";
                continue;
            }
            std::cout << "Prerequisites for " << taskId << ":\n";
            for (const Dependency& dependency : prereqs) {
                const auto found = std::find_if(manager.all().begin(), manager.all().end(),
                    [&dependency](const Task& task) { return task.id == dependency.prerequisiteId; });
                const std::string name = found != manager.all().end() ? found->name : "Unknown task";
                std::cout << "  " << dependency.prerequisiteId << " — " << name << '\n';
            }
        } else if (choice == "7") {
            std::string taskId;
            std::cout << "Task ID: ";
            if (!std::getline(std::cin, taskId)) return 0;
            const std::vector<Dependency> dependents = dependencyManager.dependentsForTask(taskId);
            if (dependents.empty()) {
                std::cout << "No tasks depend on " << taskId << ".\n";
                continue;
            }
            std::cout << "Tasks depending on " << taskId << ":\n";
            for (const Dependency& dependency : dependents) {
                const auto found = std::find_if(manager.all().begin(), manager.all().end(),
                    [&dependency](const Task& task) { return task.id == dependency.dependentId; });
                const std::string name = found != manager.all().end() ? found->name : "Unknown task";
                std::cout << "  " << dependency.dependentId << " — " << name << '\n';
            }
        } else if (choice == "8") {
            std::string taskId;
            std::cout << "Task ID: ";
            if (!std::getline(std::cin, taskId)) return 0;
            std::string error;
            const bool ready = dependencyManager.isTaskReady(taskId, manager.all(), error);
            if (ready) {
                std::cout << "Task " << taskId << " is ready to start because all prerequisites are Completed.\n";
            } else {
                std::cout << "Task " << taskId << " is blocked: " << error << '\n';
            }
        } else if (choice == "9") {
            const std::vector<std::string> blocked = dependencyManager.blockedTasks(manager.all());
            if (blocked.empty()) {
                std::cout << "No blocked tasks found.\n";
                continue;
            }
            std::cout << "Blocked tasks:\n";
            for (const std::string& taskId : blocked) {
                const auto found = std::find_if(manager.all().begin(), manager.all().end(),
                    [&taskId](const Task& task) { return task.id == taskId; });
                if (found != manager.all().end()) {
                    std::cout << "  " << taskId << " — " << found->name << '\n';
                }
            }
        } else if (choice == "10") {
            const std::vector<std::string> ready = dependencyManager.readyTasks(manager.all());
            if (ready.empty()) {
                std::cout << "No ready tasks found.\n";
                continue;
            }
            std::cout << "Ready tasks:\n";
            for (const std::string& taskId : ready) {
                const auto found = std::find_if(manager.all().begin(), manager.all().end(),
                    [&taskId](const Task& task) { return task.id == taskId; });
                if (found != manager.all().end()) {
                    std::cout << "  " << taskId << " — " << found->name << '\n';
                }
            }
        } else if (choice == "11") {
            const std::vector<std::string> order = dependencyManager.topologicalOrder(manager.all());
            if (order.empty()) {
                std::cout << "A topological order cannot be generated because the dependency graph contains a cycle.\n";
                continue;
            }
            std::cout << "Topological order:\n";
            for (std::size_t index = 0; index < order.size(); ++index) {
                const auto found = std::find_if(manager.all().begin(), manager.all().end(),
                    [&order, index](const Task& task) { return task.id == order[index]; });
                if (found != manager.all().end()) {
                    std::cout << (index + 1) << ". " << order[index] << " — " << found->name << '\n';
                } else {
                    std::cout << (index + 1) << ". " << order[index] << '\n';
                }
            }
        } else if (choice == "12") {
            std::string error;
            if (!dependencyManager.validateGraph(manager.all(), error)) {
                std::cerr << "Dependency graph validation failed: " << error << '\n';
            } else {
                std::cout << "Dependency graph is valid.\n";
            }
        } else if (choice == "13") {
            std::string error;
            if (!dependencyManager.save(error)) {
                std::cerr << "Could not save dependencies: " << error << '\n';
            } else {
                std::cout << "Dependencies saved to " << dependencyManager.all().size()
                          << " record(s).\n";
            }
        } else {
            std::cerr << "Invalid menu choice.\n";
        }
    }
}

} // namespace taskflow

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
    if (!std::all_of(digits.begin(), digits.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return false;
    }
    try {
        std::size_t parsed = 0;
        return std::stoull(digits, &parsed) != 0 && parsed == digits.size();
    } catch (const std::exception&) {
        return false;
    }
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
        } else if (ch == '"') {
            return false;
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

bool detectCycleForSet(const std::vector<Dependency>& dependencySet,
    const std::vector<Task>& tasks,
    std::vector<std::string>& cycle, std::string& error)
{
    cycle.clear();
    error.clear();

    std::set<std::string> nodes;
    for (const Task& task : tasks) nodes.insert(task.id);
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    std::unordered_set<std::string> seen;

    for (const Dependency& dependency : dependencySet) {
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
        error = "Dependency file is empty or unreadable.";
        return false;
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
    if (input.bad()) {
        error = "Cannot finish reading dependency file: " + storagePath_.string();
        return false;
    }

    std::vector<std::string> cycle;
    if (detectCycleForSet(loaded, {}, cycle, error)) return false;

    dependencies_ = std::move(loaded);
    error.clear();
    return true;
}

bool DependencyManager::save(std::string& error) const
{
    return saveDependencies(dependencies_, error);
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
    if (!taskExists(dependent, tasks) || !taskExists(prerequisite, tasks)) {
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
        error = validationError.empty() ? "The proposed dependency batch is invalid." : validationError;
        return false;
    }

    if (!saveDependencies(combined, error)) return false;
    dependencies_ = std::move(combined);
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
    std::vector<Dependency> candidate = dependencies_;
    const auto found = std::find_if(candidate.begin(), candidate.end(),
        [&dependent, &prerequisite](const Dependency& dependency) {
            return dependency.dependentId == dependent
                && dependency.prerequisiteId == prerequisite;
        });
    if (found == candidate.end()) {
        error = "Dependency not found: " + dependent + " depends on " + prerequisite;
        return false;
    }

    candidate.erase(found);
    if (!saveDependencies(candidate, error)) return false;
    dependencies_ = std::move(candidate);
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

std::vector<std::string> DependencyManager::incompletePrerequisites(
    const std::string& taskId, const std::vector<Task>& tasks) const
{
    std::vector<std::string> incomplete;
    for (const Dependency& dependency : prerequisitesForTask(taskId)) {
        const auto found = std::find_if(tasks.begin(), tasks.end(),
            [&dependency](const Task& task) {
                return task.id == dependency.prerequisiteId;
            });
        if (found == tasks.end() || found->status != Status::Completed) {
            incomplete.push_back(dependency.prerequisiteId);
        }
    }
    return incomplete;
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
    for (const Task& task : tasks) {
        if (!isTaskIdFormat(task.id)) {
            error = "Task list contains an invalid task ID: " + task.id;
            return false;
        }
        if (!knownTasks.insert(task.id).second) {
            error = "Task list contains a duplicate task ID: " + task.id;
            return false;
        }
    }
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
        if (!knownTasks.count(dependency.dependentId)
            || !knownTasks.count(dependency.prerequisiteId)) {
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
    return detectCycleForSet(dependencies_, {}, cycle, error);
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
    std::string error;
    if (!validateGraph(tasks, dependencySet, error)) return {};

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

    std::set<std::string> ready;
    for (const auto& [node, count] : indegree) {
        if (count == 0) ready.insert(node);
    }

    std::vector<std::string> order;
    while (!ready.empty()) {
        const std::string current = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(current);
        for (const std::string& neighbor : adjacency[current]) {
            --indegree[neighbor];
            if (indegree[neighbor] == 0) ready.insert(neighbor);
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
        if (task.status != Status::Completed
            && !incompletePrerequisites(task.id, tasks).empty()) {
            blocked.push_back(task.id);
        }
    }
    std::sort(blocked.begin(), blocked.end());
    return blocked;
}

std::vector<std::string> DependencyManager::readyTasks(
    const std::vector<Task>& tasks) const
{
    std::vector<std::string> ready;
    for (const Task& task : tasks) {
        if (task.status == Status::Completed) continue;
        std::string error;
        if (isTaskReady(task.id, tasks, error)) ready.push_back(task.id);
    }
    std::sort(ready.begin(), ready.end());
    return ready;
}

bool DependencyManager::isTaskReady(const std::string& taskId,
    const std::vector<Task>& tasks, std::string& error) const
{
    const auto task = std::find_if(tasks.begin(), tasks.end(),
        [&taskId](const Task& candidate) { return candidate.id == taskId; });
    if (task == tasks.end()) {
        error = "Task ID not found: " + taskId;
        return false;
    }
    if (task->status == Status::Completed) {
        error = "Task " + taskId + " is already Completed.";
        return false;
    }
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

bool DependencyManager::analyzeImpact(const std::string& taskId,
    const std::vector<Task>& tasks, DependencyImpact& impact,
    std::string& error) const
{
    if (!validateGraph(tasks, error)) return false;
    if (!taskExists(taskId, tasks)) {
        error = "Task ID not found: " + taskId;
        return false;
    }

    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const Dependency& dependency : dependencies_) {
        adjacency[dependency.prerequisiteId].push_back(dependency.dependentId);
    }
    for (auto& item : adjacency) {
        std::sort(item.second.begin(), item.second.end());
        item.second.erase(std::unique(item.second.begin(), item.second.end()),
            item.second.end());
    }

    DependencyImpact result;
    result.taskId = taskId;
    result.directDependentCount = adjacency[taskId].size();
    std::set<std::string> visited;
    std::vector<std::string> pending(adjacency[taskId].begin(),
        adjacency[taskId].end());
    while (!pending.empty()) {
        const std::string current = pending.back();
        pending.pop_back();
        if (current == taskId || !visited.insert(current).second) continue;
        const auto downstream = adjacency.find(current);
        if (downstream != adjacency.end()) {
            pending.insert(pending.end(), downstream->second.begin(),
                downstream->second.end());
        }
    }
    result.downstreamTaskIds.assign(visited.begin(), visited.end());
    result.importanceScore = result.downstreamTaskIds.size();
    impact = std::move(result);
    error.clear();
    return true;
}

bool DependencyManager::taskHasReferences(const std::string& taskId) const
{
    return std::any_of(dependencies_.begin(), dependencies_.end(),
        [&taskId](const Dependency& dependency) {
            return dependency.dependentId == taskId
                || dependency.prerequisiteId == taskId;
        });
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
        const std::string inspectError = fileError.message();
        std::filesystem::remove(temporaryPath, fileError);
        error = "Cannot inspect existing dependency file: " + inspectError;
        return false;
    }

    bool backupCreated = false;
    if (targetExists) {
        std::error_code typeError;
        if (!std::filesystem::is_regular_file(storagePath_, typeError)) {
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
            error = typeError
                ? "Cannot inspect existing dependency path: " + typeError.message()
                : "Dependency storage path is not a regular file: "
                    + storagePath_.string();
            return false;
        }

        std::error_code backupError;
        if (std::filesystem::exists(backup, backupError) || backupError) {
            std::filesystem::remove(temporaryPath, backupError);
            error = "Cannot safely save dependencies because a backup already exists: "
                + backup.string();
            return false;
        }
        std::filesystem::rename(storagePath_, backup, fileError);
        if (fileError) {
            const std::string preserveError = fileError.message();
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
            error = "Cannot preserve the previous dependency file: " + preserveError;
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
        const std::string installError = fileError.message();
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        error = "Cannot install the new dependency file: " + installError;
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

int runSector03Demo(const std::filesystem::path& storagePath)
{
    DependencyManager dependencyManager(storagePath);
    std::string error;
    if (!dependencyManager.load(error)) {
        std::cerr << "Could not load dependencies for demo: " << error << '\n';
        return 1;
    }

    std::cout << "=== Sector 03: Dependency Management Demo ===\n";
    const std::vector<Dependency> oldDependencies = dependencyManager.all();
    for (const Dependency& dependency : oldDependencies) {
        if (!dependencyManager.removeDependency(dependency.dependentId,
                dependency.prerequisiteId, error)) {
            std::cerr << "Could not reset demo dependency data: " << error << '\n';
            return 1;
        }
    }

    const std::vector<Task> tasks = {
        {"TSK-000001", "Complete Requirements", "", Priority::High, 60,
            "2028-01-01", "Planning", Status::Completed},
        {"TSK-000002", "Implement Code", "", Priority::High, 120,
            "2028-01-02", "Development", Status::Pending},
        {"TSK-000003", "Test Application", "", Priority::Medium, 45,
            "2028-01-03", "Testing", Status::Pending},
        {"TSK-000004", "Independent Task", "", Priority::Low, 30,
            "2028-01-04", "Admin", Status::Pending}
    };
    const auto taskName = [&tasks](const std::string& id) {
        const auto found = std::find_if(tasks.begin(), tasks.end(),
            [&id](const Task& task) { return task.id == id; });
        return found == tasks.end() ? std::string("Unknown task") : found->name;
    };
    const auto printGraph = [&]() {
        std::cout << "Dependency graph (prerequisite -> dependent):\n";
        for (const Task& task : tasks) {
            std::cout << task.id << ": " << task.name << '\n';
            for (const Dependency& dependency :
                    dependencyManager.dependentsForTask(task.id)) {
                std::cout << "  -> " << dependency.dependentId << ": "
                          << taskName(dependency.dependentId) << '\n';
            }
        }
    };

    std::cout << "Adding dependencies with the DependencyManager:\n";
    if (!dependencyManager.addDependency(tasks[1].id, tasks[0].id, tasks, error)
        || !dependencyManager.addDependency(tasks[2].id, tasks[1].id, tasks, error)) {
        std::cerr << "Could not create demo dependencies: " << error << '\n';
        return 1;
    }
    std::cout << tasks[1].name << " depends on " << tasks[0].name << ".\n"
              << tasks[2].name << " depends on " << tasks[1].name << ".\n"
              << tasks[3].name << " has no prerequisites.\n\n";

    if (!dependencyManager.validateGraph(tasks, error)) {
        std::cerr << "Demo graph validation failed: " << error << '\n';
        return 1;
    }
    printGraph();

    const std::vector<std::string> order = dependencyManager.topologicalOrder(tasks);
    if (order.size() != tasks.size()) {
        std::cerr << "Could not produce a complete topological order for the demo.\n";
        return 1;
    }
    std::cout << "\nTopological order: ";
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (index != 0) std::cout << " -> ";
        std::cout << taskName(order[index]);
    }
    std::cout << '\n';

    std::cout << "Ready tasks:\n";
    for (const std::string& id : dependencyManager.readyTasks(tasks)) {
        std::cout << "  " << taskName(id) << '\n';
    }
    std::cout << "Blocked tasks:\n";
    for (const std::string& id : dependencyManager.blockedTasks(tasks)) {
        std::cout << "  " << taskName(id) << " (Waiting for";
        for (const std::string& prerequisite :
                dependencyManager.incompletePrerequisites(id, tasks)) {
            std::cout << ' ' << taskName(prerequisite);
        }
        std::cout << ")\n";
    }

    std::cout << "\nCycle test: attempt to add "
              << tasks[0].name << " depending on " << tasks[2].name << ".\n";
    if (dependencyManager.addDependency(tasks[0].id, tasks[2].id, tasks, error)) {
        std::cerr << "Cycle test unexpectedly succeeded.\n";
        return 1;
    }
    if (error.find("Circular dependency detected:") == std::string::npos) {
        std::cerr << "Cycle test failed for an unexpected reason: " << error << '\n';
        return 1;
    }
    std::cout << "ERROR: Circular dependency detected!\n"
              << "Dependency rejected. " << error << '\n';

    DependencyImpact impact;
    if (!dependencyManager.analyzeImpact(tasks[0].id, tasks, impact, error)) {
        std::cerr << "Could not analyze demo impact: " << error << '\n';
        return 1;
    }
    std::cout << "\nDependency importance for " << taskName(impact.taskId) << ":\n"
              << "  Direct dependent tasks: " << impact.directDependentCount << '\n'
              << "  Downstream dependent tasks: " << impact.downstreamTaskIds.size() << '\n'
              << "  Importance score: " << impact.importanceScore
              << " (number of unique downstream tasks)\n";
    for (const std::string& id : impact.downstreamTaskIds) {
        std::cout << "    " << id << ": " << taskName(id) << '\n';
    }

    if (!dependencyManager.removeDependency(tasks[2].id, tasks[1].id, error)) {
        std::cerr << "Could not remove the demo dependency: " << error << '\n';
        return 1;
    }
    std::cout << "\nRemoved dependency: " << taskName(tasks[2].id)
              << " no longer depends on " << taskName(tasks[1].id) << ".\n"
              << "Dependency graph valid after removal: "
              << (dependencyManager.validateGraph(tasks, error) ? "yes" : "no")
              << '\n';
    return 0;
}

int runDependencyMenu(TaskManager& manager, DependencyManager& dependencyManager)
{
    const auto taskName = [&manager](const std::string& id) {
        const auto found = std::find_if(manager.all().begin(), manager.all().end(),
            [&id](const Task& task) { return task.id == id; });
        return found == manager.all().end() ? std::string("Unknown task") : found->name;
    };
    const auto graphIsValid = [&manager, &dependencyManager]() {
        std::string error;
        if (dependencyManager.validateGraph(manager.all(), error)) return true;
        std::cerr << "Dependency graph is invalid: " << error << '\n'
                  << "Repair or remove invalid dependency records before using graph results.\n";
        return false;
    };

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
                  << "14. Search Dependencies by Task ID\n"
                  << "15. Display Dependency Graph\n"
                  << "16. Analyze Dependency Importance\n"
                  << "17. Back to Main Menu\n"
                  << "Choice: ";

        std::string choice;
        if (!std::getline(std::cin, choice)) return 0;

        if (choice == "17") return 0;

        if (choice == "1") {
            std::string dependentId;
            std::cout << "Task that depends on (Dependent Task ID): ";
            if (!std::getline(std::cin, dependentId)) return 0;
            std::string prerequisiteId;
            std::cout << "Prerequisite Task ID: ";
            if (!std::getline(std::cin, prerequisiteId)) return 0;

            std::string error;
            std::cout << "Dependency preview: " << trim(dependentId) << " ("
                      << taskName(trim(dependentId)) << ") depends on "
                      << trim(prerequisiteId) << " ("
                      << taskName(trim(prerequisiteId)) << ").\n"
                      << "Confirm adding this dependency? (Y/N): ";
            std::string confirmation;
            if (!std::getline(std::cin, confirmation)) return 0;
            const std::string response = lower(trim(confirmation));
            if (response != "y" && response != "yes") {
                std::cout << "Dependency addition cancelled.\n";
                continue;
            }
            if (!dependencyManager.addDependency(dependentId, prerequisiteId,
                    manager.all(), error)) {
                std::cerr << error << '\n';
                continue;
            }
            std::cout << "Added dependency: " << dependentId << " depends on "
                      << prerequisiteId << ".\n";
            if (!error.empty()) std::cerr << "Warning: " << error << '\n';
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
            std::cout << "Dependency preview: " << dependency.dependentId << " ("
                      << taskName(dependency.dependentId) << ") depends on "
                      << dependency.prerequisiteId << " ("
                      << taskName(dependency.prerequisiteId) << ").\n"
                      << "Confirm adding this dependency? (Y/N): ";
            std::string confirmation;
            if (!std::getline(std::cin, confirmation)) return 0;
            const std::string response = lower(trim(confirmation));
            if (response != "y" && response != "yes") {
                std::cout << "Dependency addition cancelled.\n";
                continue;
            }
            if (!dependencyManager.addDependency(dependency.dependentId,
                    dependency.prerequisiteId, manager.all(), error)) {
                std::cerr << error << '\n';
                continue;
            }
            std::cout << "Added quick dependency: " << dependency.dependentId
                      << " depends on " << dependency.prerequisiteId << ".\n";
            if (!error.empty()) std::cerr << "Warning: " << error << '\n';
        } else if (choice == "3") {
            std::vector<std::string> lines;
            std::cout << "Paste dependency records one per line; type END when done.\n";
            for (;;) {
                std::string line;
                if (!std::getline(std::cin, line)) return 0;
                if (trim(line) == "END") break;
                lines.push_back(line);
            }

            std::vector<std::string> errors(lines.size());
            std::vector<bool> valid(lines.size(), false);
            std::vector<Dependency> validEntries;
            for (std::size_t index = 0; index < lines.size(); ++index) {
                Dependency dependency;
                std::string error;
                if (!parseQuickDependency(lines[index], dependency, error)) {
                    errors[index] = error;
                    continue;
                }
                std::vector<Dependency> candidate = dependencyManager.all();
                candidate.insert(candidate.end(), validEntries.begin(), validEntries.end());
                candidate.push_back(dependency);
                if (!dependencyManager.validateGraph(manager.all(), candidate, error)) {
                    errors[index] = error;
                    continue;
                }
                valid[index] = true;
                validEntries.push_back(dependency);
            }

            std::cout << "Bulk dependency preview:\n";
            for (std::size_t index = 0; index < lines.size(); ++index) {
                std::cout << "  Line " << index + 1 << ": " << lines[index]
                          << " — " << (valid[index] ? "Valid" : "Invalid");
                if (!valid[index]) std::cout << " (" << errors[index] << ')';
                std::cout << '\n';
            }
            std::cout << "Confirm saving the entire batch? (Y/N): ";
            std::string confirmation;
            if (!std::getline(std::cin, confirmation)) return 0;
            const std::string response = lower(trim(confirmation));
            if (response != "y" && response != "yes") {
                std::cout << "Bulk dependency save cancelled.\n";
                continue;
            }
            if (validEntries.size() != lines.size()) {
                std::cout << "Bulk dependency batch rejected; no records were saved.\n";
                continue;
            }

            std::string error;
            if (!dependencyManager.addDependencies(validEntries, manager.all(), error)) {
                std::cerr << error << '\n';
                continue;
            }
            std::cout << "Saved " << validEntries.size() << " dependency record(s).\n";
            if (!error.empty()) std::cerr << "Warning: " << error << '\n';
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
            if (!error.empty()) std::cerr << "Warning: " << error << '\n';
        } else if (choice == "5") {
            if (!graphIsValid()) continue;
            const std::vector<Dependency>& dependencies = dependencyManager.all();
            if (dependencies.empty()) {
                std::cout << "No dependency records found.\n";
                continue;
            }
            std::cout << "All dependencies:\n";
            for (const Dependency& dependency : dependencies) {
                std::cout << "  " << dependency.dependentId << " ("
                          << taskName(dependency.dependentId) << ") depends on "
                          << dependency.prerequisiteId << " ("
                          << taskName(dependency.prerequisiteId) << ")\n";
            }
        } else if (choice == "6") {
            if (!graphIsValid()) continue;
            std::string taskId;
            std::cout << "Task ID: ";
            if (!std::getline(std::cin, taskId)) return 0;
            if (!taskExists(taskId, manager.all())) {
                std::cerr << "Task ID not found: " << taskId << '\n';
                continue;
            }
            const std::vector<Dependency> prereqs = dependencyManager.prerequisitesForTask(taskId);
            if (prereqs.empty()) {
                std::cout << "Task " << taskId << " has no prerequisites.\n";
                continue;
            }
            std::cout << "Prerequisites for " << taskId << ":\n";
            for (const Dependency& dependency : prereqs) {
                std::cout << "  " << dependency.prerequisiteId << " — "
                          << taskName(dependency.prerequisiteId) << '\n';
            }
        } else if (choice == "7") {
            if (!graphIsValid()) continue;
            std::string taskId;
            std::cout << "Task ID: ";
            if (!std::getline(std::cin, taskId)) return 0;
            if (!taskExists(taskId, manager.all())) {
                std::cerr << "Task ID not found: " << taskId << '\n';
                continue;
            }
            const std::vector<Dependency> dependents = dependencyManager.dependentsForTask(taskId);
            if (dependents.empty()) {
                std::cout << "No tasks depend on " << taskId << ".\n";
                continue;
            }
            std::cout << "Tasks depending on " << taskId << ":\n";
            for (const Dependency& dependency : dependents) {
                std::cout << "  " << dependency.dependentId << " — "
                          << taskName(dependency.dependentId) << '\n';
            }
        } else if (choice == "8") {
            if (!graphIsValid()) continue;
            std::string taskId;
            std::cout << "Task ID: ";
            if (!std::getline(std::cin, taskId)) return 0;
            std::string error;
            const bool ready = dependencyManager.isTaskReady(taskId, manager.all(), error);
            if (ready) {
                std::cout << "Task " << taskId << " is ready to start because all prerequisites are Completed.\n";
            } else if (error.find("already Completed") != std::string::npos) {
                std::cout << error << '\n';
            } else {
                std::cout << "Task " << taskId << " is blocked: " << error << '\n';
            }
        } else if (choice == "9") {
            if (!graphIsValid()) continue;
            const std::vector<std::string> blocked = dependencyManager.blockedTasks(manager.all());
            if (blocked.empty()) {
                std::cout << "No blocked tasks found.\n";
                continue;
            }
            std::cout << "Blocked tasks:\n";
            for (const std::string& taskId : blocked) {
                std::cout << "  " << taskId << " — " << taskName(taskId)
                          << " (waiting for:";
                for (const std::string& prerequisite :
                        dependencyManager.incompletePrerequisites(taskId, manager.all())) {
                    std::cout << ' ' << prerequisite << " — "
                              << taskName(prerequisite);
                }
                std::cout << ")\n";
            }
        } else if (choice == "10") {
            if (!graphIsValid()) continue;
            const std::vector<std::string> ready = dependencyManager.readyTasks(manager.all());
            if (ready.empty()) {
                std::cout << "No ready tasks found.\n";
                continue;
            }
            std::cout << "Ready tasks:\n";
            for (const std::string& taskId : ready) {
                std::cout << "  " << taskId << " — " << taskName(taskId) << '\n';
            }
        } else if (choice == "11") {
            if (!graphIsValid()) continue;
            const std::vector<std::string> order = dependencyManager.topologicalOrder(manager.all());
            std::cout << "Topological order:\n";
            for (std::size_t index = 0; index < order.size(); ++index) {
                std::cout << (index + 1) << ". " << order[index] << " — "
                          << taskName(order[index]) << '\n';
            }
            if (order.empty()) std::cout << "No tasks to order.\n";
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
                if (!error.empty()) std::cerr << "Warning: " << error << '\n';
            }
        } else if (choice == "14") {
            if (!graphIsValid()) continue;
            std::string taskId;
            std::cout << "Task ID to search: ";
            if (!std::getline(std::cin, taskId)) return 0;
            if (!taskExists(taskId, manager.all())) {
                std::cerr << "Task ID not found: " << taskId << '\n';
                continue;
            }
            const std::vector<Dependency> matches =
                dependencyManager.search(taskId);
            if (matches.empty()) {
                std::cout << "No dependency records reference " << taskId << ".\n";
                continue;
            }
            std::cout << "Dependencies referencing " << taskId << ":\n";
            for (const Dependency& dependency : matches) {
                std::cout << "  " << dependency.dependentId << " ("
                          << taskName(dependency.dependentId) << ") depends on "
                          << dependency.prerequisiteId << " ("
                          << taskName(dependency.prerequisiteId) << ")\n";
            }
        } else if (choice == "15") {
            if (!graphIsValid()) continue;
            std::cout << "Dependency graph (prerequisite -> dependent):\n";
            for (const Task& task : manager.all()) {
                std::cout << task.id << ": " << task.name << '\n';
                for (const Dependency& dependency :
                        dependencyManager.dependentsForTask(task.id)) {
                    std::cout << "  -> " << dependency.dependentId << ": "
                              << taskName(dependency.dependentId) << '\n';
                }
            }
        } else if (choice == "16") {
            if (!graphIsValid()) continue;
            std::string taskId;
            std::cout << "Task ID to analyze: ";
            if (!std::getline(std::cin, taskId)) return 0;
            DependencyImpact impact;
            std::string error;
            if (!dependencyManager.analyzeImpact(taskId, manager.all(),
                    impact, error)) {
                std::cerr << error << '\n';
                continue;
            }
            std::cout << "Dependency importance for " << taskId << " ("
                      << taskName(taskId) << "):\n"
                      << "  Direct dependent tasks: "
                      << impact.directDependentCount << '\n'
                      << "  Downstream dependent tasks: "
                      << impact.downstreamTaskIds.size() << '\n'
                      << "  Importance score: " << impact.importanceScore
                      << " (number of unique downstream tasks)\n";
            for (const std::string& downstream : impact.downstreamTaskIds) {
                std::cout << "    " << downstream << " — "
                          << taskName(downstream) << '\n';
            }
        } else {
            std::cerr << "Invalid menu choice.\n";
        }
    }
}

} // namespace taskflow

#ifndef TASKFLOW_DEPENDENCY_MANAGEMENT_HPP
#define TASKFLOW_DEPENDENCY_MANAGEMENT_HPP

#include <filesystem>
#include <string>
#include <vector>

#include "task_management.hpp"

namespace taskflow {

struct Dependency {
    std::string dependentId;
    std::string prerequisiteId;
};

class DependencyManager {
public:
    explicit DependencyManager(std::filesystem::path storagePath);

    bool load(std::string& error);
    bool save(std::string& error) const;

    bool addDependency(const std::string& dependentId,
        const std::string& prerequisiteId, std::string& error);
    bool addDependency(const std::string& dependentId,
        const std::string& prerequisiteId, const std::vector<Task>& tasks,
        std::string& error);
    bool addDependencies(const std::vector<Dependency>& batch,
        const std::vector<Task>& tasks, std::string& error);
    bool removeDependency(const std::string& dependentId,
        const std::string& prerequisiteId, std::string& error);

    const std::vector<Dependency>& all() const;
    std::vector<Dependency> prerequisitesForTask(
        const std::string& taskId) const;
    std::vector<Dependency> dependentsForTask(
        const std::string& taskId) const;
    std::vector<Dependency> search(const std::string& taskId) const;

    bool validateGraph(const std::vector<Task>& tasks, std::string& error) const;
    bool validateGraph(const std::vector<Task>& tasks,
        const std::vector<Dependency>& dependencySet, std::string& error) const;
    bool validateBatch(const std::vector<Dependency>& batch,
        const std::vector<Task>& tasks, std::string& error) const;

    bool detectCycle(std::vector<std::string>& cycle,
        std::string& error) const;
    bool detectCircularDependencies(std::vector<std::string>& cycle,
        std::string& error) const;
    std::vector<std::string> topologicalOrder(
        const std::vector<Task>& tasks) const;
    std::vector<std::string> topologicalOrder(
        const std::vector<Task>& tasks,
        const std::vector<Dependency>& dependencySet) const;
    std::vector<std::string> blockedTasks(const std::vector<Task>& tasks) const;
    std::vector<std::string> readyTasks(const std::vector<Task>& tasks) const;
    bool isTaskReady(const std::string& taskId,
        const std::vector<Task>& tasks, std::string& error) const;

    bool taskHasDependents(const std::string& taskId) const;
    bool hasDependency(const std::string& dependentId,
        const std::string& prerequisiteId) const;

private:
    bool saveDependencies(const std::vector<Dependency>& dependencies,
        std::string& error) const;
    bool containsDependency(const std::string& dependentId,
        const std::string& prerequisiteId) const;
    bool isTaskKnown(const std::string& taskId,
        const std::vector<Task>& tasks) const;

    std::filesystem::path storagePath_;
    std::vector<Dependency> dependencies_;
};

int runDependencyMenu(TaskManager& manager, DependencyManager& dependencyManager);
int runSector03Demo(const std::filesystem::path& storagePath);

} // namespace taskflow

#endif

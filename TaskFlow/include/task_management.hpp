#ifndef TASKFLOW_TASK_MANAGEMENT_HPP
#define TASKFLOW_TASK_MANAGEMENT_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace taskflow {

enum class Priority {
    Low,
    Medium,
    High,
    Critical
};

enum class Status {
    Pending,
    Ready,
    InProgress,
    Completed,
    Overdue
};

enum class SortField {
    Id,
    Name,
    Priority,
    Duration,
    Deadline,
    Status
};

struct Task {
    std::string id;
    std::string name;
    std::string description;
    Priority priority = Priority::Medium;
    int durationMinutes = 1;
    std::string deadline;
    std::string type;
    Status status = Status::Pending;
};

struct TaskFilter {
    std::optional<Priority> priority;
    std::optional<Status> status;
    std::string type;
};

bool isValidDate(const std::string& date);
bool validateTask(const Task& task, std::string& error);
bool parsePriority(const std::string& text, Priority& priority);
bool parseStatus(const std::string& text, Status& status);
std::string toString(Priority priority);
std::string toString(Status status);

class TaskManager {
public:
    explicit TaskManager(std::filesystem::path storagePath);

    bool load(std::string& error);
    bool save(std::string& error) const;
    bool create(const Task& task, Task& created, std::string& error);
    bool update(const std::string& id, const Task& replacement, std::string& error);
    bool erase(const std::string& id, std::string& error);

    const std::vector<Task>& all() const;
    std::vector<Task> search(const std::string& query) const;
    std::vector<Task> filter(const TaskFilter& criteria) const;
    std::vector<Task> sorted(SortField field) const;

private:
    bool saveTasks(const std::vector<Task>& tasks, std::string& error) const;
    std::string generateId(const std::vector<Task>& tasks);

    std::filesystem::path storagePath_;
    std::vector<Task> tasks_;
    unsigned long long nextId_ = 1;
};

int runTaskManagementTests(const std::filesystem::path& testDirectory);
int runSector01Demo(const std::filesystem::path& storagePath);
int runInteractiveTaskManager(const std::filesystem::path& storagePath,
    const std::filesystem::path& availabilityPath,
    const std::filesystem::path& dependencyPath = "data/dependencies.csv");

} // namespace taskflow

#endif

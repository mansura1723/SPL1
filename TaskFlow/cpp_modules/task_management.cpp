#include "task_management.hpp"
#include "availability_management.hpp"
#include "dependency_management.hpp"
#include "scheduling_management.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace taskflow {
namespace {

const char* const fileHeader =
    "id,name,description,priority,duration_minutes,deadline,type,status";

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

bool hasLineBreak(const std::string& text)
{
    return text.find('\n') != std::string::npos
        || text.find('\r') != std::string::npos;
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

    if (quoted) {
        return false;
    }
    fields.push_back(field);
    return true;
}

void writeField(std::ostream& output, const std::string& field)
{
    const bool quote = field.find_first_of(",\"\r\n") != std::string::npos;
    if (!quote) {
        output << field;
        return;
    }

    output << '"';
    for (char ch : field) {
        if (ch == '"') {
            output << '"';
        }
        output << ch;
    }
    output << '"';
}

bool parseIdNumber(const std::string& id, unsigned long long& number)
{
    if (id.size() < 5 || id.compare(0, 4, "TSK-") != 0) {
        return false;
    }

    const std::string digits = id.substr(4);
    if (!std::all_of(digits.begin(), digits.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return false;
    }

    try {
        std::size_t parsed = 0;
        number = std::stoull(digits, &parsed);
        return parsed == digits.size() && number != 0;
    } catch (const std::exception&) {
        return false;
    }
}

int priorityRank(Priority priority)
{
    switch (priority) {
    case Priority::Low: return 0;
    case Priority::Medium: return 1;
    case Priority::High: return 2;
    case Priority::Critical: return 3;
    }
    return -1;
}

int statusRank(Status status)
{
    switch (status) {
    case Status::Pending: return 0;
    case Status::Ready: return 1;
    case Status::InProgress: return 2;
    case Status::Completed: return 3;
    case Status::Overdue: return 4;
    }
    return -1;
}

bool hasUniqueIds(const std::vector<Task>& tasks, std::string& error)
{
    std::vector<std::string> ids;
    ids.reserve(tasks.size());
    for (const Task& task : tasks) {
        unsigned long long ignored = 0;
        if (!parseIdNumber(task.id, ignored)) {
            error = "Invalid task ID in storage: " + task.id;
            return false;
        }
        ids.push_back(task.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        error = "Duplicate task ID in storage: "
            + *std::adjacent_find(ids.begin(), ids.end());
        return false;
    }
    return true;
}

bool taskFromFields(const std::vector<std::string>& fields, Task& task,
    std::string& error)
{
    if (fields.size() != 8) {
        error = "A task record must contain exactly 8 fields.";
        return false;
    }

    Priority priority;
    Status status;
    if (!parsePriority(fields[3], priority)) {
        error = "Invalid priority in task record: " + fields[3];
        return false;
    }
    if (!parseStatus(fields[7], status)) {
        error = "Invalid status in task record: " + fields[7];
        return false;
    }

    int duration = 0;
    try {
        std::size_t parsed = 0;
        duration = std::stoi(fields[4], &parsed);
        if (parsed != fields[4].size()) {
            error = "Invalid duration in task record: " + fields[4];
            return false;
        }
    } catch (const std::exception&) {
        error = "Invalid duration in task record: " + fields[4];
        return false;
    }

    task = Task{fields[0], fields[1], fields[2], priority,
        duration, fields[5], fields[6], status};
    if (!validateTask(task, error)) {
        error = "Invalid task " + task.id + ": " + error;
        return false;
    }
    unsigned long long ignored = 0;
    if (!parseIdNumber(task.id, ignored)) {
        error = "Invalid task ID in storage: " + task.id;
        return false;
    }
    return true;
}

std::pair<std::string, int> deadlineSortKey(const std::string& deadline)
{
    const std::string date = deadline.substr(0, 10);
    const int minute = deadline.size() == 10 ? 1439
        : std::stoi(deadline.substr(11, 2)) * 60
            + std::stoi(deadline.substr(14, 2));
    return {date, minute};
}

bool lessBy(const Task& left, const Task& right, SortField field)
{
    switch (field) {
    case SortField::Id:
        return left.id < right.id;
    case SortField::Name:
        return lower(left.name) < lower(right.name);
    case SortField::Priority:
        return priorityRank(left.priority) > priorityRank(right.priority);
    case SortField::Duration:
        return left.durationMinutes < right.durationMinutes;
    case SortField::Deadline:
        return deadlineSortKey(left.deadline) < deadlineSortKey(right.deadline);
    case SortField::Status:
        return statusRank(left.status) < statusRank(right.status);
    }
    return false;
}

bool readLine(const std::string& prompt, std::string& value)
{
    std::cout << prompt;
    return static_cast<bool>(std::getline(std::cin, value));
}

bool readTaskDetails(Task& task)
{
    std::string input;
    while (readLine("Task name (1-120 characters): ", task.name)) {
        if (!task.name.empty() && task.name.size() <= 120
            && !hasLineBreak(task.name)) {
            break;
        }
        std::cout << "Name must contain 1-120 characters.\n";
    }
    if (!std::cin) return false;

    while (readLine("Description (up to 2000 characters): ", task.description)) {
        if (task.description.size() <= 2000 && !hasLineBreak(task.description)) {
            break;
        }
        std::cout << "Description is too long or contains a line break.\n";
    }
    if (!std::cin) return false;

    while (readLine("Priority (Low, Medium, High, Critical): ", input)) {
        if (parsePriority(input, task.priority)) break;
        std::cout << "Enter Low, Medium, High, or Critical.\n";
    }
    if (!std::cin) return false;

    while (readLine("Duration in minutes (1-1000000): ", input)) {
        try {
            std::size_t parsed = 0;
            const int duration = std::stoi(input, &parsed);
            if (parsed == input.size() && duration > 0
                && duration <= 1000000) {
                task.durationMinutes = duration;
                break;
            }
        } catch (const std::exception&) {
        }
        std::cout << "Enter a whole number from 1 to 1000000.\n";
    }
    if (!std::cin) return false;

    while (readLine("Deadline (YYYY-MM-DD or YYYY-MM-DDTHH:MM): ",
            task.deadline)) {
        if (isValidDeadline(task.deadline)) break;
        std::cout << "Enter a real date or local date-time in the displayed format.\n";
    }
    if (!std::cin) return false;

    while (readLine("Task type: ", task.type)) {
        if (!task.type.empty() && task.type.size() <= 60
            && !hasLineBreak(task.type)) {
            break;
        }
        std::cout << "Task type must contain 1-60 characters.\n";
    }
    if (!std::cin) return false;

    while (readLine("Status (Pending, Ready, In Progress, Completed, Overdue): ",
               input)) {
        if (parseStatus(input, task.status)) break;
        std::cout << "Enter one of the listed statuses.\n";
    }
    return static_cast<bool>(std::cin);
}

void printTasks(const std::vector<Task>& tasks)
{
    if (tasks.empty()) {
        std::cout << "No tasks found.\n";
        return;
    }

    for (const Task& task : tasks) {
        std::cout << task.id << " | " << task.name << " | "
                  << toString(task.priority) << " | " << task.durationMinutes
                  << " min | " << task.deadline << " | " << task.type
                  << " | " << toString(task.status) << '\n'
                  << "  " << task.description << '\n';
    }
}

void printTaskInput(const Task& task)
{
    std::cout << "  Name: " << task.name << '\n'
              << "  Description: " << task.description << '\n'
              << "  Priority: " << toString(task.priority) << '\n'
              << "  Duration: " << task.durationMinutes << " minute(s)\n"
              << "  Deadline: " << task.deadline << '\n'
              << "  Type: " << task.type << '\n'
              << "  Status: " << toString(task.status) << '\n';
}

} // namespace

bool isValidDate(const std::string& date)
{
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < date.size(); ++index) {
        if (index != 4 && index != 7
            && !std::isdigit(static_cast<unsigned char>(date[index]))) {
            return false;
        }
    }

    std::tm parsed = {};
    parsed.tm_year = std::stoi(date.substr(0, 4)) - 1900;
    parsed.tm_mon = std::stoi(date.substr(5, 2)) - 1;
    parsed.tm_mday = std::stoi(date.substr(8, 2));
    parsed.tm_isdst = -1;
    const std::tm original = parsed;
    if (std::mktime(&parsed) == static_cast<std::time_t>(-1)) {
        return false;
    }
    return parsed.tm_year == original.tm_year
        && parsed.tm_mon == original.tm_mon
        && parsed.tm_mday == original.tm_mday;
}

bool isValidDeadline(const std::string& deadline)
{
    if (isValidDate(deadline)) return true;
    if (deadline.size() != 16 || deadline[4] != '-'
        || deadline[7] != '-'
        || (deadline[10] != 'T' && deadline[10] != ' ')
        || deadline[13] != ':'
        || !isValidDate(deadline.substr(0, 10))) return false;
    for (const std::size_t index : {11u, 12u, 14u, 15u}) {
        if (!std::isdigit(static_cast<unsigned char>(deadline[index])))
            return false;
    }
    const int hour = std::stoi(deadline.substr(11, 2));
    const int minute = std::stoi(deadline.substr(14, 2));
    if (hour > 23 || minute > 59) return false;

    std::tm parsed = {};
    parsed.tm_year = std::stoi(deadline.substr(0, 4)) - 1900;
    parsed.tm_mon = std::stoi(deadline.substr(5, 2)) - 1;
    parsed.tm_mday = std::stoi(deadline.substr(8, 2));
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_isdst = -1;
    const std::tm original = parsed;
    if (std::mktime(&parsed) == static_cast<std::time_t>(-1)) return false;
    return parsed.tm_year == original.tm_year
        && parsed.tm_mon == original.tm_mon
        && parsed.tm_mday == original.tm_mday
        && parsed.tm_hour == original.tm_hour
        && parsed.tm_min == original.tm_min;
}

bool validateTask(const Task& task, std::string& error)
{
    if (task.name.empty() || task.name.size() > 120 || hasLineBreak(task.name)) {
        error = "Task name must contain 1-120 characters on one line.";
        return false;
    }
    if (task.description.size() > 2000 || hasLineBreak(task.description)) {
        error = "Description must be at most 2000 characters on one line.";
        return false;
    }
    if (task.durationMinutes < 1 || task.durationMinutes > 1000000) {
        error = "Duration must be between 1 and 1000000 minutes.";
        return false;
    }
    if (!isValidDeadline(task.deadline)) {
        error = "Deadline must be a real date or local date-time "
            "in YYYY-MM-DD[THH:MM] format.";
        return false;
    }
    if (task.type.empty() || task.type.size() > 60 || hasLineBreak(task.type)) {
        error = "Task type must contain 1-60 characters on one line.";
        return false;
    }
    if (priorityRank(task.priority) < 0 || statusRank(task.status) < 0) {
        error = "Task priority or status is invalid.";
        return false;
    }
    error.clear();
    return true;
}

bool parsePriority(const std::string& text, Priority& priority)
{
    const std::string value = lower(text);
    if (value == "low") priority = Priority::Low;
    else if (value == "medium") priority = Priority::Medium;
    else if (value == "high") priority = Priority::High;
    else if (value == "critical") priority = Priority::Critical;
    else return false;
    return true;
}

bool parseStatus(const std::string& text, Status& status)
{
    const std::string value = lower(text);
    if (value == "pending") status = Status::Pending;
    else if (value == "ready") status = Status::Ready;
    else if (value == "in progress" || value == "inprogress")
        status = Status::InProgress;
    else if (value == "completed") status = Status::Completed;
    else if (value == "overdue") status = Status::Overdue;
    else return false;
    return true;
}

std::string toString(Priority priority)
{
    switch (priority) {
    case Priority::Low: return "Low";
    case Priority::Medium: return "Medium";
    case Priority::High: return "High";
    case Priority::Critical: return "Critical";
    }
    return "Invalid";
}

std::string toString(Status status)
{
    switch (status) {
    case Status::Pending: return "Pending";
    case Status::Ready: return "Ready";
    case Status::InProgress: return "In Progress";
    case Status::Completed: return "Completed";
    case Status::Overdue: return "Overdue";
    }
    return "Invalid";
}

TaskManager::TaskManager(std::filesystem::path storagePath)
    : storagePath_(std::move(storagePath))
{
}

bool TaskManager::load(std::string& error)
{
    std::error_code fileError;
    const bool exists = std::filesystem::exists(storagePath_, fileError);
    if (fileError) {
        error = "Cannot inspect task file: " + fileError.message();
        return false;
    }
    if (!exists) {
        tasks_.clear();
        nextId_ = 1;
        error.clear();
        return true;
    }

    std::ifstream input(storagePath_);
    if (!input) {
        error = "Cannot open task file for reading: " + storagePath_.string();
        return false;
    }

    std::string line;
    if (!std::getline(input, line)) {
        if (input.eof()) {
            tasks_.clear();
            nextId_ = 1;
            error.clear();
            return true;
        }
        error = "Cannot read task file: " + storagePath_.string();
        return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != fileHeader) {
        error = "Task file has an invalid or unsupported header.";
        return false;
    }

    std::vector<Task> loaded;
    unsigned long long highestId = 0;
    std::size_t lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        Task task;
        if (!parseRecord(line, fields) || !taskFromFields(fields, task, error)) {
            if (error.empty()) error = "Malformed CSV record.";
            error = "Task file line " + std::to_string(lineNumber) + ": " + error;
            return false;
        }
        unsigned long long idNumber = 0;
        parseIdNumber(task.id, idNumber);
        highestId = std::max(highestId, idNumber);
        loaded.push_back(std::move(task));
    }
    if (input.bad()) {
        error = "An I/O error occurred while reading the task file.";
        return false;
    }
    if (!hasUniqueIds(loaded, error)) return false;
    if (highestId == std::numeric_limits<unsigned long long>::max()) {
        error = "Task IDs are exhausted; no new task ID can be generated.";
        return false;
    }

    tasks_ = std::move(loaded);
    nextId_ = highestId + 1;
    error.clear();
    return true;
}

bool TaskManager::save(std::string& error) const
{
    return saveTasks(tasks_, error);
}

bool TaskManager::create(const Task& task, Task& created, std::string& error)
{
    if (!validateTask(task, error)) return false;

    std::vector<Task> candidate = tasks_;
    Task newTask = task;
    newTask.id = generateId(candidate);
    if (newTask.id.empty()) {
        error = "Task IDs are exhausted; no new task ID can be generated.";
        return false;
    }
    candidate.push_back(newTask);
    if (!saveTasks(candidate, error)) return false;

    tasks_ = std::move(candidate);
    created = std::move(newTask);
    return true;
}

bool TaskManager::update(const std::string& id, const Task& replacement,
    std::string& error)
{
    if (!validateTask(replacement, error)) return false;
    std::vector<Task> candidate = tasks_;
    const auto found = std::find_if(candidate.begin(), candidate.end(),
        [&id](const Task& task) { return task.id == id; });
    if (found == candidate.end()) {
        error = "Task ID not found: " + id;
        return false;
    }
    found->name = replacement.name;
    found->description = replacement.description;
    found->priority = replacement.priority;
    found->durationMinutes = replacement.durationMinutes;
    found->deadline = replacement.deadline;
    found->type = replacement.type;
    found->status = replacement.status;
    if (!saveTasks(candidate, error)) return false;
    tasks_ = std::move(candidate);
    error.clear();
    return true;
}

bool TaskManager::erase(const std::string& id, std::string& error)
{
    std::vector<Task> candidate = tasks_;
    const auto found = std::find_if(candidate.begin(), candidate.end(),
        [&id](const Task& task) { return task.id == id; });
    if (found == candidate.end()) {
        error = "Task ID not found: " + id;
        return false;
    }
    candidate.erase(found);
    if (!saveTasks(candidate, error)) return false;
    tasks_ = std::move(candidate);
    error.clear();
    return true;
}

const std::vector<Task>& TaskManager::all() const
{
    return tasks_;
}

std::vector<Task> TaskManager::search(const std::string& query) const
{
    std::vector<Task> matches;
    const std::string needle = lower(query);
    for (const Task& task : tasks_) {
        if (lower(task.name).find(needle) != std::string::npos
            || lower(task.description).find(needle) != std::string::npos) {
            matches.push_back(task);
        }
    }
    return matches;
}

std::vector<Task> TaskManager::filter(const TaskFilter& criteria) const
{
    std::vector<Task> matches;
    const std::string type = lower(criteria.type);
    for (const Task& task : tasks_) {
        if (criteria.priority && task.priority != *criteria.priority) continue;
        if (criteria.status && task.status != *criteria.status) continue;
        if (!type.empty() && lower(task.type) != type) continue;
        matches.push_back(task);
    }
    return matches;
}

std::vector<Task> TaskManager::sorted(SortField field) const
{
    std::vector<Task> result = tasks_;
    std::stable_sort(result.begin(), result.end(),
        [field](const Task& left, const Task& right) {
            if (lessBy(left, right, field)) return true;
            if (lessBy(right, left, field)) return false;
            return left.id < right.id;
        });
    return result;
}

bool TaskManager::saveTasks(const std::vector<Task>& tasks,
    std::string& error) const
{
    if (!hasUniqueIds(tasks, error)) return false;

    std::error_code fileError;
    if (!storagePath_.parent_path().empty()) {
        std::filesystem::create_directories(storagePath_.parent_path(), fileError);
        if (fileError) {
            error = "Cannot create task data directory: " + fileError.message();
            return false;
        }
    }

    std::filesystem::path temporaryPath = storagePath_;
    temporaryPath += ".tmp";
    std::filesystem::path backupPath = storagePath_;
    backupPath += ".bak";

    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) {
        error = "Cannot open temporary task file for writing: "
            + temporaryPath.string();
        return false;
    }
    output << fileHeader << '\n';
    for (const Task& task : tasks) {
        writeField(output, task.id); output << ',';
        writeField(output, task.name); output << ',';
        writeField(output, task.description); output << ',';
        writeField(output, toString(task.priority)); output << ',';
        output << task.durationMinutes << ',';
        writeField(output, task.deadline); output << ',';
        writeField(output, task.type); output << ',';
        writeField(output, toString(task.status)); output << '\n';
    }
    output.flush();
    if (!output) {
        output.close();
        std::filesystem::remove(temporaryPath, fileError);
        error = "Failed while writing temporary task file: "
            + temporaryPath.string();
        return false;
    }
    output.close();
    if (!output) {
        std::filesystem::remove(temporaryPath, fileError);
        error = "Failed to close temporary task file: "
            + temporaryPath.string();
        return false;
    }

    fileError.clear();
    const bool targetExists = std::filesystem::exists(storagePath_, fileError);
    if (fileError) {
        std::filesystem::remove(temporaryPath, fileError);
        error = "Cannot inspect existing task file: " + fileError.message();
        return false;
    }
    bool backupCreated = false;
    if (targetExists) {
        if (std::filesystem::exists(backupPath, fileError) || fileError) {
            std::filesystem::remove(temporaryPath, fileError);
            error = "Cannot safely save tasks because a backup already exists: "
                + backupPath.string();
            return false;
        }
        std::filesystem::rename(storagePath_, backupPath, fileError);
        if (fileError) {
            std::filesystem::remove(temporaryPath);
            error = "Cannot preserve the previous task file: " + fileError.message();
            return false;
        }
        backupCreated = true;
    }

    std::filesystem::rename(temporaryPath, storagePath_, fileError);
    if (fileError) {
        const std::string saveError = fileError.message();
        if (backupCreated) {
            std::error_code restoreError;
            std::filesystem::rename(backupPath, storagePath_, restoreError);
            if (restoreError) {
                error = "Saving failed (" + saveError
                    + "); restoring the previous task file also failed ("
                    + restoreError.message() + "). Backup preserved at "
                    + backupPath.string();
                return false;
            }
        }
        std::filesystem::remove(temporaryPath);
        error = "Cannot install the new task file: " + saveError;
        return false;
    }

    if (backupCreated) {
        fileError.clear();
        std::filesystem::remove(backupPath, fileError);
        if (fileError) {
            error = "Tasks were saved, but the previous-file backup could not be "
                "removed: " + backupPath.string() + " (" + fileError.message() + ")";
            return true;
        }
    }
    error.clear();
    return true;
}

std::string TaskManager::generateId(const std::vector<Task>& tasks)
{
    while (nextId_ != 0) {
        std::ostringstream id;
        id << "TSK-" << std::setw(6) << std::setfill('0') << nextId_;
        const bool duplicate = std::any_of(tasks.begin(), tasks.end(),
            [&id](const Task& task) { return task.id == id.str(); });
        if (!duplicate) {
            const std::string result = id.str();
            if (nextId_ == std::numeric_limits<unsigned long long>::max()) {
                nextId_ = 0;
            } else {
                ++nextId_;
            }
            return result;
        }
        if (nextId_ == std::numeric_limits<unsigned long long>::max()) {
            nextId_ = 0;
        } else {
            ++nextId_;
        }
    }
    return {};
}

int runSector01Demo(const std::filesystem::path& storagePath)
{
    TaskManager manager(storagePath);
    std::string error;
    std::cout << "=== Sector 01: Automated Task Management Demo ===\n"
              << "Storage file: " << storagePath.string() << '\n'
              << "\n=== Operation: Load demo data ===\n";
    if (!manager.load(error)) {
        std::cerr << "Result: FAILED - " << error << '\n';
        return 1;
    }
    std::cout << "Input: dedicated demo storage file\n"
              << "Result: loaded " << manager.all().size() << " existing task(s).\n";

    if (!manager.all().empty()) {
        std::cout << "\n=== Operation: Reset previous demo tasks ===\n"
                  << "Input: delete the " << manager.all().size()
                  << " task(s) in the dedicated demo file\n";
        const std::vector<Task> previousTasks = manager.all();
        for (const Task& task : previousTasks) {
            std::cout << "  Delete ID: " << task.id << '\n';
            if (!manager.erase(task.id, error)) {
                std::cerr << "Result: FAILED - " << error << '\n';
                return 1;
            }
        }
        std::cout << "Result: demo file is ready for a fresh run.\n";
    }

    Task first{"", "Prepare TaskFlow release", "Review the Sector 01 task module",
        Priority::High, 120, "2099-12-31", "Development", Status::Pending};
    Task second{"", "Write release notes", "Document the task-management workflow",
        Priority::Medium, 45, "2099-12-30", "Documentation", Status::Ready};
    Task createdFirst;
    Task createdSecond;
    std::cout << "\n=== Operation: Create task ===\n"
              << "Input:\n";
    printTaskInput(first);
    if (!manager.create(first, createdFirst, error)) {
        std::cerr << "Result: FAILED - " << error << '\n';
        return 1;
    }
    std::cout << "Result: created task " << createdFirst.id << ".\n";

    std::cout << "\n=== Operation: Create second task ===\n"
              << "Input:\n";
    printTaskInput(second);
    if (!manager.create(second, createdSecond, error)) {
        std::cerr << "Result: FAILED - " << error << '\n';
        return 1;
    }
    std::cout << "Result: created task " << createdSecond.id << ".\n";

    std::cout << "\n=== Operation: View all tasks ===\n"
              << "Input: show every stored task\n";
    printTasks(manager.all());

    Task edited = first;
    edited.name = "Prepare TaskFlow Sector 01 release";
    edited.status = Status::InProgress;
    std::cout << "\n=== Operation: Edit task ===\n"
              << "Input task ID: " << createdFirst.id << '\n'
              << "Replacement values:\n";
    printTaskInput(edited);
    if (!manager.update(createdFirst.id, edited, error)) {
        std::cerr << "Result: FAILED - " << error << '\n';
        return 1;
    }
    std::cout << "Result: task updated.\n";

    std::cout << "\n=== Operation: Search tasks ===\n"
              << "Input query: Sector 01\n";
    const std::vector<Task> searchResults = manager.search("Sector 01");
    std::cout << "Result: found " << searchResults.size() << " task(s).\n";
    printTasks(searchResults);

    std::cout << "\n=== Operation: Filter tasks ===\n"
              << "Input: priority=High, status=any, type=any\n";
    const std::vector<Task> filtered =
        manager.filter(TaskFilter{Priority::High, std::nullopt, {}});
    std::cout << "Result: found " << filtered.size() << " task(s).\n";
    printTasks(filtered);

    std::cout << "\n=== Operation: Sort tasks ===\n"
              << "Input: deadline ascending\n";
    const std::vector<Task> ordered = manager.sorted(SortField::Deadline);
    std::cout << "Result: " << ordered.size() << " task(s), earliest deadline first.\n";
    printTasks(ordered);

    Task invalid = first;
    invalid.name = "Invalid demo task";
    invalid.deadline = "2027-02-30";
    Task rejected;
    std::cout << "\n=== Operation: Reject invalid task (failure case) ===\n"
              << "Input:\n";
    printTaskInput(invalid);
    if (manager.create(invalid, rejected, error)) {
        std::cerr << "Result: UNEXPECTEDLY ACCEPTED invalid input.\n";
        return 1;
    }
    std::cout << "Result: rejected without changing the task list: "
              << error << '\n';

    std::cout << "\n=== Operation: Delete task ===\n"
              << "Input task ID: " << createdSecond.id << '\n';
    if (!manager.erase(createdSecond.id, error)) {
        std::cerr << "Result: FAILED - " << error << '\n';
        return 1;
    }
    std::cout << "Result: deleted task " << createdSecond.id << ".\n";

    std::cout << "\n=== Operation: Save tasks ===\n"
              << "Input file: " << storagePath.string() << '\n';
    if (!manager.save(error)) {
        std::cerr << "Result: FAILED - " << error << '\n';
        return 1;
    }
    std::cout << "Result: saved " << manager.all().size() << " task(s).\n";
    if (!error.empty()) std::cout << "Warning: " << error << '\n';

    TaskManager reloaded(storagePath);
    if (!reloaded.load(error)) {
        std::cerr << "Persistence check: FAILED - " << error << '\n';
        return 1;
    }
    std::cout << "\n=== Final task list (loaded from disk) ===\n"
              << "Persistence result: reloaded " << reloaded.all().size()
              << " task(s) from " << storagePath.string() << ".\n";
    printTasks(reloaded.all());
    return 0;
}

int runInteractiveTaskManager(const std::filesystem::path& storagePath,
    const std::filesystem::path& availabilityPath,
    const std::filesystem::path& dependencyPath)
{
    TaskManager manager(storagePath);
    std::string error;
    if (!manager.load(error)) {
        std::cerr << "Could not load tasks: " << error << '\n';
        return 1;
    }

    AvailabilityManager availabilityManager(availabilityPath);
    if (!availabilityManager.load(error)) {
        std::cerr << "Could not load availability: " << error << '\n';
        return 1;
    }

    DependencyManager dependencyManager(dependencyPath);
    if (!dependencyManager.load(error)) {
        std::cerr << "Could not load dependency data: " << error << '\n';
        return 1;
    }
    if (!dependencyManager.validateGraph(manager.all(), error)) {
        std::cerr << "Dependency graph validation warning: " << error << '\n'
                  << "Dependency graph reports will be disabled until the records are repaired.\n";
    }

    std::cout << "Loaded " << manager.all().size() << " task(s), "
              << availabilityManager.all().size()
              << " availability window(s), and "
              << dependencyManager.all().size()
              << " dependency record(s).\n";
    for (;;) {
        std::cout << "\nTaskFlow - Task and Availability Management\n"
                  << "1. Create task\n2. Edit task\n3. Delete task\n"
                  << "4. View all tasks\n5. Search tasks\n6. Filter tasks\n"
                  << "7. Sort tasks\n8. Save tasks now\n"
                  << "9. Availability Management\n"
                  << "10. Dependency Management\n"
                  << "11. Scheduling Strategies\n0. Save and exit\nChoice: ";
        std::string choice;
        if (!std::getline(std::cin, choice)) return 0;

        if (choice == "0") {
            std::cout << "\n=== Save tasks and exit ===\n";
            if (!manager.save(error)) {
                std::cerr << "Could not save tasks: " << error << '\n';
                return 1;
            }
            if (!dependencyManager.save(error)) {
                std::cerr << "Could not save dependencies: " << error << '\n';
                return 1;
            }
            if (!error.empty()) std::cerr << "Warning: " << error << '\n';
            std::cout << "Tasks and dependencies saved.\n";
            return 0;
        }
        if (choice == "1") {
            std::cout << "\n=== Create task ===\n";
            Task task;
            if (!readTaskDetails(task)) return 0;
            Task created;
            if (manager.create(task, created, error)) {
                std::cout << "Created " << created.id << ".\n";
                if (!error.empty()) std::cerr << "Warning: " << error << '\n';
            } else {
                std::cerr << error << '\n';
            }
        } else if (choice == "2") {
            std::cout << "\n=== Edit task ===\n";
            std::string id;
            if (!readLine("Task ID to edit: ", id)) return 0;
            Task replacement;
            if (!readTaskDetails(replacement)) return 0;
            if (manager.update(id, replacement, error)) {
                std::cout << "Updated " << id << ".\n";
                if (!error.empty()) std::cerr << "Warning: " << error << '\n';
            } else {
                std::cerr << error << '\n';
            }
        } else if (choice == "3") {
            std::cout << "\n=== Delete task ===\n";
            std::string id;
            if (!readLine("Task ID to delete: ", id)) return 0;
            if (dependencyManager.taskHasReferences(id)) {
                std::cout << "Cannot delete task " << id
                          << " because it is referenced by dependency records:\n";
                for (const Dependency& dependency : dependencyManager.all()) {
                    if (dependency.dependentId == id) {
                        std::cout << "  " << id << " depends on "
                                  << dependency.prerequisiteId << '\n';
                    }
                    if (dependency.prerequisiteId == id) {
                        std::cout << "  " << dependency.dependentId
                                  << " depends on " << id << '\n';
                    }
                }
                std::cout << "Remove all dependency records referencing this task first.\n";
                continue;
            }
            std::string confirmation;
            if (!readLine("Delete this task permanently? (yes/no): ",
                    confirmation)) return 0;
            if (lower(confirmation) != "yes") {
                std::cout << "Deletion cancelled; task data was not changed.\n";
                continue;
            }
            if (manager.erase(id, error)) {
                std::cout << "Deleted " << id << ".\n";
                if (!error.empty()) std::cerr << "Warning: " << error << '\n';
            } else {
                std::cerr << error << '\n';
            }
        } else if (choice == "4") {
            std::cout << "\n=== View all tasks ===\n";
            printTasks(manager.all());
        } else if (choice == "5") {
            std::cout << "\n=== Search tasks ===\n";
            std::string query;
            if (!readLine("Search name and description: ", query)) return 0;
            printTasks(manager.search(query));
        } else if (choice == "6") {
            std::cout << "\n=== Filter tasks ===\n";
            TaskFilter filter;
            std::string input;
            if (!readLine("Priority (blank for any): ", input)) return 0;
            if (!input.empty()) {
                Priority priority;
                if (!parsePriority(input, priority)) {
                    std::cerr << "Invalid priority.\n";
                    continue;
                }
                filter.priority = priority;
            }
            if (!readLine("Status (blank for any): ", input)) return 0;
            if (!input.empty()) {
                Status status;
                if (!parseStatus(input, status)) {
                    std::cerr << "Invalid status.\n";
                    continue;
                }
                filter.status = status;
            }
            if (!readLine("Type (blank for any): ", filter.type)) return 0;
            printTasks(manager.filter(filter));
        } else if (choice == "7") {
            std::cout << "\n=== Sort tasks ===\n";
            std::cout << "Sort by: 1 ID, 2 name, 3 priority, 4 duration, "
                         "5 deadline, 6 status: ";
            if (!std::getline(std::cin, choice)) return 0;
            const SortField fields[] = {SortField::Id, SortField::Name,
                SortField::Priority, SortField::Duration, SortField::Deadline,
                SortField::Status};
            if (choice.size() != 1 || choice[0] < '1' || choice[0] > '6') {
                std::cerr << "Invalid sort field.\n";
                continue;
            }
            printTasks(manager.sorted(fields[choice[0] - '1']));
        } else if (choice == "8") {
            std::cout << "\n=== Save tasks ===\n";
            if (!manager.save(error)) {
                std::cerr << "Could not save tasks: " << error << '\n';
            } else {
                std::cout << "Saved " << manager.all().size() << " task(s).\n";
                if (!error.empty()) std::cerr << "Warning: " << error << '\n';
            }
        } else if (choice == "9") {
            if (runAvailabilityMenu(availabilityManager) != 0) return 1;
        } else if (choice == "10") {
            if (runDependencyMenu(manager, dependencyManager) != 0) return 1;
        } else if (choice == "11") {
            if (runSchedulingMenu(manager.all(), availabilityManager,
                    dependencyManager) != 0) return 1;
        } else {
            std::cerr << "Invalid menu choice.\n";
        }
    }
}

} // namespace taskflow

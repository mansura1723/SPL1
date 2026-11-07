#ifndef TASKFLOW_AVAILABILITY_MANAGEMENT_HPP
#define TASKFLOW_AVAILABILITY_MANAGEMENT_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace taskflow {

enum class DayOfWeek {
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
    Sunday
};

struct AvailabilityInput {
    DayOfWeek day = DayOfWeek::Monday;
    int startMinute = 0;
    int endMinute = 0;
    std::string label;
};

struct AvailabilityWindow {
    std::string id;
    DayOfWeek day = DayOfWeek::Monday;
    int startMinute = 0;
    int endMinute = 0;
    std::string label;
};

bool parseDay(const std::string& text, DayOfWeek& day);
bool parseTime12Hour(const std::string& text, int& minuteOfDay);
std::string formatTime12Hour(int minuteOfDay);
std::string toString(DayOfWeek day);
bool parseQuickAvailability(const std::string& line,
    AvailabilityInput& availability, std::string& error);
bool validateAvailabilityInput(const AvailabilityInput& input,
    std::string& error);

class AvailabilityManager {
public:
    explicit AvailabilityManager(std::filesystem::path storagePath);

    bool load(std::string& error);
    bool save(std::string& error) const;
    bool checkInputs(const std::vector<AvailabilityInput>& inputs,
        std::vector<std::string>& errors) const;
    bool addMany(const std::vector<AvailabilityInput>& inputs,
        std::vector<AvailabilityWindow>& created, std::string& error);
    bool add(const AvailabilityInput& input, AvailabilityWindow& created,
        std::string& error);
    bool update(const std::string& id, const AvailabilityInput& replacement,
        std::string& error);
    bool erase(const std::string& id, std::string& error);
    bool setDefaultWeekly(int startMinute, int endMinute,
        const std::string& label, std::string& error);
    bool resetDay(DayOfWeek day, std::string& error);

    const std::vector<AvailabilityWindow>& all() const;
    std::vector<AvailabilityWindow> byDay(DayOfWeek day) const;
    std::vector<AvailabilityWindow> search(const std::string& query) const;
    int dailyMinutes(DayOfWeek day) const;
    int weeklyMinutes() const;

private:
    bool saveWindows(const std::vector<AvailabilityWindow>& windows,
        std::string& error) const;
    std::string generateId(const std::vector<AvailabilityWindow>& windows);

    std::filesystem::path storagePath_;
    std::vector<AvailabilityWindow> windows_;
    unsigned long long nextId_ = 1;
};

int runAvailabilityTests(const std::filesystem::path& testDirectory);
int runSector02Demo(const std::filesystem::path& storagePath);
int runAvailabilityMenu(AvailabilityManager& manager);

} // namespace taskflow

#endif

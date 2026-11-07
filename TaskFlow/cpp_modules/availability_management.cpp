#include "availability_management.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace taskflow {
namespace {

const char* const fileHeader = "id,day,start,end,label";

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

bool overlaps(const AvailabilityInput& left, const AvailabilityInput& right)
{
    return left.day == right.day
        && left.startMinute < right.endMinute
        && right.startMinute < left.endMinute;
}

bool overlaps(const AvailabilityInput& input,
    const AvailabilityWindow& window)
{
    return input.day == window.day
        && input.startMinute < window.endMinute
        && window.startMinute < input.endMinute;
}

bool sameWindow(const AvailabilityInput& left, const AvailabilityInput& right)
{
    return left.day == right.day
        && left.startMinute == right.startMinute
        && left.endMinute == right.endMinute;
}

bool sameWindow(const AvailabilityInput& input,
    const AvailabilityWindow& window)
{
    return input.day == window.day
        && input.startMinute == window.startMinute
        && input.endMinute == window.endMinute;
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

bool parseAvailabilityId(const std::string& id, unsigned long long& number)
{
    if (id.size() < 4 || id.compare(0, 3, "AV-") != 0) return false;
    const std::string digits = id.substr(3);
    if (digits.empty()
        || !std::all_of(digits.begin(), digits.end(),
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

std::string describeConflict(const AvailabilityInput& input,
    const AvailabilityWindow& other)
{
    if (sameWindow(input, other)) {
        return "Duplicate availability window " + other.id + " on "
            + toString(input.day) + ".";
    }
    return "Overlaps availability window " + other.id + " on "
        + toString(input.day) + " ("
        + formatTime12Hour(other.startMinute) + "-"
        + formatTime12Hour(other.endMinute) + ").";
}

int dayRank(DayOfWeek day)
{
    return static_cast<int>(day);
}

void printWindow(const AvailabilityWindow& window)
{
    std::cout << window.id << " | " << toString(window.day) << " | "
              << formatTime12Hour(window.startMinute) << " - "
              << formatTime12Hour(window.endMinute) << " | "
              << (window.label.empty() ? "(no label)" : window.label) << '\n';
}

void printWindows(const std::vector<AvailabilityWindow>& windows)
{
    if (windows.empty()) {
        std::cout << "No availability windows found.\n";
        return;
    }
    std::cout << "ID | Day | Start - End | Label\n";
    for (const AvailabilityWindow& window : windows) printWindow(window);
}

bool promptLine(const std::string& prompt, std::string& value)
{
    std::cout << prompt;
    return static_cast<bool>(std::getline(std::cin, value));
}

bool promptDay(DayOfWeek& day)
{
    std::string input;
    for (;;) {
        if (!promptLine("Day (MON or Monday): ", input)) return false;
        if (parseDay(input, day)) return true;
        std::cout << "Invalid day. Enter a weekday such as MON or Monday.\n";
    }
}

bool promptTime(const std::string& field, int& minutes)
{
    const std::string example = field == "End Time" ? "1:00 PM" : "9:00 AM";
    std::string input;
    for (;;) {
        if (!promptLine(field + " (for example " + example + "): ", input))
            return false;
        if (parseTime12Hour(input, minutes)) return true;
        std::cout << "Invalid time. Use 12-hour format, for example 9:00 AM "
                     "or 2:30 PM.\n";
    }
}

bool promptGuidedInput(AvailabilityInput& input)
{
    if (!promptDay(input.day)) return false;
    if (!promptTime("Start Time", input.startMinute)) return false;
    if (!promptTime("End Time", input.endMinute)) return false;
    for (;;) {
        if (!promptLine("Label (example Morning Study; optional, blank allowed): ",
                input.label)) return false;
        input.label = trim(input.label);
        if (input.label.size() <= 120 && !hasLineBreak(input.label)) break;
        std::cout << "Label must be at most 120 characters on one line.\n";
    }
    std::string error;
    while (!validateAvailabilityInput(input, error)) {
        std::cout << "Invalid availability: " << error << '\n';
        if (!promptTime("Start Time", input.startMinute)) return false;
        if (!promptTime("End Time", input.endMinute)) return false;
    }
    return true;
}

void printInputPreview(const AvailabilityInput& input)
{
    std::cout << "Day: " << toString(input.day) << '\n'
              << "Start: " << formatTime12Hour(input.startMinute) << '\n'
              << "End: " << formatTime12Hour(input.endMinute) << '\n'
              << "Label: " << (input.label.empty() ? "(no label)" : input.label)
              << '\n';
}

bool confirmed(const std::string& prompt)
{
    std::string answer;
    if (!promptLine(prompt + " (Y/N): ", answer)) return false;
    const std::string value = lower(trim(answer));
    return value == "y" || value == "yes";
}

std::vector<AvailabilityWindow> sortedWindows(
    const std::vector<AvailabilityWindow>& windows)
{
    std::vector<AvailabilityWindow> result = windows;
    std::stable_sort(result.begin(), result.end(),
        [](const AvailabilityWindow& left, const AvailabilityWindow& right) {
            if (dayRank(left.day) != dayRank(right.day))
                return dayRank(left.day) < dayRank(right.day);
            if (left.startMinute != right.startMinute)
                return left.startMinute < right.startMinute;
            return left.id < right.id;
        });
    return result;
}

} // namespace

bool parseDay(const std::string& text, DayOfWeek& day)
{
    const std::string value = lower(trim(text));
    const char* const names[] = {"monday", "tuesday", "wednesday",
        "thursday", "friday", "saturday", "sunday"};
    const char* const abbreviations[] = {"mon", "tue", "wed", "thu",
        "fri", "sat", "sun"};
    for (int index = 0; index < 7; ++index) {
        if (value == names[index] || value == abbreviations[index]) {
            day = static_cast<DayOfWeek>(index);
            return true;
        }
    }
    return false;
}

bool parseTime12Hour(const std::string& text, int& minuteOfDay)
{
    const std::string value = trim(text);
    const std::size_t colon = value.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    if (value.size() < colon + 6) return false;
    const std::string hourText = value.substr(0, colon);
    const std::string minuteText = value.substr(colon + 1, 2);
    if (hourText.size() > 2 || minuteText.size() != 2
        || !std::all_of(hourText.begin(), hourText.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; })
        || !std::all_of(minuteText.begin(), minuteText.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return false;
    }
    if (colon + 3 >= value.size() || value[colon + 3] != ' ') return false;
    const std::string suffix = lower(trim(value.substr(colon + 4)));
    if (suffix != "am" && suffix != "pm") return false;

    const int hour = std::stoi(hourText);
    const int minute = std::stoi(minuteText);
    if (hour < 1 || hour > 12 || minute < 0 || minute > 59) return false;
    const int hour24 = (hour % 12) + (suffix == "pm" ? 12 : 0);
    minuteOfDay = hour24 * 60 + minute;
    return true;
}

std::string formatTime12Hour(int minuteOfDay)
{
    if (minuteOfDay < 0 || minuteOfDay >= 24 * 60) return "Invalid time";
    const int hour24 = minuteOfDay / 60;
    const int hour12 = hour24 % 12 == 0 ? 12 : hour24 % 12;
    const int minute = minuteOfDay % 60;
    std::ostringstream result;
    result << hour12 << ':' << std::setw(2) << std::setfill('0') << minute
           << (hour24 < 12 ? " AM" : " PM");
    return result.str();
}

std::string toString(DayOfWeek day)
{
    switch (day) {
    case DayOfWeek::Monday: return "Monday";
    case DayOfWeek::Tuesday: return "Tuesday";
    case DayOfWeek::Wednesday: return "Wednesday";
    case DayOfWeek::Thursday: return "Thursday";
    case DayOfWeek::Friday: return "Friday";
    case DayOfWeek::Saturday: return "Saturday";
    case DayOfWeek::Sunday: return "Sunday";
    }
    return "Invalid day";
}

bool parseQuickAvailability(const std::string& line,
    AvailabilityInput& availability, std::string& error)
{
    if (trim(line).empty()) {
        error = "Input is empty.";
        return false;
    }
    const std::size_t first = line.find('|');
    const std::size_t second = first == std::string::npos
        ? std::string::npos : line.find('|', first + 1);
    const std::size_t third = second == std::string::npos
        ? std::string::npos : line.find('|', second + 1);
    if (first == std::string::npos || second == std::string::npos
        || third == std::string::npos
        || line.find('|', third + 1) != std::string::npos) {
        error = "Expected DAY|START TIME|END TIME|LABEL.";
        return false;
    }

    const std::string dayText = trim(line.substr(0, first));
    const std::string startText = trim(line.substr(first + 1, second - first - 1));
    const std::string endText = trim(line.substr(second + 1, third - second - 1));
    availability.label = trim(line.substr(third + 1));
    if (!parseDay(dayText, availability.day)) {
        error = "Invalid day '" + dayText + "'. Use MON through SUN or full day names.";
        return false;
    }
    if (!parseTime12Hour(startText, availability.startMinute)) {
        error = "Invalid start time '" + startText
            + "'. Use 12-hour time such as 9:00 AM.";
        return false;
    }
    if (!parseTime12Hour(endText, availability.endMinute)) {
        error = "Invalid end time '" + endText
            + "'. Use 12-hour time such as 1:00 PM.";
        return false;
    }
    return validateAvailabilityInput(availability, error);
}

bool validateAvailabilityInput(const AvailabilityInput& input,
    std::string& error)
{
    const int day = dayRank(input.day);
    if (day < 0 || day > 6) {
        error = "Day of week is invalid.";
        return false;
    }
    if (input.startMinute < 0 || input.startMinute >= 24 * 60
        || input.endMinute < 0 || input.endMinute >= 24 * 60) {
        error = "Start and end times must be valid times within one day.";
        return false;
    }
    if (input.startMinute == input.endMinute) {
        error = "Start time and end time must be different.";
        return false;
    }
    if (input.startMinute > input.endMinute) {
        error = "Start time must be earlier than end time; overnight windows "
                "are not supported.";
        return false;
    }
    if (input.label.size() > 120 || hasLineBreak(input.label)) {
        error = "Label must be at most 120 characters on one line.";
        return false;
    }
    error.clear();
    return true;
}

AvailabilityManager::AvailabilityManager(std::filesystem::path storagePath)
    : storagePath_(std::move(storagePath))
{
}

bool AvailabilityManager::load(std::string& error)
{
    std::error_code fileError;
    const bool exists = std::filesystem::exists(storagePath_, fileError);
    if (fileError) {
        error = "Cannot inspect availability file: " + fileError.message();
        return false;
    }
    if (!exists) {
        windows_.clear();
        nextId_ = 1;
        error.clear();
        return true;
    }
    std::ifstream input(storagePath_);
    if (!input) {
        error = "Cannot open availability file for reading: "
            + storagePath_.string();
        return false;
    }
    std::string line;
    if (!std::getline(input, line)) {
        error = "Availability file is empty or unreadable.";
        return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != fileHeader) {
        error = "Availability file has an invalid or unsupported header.";
        return false;
    }

    std::vector<AvailabilityWindow> loaded;
    unsigned long long highestId = 0;
    std::size_t lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        if (!parseRecord(line, fields) || fields.size() != 5) {
            error = "Availability file line " + std::to_string(lineNumber)
                + " is malformed; expected 5 CSV fields.";
            return false;
        }
        DayOfWeek day;
        int start = 0;
        int end = 0;
        unsigned long long idNumber = 0;
        if (!parseAvailabilityId(fields[0], idNumber)
            || !parseDay(fields[1], day)
            || !parseTime12Hour(fields[2], start)
            || !parseTime12Hour(fields[3], end)) {
            error = "Availability file line " + std::to_string(lineNumber)
                + " contains an invalid ID, day, or time.";
            return false;
        }
        AvailabilityInput candidate{day, start, end, fields[4]};
        if (!validateAvailabilityInput(candidate, error)) {
            error = "Availability file line " + std::to_string(lineNumber)
                + ": " + error;
            return false;
        }
        for (const AvailabilityWindow& existing : loaded) {
            if (existing.id == fields[0]) {
                error = "Duplicate availability ID in file: " + fields[0];
                return false;
            }
            if (sameWindow(candidate, existing)) {
                error = "Duplicate availability entry in file on "
                    + toString(day) + ".";
                return false;
            }
            if (overlaps(candidate, existing)) {
                error = "Overlapping availability entries in file on "
                    + toString(day) + ".";
                return false;
            }
        }
        highestId = std::max(highestId, idNumber);
        loaded.push_back(AvailabilityWindow{
            fields[0], day, start, end, fields[4]});
    }
    if (input.bad()) {
        error = "An I/O error occurred while reading availability.";
        return false;
    }
    if (highestId == std::numeric_limits<unsigned long long>::max()) {
        error = "Availability IDs are exhausted.";
        return false;
    }
    windows_ = std::move(loaded);
    nextId_ = highestId + 1;
    error.clear();
    return true;
}

bool AvailabilityManager::save(std::string& error) const
{
    return saveWindows(windows_, error);
}

bool AvailabilityManager::checkInputs(
    const std::vector<AvailabilityInput>& inputs,
    std::vector<std::string>& errors) const
{
    errors.assign(inputs.size(), {});
    std::vector<AvailabilityInput> accepted;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const AvailabilityInput& input = inputs[index];
        if (!validateAvailabilityInput(input, errors[index])) continue;
        for (const AvailabilityWindow& existing : windows_) {
            if (sameWindow(input, existing)) {
                errors[index] = describeConflict(input, existing);
                break;
            }
            if (overlaps(input, existing)) {
                errors[index] = describeConflict(input, existing);
                break;
            }
        }
        if (!errors[index].empty()) continue;
        for (const AvailabilityInput& earlier : accepted) {
            if (sameWindow(input, earlier)) {
                errors[index] = "Duplicate availability entry in this batch.";
                break;
            }
            if (overlaps(input, earlier)) {
                errors[index] = "Overlaps another availability window in this batch.";
                break;
            }
        }
        if (errors[index].empty()) accepted.push_back(input);
    }
    return std::all_of(errors.begin(), errors.end(),
        [](const std::string& value) { return value.empty(); });
}

bool AvailabilityManager::addMany(
    const std::vector<AvailabilityInput>& inputs,
    std::vector<AvailabilityWindow>& created, std::string& error)
{
    if (inputs.empty()) {
        error = "No availability records were provided.";
        return false;
    }
    std::vector<std::string> errors;
    if (!checkInputs(inputs, errors)) {
        std::ostringstream details;
        for (std::size_t index = 0; index < errors.size(); ++index) {
            if (!errors[index].empty()) {
                if (details.tellp() > 0) details << ' ';
                details << "Record " << index + 1 << ": " << errors[index];
            }
        }
        error = details.str();
        return false;
    }

    std::vector<AvailabilityWindow> candidate = windows_;
    std::vector<AvailabilityWindow> newWindows;
    for (const AvailabilityInput& input : inputs) {
        const std::string id = generateId(candidate);
        if (id.empty()) {
            error = "Availability IDs are exhausted.";
            return false;
        }
        AvailabilityWindow window{id, input.day, input.startMinute,
            input.endMinute, input.label};
        candidate.push_back(window);
        newWindows.push_back(std::move(window));
    }
    if (!saveWindows(candidate, error)) return false;
    windows_ = std::move(candidate);
    created = std::move(newWindows);
    return true;
}

bool AvailabilityManager::add(const AvailabilityInput& input,
    AvailabilityWindow& created, std::string& error)
{
    std::vector<AvailabilityWindow> result;
    if (!addMany({input}, result, error)) return false;
    created = result.front();
    return true;
}

bool AvailabilityManager::update(const std::string& id,
    const AvailabilityInput& replacement, std::string& error)
{
    if (!validateAvailabilityInput(replacement, error)) return false;
    const auto found = std::find_if(windows_.begin(), windows_.end(),
        [&id](const AvailabilityWindow& window) { return window.id == id; });
    if (found == windows_.end()) {
        error = "Availability ID not found: " + id;
        return false;
    }
    std::vector<AvailabilityWindow> candidate = windows_;
    auto replacementWindow = std::find_if(candidate.begin(), candidate.end(),
        [&id](const AvailabilityWindow& window) { return window.id == id; });
    replacementWindow->day = replacement.day;
    replacementWindow->startMinute = replacement.startMinute;
    replacementWindow->endMinute = replacement.endMinute;
    replacementWindow->label = replacement.label;

    for (auto current = candidate.begin(); current != candidate.end(); ++current) {
        if (current->id != id) {
            AvailabilityInput input{replacementWindow->day,
                replacementWindow->startMinute, replacementWindow->endMinute,
                replacementWindow->label};
            if (sameWindow(input, *current) || overlaps(input, *current)) {
                error = describeConflict(input, *current);
                return false;
            }
        }
    }
    if (!saveWindows(candidate, error)) return false;
    windows_ = std::move(candidate);
    return true;
}

bool AvailabilityManager::erase(const std::string& id, std::string& error)
{
    std::vector<AvailabilityWindow> candidate = windows_;
    const auto found = std::find_if(candidate.begin(), candidate.end(),
        [&id](const AvailabilityWindow& window) { return window.id == id; });
    if (found == candidate.end()) {
        error = "Availability ID not found: " + id;
        return false;
    }
    candidate.erase(found);
    if (!saveWindows(candidate, error)) return false;
    windows_ = std::move(candidate);
    return true;
}

bool AvailabilityManager::setDefaultWeekly(int startMinute, int endMinute,
    const std::string& label, std::string& error)
{
    std::vector<AvailabilityInput> inputs;
    for (int index = 0; index < 7; ++index) {
        inputs.push_back(AvailabilityInput{
            static_cast<DayOfWeek>(index), startMinute, endMinute, label});
    }
    for (const AvailabilityInput& input : inputs) {
        if (!validateAvailabilityInput(input, error)) return false;
    }
    std::vector<AvailabilityWindow> candidate;
    for (const AvailabilityInput& input : inputs) {
        const std::string id = generateId(candidate);
        if (id.empty()) {
            error = "Availability IDs are exhausted.";
            return false;
        }
        candidate.push_back(AvailabilityWindow{id, input.day, input.startMinute,
            input.endMinute, input.label});
    }
    if (!saveWindows(candidate, error)) return false;
    windows_ = std::move(candidate);
    return true;
}

bool AvailabilityManager::resetDay(DayOfWeek day, std::string& error)
{
    std::vector<AvailabilityWindow> candidate = windows_;
    candidate.erase(std::remove_if(candidate.begin(), candidate.end(),
        [day](const AvailabilityWindow& window) { return window.day == day; }),
        candidate.end());
    if (candidate.size() == windows_.size()) {
        error.clear();
        return true;
    }
    if (!saveWindows(candidate, error)) return false;
    windows_ = std::move(candidate);
    error.clear();
    return true;
}

const std::vector<AvailabilityWindow>& AvailabilityManager::all() const
{
    return windows_;
}

std::vector<AvailabilityWindow> AvailabilityManager::byDay(DayOfWeek day) const
{
    std::vector<AvailabilityWindow> result;
    for (const AvailabilityWindow& window : windows_) {
        if (window.day == day) result.push_back(window);
    }
    return sortedWindows(result);
}

std::vector<AvailabilityWindow> AvailabilityManager::search(
    const std::string& query) const
{
    const std::string needle = lower(trim(query));
    std::vector<AvailabilityWindow> result;
    if (needle.empty()) return result;
    for (const AvailabilityWindow& window : windows_) {
        if (lower(window.id).find(needle) != std::string::npos
            || lower(toString(window.day)).find(needle) != std::string::npos
            || lower(window.label).find(needle) != std::string::npos
            || lower(formatTime12Hour(window.startMinute)).find(needle)
                != std::string::npos
            || lower(formatTime12Hour(window.endMinute)).find(needle)
                != std::string::npos) {
            result.push_back(window);
        }
    }
    return sortedWindows(result);
}

int AvailabilityManager::dailyMinutes(DayOfWeek day) const
{
    int total = 0;
    for (const AvailabilityWindow& window : windows_) {
        if (window.day == day) total += window.endMinute - window.startMinute;
    }
    return total;
}

int AvailabilityManager::weeklyMinutes() const
{
    int total = 0;
    for (const AvailabilityWindow& window : windows_)
        total += window.endMinute - window.startMinute;
    return total;
}

bool AvailabilityManager::saveWindows(
    const std::vector<AvailabilityWindow>& windows, std::string& error) const
{
    std::error_code fileError;
    if (!storagePath_.parent_path().empty()) {
        std::filesystem::create_directories(storagePath_.parent_path(), fileError);
        if (fileError) {
            error = "Cannot create availability data directory: "
                + fileError.message();
            return false;
        }
    }
    const std::filesystem::path temporaryPath = storagePath_.string() + ".tmp";
    const std::filesystem::path backupPath = storagePath_.string() + ".bak";
    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) {
        error = "Cannot open temporary availability file for writing: "
            + temporaryPath.string();
        return false;
    }
    output << fileHeader << '\n';
    for (const AvailabilityWindow& window : windows) {
        writeField(output, window.id); output << ',';
        writeField(output, toString(window.day)); output << ',';
        writeField(output, formatTime12Hour(window.startMinute)); output << ',';
        writeField(output, formatTime12Hour(window.endMinute)); output << ',';
        writeField(output, window.label); output << '\n';
    }
    output.flush();
    if (!output) {
        output.close();
        std::filesystem::remove(temporaryPath, fileError);
        error = "Failed while writing availability data.";
        return false;
    }
    output.close();
    if (!output) {
        std::filesystem::remove(temporaryPath, fileError);
        error = "Failed to close availability data file.";
        return false;
    }

    fileError.clear();
    const bool targetExists = std::filesystem::exists(storagePath_, fileError);
    if (fileError) {
        std::filesystem::remove(temporaryPath);
        error = "Cannot inspect availability file: " + fileError.message();
        return false;
    }
    bool backupCreated = false;
    if (targetExists) {
        if (std::filesystem::exists(backupPath, fileError) || fileError) {
            std::filesystem::remove(temporaryPath, fileError);
            error = "Cannot safely save; an availability backup already exists: "
                + backupPath.string();
            return false;
        }
        std::filesystem::rename(storagePath_, backupPath, fileError);
        if (fileError) {
            std::filesystem::remove(temporaryPath);
            error = "Cannot preserve the previous availability file: "
                + fileError.message();
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
                error = "Saving availability failed (" + saveError
                    + "); restoring the prior file failed ("
                    + restoreError.message() + "). Backup preserved at "
                    + backupPath.string();
                return false;
            }
        }
        std::filesystem::remove(temporaryPath);
        error = "Cannot install the new availability file: " + saveError;
        return false;
    }
    if (backupCreated) {
        fileError.clear();
        std::filesystem::remove(backupPath, fileError);
        if (fileError) {
            error = "Availability saved, but the prior-file backup remains at "
                + backupPath.string() + ": " + fileError.message();
            return true;
        }
    }
    error.clear();
    return true;
}

std::string AvailabilityManager::generateId(
    const std::vector<AvailabilityWindow>& windows)
{
    while (nextId_ != 0) {
        std::ostringstream result;
        result << "AV-" << std::setw(6) << std::setfill('0') << nextId_;
        const bool duplicate = std::any_of(windows.begin(), windows.end(),
            [&result](const AvailabilityWindow& window) {
                return window.id == result.str();
            });
        if (!duplicate) {
            const std::string id = result.str();
            nextId_ = nextId_ == std::numeric_limits<unsigned long long>::max()
                ? 0 : nextId_ + 1;
            return id;
        }
        nextId_ = nextId_ == std::numeric_limits<unsigned long long>::max()
            ? 0 : nextId_ + 1;
    }
    return {};
}

int runAvailabilityMenu(AvailabilityManager& manager)
{
    for (;;) {
        std::cout << "\nAVAILABILITY MANAGEMENT\n"
                  << "1. Add Availability (Guided Input)\n"
                  << "2. Add Availability (Quick Input)\n"
                  << "3. Add Multiple Availability (Bulk Input)\n"
                  << "4. Edit Availability\n"
                  << "5. Delete Availability\n"
                  << "6. View All Availability\n"
                  << "7. View Availability by Day\n"
                  << "8. Search Availability\n"
                  << "9. Weekly Summary\n"
                  << "10. Reset Day Availability\n"
                  << "11. Set Default Weekly Availability\n"
                  << "12. Save Availability to File\n"
                  << "13. Back to Main Menu\nChoice: ";
        std::string choice;
        if (!std::getline(std::cin, choice)) return 0;
        std::string error;

        if (choice == "1") {
            std::cout << "\n=== Add Availability: Guided Input ===\n"
                      << "Enter one value per prompt; examples show the expected format.\n";
            AvailabilityInput input;
            if (!promptGuidedInput(input)) return 0;
            printInputPreview(input);
            std::vector<std::string> errors;
            if (!manager.checkInputs({input}, errors)) {
                std::cout << "Cannot save: " << errors.front() << '\n';
                continue;
            }
            if (!confirmed("Confirm save?")) {
                std::cout << "Cancelled; availability was not changed.\n";
                continue;
            }
            AvailabilityWindow created;
            if (manager.add(input, created, error)) {
                std::cout << "Saved availability " << created.id << ".\n";
                if (!error.empty()) std::cout << "Warning: " << error << '\n';
            } else {
                std::cout << "Could not save availability: " << error << '\n';
            }
        } else if (choice == "2") {
            std::cout << "\n=== Add Availability: Quick Input ===\n"
                      << "Enter DAY|START TIME|END TIME|LABEL\n"
                      << "Example: MON|9:00 AM|1:00 PM|Morning Study\n";
            std::string line;
            if (!std::getline(std::cin, line)) return 0;
            AvailabilityInput input;
            if (!parseQuickAvailability(line, input, error)) {
                std::cout << "Invalid quick input: " << error << '\n';
                continue;
            }
            std::cout << "\nPreview:\n";
            printInputPreview(input);
            std::vector<std::string> errors;
            if (!manager.checkInputs({input}, errors)) {
                std::cout << "Cannot save: " << errors.front() << '\n';
                continue;
            }
            if (!confirmed("Confirm save?")) {
                std::cout << "Cancelled; availability was not changed.\n";
                continue;
            }
            AvailabilityWindow created;
            if (manager.add(input, created, error)) {
                std::cout << "Saved availability " << created.id << ".\n";
                if (!error.empty()) std::cout << "Warning: " << error << '\n';
            } else {
                std::cout << "Could not save availability: " << error << '\n';
            }
        } else if (choice == "3") {
            std::cout << "\n=== Add Multiple Availability: Bulk Input ===\n"
                      << "Paste one DAY|START TIME|END TIME|LABEL record per line.\n"
                      << "Type END on its own line when finished.\n";
            std::vector<std::string> lines;
            std::string line;
            bool ended = false;
            while (std::getline(std::cin, line)) {
                if (lower(trim(line)) == "end") {
                    ended = true;
                    break;
                }
                lines.push_back(line);
            }
            if (!ended) {
                std::cout << "Bulk input ended before END; nothing was saved.\n";
                return 0;
            }
            struct ParsedLine {
                std::size_t lineNumber;
                AvailabilityInput input;
            };
            std::vector<ParsedLine> parsed;
            std::vector<std::string> lineErrors(lines.size());
            std::vector<bool> parsedOk(lines.size(), false);
            bool invalid = lines.empty();
            if (lines.empty()) std::cout << "Invalid line 1: no records entered.\n";
            for (std::size_t index = 0; index < lines.size(); ++index) {
                AvailabilityInput input;
                if (!parseQuickAvailability(lines[index], input, lineErrors[index])) {
                    invalid = true;
                    std::cout << "Invalid line " << index + 1 << " ("
                              << lines[index] << "): " << lineErrors[index] << '\n';
                } else {
                    parsedOk[index] = true;
                    parsed.push_back(ParsedLine{index + 1, input});
                }
            }
            std::vector<AvailabilityInput> inputs;
            for (const ParsedLine& row : parsed) inputs.push_back(row.input);
            std::vector<std::string> errors;
            manager.checkInputs(inputs, errors);
            for (std::size_t index = 0; index < errors.size(); ++index) {
                if (!errors[index].empty()) {
                    invalid = true;
                    lineErrors[parsed[index].lineNumber - 1] = errors[index];
                    std::cout << "Invalid/conflicting line "
                              << parsed[index].lineNumber << " ("
                              << lines[parsed[index].lineNumber - 1] << "): "
                              << errors[index] << '\n';
                }
            }
            std::cout << "\nBulk preview:\n"
                      << "Line | Day | Start - End | Label | Result\n";
            for (std::size_t index = 0; index < lines.size(); ++index) {
                if (!parsedOk[index]) {
                    std::cout << index + 1 << " | " << lines[index]
                              << " | INVALID: " << lineErrors[index] << '\n';
                    continue;
                }
                const auto row = std::find_if(parsed.begin(), parsed.end(),
                    [index](const ParsedLine& value) {
                        return value.lineNumber == index + 1;
                    });
                if (row == parsed.end()) {
                    std::cout << index + 1 << " | " << lines[index]
                              << " | INVALID: could not create preview\n";
                    invalid = true;
                    continue;
                }
                std::cout << row->lineNumber << " | " << toString(row->input.day)
                          << " | " << formatTime12Hour(row->input.startMinute)
                          << " - " << formatTime12Hour(row->input.endMinute)
                          << " | " << (row->input.label.empty()
                                ? "(no label)" : row->input.label)
                          << " | " << (lineErrors[index].empty()
                                ? "Valid" : "INVALID: " + lineErrors[index])
                          << '\n';
            }
            if (invalid) {
                std::cout << "Batch rejected: at least one line is invalid. "
                             "No availability was saved.\n";
                continue;
            }
            if (!confirmed("Save all previewed records?")) {
                std::cout << "Cancelled; no availability was changed.\n";
                continue;
            }
            std::vector<AvailabilityWindow> created;
            if (!manager.addMany(inputs, created, error)) {
                std::cout << "Batch save failed; no records were added: "
                          << error << '\n';
            } else {
                std::cout << "Saved all " << created.size()
                          << " availability window(s):\n";
                printWindows(created);
                if (!error.empty()) std::cout << "Warning: " << error << '\n';
            }
        } else if (choice == "4") {
            std::cout << "\n=== Edit Availability ===\n";
            std::string id;
            if (!promptLine("Availability ID (example AV-000001): ", id)) return 0;
            const auto found = std::find_if(manager.all().begin(), manager.all().end(),
                [&id](const AvailabilityWindow& window) { return window.id == id; });
            if (found == manager.all().end()) {
                std::cout << "Availability ID not found: " << id << '\n';
                continue;
            }
            std::cout << "Current record:\n";
            printWindow(*found);
            AvailabilityInput replacement;
            if (!promptGuidedInput(replacement)) return 0;
            std::cout << "Replacement preview:\n";
            printInputPreview(replacement);
            if (!confirmed("Confirm edit?")) {
                std::cout << "Cancelled; availability was not changed.\n";
                continue;
            }
            if (manager.update(id, replacement, error)) {
                std::cout << "Updated availability " << id << ".\n";
                if (!error.empty()) std::cout << "Warning: " << error << '\n';
            } else {
                std::cout << "Could not update availability: " << error << '\n';
            }
        } else if (choice == "5") {
            std::cout << "\n=== Delete Availability ===\n";
            std::string id;
            if (!promptLine("Availability ID (example AV-000001): ", id)) return 0;
            if (!confirmed("Delete this availability window?")) {
                std::cout << "Cancelled; availability was not changed.\n";
                continue;
            }
            if (manager.erase(id, error)) {
                std::cout << "Deleted availability " << id << ".\n";
                if (!error.empty()) std::cout << "Warning: " << error << '\n';
            } else {
                std::cout << "Could not delete availability: " << error << '\n';
            }
        } else if (choice == "6") {
            std::cout << "\n=== View All Availability ===\n";
            printWindows(sortedWindows(manager.all()));
        } else if (choice == "7") {
            std::cout << "\n=== View Availability by Day ===\n";
            DayOfWeek day;
            if (!promptDay(day)) return 0;
            const std::vector<AvailabilityWindow> dayWindows = manager.byDay(day);
            printWindows(dayWindows);
            std::cout << toString(day) << " total: "
                      << manager.dailyMinutes(day) / 60 << " hour(s) "
                      << manager.dailyMinutes(day) % 60 << " minute(s).\n";
        } else if (choice == "8") {
            std::cout << "\n=== Search Availability ===\n";
            std::string query;
            if (!promptLine("Search ID, day, time, or label: ", query)) return 0;
            if (trim(query).empty()) {
                std::cout << "Search input cannot be empty.\n";
                continue;
            }
            printWindows(manager.search(query));
        } else if (choice == "9") {
            std::cout << "\n=== Weekly Availability Summary ===\n";
            for (int index = 0; index < 7; ++index) {
                const DayOfWeek day = static_cast<DayOfWeek>(index);
                const std::vector<AvailabilityWindow> dayWindows = manager.byDay(day);
                std::cout << toString(day) << ": "
                          << dayWindows.size() << " window(s), "
                          << manager.dailyMinutes(day) / 60 << " hour(s) "
                          << manager.dailyMinutes(day) % 60 << " minute(s)\n";
                for (const AvailabilityWindow& window : dayWindows) {
                    std::cout << "  " << formatTime12Hour(window.startMinute)
                              << " - " << formatTime12Hour(window.endMinute)
                              << " | " << (window.label.empty()
                                    ? "(no label)" : window.label) << '\n';
                }
            }
            std::cout << "Weekly total: " << manager.weeklyMinutes() / 60
                      << " hour(s) " << manager.weeklyMinutes() % 60
                      << " minute(s).\n";
        } else if (choice == "11") {
            std::cout << "\n=== Set Default Weekly Availability ===\n"
                      << "This replaces all current availability with one "
                         "window on each day.\n";
            int start = 0;
            int end = 0;
            if (!promptTime("Start Time", start) || !promptTime("End Time", end))
                return 0;
            std::string label;
            if (!promptLine("Label (optional): ", label)) return 0;
            AvailabilityInput sample{DayOfWeek::Monday, start, end, trim(label)};
            if (!validateAvailabilityInput(sample, error)) {
                std::cout << "Invalid default window: " << error << '\n';
                continue;
            }
            std::cout << "Preview: " << formatTime12Hour(start) << " - "
                      << formatTime12Hour(end) << " on all 7 days; label: "
                      << (sample.label.empty() ? "(no label)" : sample.label)
                      << '\n';
            if (!confirmed("Replace the complete weekly availability?")) {
                std::cout << "Cancelled; availability was not changed.\n";
                continue;
            }
            if (manager.setDefaultWeekly(start, end, sample.label, error)) {
                std::cout << "Default weekly availability saved for all 7 days.\n";
                if (!error.empty()) std::cout << "Warning: " << error << '\n';
            } else {
                std::cout << "Could not set weekly availability: " << error << '\n';
            }
        } else if (choice == "10") {
            std::cout << "\n=== Reset Day Availability ===\n";
            DayOfWeek day;
            if (!promptDay(day)) return 0;
            const std::vector<AvailabilityWindow> dayWindows = manager.byDay(day);
            if (dayWindows.empty()) {
                std::cout << "No availability is set for " << toString(day) << ".\n";
                continue;
            }
            std::cout << "The following " << dayWindows.size()
                      << " window(s) will be removed:\n";
            printWindows(dayWindows);
            if (!confirmed("Reset all availability for this day?")) {
                std::cout << "Cancelled; availability was not changed.\n";
                continue;
            }
            if (manager.resetDay(day, error)) {
                std::cout << "Reset availability for " << toString(day) << ".\n";
                if (!error.empty()) std::cout << "Warning: " << error << '\n';
            } else {
                std::cout << "Could not reset day: " << error << '\n';
            }
        } else if (choice == "12") {
            std::cout << "\n=== Save Availability to File ===\n";
            if (manager.save(error)) {
                std::cout << "Saved " << manager.all().size()
                          << " window(s) to the availability file.\n";
                if (!error.empty()) std::cout << "Warning: " << error << '\n';
            } else {
                std::cout << "Could not save availability: " << error << '\n';
            }
        } else if (choice == "13") {
            return 0;
        } else {
            std::cout << "Invalid menu choice. Enter a number from 1 to 13.\n";
        }
    }
}

int runSector02Demo(const std::filesystem::path& storagePath)
{
    AvailabilityManager manager(storagePath);
    std::string error;
    std::cout << "=== Sector 02: Availability Management Demo ===\n"
              << "Dedicated demo storage: " << storagePath.string() << '\n';
    if (!manager.load(error)) {
        std::cerr << "Could not load demo availability: " << error << '\n';
        return 1;
    }
    const std::vector<AvailabilityWindow> oldDemoWindows = manager.all();
    for (const AvailabilityWindow& window : oldDemoWindows) {
        if (!manager.erase(window.id, error)) {
            std::cerr << "Could not reset demo data: " << error << '\n';
            return 1;
        }
    }
    std::cout << "Cleared " << oldDemoWindows.size()
              << " existing demo window(s).\n";

    AvailabilityInput guided;
    if (!parseDay("MON", guided.day)
        || !parseTime12Hour("9:00 AM", guided.startMinute)
        || !parseTime12Hour("1:00 PM", guided.endMinute)) {
        std::cerr << "Guided example input could not be parsed.\n";
        return 1;
    }
    guided.label = "Morning Study";
    std::cout << "\n=== Guided input example ===\n"
              << "Day: Monday\nStart Time: 9:00 AM\nEnd Time: 1:00 PM\n"
              << "Label: Morning Study\n";
    if (!validateAvailabilityInput(guided, error)) {
        std::cerr << "Guided example rejected: " << error << '\n';
        return 1;
    }
    std::cout << "Parsed preview:\n";
    printInputPreview(guided);
    AvailabilityWindow created;
    if (!manager.add(guided, created, error)) {
        std::cerr << "Guided example could not be saved: " << error << '\n';
        return 1;
    }
    std::cout << "Actual result: saved as " << created.id << ".\n";

    const std::string quickExample = "MON|2:00 PM|5:00 PM|Afternoon Study";
    AvailabilityInput quick;
    std::cout << "\n=== Quick input example ===\n"
              << "Input: " << quickExample << "\n";
    if (!parseQuickAvailability(quickExample, quick, error)) {
        std::cerr << "Quick example rejected: " << error << '\n';
        return 1;
    }
    printInputPreview(quick);
    if (!manager.add(quick, created, error)) {
        std::cerr << "Quick example could not be saved: " << error << '\n';
        return 1;
    }
    std::cout << "Actual result: saved as " << created.id << ".\n";

    const std::vector<std::string> bulkLines = {
        "TUE|10:00 AM|1:00 PM|Free Time",
        "WED|6:00 PM|8:00 PM|Programming"
    };
    std::cout << "\n=== Bulk input example ===\n";
    std::cout << "Input records (terminate with END):\n";
    std::vector<AvailabilityInput> bulk;
    for (const std::string& line : bulkLines) {
        std::cout << line << '\n';
        AvailabilityInput input;
        if (!parseQuickAvailability(line, input, error)) {
            std::cerr << "Bulk example rejected: " << error << '\n';
            return 1;
        }
        bulk.push_back(input);
    }
    std::cout << "END\n";
    std::vector<std::string> rowErrors;
    if (!manager.checkInputs(bulk, rowErrors)) {
        std::cerr << "Bulk example validation failed.\n";
        return 1;
    }
    std::cout << "Preview:\nLine | Day | Start - End | Label | Result\n";
    for (std::size_t index = 0; index < bulk.size(); ++index) {
        std::cout << index + 1 << " | " << toString(bulk[index].day)
                  << " | " << formatTime12Hour(bulk[index].startMinute)
                  << " - " << formatTime12Hour(bulk[index].endMinute)
                  << " | " << (bulk[index].label.empty()
                        ? "(no label)" : bulk[index].label)
                  << " | Valid\n";
    }
    std::vector<AvailabilityWindow> bulkCreated;
    if (!manager.addMany(bulk, bulkCreated, error)) {
        std::cerr << "Bulk example could not be saved: " << error << '\n';
        return 1;
    }
    std::cout << "Actual result: saved " << bulkCreated.size()
              << " records as one batch.\n";

    std::cout << "\n=== Invalid time rejection ===\n"
              << "Input: THU|13:00 PM|2:00 PM|Invalid Clock\n";
    AvailabilityInput invalid;
    if (parseQuickAvailability("THU|13:00 PM|2:00 PM|Invalid Clock",
            invalid, error)) {
        std::cerr << "Invalid time was unexpectedly accepted.\n";
        return 1;
    }
    std::cout << "Actual result: rejected: " << error << '\n';

    std::cout << "\n=== Overlap detection ===\n"
              << "Input: MON|12:00 PM|3:00 PM|Overlapping Window\n";
    AvailabilityInput overlapping;
    if (!parseQuickAvailability("MON|12:00 PM|3:00 PM|Overlapping Window",
            overlapping, error)) {
        std::cerr << "Overlap demo input parsing failed: " << error << '\n';
        return 1;
    }
    if (manager.add(overlapping, created, error)) {
        std::cerr << "Overlapping availability was unexpectedly accepted.\n";
        return 1;
    }
    std::cout << "Actual result: rejected: " << error << '\n';

    std::cout << "\n=== Daily and weekly summary ===\n";
    for (int index = 0; index < 7; ++index) {
        const DayOfWeek day = static_cast<DayOfWeek>(index);
        std::cout << toString(day) << ": " << manager.byDay(day).size()
                  << " window(s), " << manager.dailyMinutes(day) / 60
                  << " hour(s) " << manager.dailyMinutes(day) % 60
                  << " minute(s)\n";
    }
    std::cout << "Weekly total: " << manager.weeklyMinutes() / 60
              << " hour(s) " << manager.weeklyMinutes() % 60 << " minute(s)\n";

    std::cout << "\n=== Save availability ===\n"
              << "Input file: " << storagePath.string() << '\n';
    if (!manager.save(error)) {
        std::cerr << "Could not save availability: " << error << '\n';
        return 1;
    }
    std::cout << "Actual result: saved " << manager.all().size()
              << " availability window(s).\n";

    AvailabilityManager reloaded(storagePath);
    if (!reloaded.load(error)) {
        std::cerr << "Could not reload availability: " << error << '\n';
        return 1;
    }
    std::cout << "\n=== Save and reload ===\n"
              << "Actual result: reloaded " << reloaded.all().size()
              << " saved window(s).\n"
              << "Final availability list:\n";
    printWindows(sortedWindows(reloaded.all()));
    return 0;
}

} // namespace taskflow

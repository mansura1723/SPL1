#include "availability_management.hpp"

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

std::filesystem::path prepareDirectory(
    const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    if (error) {
        throw std::runtime_error("Cannot prepare test directory: "
            + error.message());
    }
    std::filesystem::create_directories(directory);
    return directory;
}

AvailabilityInput input(const std::string& day, const std::string& start,
    const std::string& end, const std::string& label = {})
{
    DayOfWeek parsedDay;
    int startMinute = 0;
    int endMinute = 0;
    require(parseDay(day, parsedDay), "Test day did not parse: " + day);
    require(parseTime12Hour(start, startMinute),
        "Test start time did not parse: " + start);
    require(parseTime12Hour(end, endMinute),
        "Test end time did not parse: " + end);
    return AvailabilityInput{parsedDay, startMinute, endMinute, label};
}

void timeAndDayTests(const std::filesystem::path&)
{
    DayOfWeek day;
    require(parseDay("MON", day) && day == DayOfWeek::Monday,
        "Weekday abbreviation did not parse.");
    require(parseDay("wednesday", day) && day == DayOfWeek::Wednesday,
        "Full weekday name did not parse case-insensitively.");
    require(!parseDay("Funday", day), "Invalid weekday was accepted.");

    int minute = -1;
    require(parseTime12Hour("9:00 AM", minute) && minute == 9 * 60,
        "Morning time parsed incorrectly.");
    require(parseTime12Hour("1:00 PM", minute) && minute == 13 * 60,
        "Afternoon time parsed incorrectly.");
    require(parseTime12Hour("12:00 AM", minute) && minute == 0,
        "Midnight parsed incorrectly.");
    require(parseTime12Hour("12:00 PM", minute) && minute == 12 * 60,
        "Noon parsed incorrectly.");
    require(formatTime12Hour(0) == "12:00 AM",
        "Midnight display is incorrect.");
    require(formatTime12Hour(12 * 60) == "12:00 PM",
        "Noon display is incorrect.");
    require(!parseTime12Hour("13:00 PM", minute),
        "13:00 PM was accepted.");
    require(!parseTime12Hour("15:00 AM", minute),
        "15:00 AM was accepted.");
    require(!parseTime12Hour("0:30 PM", minute),
        "0:30 PM was accepted.");
    require(!parseTime12Hour("9:60 AM", minute),
        "Out-of-range minutes were accepted.");
    require(!parseTime12Hour("", minute), "Empty time was accepted.");
}

void inputValidationTests(const std::filesystem::path&)
{
    std::string error;
    AvailabilityInput valid = input("MON", "9:00 AM", "1:00 PM", "Study");
    require(validateAvailabilityInput(valid, error), error);
    require(!validateAvailabilityInput(
        input("TUE", "3:00 PM", "1:00 PM"), error),
        "End-before-start was accepted.");
    require(!validateAvailabilityInput(
        input("TUE", "1:00 PM", "1:00 PM"), error),
        "Equal start and end times were accepted.");
    AvailabilityInput overlong = valid;
    overlong.label = std::string(121, 'x');
    require(!validateAvailabilityInput(overlong, error),
        "Overlong label was accepted.");

    AvailabilityInput parsed;
    require(!parseQuickAvailability("", parsed, error),
        "Empty quick input was accepted.");
    require(!parseQuickAvailability("MON|9:00 AM|", parsed, error),
        "Incomplete quick input was accepted.");
    require(!parseQuickAvailability("FUNDAY|9:00 AM|1:00 PM|Study",
        parsed, error), "Invalid quick-input weekday was accepted.");
    require(!parseQuickAvailability("MON|13:00 PM|2:00 PM|Study",
        parsed, error), "Invalid quick-input time was accepted.");
}

void overlapAndDuplicateTests(const std::filesystem::path& directory)
{
    const std::filesystem::path path =
        prepareDirectory(directory) / "overlap.csv";
    AvailabilityManager manager(path);
    std::string error;
    require(manager.load(error), error);
    AvailabilityWindow first;
    require(manager.add(input("MON", "9:00 AM", "12:00 PM", "Study"),
        first, error), error);

    std::vector<std::string> errors;
    require(!manager.checkInputs(
        {input("MON", "9:00 AM", "12:00 PM", "Duplicate label")},
        errors), "Duplicate window was accepted.");
    require(!errors.front().empty(), "Duplicate error was not explained.");
    require(!manager.checkInputs(
        {input("MON", "11:00 AM", "1:00 PM", "Overlap")}, errors),
        "Overlapping window was accepted.");
    require(errors.front().find("Overlaps") != std::string::npos,
        "Overlap error was not explained.");

    AvailabilityWindow adjacent;
    require(manager.add(input("MON", "12:00 PM", "1:00 PM", "Adjacent"),
        adjacent, error), "Adjacent non-overlapping window rejected: " + error);
    require(manager.all().size() == 2,
        "Rejected records altered the availability list.");
}

void bulkValidationTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    AvailabilityManager manager(directory / "bulk.csv");
    std::string error;
    require(manager.load(error), error);
    const std::vector<std::string> lines = {
        "MON|9:00 AM|12:00 PM|Morning Study",
        "TUE|10:00 AM|1:00 PM|Free Time",
        "WED|13:00 PM|2:00 PM|Bad Clock",
        "MON|11:00 AM|1:00 PM|Conflicts with first"
    };

    std::vector<AvailabilityInput> accepted;
    std::vector<std::size_t> sourceLines;
    std::vector<std::string> errors;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        AvailabilityInput row;
        if (parseQuickAvailability(lines[index], row, error)) {
            accepted.push_back(row);
            sourceLines.push_back(index + 1);
        } else {
            errors.push_back("line " + std::to_string(index + 1) + ": " + error);
        }
    }
    std::vector<std::string> batchErrors;
    manager.checkInputs(accepted, batchErrors);
    for (std::size_t index = 0; index < batchErrors.size(); ++index) {
        if (!batchErrors[index].empty()) {
            errors.push_back("line " + std::to_string(sourceLines[index])
                + ": " + batchErrors[index]);
        }
    }
    require(errors.size() == 2,
        "Invalid bulk lines were not all identified.");
    require(manager.all().empty(),
        "Valid subset from invalid bulk input was saved unexpectedly.");
    std::vector<AvailabilityWindow> created;
    require(!manager.addMany(accepted, created, error),
        "Overlapping bulk records were partially accepted.");
    require(manager.all().empty(),
        "Failed bulk operation changed the list.");

    const std::vector<AvailabilityInput> validBatch = {
        input("MON", "9:00 AM", "12:00 PM", "Morning Study"),
        input("MON", "2:00 PM", "5:00 PM", "Afternoon Study"),
        input("TUE", "10:00 AM", "1:00 PM", "Free Time"),
        input("WED", "6:00 PM", "8:00 PM", "Programming")
    };
    require(manager.addMany(validBatch, created, error), error);
    require(created.size() == 4 && manager.all().size() == 4,
        "Valid batch was not saved atomically.");
}

void crudAndSummaryTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    AvailabilityManager manager(directory / "crud.csv");
    std::string error;
    require(manager.load(error), error);
    require(manager.all().empty() && manager.byDay(DayOfWeek::Monday).empty(),
        "Empty availability list was not handled.");

    AvailabilityWindow first;
    require(manager.add(input("MON", "9:00 AM", "12:00 PM", "Study"),
        first, error), error);
    require(first.id == "AV-000001", "First availability ID is incorrect.");
    AvailabilityWindow second;
    require(manager.add(input("TUE", "10:00 AM", "1:00 PM", "Free"),
        second, error), error);
    require(first.id != second.id, "Availability IDs are not unique.");

    require(manager.search("study").size() == 1,
        "Availability search did not find the label.");
    require(manager.search("monday").size() == 1,
        "Availability search did not find the day.");
    require(manager.dailyMinutes(DayOfWeek::Monday) == 180
        && manager.weeklyMinutes() == 360,
        "Daily or weekly summary is incorrect.");

    require(manager.update(first.id,
        input("MON", "9:30 AM", "12:30 PM", "Edited Study"), error), error);
    require(manager.all().front().id == first.id
        && manager.all().front().label == "Edited Study",
        "Edit did not preserve the availability ID.");
    require(!manager.update(first.id,
        input("TUE", "11:00 AM", "2:00 PM", "Conflict"), error),
        "Edit that would overlap another day window was accepted.");
    require(!manager.erase("AV-999999", error),
        "Deleting a missing availability unexpectedly succeeded.");
    require(manager.erase(first.id, error), error);
    require(manager.all().size() == 1, "Delete did not remove the record.");
    require(manager.resetDay(DayOfWeek::Tuesday, error), error);
    require(manager.all().empty(), "Reset day did not remove that day's data.");

    require(manager.setDefaultWeekly(9 * 60, 17 * 60, "Default", error), error);
    require(manager.all().size() == 7
        && manager.byDay(DayOfWeek::Sunday).size() == 1
        && manager.weeklyMinutes() == 7 * 8 * 60,
        "Default weekly availability was not applied to all days.");
    require(manager.resetDay(DayOfWeek::Wednesday, error), error);
    require(manager.byDay(DayOfWeek::Wednesday).empty()
        && manager.all().size() == 6,
        "Selected-day reset failed.");
}

void persistenceTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    const std::filesystem::path path = directory / "availability.csv";
    AvailabilityManager manager(path);
    std::string error;
    AvailabilityWindow created;
    require(manager.add(input("THU", "12:00 AM", "1:00 AM",
        "Comma, quotes \"saved\""), created, error), error);
    require(manager.save(error), error);

    AvailabilityManager reloaded(path);
    require(reloaded.load(error), error);
    require(reloaded.all().size() == 1, "Availability did not reload.");
    const AvailabilityWindow& restored = reloaded.all().front();
    require(restored.id == created.id
        && restored.startMinute == 0
        && restored.endMinute == 60
        && restored.label == "Comma, quotes \"saved\"",
        "Persistence did not preserve time or CSV-escaped label.");
    AvailabilityWindow next;
    require(reloaded.add(input("FRI", "12:00 PM", "1:00 PM", "Noon"),
        next, error), error);
    require(next.id == "AV-000002",
        "ID counter did not continue after reload.");
}

void malformedPersistenceTests(const std::filesystem::path& directory)
{
    prepareDirectory(directory);
    const std::filesystem::path path = directory / "malformed.csv";
    AvailabilityManager manager(path);
    std::string error;
    AvailabilityWindow kept;
    require(manager.add(input("MON", "9:00 AM", "10:00 AM", "Keep"),
        kept, error), error);
    {
        std::ofstream output(path, std::ios::trunc);
        output << "id,day,start,end,label\n"
               << "AV-000001,Monday,13:00 PM,2:00 PM,Bad\n";
    }
    require(!manager.load(error), "Invalid persisted time was accepted.");
    require(manager.all().size() == 1 && manager.all().front().id == kept.id,
        "Failed load replaced the current availability.");
}

} // namespace

int runAvailabilityTests(const std::filesystem::path& testDirectory)
{
    using Test = std::pair<const char*, void (*)(const std::filesystem::path&)>;
    const Test tests[] = {
        {"unit: weekday and 12-hour time parsing", timeAndDayTests},
        {"unit: time range and record validation", inputValidationTests},
        {"unit: duplicate and overlap detection", overlapAndDuplicateTests},
        {"integration: bulk preview validation and atomic save", bulkValidationTests},
        {"integration: CRUD, reset, defaults, and summaries", crudAndSummaryTests},
        {"integration: persistence and time-of-day boundaries", persistenceTests},
        {"integration: malformed persisted data is rejected", malformedPersistenceTests}
    };

    try {
        std::filesystem::create_directories(testDirectory);
        for (std::size_t index = 0; index < sizeof(tests) / sizeof(tests[0]);
             ++index) {
            tests[index].second(
                testDirectory / ("availability-case-" + std::to_string(index + 1)));
            std::cout << "PASS Sector 02 " << tests[index].first << '\n';
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL Sector 02: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "All " << (sizeof(tests) / sizeof(tests[0]))
              << " Sector 02 Availability tests passed.\n";
    return 0;
}

} // namespace taskflow

# TaskFlow — Constraint-Aware Task Scheduling and Decision Engine

TaskFlow is a C++17 command-line application for managing tasks, recurring
weekly availability, prerequisite relationships, and deterministic schedules.
It combines four sector modules into a single interactive application and
keeps task, availability, and dependency data in separate CSV files.

## Technologies

- C++17 for the TaskFlow application and C11 for the preserved C module check.
- CMake 3.16+ for configuring, building, and registering tests.
- The C and C++ standard libraries; application records are stored in CSV.

## Problem and objectives

Task lists alone do not account for when work can be done or what must happen
first. TaskFlow models those constraints and provides explainable scheduling
strategies without silently changing task status. The project demonstrates
modular C/C++ integration, validation, file persistence, graph algorithms,
automated tests, and CMake-based builds.

## Features by sector

| Sector | Capabilities |
|---|---|
| 01 — Task Management | Validated task CRUD, generated IDs, search, filters, sorting, CSV persistence |
| 02 — Availability Management | Weekly time windows, overlap checks, bulk input, daily/weekly summaries |
| 03 — Dependency Management | Graph validation, cycle detection, topological order, readiness, blockers, downstream impact |
| 04 — Scheduling | Priority, EDF, SJF, LST, and custom ranking with deterministic tie-breaks |

Sector 04 schedules only unblocked Pending and In Progress tasks. It reports
completed, blocked, and excluded tasks separately, calculates deadline slack
from weekly availability, and leaves source task statuses unchanged. See the
[Sector 04 algorithm notes](docs/sectors/SECTOR_04_ALGORITHMS.md) for its
availability model, edge cases, and ordering rules.

## Architecture

`src/main.cpp` routes interactive use, sector demos, and the test runner.
Managers own the data and CSV persistence for their respective sectors. The
scheduling engine consumes task, availability, and dependency snapshots.

- [System architecture](docs/architecture/SYSTEM_ARCHITECTURE.md)
- [Module interaction](docs/architecture/MODULE_INTERACTION.md)
- [Data flow](docs/architecture/DATA_FLOW.md)
- [Task lifecycle, DAG, and scheduling algorithms](docs/architecture/ALGORITHM_DESIGN.md)

## Project structure

```text
TaskFlow/
├── CMakeLists.txt
├── README.md
├── cpp_modules/       # Sector implementations and preserved C++ check
├── c_modules/         # Preserved C11 regression check
├── include/           # Public module headers
├── src/               # Application entry point
├── tests/              # Sector tests, built into taskflow
├── data/               # Local interactive CSV data (not committed)
└── docs/
    ├── architecture/
    ├── demos/
    └── sectors/
```

## Prerequisites and build

- CMake 3.16 or newer.
- A C11 and C++17-capable compiler (for example GCC, Clang, or MinGW-w64).
- Git is needed to clone the repository.

Clone the repository and enter the project directory:

```sh
git clone --branch squash-practice https://github.com/mansura1723/SPL1.git
cd SPL1/TaskFlow
```

On Windows with MinGW-w64, configure and build from the TaskFlow directory:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure
```

On Linux or macOS with a suitable default CMake generator:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the interactive manager from this directory so the relative `data/` and
demo paths resolve correctly:

```sh
./build/taskflow
```

On Windows PowerShell, use `.\build\taskflow.exe`. The CMake project is in the
`TaskFlow/` subdirectory of the Git repository; the root-level GitHub Actions
workflow configures it with `cmake -S TaskFlow`.

## Sector documentation and demos

Dedicated technical notes describe purpose, data/API, algorithms, validation,
integration, test coverage, and demo execution:

- [Sector 01 — Task Management](docs/sectors/SECTOR_01_TASK_MANAGEMENT.md)
- [Sector 02 — Availability Management](docs/sectors/SECTOR_02_AVAILABILITY_MANAGEMENT.md)
- [Sector 03 — Dependency Management](docs/sectors/SECTOR_03_DEPENDENCY_MANAGEMENT.md)
- [Sector 04 — Scheduling](docs/sectors/SECTOR_04_ALGORITHMS.md)

Each demo can be run from the TaskFlow directory:

```powershell
.\build\taskflow.exe --demo sector-01
.\build\taskflow.exe --demo sector-02
.\build\taskflow.exe --demo sector-03
.\build\taskflow.exe --demo sector-04
```

Captured outputs are in [`docs/demos/`](docs/demos/). The demo commands use
separate files under `build/`, not the interactive CSV data. Sector 01 and 02
demos retain their dedicated CSV files between runs, so generated IDs in
captured output can differ after previous local runs.

For example, the captured Sector 04 run includes:

```text
Strategy: Earliest Deadline First (EDF)
Validation: PASS
Eligible scheduling order:
  1. TSK-000002 | Task B | slack 60 min | downstream impact 0
Determinism check: PASS
```

## Test and validation

CMake registers one CTest test, `taskflow_all_sectors`, which runs the
combined test harness. It includes the preserved C11 and C++17 module checks
and the Sector 01–04 test suites. To run the same harness directly:

```powershell
.\build\taskflow.exe --test
```

In the fresh local run, the direct harness reported 7 Sector 01 tests, 7
Sector 02 tests, 8 Sector 03 tests, and 41 Sector 04 checks (0 failed).
Generated test data is written beneath the build directory. The repository
also has a GitHub Actions workflow that configures, builds, and runs CTest on
Ubuntu for pushes and pull requests. A workflow file being present does not
itself establish that a GitHub Actions run has passed.

## License

No license is currently included. Without an explicit license, default
copyright restrictions generally apply; this repository does not grant
general permission to reuse, modify, or redistribute its contents. A license
should be added only after the rights holder chooses and authorizes its terms.

## Future improvements

- Add time-zone-aware availability and dated calendar exceptions.
- Persist scheduled commitments for use by the slack calculation.
- Add remaining-duration tracking for in-progress tasks.
- Expand portability checks across supported compiler and operating-system
  combinations.

## Step-by-step user guide

### 1. Build the project

Change to the TaskFlow project directory, then configure and build with the
MinGW generator (or use the generator configured for your compiler):

```powershell
Set-Location 'C:\Users\Admin\Desktop\SPL1\TaskFlow'
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

### 2. Start interactive mode

```powershell
.\build\taskflow.exe
```

The menu appears in the terminal. Type the displayed number at `Choice:` and
press Enter. The program prompts for each field; type one value per prompt and
press Enter. After an operation it returns to the menu.

| Choice | Operation |
|---|---|
| 1 | Create a task |
| 2 | Edit a task by ID (enter replacement values for all fields) |
| 3 | Delete a task by ID (type `yes` to confirm; anything else cancels) |
| 4 | View all tasks |
| 5 | Search names and descriptions |
| 6 | Filter by priority, status, and/or type; leave a field blank for any |
| 7 | Sort by ID, name, priority, duration, deadline, or status |
| 8 | Save tasks immediately |
| 9 | Open Availability Management |
| 10 | Open Dependency Management |
| 11 | Open Scheduling Strategies |
| 0 | Save and exit |

Input formats and limits:

- **Name:** required, 1-120 characters.
- **Description:** up to 2,000 characters; it may be blank.
- **Priority:** `Low`, `Medium`, `High`, or `Critical`.
- **Duration:** whole minutes from 1 to 1,000,000.
- **Deadline:** a real local date written `YYYY-MM-DD` (treated as 11:59 PM),
  or a local date-time written `YYYY-MM-DDTHH:MM` (for example
  `2028-02-29T15:30`; impossible dates and times are rejected).
- **Type:** required, 1-60 characters.
- **Status:** `Pending`, `Ready`, `In Progress`, `Completed`, or `Overdue`.

The app loads `data/tasks.csv`, `data/availability.csv`, and
`data/dependencies.csv` when it starts. Each sector has a separate file;
availability and dependency operations never overwrite task data. A missing
file starts that sector with an empty list. Task IDs are generated as
`TSK-000001`, `TSK-000002`, and so on. Search ignores case. Priority sorting is
highest-first; duration and deadline sorting are ascending.

### 3. Run the automated demo

```powershell
.\build\taskflow.exe --demo sector-01
```

The demo displays each operation's inputs and actual results: load, create,
view, edit, search, filter, sort, invalid-input rejection, delete, save, and
reload. It uses `build/sector-01-demo-tasks.csv`, separate from the interactive
user file. On each run it removes the previous contents of that dedicated demo
file through the normal delete operation, then creates fresh sample tasks.

### 4. Run tests

Run the CTest target:

```powershell
ctest --test-dir build --output-on-failure
```

Or show each test result directly:

```powershell
.\build\taskflow.exe --test
```

The suite covers preserved C11/C++17 module checks, task and availability
regressions, dependency CRUD and persistence, malformed records, cycles,
deterministic topological order, readiness and multiple blockers, downstream
impact, all five scheduling strategies, weekly availability-based slack,
commitments, blocked-task filtering, and deterministic ties. Test data is
written under `build`.

### 5. Sector 04 scheduling

Choose `11. Scheduling Strategies` from the main menu. The scheduler uses the
loaded weekly availability and dependency data without changing task statuses.
Deadlines with a date only are interpreted at the end of that local day.
Available working time excludes configured unavailable periods and optional
committed intervals supplied to the scheduling engine.

Run the reproducible, non-interactive demonstration:

```powershell
.\build\taskflow.exe --demo sector-04
```

Sector 04's algorithm notes and the captured demo output are in
`docs/sectors/SECTOR_04_ALGORITHMS.md` and `docs/demos/SECTOR_04_OUTPUT.txt`.

## Sector 02 - Availability Management

From the TaskFlow main menu, enter `9` to open **AVAILABILITY MANAGEMENT**.
The submenu supports guided, quick, and bulk entry; edit/delete; viewing all
windows or one weekday; search; daily and weekly summaries; default weekly
availability; resetting one day; saving; and returning to the main menu.
Availability is loaded automatically from `data/availability.csv` at startup
and saved independently from Sector 01 task data.

Every time input and display uses **12-hour AM/PM format**, such as `9:00 AM`,
`10:30 AM`, `1:00 PM`, or `8:30 PM`. Hours must be 1 through 12, minutes 00
through 59, followed by AM or PM. Midnight is `12:00 AM`; noon is
`12:00 PM`. Inputs such as `13:00 PM`, `15:00 AM`, and `0:30 PM` are rejected.
Intervals must start before they end on the same day. Adjacent windows are
allowed; overlapping or duplicate day/time windows are not.

### Guided Input (one field at a time)

Choose `1. Add Availability (Guided Input)`. Type only the value requested
after each prompt and press Enter; do not type field names. For example:

```text
Day (MON or Monday): MON
Start Time (for example 9:00 AM): 9:00 AM
End Time (for example 9:00 AM): 1:00 PM
Label (example Morning Study; optional, blank allowed): Morning Study
```

The app validates the day and time fields, shows a preview, checks for
duplicates/overlaps, and asks `Confirm save? (Y/N)` before saving.

### Quick Input (one line)

Choose `2. Add Availability (Quick Input)` and enter four pipe-separated
fields:

```text
MON|9:00 AM|1:00 PM|Morning Study
```

The fields are day, start time, end time, and optional label. The application
parses and validates the whole line, then displays a preview before asking for
confirmation:

```text
Day: Monday
Start: 9:00 AM
End: 1:00 PM
Label: Morning Study
Confirm save? (Y/N):
```

Weekdays accept abbreviations `MON`, `TUE`, `WED`, `THU`, `FRI`, `SAT`, `SUN`
or full names.

### Complete Sector 02 example

This is a complete example transcript showing the quick and bulk inputs and
the corresponding availability records:

```text
Quick input: MON|9:00 AM|1:00 PM|Morning Study
Preview:
Day: Monday
Start: 9:00 AM
End: 1:00 PM
Label: Morning Study
Confirm save? (Y/N): Y
Result: saved as AV-000001.

Bulk input:
MON|2:00 PM|5:00 PM|Afternoon Study
TUE|10:00 AM|1:00 PM|Free Time
WED|6:00 PM|8:00 PM|Programming
END
Preview:
Line | Day | Start - End | Label | Result
1 | Monday | 2:00 PM - 5:00 PM | Afternoon Study | Valid
2 | Tuesday | 10:00 AM - 1:00 PM | Free Time | Valid
3 | Wednesday | 6:00 PM - 8:00 PM | Programming | Valid
Save all previewed records? (Y/N): Y
Result: saved all 3 records as one batch.

Final records:
AV-000001 | Monday | 9:00 AM - 1:00 PM | Morning Study
AV-000002 | Monday | 2:00 PM - 5:00 PM | Afternoon Study
AV-000003 | Tuesday | 10:00 AM - 1:00 PM | Free Time
AV-000004 | Wednesday | 6:00 PM - 8:00 PM | Programming
```

### Bulk Input (paste multiple records)

Choose `3. Add Multiple Availability (Bulk Input)`, paste one pipe-separated
record per line, then type `END` alone on the last line:

```text
MON|9:00 AM|1:00 PM|Morning Study
MON|2:00 PM|5:00 PM|Afternoon Study
TUE|10:00 AM|1:00 PM|Free Time
WED|6:00 PM|8:00 PM|Programming
END
```

Each row is parsed independently. The program displays a preview table with
valid and invalid rows and explains every invalid row. It saves the batch only
if every row is valid, non-overlapping, and confirmed. If any row is invalid,
the entire batch is rejected and no rows are saved.

### Other availability operations

- **Edit:** enter an `AV-000001` ID, provide replacement values in guided
  prompts, review the preview, and confirm. The ID stays the same.
- **Delete:** enter an availability ID and confirm the deletion.
- **View all / by day:** lists IDs, day, 12-hour start/end times, and label.
- **Search:** matches availability ID, weekday, time, or label (case-insensitive).
- **Weekly Summary:** lists every day, windows, daily free-time totals, and the
  total across the week.
- **Set Default Weekly Availability:** replaces the current week with the same
  confirmed time window on each of the seven days.
- **Reset Day Availability:** previews and removes all records for one
  confirmed weekday only.
- **Save Availability to File:** explicitly saves to `data/availability.csv`.
  Successful changes are also persisted immediately.

### Sector 02 demo and tests

Run the demo from the project root:

```powershell
.\build\taskflow.exe --demo sector-02
```

The demo exercises actual availability parsing, validation, insertion, bulk
insertion, overlap rejection, summary calculations, persistence, and reload.
It uses `build/sector-02-demo-availability.csv`, not the user's availability
file, and starts each run with fresh demo records.

Run the combined test harness for Sectors 01–04:

```powershell
.\build\taskflow.exe --test
ctest --test-dir build --output-on-failure
```

Tests cover weekday and 12-hour time parsing (including midnight/noon),
invalid times and weekdays, invalid/equal/reversed ranges, duplicate and
overlapping records, bulk validation and all-or-nothing saving, empty lists,
CRUD, default/reset behavior, summaries, CSV persistence, malformed files, and
Sector 01 regression cases.

## Sector 03 - Dependency Management

The main menu opens **DEPENDENCY MANAGEMENT** at choice `10`. Relationships
are written as `DEPENDENT_TASK_ID|PREREQUISITE_TASK_ID`; for example,
`TSK-000002|TSK-000001` means task 2 depends on task 1. Add relationships using
guided fields, a one-line quick entry, or a bulk paste terminated by `END`.
Guided and quick entry show a preview and request confirmation. A bulk batch
lists every valid and invalid row and saves nothing unless every row is valid
and confirmed.

The submenu supports add, remove, view all, prerequisites, direct dependents,
readiness checks, blocked/ready task lists, topological order, full validation,
explicit save, dependency search by task ID, graph display, and downstream
importance analysis. Choices 14-17 are search, graph, importance, and return.
The graph is displayed as prerequisite-to-dependent arrows and includes task
IDs and names.

The project implements all ten Sector 03 capabilities:

1. **Add dependencies** only between existing tasks; duplicates, invalid IDs,
   self-dependencies, and cycle-forming edges are rejected.
2. **Remove dependencies** only when the exact record exists; each successful
   change is persisted.
3. **View dependencies** as all records, one task's prerequisites, or direct
   dependent tasks.
4. **Display the directed graph** as prerequisite-to-dependent adjacency
   lines, including branches and isolated tasks.
5. **Generate topological ordering** with Kahn's algorithm. A lexicographically
   ordered set breaks ties so equivalent graph inputs always produce the same
   order.
6. **Detect cycles** with depth-first search; cycle errors include the task-ID
   path when available. Cyclic persisted data is rejected on load.
7. **Identify ready tasks** only when all prerequisites have status
   `Completed`. Already completed tasks are not listed as ready, and readiness
   analysis never changes task status.
8. **Validate the complete graph** for malformed IDs/records, missing task
   references, duplicate edges, self-dependencies, and cycles.
9. **Identify blocked tasks** and list every prerequisite that is missing or
   not `Completed`.
10. **Analyze dependency importance** using graph traversal. The importance
    score is the number of *unique downstream tasks* reachable from the
    selected task; direct-dependent count is also reported separately. This
    count-based score is intentionally explainable and is not a scheduling
    priority.

Dependencies are stored separately in `data/dependencies.csv` and loaded on
startup; task and availability CSV files are untouched. Successful changes
save immediately, and the submenu also offers an explicit save. Saves use a
temporary file and preserve the previous file if installation fails. The
application warns about invalid task references on startup, disables graph
reports until the graph is repaired, and prevents deleting any task referenced
as either a dependent or prerequisite until those links are removed.

### Sector 03 demo and tests

Build and run:

```powershell
cmake --build build --config Debug
.\build\taskflow.exe --demo sector-03
ctest --test-dir build --output-on-failure
```

The demo creates the `Complete Requirements -> Implement Code -> Test
Application` chain through `DependencyManager`, with Complete Requirements
already completed and an additional isolated task. It prints the complete
graph and topological order, identifies Implement Code and the isolated task
as ready, identifies Test Application as blocked, rejects the cycle-forming
edge from Test Application back to Complete Requirements, reports downstream
impact, removes an edge, and validates the resulting graph. It uses only
`build/sector-03-demo-dependencies.csv` and an in-memory sample task list; it
does not read or modify the user's task or dependency data.

```powershell
.\build\taskflow.exe --demo sector-03
```

Example output from the demo:

```text
Dependency graph (prerequisite -> dependent):
TSK-000001: Complete Requirements
  -> TSK-000002: Implement Code
TSK-000002: Implement Code
  -> TSK-000003: Test Application
TSK-000003: Test Application
TSK-000004: Independent Task

Topological order: Complete Requirements -> Implement Code -> Test Application -> Independent Task
Ready tasks:
  Implement Code
  Independent Task
Blocked tasks:
  Test Application (Waiting for Implement Code)

ERROR: Circular dependency detected!
Dependency rejected. Circular dependency detected: TSK-000001 -> TSK-000002 -> TSK-000003 -> TSK-000001

Dependency importance for Complete Requirements:
  Direct dependent tasks: 1
  Downstream dependent tasks: 2
  Importance score: 2 (number of unique downstream tasks)

Removed dependency: Test Application no longer depends on Implement Code.
Dependency graph valid after removal: yes
```

## Complete interactive walkthrough

Starting with an empty `data/tasks.csv`, the following values were entered
one line at a time at the menu and field prompts. This example creates and
edits a task, searches and filters it, sorts and views it, then confirms its
deletion and saves the empty list:

```text
1
Interactive Sample
Created by smoke test
High
30
2027-12-01
Testing
Pending
2
TSK-000001
Edited Interactive Sample
Edit-path smoke test
Critical
45
2027-12-02
Testing
In Progress
5
edited
6
Critical
In Progress
Testing
7
5
4
3
TSK-000001
yes
4
8
0
```

The program reported `Created TSK-000001.` and `Updated TSK-000001.`. Search
for `edited`, the Critical/In Progress/Testing filter, and the deadline sort
each displayed:

```text
TSK-000001 | Edited Interactive Sample | Critical | 45 min | 2027-12-02 | Testing | In Progress
  Edit-path smoke test
```

After entering `yes` at the delete confirmation, it reported
`Deleted TSK-000001.` and viewing tasks showed `No tasks found.` Choice 8
reported `Saved 0 task(s).`; choice 0 then saved and exited.

## Architecture and files

- `src/main.cpp` contains the single `main()` entry point. It routes the
  interactive app, test runner, and demo, and checks the preserved starter
  modules.
- `include/task_management.hpp` defines the task model, enums, filters, sort
  fields, and `TaskManager` interface.
- `cpp_modules/task_management.cpp` implements validation, unique ID
  generation, CRUD, queries, CSV persistence, the menu, and the demo.
- `tests/task_management_tests.cpp` contains the unit and integration test
  cases invoked by `--test` and CTest.
- `include/availability_management.hpp` defines the weekday, availability
  input/window models, time/day helpers, and `AvailabilityManager` interface.
- `cpp_modules/availability_management.cpp` implements Sector 02 validation,
  parsing, overlap checks, CRUD, bulk operations, summaries, persistence, the
  submenu, and demo.
- `include/dependency_management.hpp` defines the dependency record model,
  validation helpers, graph checks, and `DependencyManager` interface.
- `cpp_modules/dependency_management.cpp` implements Sector 03 parsing,
  validation, cycle detection, topological ordering, readiness and blocked-task
  checks, downstream impact, CSV persistence, the submenu, and demo.
- `tests/availability_tests.cpp` contains Sector 02 unit/integration tests.
- `tests/dependency_tests.cpp` covers Sector 03 graph logic, persistence,
  failure handling, and task-deletion protection.
- `c_modules/c_test.c` and `include/c_test.h` are the preserved C11 test
  module; `cpp_modules/cpp_test.cpp` and `include/cpp_test.hpp` retain the
  original C++ starter-module check.
- `data/tasks.csv` stores Sector 01 tasks; `data/availability.csv` stores
  Sector 02 availability separately; `data/dependencies.csv` stores Sector 03
  dependency records.
- `CMakeLists.txt` builds the modules together and registers the test target.

Each task stores its ID, name, description, priority, duration in minutes,
deadline, type, and status. `TaskManager::create`, `update`, and `erase`
validate changes and save a complete replacement file before committing the
new in-memory list. The CSV writer escapes commas and quotes; loading rejects
malformed records and duplicate IDs without replacing current in-memory data.
Saving uses a temporary file and preserves the previous file while installing
the replacement. Status is explicitly managed; a deadline does not
automatically change status. Sector 01 does not add scheduling behavior.

Availability records keep their weekday, start/end minute-of-day values, and
optional label in memory; minute values make comparison and overlap checks
unambiguous. CSV files store canonical 12-hour AM/PM strings. `AV-` IDs are
unique and continue from the highest ID on load. `AvailabilityManager::addMany`
validates all rows and persists the entire candidate list before updating
memory, so a failed batch cannot partially change saved data.

## Copy-ready PowerShell commands

```powershell
Set-Location 'C:\Users\Admin\Desktop\SPL1\TaskFlow'
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
.\build\taskflow.exe
.\build\taskflow.exe --demo sector-02
.\build\taskflow.exe --demo sector-03
.\build\taskflow.exe --test
ctest --test-dir build --output-on-failure
```

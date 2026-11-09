# Sector 01: Task Management

## Purpose

Sector 01 provides the task records used throughout TaskFlow. It supports
validated task creation and maintenance, local CSV persistence, and common
queries without automatically changing a task's status based on its deadline.

## Main functionality

- Load and save task records in `data/tasks.csv`.
- Create, edit, and delete tasks with generated `TSK-` IDs.
- Search task names and descriptions without case sensitivity.
- Filter by priority, status, and type.
- Sort by ID, name, priority, duration, deadline, or status.
- Run the interactive task menu or an isolated Sector 01 demo.

The task model stores an ID, name, description, priority, duration in minutes,
deadline, type, and status. Status is explicitly set by the user; a deadline
does not automatically mark a task overdue or complete.

## Data structures and API

`Task` is the shared record type. `Priority`, `Status`, and `SortField` define
the task's enumerated values and query sort keys. `TaskFilter` contains optional
priority and status criteria plus a type filter.

`TaskManager` owns the in-memory task list and a storage path. Its public API
includes `load`, `save`, `create`, `update`, `erase`, `all`, `search`,
`filter`, and `sorted`. Validation and text conversion are exposed through
`validateTask`, `isValidDate`, `isValidDeadline`, `parsePriority`,
`parseStatus`, and the corresponding `toString` functions.

## Validation and persistence

- Names are required and limited to 120 characters.
- Descriptions may be blank and are limited to 2,000 characters.
- Priority and status must be recognized values.
- Duration is a positive whole number, limited to 1,000,000 minutes.
- Type is required and limited to 60 characters.
- Deadlines must be valid local calendar dates in `YYYY-MM-DD` form or local
  date-times in `YYYY-MM-DDTHH:MM` form. A date-only deadline is interpreted
  as the end of that day.
- Loaded records must have eight fields, valid values, valid unique IDs, and
  unique task IDs.

CSV fields containing commas or quotes are escaped. A missing storage file
starts with an empty task list. Mutations save a candidate list before
updating the manager's in-memory state, so a failed save does not silently
report a successful change.

## Algorithms and edge cases

Search uses a case-insensitive substring match over names and descriptions.
Sorting uses a field-specific comparison; priorities sort highest first,
duration and deadlines sort earliest/smallest first. Malformed CSV rows,
invalid dates, invalid durations, and duplicate IDs are rejected. Deletion of
a task referenced by a dependency is guarded by the integrated interactive
manager until those links are removed.

## Integration

Sector 01 is the source of `Task` records consumed by Sectors 03 and 04.
`runInteractiveTaskManager` loads task, availability, and dependency managers
and routes to the other sector menus. The scheduler consumes task snapshots
and does not mutate task status.

## Tests and demo

`tests/task_management_tests.cpp` covers validation, ID generation, CRUD,
search/filter/sort, CSV persistence and malformed data, and task-management
regressions. The CTest registration runs the shared test harness across all
sectors.

From the TaskFlow project directory:

```powershell
.\build\taskflow.exe --demo sector-01
```

The demo writes only to `build/sector-01-demo-tasks.csv`. Its captured output
is in [`../demos/SECTOR_01_OUTPUT.txt`](../demos/SECTOR_01_OUTPUT.txt). The
demo file persists between runs; generated IDs continue from its existing
records, so the exact IDs in a later run can differ from the captured sample.

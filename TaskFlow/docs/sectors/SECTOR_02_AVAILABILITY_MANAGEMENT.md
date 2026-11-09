# Sector 02: Availability Management

## Purpose

Sector 02 stores a user's recurring weekly availability in a separate manager
and CSV file. Its weekly windows provide working-time constraints to the
Sector 04 scheduler.

## Main functionality

- Add one window through guided prompts, a pipe-delimited quick entry, or a
  validated all-or-nothing bulk batch.
- Edit or remove a window, view all windows or one day, and search records.
- Set a default window across the week or reset a selected day's windows.
- Calculate daily and weekly available minutes.
- Persist availability separately in `data/availability.csv`.

## Data structures and API

`DayOfWeek` enumerates Monday through Sunday. `AvailabilityInput` holds a
proposed day, start/end minute-of-day values, and label; `AvailabilityWindow`
adds the persisted `AV-` ID.

`AvailabilityManager` provides `load`, `save`, `checkInputs`, `addMany`,
`add`, `update`, `erase`, `setDefaultWeekly`, `resetDay`, `all`, `byDay`,
`search`, `dailyMinutes`, and `weeklyMinutes`. Parsing and formatting helpers
include `parseDay`, `parseTime12Hour`, `formatTime12Hour`, and
`validateAvailabilityInput`.

## Validation and edge cases

- Day names and abbreviations are parsed case-insensitively.
- Times use 12-hour `H:MM AM/PM` input and are represented internally as
  minute-of-day integers.
- Each window must start before it ends on the same day.
- Duplicate or overlapping windows on a day are rejected; adjacent windows
  are allowed.
- Labels are optional and limited to 120 characters on one line.
- A bulk operation is committed only when every row is valid and the batch is
  confirmed. It does not partially save a mixed-validity batch.
- CSV records are parsed with quoted-field support; malformed persisted
  records are rejected.

## Algorithms and integration

Overlap is tested by intersecting the two half-open minute ranges on the same
weekday. Daily totals sum window lengths, and the weekly summary aggregates
those day totals. Availability is recurring by weekday, not a dated exception
calendar. Sector 04 intersects these weekly windows with a deadline interval
and subtracts supplied commitments when computing available work time.

## Tests and demo

`tests/availability_tests.cpp` covers weekday/time parsing, invalid ranges,
overlap and duplicate rejection, batch validation, CRUD, summaries, default
and reset behavior, persistence, and malformed input.

From the TaskFlow project directory:

```powershell
.\build\taskflow.exe --demo sector-02
```

The demo writes to `build/sector-02-demo-availability.csv`, separate from
interactive user data. Captured output:
[`../demos/SECTOR_02_OUTPUT.txt`](../demos/SECTOR_02_OUTPUT.txt). The demo
file persists between runs, so generated availability IDs may differ from
those in the captured sample.

# Sector 05: Automatic Schedule Generator

## Objective and status

Sector 05 audits and exercises the existing scheduling engine without rewriting
the broader Sector 01-04 model. It validates overlap prevention, dependency-aware
scheduling, deadline handling, recurring weekly availability, idle-time metrics,
strategy comparison, explanatory output, and human-readable demo timestamps.

| Feature | Status |
|---|---|
| Slot overlap prevention | Implemented and verified |
| Dependency-aware ordering | Implemented and verified |
| Multi-day recurring availability | Implemented and verified |
| Exact and impossible deadline checks | Implemented and verified |
| Unscheduled reasons and classification | Implemented and verified |
| Idle-time calculation | Implemented and verified |
| Five strategy comparison | Implemented and verified |
| Explanation text matches the strategy | Implemented and verified |
| Readable date/time rendering | Implemented and verified |
| Build/test/demo verification | Passed |

## Files

- `include/scheduling_management.hpp` defines the schedule-generation public API,
  strategy metadata, and result structures.
- `cpp_modules/scheduling_management.cpp` contains the slot allocator,
  dependency gating, recurring availability math, metric calculations, and demo
  output generation.
- `src/main.cpp` registers `--demo sector-05` in the CLI dispatcher.
- `tests/scheduling_tests.cpp` adds sector-specific assertions for overlap,
  dependency chains, deadlines, fixed commitments, multi-day windows, idle time,
  determinism, empty input, invalid data, and duplicate IDs.
- `CMakeLists.txt` keeps the project and `ctest` target wired to the same
  executable and test suite.

## Core scheduling rules

1. A scheduled entry is considered occupied for subsequent allocation decisions.
   The allocator rejects any slot where the new task overlaps an already-chosen
   interval in the same recurring window.
2. Dependency readiness is checked from the actual dependency graph. A task is
   not eligible until its prerequisites have been scheduled successfully, and the
   dependent task can become eligible only after the prerequisite's scheduled end
   time.
3. Availability is a recurring weekly calendar, not a date-specific exception
   calendar. The scheduling horizon is the same range used for total available
   time, idle time, and schedule comparison metrics.
4. Deadline checks use `entry.end <= task.deadline`. The engine accepts an exact
   deadline match and reports impossible or infeasible tasks as unscheduled.
5. Unscheduled tasks receive specific reasons, including dependency blocks,
   insufficient continuous availability, deadline exceeded, no available window,
   and invalid task data.
6. The strategy comparison uses the same task set and the same scheduling
   context for each strategy, then picks the best result from actual schedule
   metrics rather than from a fixed strategy preference.
7. Demo output formats times as local `YYYY-MM-DD HH:MM` strings instead of raw
   epoch values.

## Verified scenarios

The project validates the following sector requirements with automated tests:

- three tasks in one window with no overlap
- A -> B -> C dependency scheduling
- multiple prerequisite handling
- blocked prerequisite reporting
- exact deadline acceptance
- impossible deadline rejection or unscheduled reporting
- fragmented availability rejection
- fixed-commitment exclusion
- multi-day recurring availability
- idle-time calculation
- all five strategy comparisons
- deterministic scheduling behavior
- empty input handling
- invalid duration rejection
- duplicate task ID rejection

## Build, demo, and test validation

From the project root:

```powershell
cmake -S . -B build
cmake --build build
.\build\taskflow.exe --demo sector-05
ctest --test-dir build --output-on-failure
```

Latest verified results:

- Build result: succeeded
- CTest result: 1/1 suite passed
- Sector 05 demo result: succeeded with readable local times and a best-strategy
  selection based on actual schedule metrics

## Known limitations

- Tasks are assumed to fit into a single contiguous allocation slot inside one
  recurring availability window; fragmented windows are rejected when no single
  span is available.
- The project uses recurring weekly windows rather than arbitrary date-specific
  calendars.
- The scheduling model remains intentionally lightweight and does not support
  arbitrary preemption or time-zone offset handling.
- Strategy explanations are constructed from the actual selected strategy, but
  they still describe the chosen task in general terms rather than a detailed
  per-minute plan.

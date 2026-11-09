# Sector 04: Scheduling Algorithms & Decision Engine

## Objective and status

Sector 04 orders eligible work without changing stored task statuses. It
integrates the existing `Task`, weekly `AvailabilityWindow`, and dependency
records rather than introducing competing data models.

| Feature | Status |
|---|---|
| Priority-based scheduling | Implemented and tested |
| Earliest Deadline First (EDF) | Implemented and tested |
| Shortest Job First (SJF) | Implemented and tested |
| Least Slack Time First (LST) | Implemented and tested with weekly availability |
| User-defined ranking | Implemented and tested |
| Multi-level tie-breaking | Implemented and tested |
| Deterministic ordering | Implemented and tested |
| Dependency-aware filtering and cycle rejection | Implemented and tested |
| Strategy menu and isolated demo | Implemented and tested |

## Files

- `include/scheduling_management.hpp` defines the strategy, context, result,
  and metric types.
- `cpp_modules/scheduling_management.cpp` implements sorting, eligibility,
  calendar/availability calculations, menu, and demo.
- `tests/scheduling_tests.cpp` tests all strategies, blockers, slack,
  invalid inputs, and repeatability.
- `tests/task_management_tests.cpp` also checks timestamp deadline validation
  and sorting.
- `src/main.cpp` registers `--demo sector-04` and includes Sector 04 in
  `--test`.
- `CMakeLists.txt` adds the scheduling implementation and tests to the C++17
  executable.

## Strategies

Each strategy selects one primary ordering rule:

1. **Priority:** Critical, High, Medium, then Low.
2. **EDF:** Compare deadline date-times chronologically, earliest first.
3. **SJF:** Compare remaining execution minutes, shortest first.
4. **LST:** Compare available working minutes until the deadline minus
   remaining execution minutes, lowest slack first.
5. **Custom:** Compare caller-provided positive integer ranks, lower first.

The current `Task` type has a total `durationMinutes` field but no remaining
duration field. Consequently, SJF and LST use the task duration for both
Pending and In Progress tasks.

## Deadline and availability model

The task manager accepts `YYYY-MM-DD` (interpreted as 23:59 local time) and
`YYYY-MM-DDTHH:MM` or `YYYY-MM-DD HH:MM` local timestamps. Existing date-only
records remain valid. Scheduling's direct API also accepts `HH:MM`, anchored
to the context's current local calendar date, for compact in-memory examples.
EDF compares parsed local date-times, not the input strings.

Sector 02 provides recurring weekly windows, with weekday and minute-of-day
bounds. For a deadline spanning multiple dates, the engine walks local
calendar days, intersects each date's availability with the interval from
`now` to the deadline, and subtracts committed intervals. If no valid windows
exist, the engine returns a validation error rather than inventing time.
Negative slack remains negative and is explicitly displayed as **INFEASIBLE**.

The commitment list is an injectable scheduling-context value. Sector 02 does
not persist commitments, so the interactive menu currently has none to pass;
callers and tests can supply real committed periods through the engine API.

For example, with Monday availability from 16:00 to 22:00, a 20:00 deadline,
and 120 minutes of work:

```text
AvailableWorkMinutes = 20:00 - 16:00 = 240
Slack = 240 - 120 = 120 minutes
```

## Eligibility and dependency impact

The dependency graph is validated before ordering; invalid references,
duplicate edges, and cycles reject the request. Completed tasks are reported
separately. A non-completed task whose prerequisite is not Completed is
reported as blocked with the prerequisite IDs and is never placed in the
eligible order. Only unblocked Pending and In Progress tasks are schedulable;
other statuses are reported as excluded. Input task statuses are never
mutated.

Downstream impact follows the actual prerequisite-to-dependent direction and
counts unique unfinished descendants using graph traversal. A visited set
prevents duplicate counts and infinite traversal.

## Tie-breaking

After the strategy's primary value ties, the engine compares:

1. Earlier deadline.
2. Lower slack.
3. Higher unfinished downstream impact.
4. Shorter remaining duration.
5. Lexicographically lower Task ID.

EDF does not compare the deadline again, SJF does not compare duration again,
and LST does not compare slack again. All deadline, slack, and graph metrics
are calculated before `std::sort`; the comparator only compares stored values.

The sort costs `O(n log n)` for `n` eligible tasks. Downstream-impact
precalculation uses one graph traversal per task, `O(n(n + e))` for `e`
dependency edges. Working-time calculation additionally depends on the number
of calendar days, availability windows, and supplied commitments between
`now` and each eligible deadline.

## Demo dataset and expected orders

The demo fixes one context (Monday 2028-01-03, 16:00, 16:00-22:00 availability)
and applies every strategy to the same in-memory tasks and dependency graph.
Task A and Task C share Critical priority, demonstrating the earlier-deadline
tie-break. A dependent of A is blocked; a Completed task is excluded.

| Task | Priority | Duration | Deadline | State |
|---|---|---:|---|---|
| A | Critical | 120 min | 20:00 | Pending |
| B | High | 60 min | 18:00 | Pending |
| C | Critical | 30 min | 19:00 | Pending |
| D | Medium | 45 min | 21:00 | Pending |
| Blocked dependent | Low | 25 min | 21:30 | Pending; depends on A |
| Already completed | Low | 10 min | 2028-01-03 | Completed |

The custom ranks for the four eligible tasks are A=4, B=2, C=3, and D=1.

| Strategy | Order |
|---|---|
| Priority | C, A, B, D |
| EDF | B, C, A, D |
| SJF | C, D, B, A |
| LST | B, A, C, D |
| Custom (D=1, B=2, C=3, A=4) | D, B, C, A |

The demo runs each strategy twice against the same snapshot and checks that
the results match. It also checks that a missing custom rank is rejected.
Actual output is captured in `../demos/SECTOR_04_OUTPUT.txt`.

## Build, demo, and tests

From the TaskFlow project directory:

```powershell
cmake -S . -B build
cmake --build build
.\build\taskflow.exe --demo sector-04
.\build\taskflow.exe --test .\build\sector04-validation-data
```

The latest verified run built the full target successfully. Automated results:

- Sector 01: 7 tests passed.
- Sector 02: 7 tests passed.
- Sector 03: 8 tests passed.
- Sector 04: 41 checks passed, 0 failed.

## Known limitations

- Availability is a recurring weekly calendar, not a dated exception
  calendar. The engine can subtract explicit committed time intervals, but
  this project has no commitment persistence or menu yet.
- The task model has no remaining-duration field; In Progress work uses
  `durationMinutes`.
- Local system time is used for dates and weekly weekdays; explicit time-zone
  offsets are not supported.
- A very long availability horizon is bounded at 1,000 years to avoid
  unbounded calendar iteration.

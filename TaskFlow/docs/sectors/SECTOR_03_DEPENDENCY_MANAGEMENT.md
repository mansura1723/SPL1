# Sector 03: Dependency Management

## Purpose

Sector 03 records prerequisite relationships between existing tasks. It
validates the resulting directed graph and reports which tasks can proceed,
which are blocked, and how much unfinished downstream work depends on a task.

## Data model and API

`Dependency` stores a `dependentId` and a `prerequisiteId`: the first task
requires the second. `DependencyImpact` reports direct dependents, unique
downstream task IDs, and a count-based importance score.

`DependencyManager` loads and saves `data/dependencies.csv`. Its API supports
adding/removing one relationship or an atomic batch; querying prerequisites
and dependents; searching by task ID; identifying incomplete prerequisites;
validating a graph or batch; detecting cycles; producing topological order;
listing blocked and ready tasks; checking readiness; analyzing downstream
impact; and checking whether a task has references.

## Validation and algorithms

- Both IDs must refer to existing tasks and have the expected task-ID format.
- Self-dependencies, duplicate edges, and cycle-forming relationships are
  rejected.
- Full graph validation checks references, duplicates, self-edges, and cycles;
  invalid persisted graphs are not accepted as valid data.
- Depth-first search detects cycles and returns a cycle path when available.
- Kahn's algorithm produces topological order; a lexicographically ordered
  ready set makes tie order deterministic.
- A task is ready only when every prerequisite is `Completed`. A task with one
  or more missing or incomplete prerequisites is blocked.
- Downstream impact counts unique reachable unfinished tasks. It is an
  explainable graph metric, not a task priority.

Quick and bulk interactive input use
`DEPENDENT_TASK_ID|PREREQUISITE_TASK_ID`. Bulk additions are validated as a
whole before any relationship is persisted. Referenced tasks cannot be
deleted through the interactive task manager until their relationships are
removed.

## Integration

Dependencies refer to Sector 01 task IDs and task statuses. Sector 04 first
validates the graph, then uses prerequisite completion to classify schedulable
and blocked tasks. Graph direction is prerequisite-to-dependent for traversal
and topological ordering, even though each stored record is written as
dependent ID followed by prerequisite ID.

## Tests and demo

`tests/dependency_tests.cpp` covers graph validation, cycle detection,
deterministic topological order, readiness, blockers, downstream impact,
persistence, invalid records, and task-deletion protection.

From the TaskFlow project directory:

```powershell
.\build\taskflow.exe --demo sector-03
```

The demo uses an isolated file at `build/sector-03-demo-dependencies.csv`.
Captured output: [`../demos/SECTOR_03_OUTPUT.txt`](../demos/SECTOR_03_OUTPUT.txt).

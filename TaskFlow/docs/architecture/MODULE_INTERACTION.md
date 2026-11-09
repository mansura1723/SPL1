# Module Interaction

The diagram reflects the source/header dependencies and runtime integration.
The scheduler depends on all three domain models; Sector 03 also uses the
shared `Task` type.

```mermaid
flowchart LR
    Main[src/main.cpp] --> TaskModule[task_management]
    Main --> AvailabilityModule[availability_management]
    Main --> DependencyModule[dependency_management]
    Main --> SchedulingModule[scheduling_management]
    TaskModule -->|TaskManager menu integration| AvailabilityModule
    TaskModule -->|TaskManager menu integration| DependencyModule
    TaskModule -->|Scheduling menu integration| SchedulingModule
    DependencyModule -->|Task records and statuses| TaskModule
    SchedulingModule -->|Task model| TaskModule
    SchedulingModule -->|Weekly windows| AvailabilityModule
    SchedulingModule -->|Edges and graph validation| DependencyModule
    TaskTests[Task management tests] --> TaskModule
    AvailabilityTests[Availability tests] --> AvailabilityModule
    DependencyTests[Dependency tests] --> DependencyModule
    SchedulingTests[Scheduling tests] --> SchedulingModule
```

At the API level, Sector 04 accepts task, availability, and dependency
collections, a `DependencyManager` for graph validation, and a
`SchedulingContext` containing the current time, optional commitments, and
custom ranks. The interactive manager supplies the collections maintained by
the other managers.

The test source files are compiled into the `taskflow` executable and invoked
by `--test`; CTest registers that combined command as
`taskflow_all_sectors`.

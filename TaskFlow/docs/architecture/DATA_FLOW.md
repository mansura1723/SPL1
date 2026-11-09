# Data Flow

```mermaid
flowchart TD
    TaskCSV[(tasks.csv)] --> TaskLoad[TaskManager load]
    AvailabilityCSV[(availability.csv)] --> AvailabilityLoad[AvailabilityManager load]
    DependencyCSV[(dependencies.csv)] --> DependencyLoad[DependencyManager load]
    TaskLoad --> TaskState[In-memory tasks]
    AvailabilityLoad --> AvailabilityState[In-memory weekly windows]
    DependencyLoad --> DependencyState[In-memory dependency edges]
    UserInput[Interactive user input] --> Validation[Parse and validate]
    Validation -->|accepted change| Candidate[Candidate state]
    Candidate -->|successful save| Storage[Sector CSV persistence]
    Storage --> UpdatedState[Updated in-memory manager state]
    TaskState --> Scheduler[Scheduling engine]
    AvailabilityState --> Scheduler
    DependencyState --> Scheduler
    Context[Current time, commitments, custom ranks] --> Scheduler
    Scheduler --> ScheduleResult[Ordered tasks, blockers, completed and excluded tasks, metrics]
    ScheduleResult --> Display[Terminal output]
```

Each manager loads and saves its own CSV file. The task, availability, and
dependency stores are independent; changing one sector does not rewrite the
others. Interactive data CSVs are local application state and are ignored by
the project `.gitignore`.

Sector 04 validates the graph before scheduling, classifies tasks, calculates
metrics, then orders eligible tasks using the selected strategy and
tie-breakers. The result is a report; source task statuses and stored CSV rows
are not changed by scheduling.

Sector demos and tests use dedicated build-directory paths. They are separate
from the interactive files under `data/`.

# Algorithm Design

## Task lifecycle

Task status is a user-managed field, not an automatic state machine. An edit
can explicitly change it; deadlines do not silently transition a task.

```mermaid
flowchart TD
    Start[Interactive task operation] --> Input[Enter or select task fields]
    Input --> Validate{Task fields valid?}
    Validate -->|No| Reject[Report validation error]
    Validate -->|Yes| Candidate[Build candidate task collection]
    Candidate --> Save{Candidate CSV save succeeds?}
    Save -->|No| Error[Report persistence error]
    Save -->|Yes| State[Update in-memory task collection]
    State --> Query[View, search, filter, or sort]
    State --> Edit[Explicitly edit fields or status]
    State --> Delete{Delete requested?}
    Delete -->|Referenced by dependency| Guard[Require dependency removal first]
    Delete -->|No references| Remove[Persist deletion]
```

## Dependency DAG workflow

Stored edges name the dependent first and prerequisite second. Graph traversal
uses the corresponding prerequisite-to-dependent direction.

```mermaid
flowchart TD
    Records[Tasks and dependency records] --> Validate[Validate IDs, references, duplicate and self edges]
    Validate --> Cycle{Cycle exists?}
    Cycle -->|Yes| Invalid[Reject graph or edge]
    Cycle -->|No| DAG[Validated directed acyclic graph]
    DAG --> Topo[Kahn topological sort with ordered ready set]
    DAG --> Ready[Ready when all prerequisites are Completed]
    DAG --> Blocked[Report every incomplete prerequisite]
    DAG --> Impact[Traverse unique downstream tasks]
```

## Scheduling decision workflow

```mermaid
flowchart TD
    Inputs[Tasks, weekly windows, dependencies, context, strategy] --> Graph[Validate dependency graph]
    Graph -->|Invalid| Error[Return invalid result and error]
    Graph -->|Valid| Classify[Separate completed, blocked, excluded, eligible tasks]
    Classify --> Metrics[Calculate deadline, availability, slack, duration and downstream impact]
    Metrics --> Primary[Apply selected strategy's primary ordering key]
    Primary --> TieBreak[Apply deterministic secondary tie-breakers]
    TieBreak --> Result[Return ordered eligible tasks and other task categories]
```

## Strategy keys

Sector 04 supports five primary ordering rules:

1. **Priority:** Critical, High, Medium, then Low.
2. **Earliest Deadline First (EDF):** earliest parsed local deadline.
3. **Shortest Job First (SJF):** shortest task duration.
4. **Least Slack Time First (LST):** lowest available working minutes before
   the deadline minus task duration.
5. **User-defined ranking:** lower positive rank first.

After primary values tie, the scheduler uses earlier deadline, lower slack,
higher unfinished downstream impact, shorter duration, then lexicographically
lower task ID. A strategy does not repeat its own primary key as a secondary
comparison. Availability is weekly; supplied committed intervals are
subtracted from working time. The scheduler reports infeasible negative slack
and does not mutate task records.

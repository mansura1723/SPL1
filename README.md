# TaskFlow

TaskFlow is a C++17 command-line project for managing tasks, recurring weekly
availability, task dependencies, and constraint-aware schedules.

The application and its full documentation live in [`TaskFlow/`](./TaskFlow/).
Start with the [project guide](./TaskFlow/README.md) for prerequisites, build
and test commands, interactive usage, and sector documentation.

## Quick start

From the repository root, with CMake 3.16+ and a C++17-capable compiler:

```powershell
cmake -S TaskFlow -B TaskFlow/build -G "MinGW Makefiles"
cmake --build TaskFlow/build
ctest --test-dir TaskFlow/build --output-on-failure
```

On Ubuntu and other systems with an appropriate default CMake generator, omit
`-G "MinGW Makefiles"` from the configure command.

Run the application from the `TaskFlow/` directory so its relative data paths
resolve as documented:

```sh
cd TaskFlow
./build/taskflow --help
```

On Windows, run `.\build\taskflow.exe --help` from PowerShell instead.

## Documentation

- [Project guide and sector overview](./TaskFlow/README.md)
- [Sector documentation](./TaskFlow/docs/sectors/)
- [Architecture and data flow](./TaskFlow/docs/architecture/)
- [Captured demo output](./TaskFlow/docs/demos/)

The repository does not currently include a software license. See the project
guide for that status; no license is implied by this README.

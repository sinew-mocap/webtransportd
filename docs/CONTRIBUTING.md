# Contributing to WebTransportD

## Code Organization

The daemon is laid out as a ports-and-adapters hexagon, matching the
sinew-mocap cluster convention (top-level `core/` + `ports/` +
`adapters/`). Headers sit beside their `.c` and are included by bare name.

| Directory      | Purpose                                                        |
|----------------|----------------------------------------------------------------|
| `core/`        | Hexagon core: session policy, frame codec, work queue — no QUIC/OS dependencies |
| `ports/`       | Port interfaces (header-only)                                  |
| `adapters/`    | Driven/driving edges and composition root: picoquic transport, child process, child-output reader, autocert, logging, CLI, `main.c` |
| `tests/`       | Unit, property, and end-to-end tests                          |
| `examples/`    | Example child programs                                        |
| `prototypes/`  | Standalone HTTP/3 experiments (not built by default)          |
| `docs/`        | Documentation                                                 |
| `scripts/`     | Build/test scripts                                            |

## Building

1. **Prerequisites**:
   - CMake 3.16+
   - GCC or Clang (MSYS2 + mingw-w64 on Windows)
   - No system TLS package: mbedtls, picoquic, and picotls are vendored under `thirdparty/`

2. **Build**:
   ```bash
   make
   ```

3. **Run Tests**:
   ```bash
   make test
   ```
   The test suite builds and runs under `ctest` with `-fsanitize=address,undefined`.

## Code Style

- **Indentation**: tabs.
- **Naming**: `snake_case` for variables/functions, `SCREAMING_SNAKE_CASE` for macros.
- **Headers**: `#ifndef` include guards (one per header).
- **Commits**: Atomic, with clear sentence-case messages (e.g., "Fix datagram length check in frame.c").

## Pull Requests

- **Branch**: a short descriptive name (e.g., `add-merge-queue-governance`).
- **Tests**: Include unit tests for new behavior.
- **Docs**: Update `docs/` if adding new APIs or behaviors.
- **Merge queue**: `main` is protected; pull requests land through the repository merge queue once required checks pass, not via direct push.

# Tests

pytest suite for DroidSource, managed by [uv](https://docs.astral.sh/uv/).

## Setup

```bash
cd tests
uv sync    # first time: creates .venv and installs deps
```

Requires `data/bin/` populated with `scrcpy.exe`, `scrcpy-server`, and DLLs (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

## Wire tests

Validate the raw H.264 packet stream from patched scrcpy. No OBS required; needs a connected Android device (or emulator).

```bash
# defaults to emulator-5554; or set ADB_SERIAL=<serial>
uv run pytest wire/ -v
```

## E2E tests

Validate the full pipeline through OBS. Requires OBS running with WebSocket enabled (Tools → WebSocket Server Settings, port 4455).

```bash
OBS_PASSWORD=<your-password> ADB_SERIAL=<serial> uv run pytest e2e/ -v
```

## Environment variables

| Variable         | Default         | Description                                   |
| ---------------- | --------------- | --------------------------------------------- |
| `OBS_HOST`       | `127.0.0.1`     | OBS WebSocket host                            |
| `OBS_PORT`       | `4455`          | OBS WebSocket port                            |
| `OBS_PASSWORD`   | _(empty)_       | OBS WebSocket password                        |
| `ADB_SERIAL`     | `emulator-5554` | ADB device serial                             |
| `SCRCPY_BIN_DIR` | `data/bin`      | Directory with `scrcpy.exe` + `scrcpy-server` |

## OBS WebSocket

OBS WebSocket enable testing without clicking. Enable it under **Tools → WebSocket Server Settings** (default port 4455).

The E2E tests use this to:

- Create and remove sources programmatically
- Read source properties
- Capture screenshots to verify frames

Useful for verifying multiple simultaneous scrcpy sources without manual UI interaction.

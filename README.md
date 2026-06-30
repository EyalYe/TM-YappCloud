# yappcloud — a TaskMaster-C3 "Yapp" source (Todoist)

A self-contained **task source** for [TaskMaster-C3](https://github.com/EyalYe/TaskMaster):
a host server that maps the device REST contract onto **Todoist**, reusing the
existing [`yapp-cli`](../yapp-cli) code (`get_api` + the paginator/priority
handling from `todolst.py`). The device speaks only the contract; this proxy
hides every Todoist-library quirk server-side. It's the `todomark`-on-hardware
experience.

This repo is one half of a **source product**: the host server (here) plus the
device app component (added under `app/` once the TaskMaster Task Manager lands).
Keeping it in its own repo means core and app development stay independent — the
device's stable app API + REST contract are the only coupling.

## The contract

```
GET  /tasks                      -> { "etag", "tasks":[ {id, parent_id, title,
                                      due, priority(1..4, 4=highest), done}, ... ] }
POST /tasks/{id}/complete        -> close the Todoist task
POST /tasks/{id}/postpone {due}  -> reschedule (due string, e.g. "tomorrow")
GET  /health                     -> { "ok": true }   (no token needed)
```

## Setup & run

Needs a Todoist token. The proxy reuses `~/yapp-cli/.env` (override the scripts
path with `YAPP_CLI_SCRIPTS`).

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python server.py --port 8080
```

Then point the device's "Yapp" source URL at `http://<this-machine-ip>:8080`.

## Verify

```bash
curl localhost:8080/health        # works without a token
curl localhost:8080/tasks         # your real Todoist tasks, in contract shape
```

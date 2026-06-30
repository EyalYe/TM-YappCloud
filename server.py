#!/usr/bin/env python3
"""
yappcloud — the "Yapp" task source for TaskMaster-C3, backed by Todoist.

Maps the device REST contract (the "known endpoints") onto Todoist by reusing
the existing ~/yapp-cli code (get_api + the paginator/priority handling from
todolst.py). The device speaks only the contract; this proxy hides every
Todoist-library quirk server-side.

  GET  /tasks                      -> { "etag", "tasks":[ {id,parent_id,title,
                                        due,priority(1..4),done}, ... ] }
  POST /tasks/{id}/complete        -> close the Todoist task
  POST /tasks/{id}/postpone {due}  -> reschedule (due string, e.g. "tomorrow")
  GET  /health                     -> { "ok": true }  (no token needed)

Run:  python3 server.py [--port 8080]
Needs: pip install -r requirements.txt, and a Todoist token in ~/yapp-cli/.env
(reused via YAPP_CLI_SCRIPTS, default ~/yapp-cli/scripts).
"""
import argparse
import hashlib
import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_PORT = 8080
TITLE_MAX = 47                      # <= device TASK_TITLE_MAX-1
PRIO_MIN, PRIO_MAX = 1, 4           # Todoist already uses 4 = P1 (highest)

# Reuse ~/yapp-cli's get_api() (token loading + TodoistAPI wiring).
YAPP_SCRIPTS = os.environ.get(
    "YAPP_CLI_SCRIPTS", os.path.expanduser("~/yapp-cli/scripts"))
sys.path.insert(0, YAPP_SCRIPTS)

_api = None


def api():
    """Lazily build the TodoistAPI so /health works without a token."""
    global _api
    if _api is None:
        from _yapp_common import get_api          # noqa: E402 (path injected above)
        _api = get_api()
    return _api


def _flatten(response):
    """Todoist libs return a list OR a paginator of lists — yield Task objects."""
    items = response if isinstance(response, list) else list(response)
    tasks = []
    for it in items:
        if isinstance(it, list):
            tasks.extend(it)
        else:
            tasks.append(it)
    return [t for t in tasks if hasattr(t, "content")]


def _due_str(task):
    due = getattr(task, "due", None)
    if not due:
        return ""
    return getattr(due, "date", None) or getattr(due, "datetime", None) or ""


def list_tasks():
    tasks = _flatten(api().get_tasks())
    out = []
    for t in tasks:
        prio = int(getattr(t, "priority", PRIO_MIN) or PRIO_MIN)
        out.append({
            "id": str(getattr(t, "id", "")),
            "parent_id": str(getattr(t, "parent_id", "") or ""),
            "title": (getattr(t, "content", "") or "")[:TITLE_MAX],
            "due": str(_due_str(t)),
            "priority": max(PRIO_MIN, min(PRIO_MAX, prio)),
            "done": False,
        })
    out.sort(key=lambda x: x["priority"], reverse=True)   # highest first (§8.2)
    return out


def complete(task_id):
    a = api()
    if hasattr(a, "close_task"):
        a.close_task(task_id=task_id)
    else:
        a.delete_task(task_id=task_id)


def postpone(task_id, due):
    api().update_task(task_id=task_id, due_string=due or "today")


def etag_for(tasks):
    raw = json.dumps(tasks, sort_keys=True).encode()
    return hashlib.sha1(raw).hexdigest()[:16]


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj=None):
        body = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send(200, {"ok": True})
        elif self.path.startswith("/tasks"):
            try:
                tasks = list_tasks()
                self._send(200, {"etag": etag_for(tasks), "tasks": tasks})
            except SystemExit:
                self._send(503, {"error": "no Todoist token (see ~/yapp-cli/.env)"})
            except Exception as e:
                self._send(502, {"error": f"todoist: {e}"})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        m = re.match(r"^/tasks/([^/]+)/(complete|postpone)$", self.path)
        if not m:
            self._send(404, {"error": "not found"})
            return
        task_id, action = m.group(1), m.group(2)
        try:
            if action == "complete":
                complete(task_id)
            else:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length) if length else b"{}"
                due = json.loads(body or b"{}").get("due", "")
                postpone(task_id, due)
            self._send(200, {"ok": True})
        except Exception as e:
            self._send(502, {"error": f"todoist: {e}"})

    def log_message(self, fmt, *args):
        print("yappcloud:", fmt % args)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = ap.parse_args()
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"yappcloud (Todoist) serving the device contract on http://0.0.0.0:{args.port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()

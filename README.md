# yappcloud — a TaskMaster-C3 "Yapp" source (direct Todoist)

The "Yapp" task source for [TaskMaster-C3](https://github.com/EyalYe/TaskMaster):
the device app talks **directly to Todoist** over HTTPS — **no host server / proxy
to run**. It's the `todomark`-on-hardware experience, served straight from the
cloud.

This repo holds the **device app component only** (under `app/`, added with the
Task Manager work). It depends only on the stable TaskMaster app API + the
generic UI/async facilities core provides (core ⟂ userspace). Tasks themselves are
userspace: this app fetches Todoist's JSON over `esp_http_client` + `esp-tls`
(`esp_crt_bundle` for the cert chain), parses it into the shared `task_t` model,
and renders via core's generic list widget.

## How it talks to Todoist

```
GET  https://api.todoist.com/rest/v2/tasks      (Bearer <token>)   -> active tasks
POST https://api.todoist.com/rest/v2/tasks/{id}/close              -> complete
POST https://api.todoist.com/rest/v2/tasks/{id}  { due_string }    -> postpone
```

The **Todoist token** is the app's declared config (`TASKMASTER_REGISTER_APP_CONFIG`,
a paste field in the device's setup form) — never hardcoded, never in core.

## Install on the device

Add it to a TaskMaster firmware build's `main/idf_component.yml`:

```yaml
dependencies:
  tm_yapp:
    git: git@github.com:EyalYe/TM-YappCloud.git
    path: app
```

The app self-registers; configure your Todoist token via the device's Wi-Fi setup
form. No server, no LAN box — just the device and Todoist.

> The earlier `yapp_server` proxy was **dropped**: the device reaches Todoist
> directly, so there's nothing to host. (The LAN-box approach still exists as a
> separate source — see `yapplocal`.)

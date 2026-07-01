/*
 * app_yapp.c — the "Yapp" device app for TaskMaster-C3 (DIRECT Todoist, HTTPS).
 *
 * Talks to api.todoist.com directly — no proxy (PLAN §8). The fetch runs through
 * core's async_job (off the UI task, cooperative cancel); the Todoist token is
 * app-declared config (§9.4), pasted in the setup form. esp-tls + esp_crt_bundle
 * provide TLS; the session is opened, fetched, parsed into the task model, and
 * torn down each fetch (not held), keeping heap flat (§8 note).
 *
 *   GET  https://api.todoist.com/rest/v2/tasks            (Bearer)  → active tasks
 *   POST https://api.todoist.com/rest/v2/tasks/{id}/close (Bearer)  → complete
 *
 * Parser: cJSON on a hard-capped buffer (the review's payload risk is bounded
 * here; jsmn/SAX is the documented upgrade if a large account proves heavy).
 */
#include "app.h"
#include "input.h"
#include "ui_frame.h"
#include "net_status.h"
#include "app_store.h"
#include "app_config.h"
#include "async_job.h"
#include "tasks.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app.yapp";

#define YAPP_NS           "yapp"
#define YAPP_TOKEN_MAX    64
#define YAPP_BODY_MAX     32768   /* hard cap on the Todoist response */
#define YAPP_HTTP_TMO_MS  15000   /* HTTPS is slower than LAN */
#define YAPP_API_TASKS    "https://api.todoist.com/api/v1/tasks"   /* unified v1 (rest/v2 retired) */

/* LIST: click opens the detail submenu (§8.5 step-10 polish); Select one-tap Done.
 * Offline click is disabled (submenu actions need the network). */
static const control_hints_t YAPP_HINTS         = { .rotate = "<>", .click = "MNU", .select = "DON" };
static const control_hints_t YAPP_HINTS_OFFLINE = { .rotate = "<>", .click = "---", .select = "DON" };
static const control_hints_t YAPP_HINTS_MENU    = { .rotate = "<>", .click = "SEL", .select = "SEL" };
static const control_hints_t YAPP_HINTS_DESC    = { .rotate = "  ", .click = "BAK", .select = "BAK" };

static app_store_t   s_store;
static char          s_token[YAPP_TOKEN_MAX];
static task_view_t   s_view;
static task_queue_t  s_queue;     /* completes done offline, replayed on reconnect */
static task_detail_t s_detail;    /* per-task action submenu / description view */
static bool          s_syncing;
static bool          s_error;
static bool          s_online;    /* last-seen connectivity — to fire replay on reconnect */
static async_job_t  *s_job;

/* App config (§9.4): the Todoist API token (paste field, masked). */
static const app_cfg_field_t YAPP_CFG[] = {
    { .key = "token", .label = "Todoist token", .type = ACFG_STR, .input = ACFG_PASTE,
      .secret = true, .max_len = YAPP_TOKEN_MAX - 1 },
};
TASKMASTER_REGISTER_APP_CONFIG(YAPP_NS, "Yapp", YAPP_CFG);

/* The HTTP client handle is worker-OWNED (a local in each *_work function), never a
 * shared static: esp_http_client is not thread-safe, so the UI thread must never
 * touch it. Cancel is cooperative — the worker polls async_job_cancelled() in its
 * read loop and tears its own client down; exit() only sets the flag (§6A, step 13
 * fix: the old cross-thread esp_http_client_close() abort crashed on Home-mid-fetch). */

typedef struct {
    char   token[YAPP_TOKEN_MAX];
    int    count;
    bool   ok;
    task_t items[TASK_MAX];
} fetch_ctx_t;

static void copy_field(char *dst, cJSON *obj, const char *key, size_t dst_sz)
{
    cJSON *it = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        strlcpy(dst, it->valuestring, dst_sz);
    }
}

/* Todoist v1 returns { "results": [ {task}, ... ], "next_cursor": ... }. Map each:
 * content→title, priority (1..4, 4=urgent=P1) passes through, due.date→due,
 * parent_id (null→""), checked→done. (Pagination via next_cursor is ignored — we
 * take the first page, capped at TASK_MAX; fine for small accounts.) */
static bool parse_todoist(const char *json, fetch_ctx_t *f)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON *arr = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return false;
    }
    int n = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= TASK_MAX) break;
        task_t *t = &f->items[n];
        memset(t, 0, sizeof(*t));
        copy_field(t->id,      it, "id",      sizeof(t->id));
        copy_field(t->title,   it, "content", sizeof(t->title));
        cJSON *parent = cJSON_GetObjectItem(it, "parent_id");
        if (cJSON_IsString(parent) && parent->valuestring) {
            strlcpy(t->parent_id, parent->valuestring, sizeof(t->parent_id));
        }
        cJSON *due = cJSON_GetObjectItem(it, "due");
        if (cJSON_IsObject(due)) {
            copy_field(t->due, due, "date", sizeof(t->due));
        }
        cJSON *p = cJSON_GetObjectItem(it, "priority");
        t->priority = cJSON_IsNumber(p) ? (uint8_t)p->valueint : TASK_PRIO_MIN;
        t->done = cJSON_IsTrue(cJSON_GetObjectItem(it, "checked"));
        n++;
    }
    cJSON_Delete(root);
    f->count = n;
    return true;
}

static esp_http_client_handle_t todoist_client(const char *url, const char *token,
                                               esp_http_client_method_t method)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = YAPP_HTTP_TMO_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char auth[YAPP_TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(c, "Authorization", auth);
    return c;
}

static bool fetch_work(async_job_t *job, void *ctx)
{
    fetch_ctx_t *f = (fetch_ctx_t *)ctx;
    f->count = 0;
    f->ok = false;

    size_t heap0 = esp_get_free_heap_size();
    esp_http_client_handle_t client = todoist_client(YAPP_API_TASKS, f->token, HTTP_METHOD_GET);

    char *body = malloc(YAPP_BODY_MAX);
    if (body && esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int total = 0, r;
        /* Poll the cancel flag each iteration so Home mid-fetch bails cooperatively
         * (no cross-thread client teardown — that crashed, step 13 fix). */
        while (total < YAPP_BODY_MAX - 1 && !async_job_cancelled(job) &&
               (r = esp_http_client_read(client, body + total, YAPP_BODY_MAX - 1 - total)) > 0) {
            total += r;
        }
        body[total] = '\0';
        int status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        if (!async_job_cancelled(job) && status == 200 && total > 0) {
            f->ok = parse_todoist(body, f);
        } else {
            ESP_LOGW(TAG, "GET status=%d len=%d", status, total);
        }
    }
    free(body);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "fetch: ok=%d count=%d heap %u->%u (min %u)",
             f->ok, f->count, (unsigned)heap0, (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    return f->ok;
}

static void fetch_done(void *ctx, bool ok)
{
    fetch_ctx_t *f = (fetch_ctx_t *)ctx;
    s_syncing = false;
    s_job = NULL;
    if (ok) {
        memcpy(s_view.items, f->items, (size_t)f->count * sizeof(task_t));
        task_view_set_count(&s_view, f->count);
        s_error = false;
    } else {
        s_error = true;
    }
}

static void do_sync(void)
{
    if (s_token[0] == '\0' || !net_is_online() || s_syncing) {
        return;
    }
    fetch_ctx_t f = {0};
    strlcpy(f.token, s_token, sizeof(f.token));
    s_job = async_job_submit(fetch_work, fetch_done, &f, sizeof(f));
    s_syncing = (s_job != NULL);
}

/* ── complete: POST /tasks/{id}/close ── */
typedef struct { char token[YAPP_TOKEN_MAX]; char id[TASK_ID_MAX]; bool ok; } close_ctx_t;

static bool close_work(async_job_t *job, void *ctx)
{
    (void)job;
    close_ctx_t *c = (close_ctx_t *)ctx;
    char url[sizeof(YAPP_API_TASKS) + TASK_ID_MAX + 8];
    snprintf(url, sizeof(url), "%s/%s/close", YAPP_API_TASKS, c->id);
    esp_http_client_handle_t client = todoist_client(url, c->token, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, "", 0);
    esp_err_t err = esp_http_client_perform(client);   /* bounded by timeout_ms */
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    c->ok = (err == ESP_OK && status >= 200 && status < 300);
    ESP_LOGI(TAG, "close %s: ok=%d status=%d", c->id, c->ok, status);
    return c->ok;
}

static void close_done(void *ctx, bool ok) { (void)ctx; (void)ok; s_syncing = false; s_job = NULL; do_sync(); }

/* ── postpone: POST /tasks/{id} { "due_string":"tomorrow" } (detail submenu) ── */
typedef struct { char token[YAPP_TOKEN_MAX]; char id[TASK_ID_MAX]; bool ok; } post_ctx_t;

static bool postpone_work(async_job_t *job, void *ctx)
{
    (void)job;
    post_ctx_t *p = (post_ctx_t *)ctx;
    char url[sizeof(YAPP_API_TASKS) + TASK_ID_MAX + 2];
    snprintf(url, sizeof(url), "%s/%s", YAPP_API_TASKS, p->id);
    esp_http_client_handle_t client = todoist_client(url, p->token, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    static const char BODY[] = "{\"due_string\":\"tomorrow\"}";
    esp_http_client_set_post_field(client, BODY, sizeof(BODY) - 1);
    esp_err_t err = esp_http_client_perform(client);   /* bounded by timeout_ms */
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    p->ok = (err == ESP_OK && status >= 200 && status < 300);
    ESP_LOGI(TAG, "postpone %s: ok=%d status=%d", p->id, p->ok, status);
    return p->ok;
}

static void postpone_done(void *ctx, bool ok)
{
    (void)ctx; (void)ok;
    s_syncing = false;
    s_job = NULL;
    s_detail.mode = TASK_VIEW_LIST;   /* back to the list, then confirm via re-sync */
    do_sync();
}

/* ── view description: GET /tasks/{id} → "description" (on-demand, not stored) ── */
typedef struct { char token[YAPP_TOKEN_MAX]; char id[TASK_ID_MAX]; char desc[TASK_DESC_MAX]; bool ok; } desc_ctx_t;

static bool desc_work(async_job_t *job, void *ctx)
{
    desc_ctx_t *d = (desc_ctx_t *)ctx;
    d->desc[0] = '\0';
    d->ok = false;
    char url[sizeof(YAPP_API_TASKS) + TASK_ID_MAX + 2];
    snprintf(url, sizeof(url), "%s/%s", YAPP_API_TASKS, d->id);
    esp_http_client_handle_t client = todoist_client(url, d->token, HTTP_METHOD_GET);
    char *body = malloc(YAPP_BODY_MAX);
    if (body && esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int total = 0, r;
        while (total < YAPP_BODY_MAX - 1 && !async_job_cancelled(job) &&
               (r = esp_http_client_read(client, body + total, YAPP_BODY_MAX - 1 - total)) > 0) {
            total += r;
        }
        body[total] = '\0';
        int status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        if (!async_job_cancelled(job) && status == 200 && total > 0) {
            cJSON *root = cJSON_Parse(body);
            cJSON *desc = cJSON_GetObjectItem(root, "description");
            if (cJSON_IsString(desc) && desc->valuestring) {
                strlcpy(d->desc, desc->valuestring, sizeof(d->desc));
            }
            cJSON_Delete(root);
            d->ok = true;
        }
        ESP_LOGI(TAG, "desc %s: status=%d len=%d desc_len=%d",
                 d->id, status, total, (int)strlen(d->desc));
    }
    free(body);
    esp_http_client_cleanup(client);
    return d->ok;
}

static void desc_done(void *ctx, bool ok)
{
    desc_ctx_t *d = (desc_ctx_t *)ctx;
    s_syncing = false;
    s_job = NULL;
    s_detail.desc_loading = false;
    if (ok) {
        strlcpy(s_detail.desc, d->desc, sizeof(s_detail.desc));
    }
}

/* Detail-submenu action submitters (online, one job at a time). */
static void do_postpone(void)
{
    if (s_detail.task < 0 || s_detail.task >= s_view.count || s_syncing || !net_is_online()) {
        return;
    }
    post_ctx_t p = {0};
    strlcpy(p.token, s_token, sizeof(p.token));
    strlcpy(p.id, s_view.items[s_detail.task].id, sizeof(p.id));
    s_job = async_job_submit(postpone_work, postpone_done, &p, sizeof(p));
    s_syncing = (s_job != NULL);
}

static void do_view_desc(void)
{
    if (s_detail.task < 0 || s_detail.task >= s_view.count || s_syncing || !net_is_online()) {
        return;
    }
    desc_ctx_t d = {0};
    strlcpy(d.token, s_token, sizeof(d.token));
    strlcpy(d.id, s_view.items[s_detail.task].id, sizeof(d.id));
    s_job = async_job_submit(desc_work, desc_done, &d, sizeof(d));
    if (s_job) {
        s_syncing = true;
        s_detail.mode = TASK_VIEW_DESC;
        s_detail.desc_loading = true;
        s_detail.desc[0] = '\0';
    }
}

/* ── offline write queue: replay queued completes on reconnect (§8.5 step 12) ── */
static void drain_queue(void);

static void replay_done(void *ctx, bool ok)
{
    (void)ctx;
    s_syncing = false;
    s_job = NULL;
    if (ok) {
        task_queue_pop_head(&s_queue);           /* replayed → drop it */
    } else if (task_queue_fail_head(&s_queue)) {
        ESP_LOGW(TAG, "dropping poison queue entry after %d tries", TASK_QUEUE_TRIES);
        task_queue_pop_head(&s_queue);           /* poison → give up on it */
    } else {
        task_queue_save(&s_store, &s_queue);     /* persist the bumped try count */
        return;                                  /* transient: retry next reconnect */
    }
    task_queue_save(&s_store, &s_queue);
    if (s_queue.n > 0) {
        drain_queue();   /* next queued close */
    } else {
        do_sync();       /* queue drained → refresh the list from the server */
    }
}

/* Submit the head of the queue as a close job; chains via replay_done. */
static void drain_queue(void)
{
    if (s_queue.n == 0 || !net_is_online() || s_syncing) {
        return;
    }
    close_ctx_t c = {0};
    strlcpy(c.token, s_token, sizeof(c.token));
    strlcpy(c.id, s_queue.ids[0], sizeof(c.id));
    s_job = async_job_submit(close_work, replay_done, &c, sizeof(c));
    if (s_job) {
        s_syncing = true;
    }
}

/* Drop the selected task from the view (optimistic completion). */
static void view_remove(int sel)
{
    for (int i = sel; i < s_view.count - 1; i++) {
        s_view.items[i] = s_view.items[i + 1];
    }
    task_view_set_count(&s_view, s_view.count - 1);
}

static void do_complete(void)
{
    int sel = task_view_sel(&s_view);
    if (sel < 0 || sel >= s_view.count || s_syncing) {
        return;
    }
    if (!net_is_online()) {
        /* Offline: queue the close + remove optimistically; persist both so the
         * completion survives a reboot and replays on reconnect (§8.5 step 12). */
        if (task_queue_push(&s_queue, s_view.items[sel].id)) {
            task_queue_save(&s_store, &s_queue);
            view_remove(sel);
            task_cache_save(&s_store, &s_view);   /* keep cache consistent with the view */
        }
        return;
    }
    close_ctx_t c = {0};
    strlcpy(c.token, s_token, sizeof(c.token));
    strlcpy(c.id, s_view.items[sel].id, sizeof(c.id));
    s_job = async_job_submit(close_work, close_done, &c, sizeof(c));
    if (s_job) {
        s_syncing = true;
        view_remove(sel);
    }
}

/* ── app lifecycle ── */
static void yapp_init(void)
{
    task_view_init(&s_view, UI_ROWS);
    task_detail_reset(&s_detail);
    s_syncing = false;
    s_error = false;
    s_job = NULL;
    app_store_open(&s_store, YAPP_NS);
    app_store_get_str(&s_store, "token", s_token, sizeof(s_token), "");
    task_cache_load(&s_store, &s_view);    /* show tasks instantly, even offline */
    task_queue_load(&s_store, &s_queue);
    s_online = net_is_online();
    if (s_online && s_queue.n > 0) {
        drain_queue();   /* replay pending completes, then it re-syncs */
    } else {
        do_sync();       /* no-op when offline */
    }
}

static void yapp_on_event(uint8_t ev)
{
    /* Detail submenu (per-task actions) and description view take over input. */
    if (s_detail.mode == TASK_VIEW_MENU) {
        switch (ev) {
        case EV_ENCODER_CW:  ui_list_move(&s_detail.menu, +1); break;
        case EV_ENCODER_CCW: ui_list_move(&s_detail.menu, -1); break;
        case EV_ENCODER_CLICK:
        case EV_SELECT:
            switch (ui_list_sel(&s_detail.menu)) {
            case TASK_ACT_VIEW:     do_view_desc();                       break;
            case TASK_ACT_POSTPONE: do_postpone();                        break;
            case TASK_ACT_SYNC:     s_detail.mode = TASK_VIEW_LIST; do_sync(); break;
            case TASK_ACT_BACK:     s_detail.mode = TASK_VIEW_LIST;        break;
            default: break;
            }
            break;
        default: break;
        }
        return;
    }
    if (s_detail.mode == TASK_VIEW_DESC) {
        if (ev == EV_ENCODER_CLICK || ev == EV_SELECT) s_detail.mode = TASK_VIEW_MENU;
        return;
    }

    switch (ev) {
    case EV_ENCODER_CW:    task_view_move(&s_view, +1); break;
    case EV_ENCODER_CCW:   task_view_move(&s_view, -1); break;
    case EV_ENCODER_CLICK:
        /* Open the per-task action submenu (online only — its actions need net). */
        if (net_is_online() && s_view.count > 0 && !s_syncing) {
            task_detail_open(&s_detail, task_view_sel(&s_view));
        }
        break;
    case EV_SELECT:        do_complete();               break;
    default: break;
    }
}

static void yapp_render(void)
{
    lv_obj_clean(ui_frame_content());

    /* The UI task re-runs render() on every connectivity change, so this is where we
     * notice a reconnect and replay queued completes / resume syncing (§8.5 step 12). */
    bool online = net_is_online();
    if (online && !s_online) {
        if (s_queue.n > 0) drain_queue();
        else               do_sync();
    }
    s_online = online;

    /* Detail submenu / description view own the whole screen (set hints first so the
     * content area is sized before drawing). */
    if (s_detail.mode == TASK_VIEW_MENU) {
        ui_frame_set_hints(&YAPP_HINTS_MENU);
        task_menu_render(&s_detail, &s_view);
        return;
    }
    if (s_detail.mode == TASK_VIEW_DESC) {
        ui_frame_set_hints(&YAPP_HINTS_DESC);
        task_desc_render(&s_detail, &s_view);
        return;
    }

    ui_frame_set_hints(online ? &YAPP_HINTS : &YAPP_HINTS_OFFLINE);

    if (s_token[0] == '\0') {
        ui_text_row(0, "Set token in");
        ui_text_row(1, "setup (Yapp)");
        return;
    }
    if (!online) {
        task_view_render_offline(&s_view, s_queue.n);   /* cached list + OFFLINE banner */
        return;
    }
    if (s_syncing && s_view.count == 0) {
        ui_text_row(0, "Syncing...");
        return;
    }
    if (s_error && s_view.count == 0) {
        ui_text_row(0, "Sync failed");
        ui_text_row(1, "click to retry");
        return;
    }
    task_view_render(&s_view);
}

static void yapp_exit(void)
{
    if (s_job) {
        async_job_cancel(s_job);
        s_job = NULL;
    }
    task_cache_save(&s_store, &s_view);   /* persist for the next (maybe offline) open */
    app_store_close(&s_store);
}

/* Launcher visibility: hide Yapp until a Todoist token is configured (§8.5). Runs
 * while the app is inactive, so read the token straight from our NVS namespace. */
static bool yapp_available(void)
{
    app_store_t st;
    char token[YAPP_TOKEN_MAX] = {0};
    if (app_store_open(&st, YAPP_NS) == ESP_OK) {
        app_store_get_str(&st, "token", token, sizeof(token), "");
        app_store_close(&st);
    }
    return token[0] != '\0';
}

static const device_app_t yapp_app = {
    .name      = "Yapp",
    .init      = yapp_init,
    .on_event  = yapp_on_event,
    .render    = yapp_render,
    .exit      = yapp_exit,
    .available = yapp_available,
};

TASKMASTER_REGISTER_APP(yapp_app);

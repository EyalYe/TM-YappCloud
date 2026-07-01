/*
 * tasks.h — the on-device task model + list view (PLAN §8.5 step 9). HEADER-ONLY.
 *
 * USERSPACE (tasks are not a core concept, §8). A bounded task_t array (no
 * per-task malloc, §6A.1) plus a view that formats tasks into the core's generic
 * ui_list — priority marker + parent nesting + title.
 *
 * Both source apps share this thin module by *copying* it into their repo. The
 * functions are `static` (internal linkage) so each app gets its own private copy
 * — two source apps in one firmware binary don't collide at link time. The
 * fetch/parse that fills `items` differs per source (Local = LAN contract; Yapp =
 * direct Todoist).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "ui_list.h"
#include "ui_frame.h"
#include "app_store.h"   /* offline cache + write queue persistence (step 12) */

#define TASK_MAX        50   /* cap on tasks held on-device */
#define TASK_ID_MAX     24   /* id / parent_id string (incl NUL) */
#define TASK_TITLE_MAX  48   /* title (server-truncated) */
#define TASK_DUE_MAX    16   /* due date/label */
#define TASK_PRIO_MIN   1    /* lowest */
#define TASK_PRIO_MAX   4    /* highest (Todoist P1) */
#define TASK_ROW_MAX    40   /* max chars per rendered row */
#define TASK_EMPTY_ROW  0

/* ── offline cache + bounded write queue (§8.5 step 12) ── */
#define TASK_QUEUE_MAX   16        /* max pending writes (completes) held offline */
#define TASK_QUEUE_TRIES 5         /* drop a queued write after this many failed replays (poison guard, step 13) */
#define TASK_BANNER_ROWS 1         /* rows the OFFLINE banner steals from the list */
#define TASK_OFFLINE_ROW 0         /* banner row */
#define TASK_CACHE_KEY   "cache"   /* NVS blob: the cached task_t[] */
#define TASK_CACHE_N_KEY "cachen"  /* NVS u32:  cached task count */
#define TASK_QUEUE_KEY   "wrq"     /* NVS blob: the write queue */

typedef struct {
    char    id[TASK_ID_MAX];
    char    parent_id[TASK_ID_MAX];   /* empty = top-level */
    char    title[TASK_TITLE_MAX];
    char    due[TASK_DUE_MAX];        /* empty = no due date */
    uint8_t priority;                 /* TASK_PRIO_MIN..TASK_PRIO_MAX */
    bool    done;
} task_t;

typedef struct {
    task_t    items[TASK_MAX];
    int       count;
    ui_list_t list;
} task_view_t;

static inline void task_view_init(task_view_t *v, int visible_rows)
{
    v->count = 0;
    ui_list_init(&v->list, visible_rows);
}

static inline void task_view_set_count(task_view_t *v, int count)
{
    v->count = count < 0 ? 0 : (count > TASK_MAX ? TASK_MAX : count);
    ui_list_set_count(&v->list, v->count);
}

static inline void task_view_move(task_view_t *v, int delta)
{
    ui_list_move(&v->list, delta);
}

static inline int task_view_sel(const task_view_t *v)
{
    return v->count > 0 ? ui_list_sel(&v->list) : -1;
}

/* ui_list row text: "<indent>P<n> <title>"; P1 = highest (priority TASK_PRIO_MAX). */
static inline void task_row_text(int index, char *buf, int buf_sz, void *ctx)
{
    const task_view_t *v = (const task_view_t *)ctx;
    const task_t *t = &v->items[index];
    int prio = t->priority;
    if (prio < TASK_PRIO_MIN) prio = TASK_PRIO_MIN;
    if (prio > TASK_PRIO_MAX) prio = TASK_PRIO_MAX;
    int pnum = (TASK_PRIO_MAX + 1) - prio;             /* 4→P1, 1→P4 */
    const char *indent = t->parent_id[0] ? " " : "";   /* subtasks indented */
    snprintf(buf, buf_sz, "%sP%d %s", indent, pnum, t->title);
}

/* Render the task list into the content area. Empty state "No open tasks".
 * Call after lv_obj_clean(content). */
static inline void task_view_render(task_view_t *v)
{
    if (v->count == 0) {
        ui_text_row(TASK_EMPTY_ROW, "No open tasks");
        return;
    }
    /* Full window (restores it after an offline render shrank it by the banner). */
    ui_list_set_rows(&v->list, UI_ROWS);
    ui_list_draw(&v->list, 0, task_row_text, v);
}

/* ── offline cache (NVS blob, written on exit, loaded on init) ── */

/* Persist the current view so it shows even offline / after a reboot (§8.5 step 12).
 * Called on app exit (not per-sync) to bound flash wear. */
static inline void task_cache_save(app_store_t *st, const task_view_t *v)
{
    app_store_set_u32(st, TASK_CACHE_N_KEY, (uint32_t)v->count);
    if (v->count > 0) {
        app_store_set_blob(st, TASK_CACHE_KEY, v->items,
                           (size_t)v->count * sizeof(task_t));
    }
}

/* Load the cached view (no-op when nothing is stored). */
static inline void task_cache_load(app_store_t *st, task_view_t *v)
{
    uint32_t n = 0;
    app_store_get_u32(st, TASK_CACHE_N_KEY, &n, 0);
    if (n > TASK_MAX) n = TASK_MAX;
    if (n == 0) return;
    size_t len = (size_t)n * sizeof(task_t);
    if (app_store_get_blob(st, TASK_CACHE_KEY, v->items, &len) == 0) {
        task_view_set_count(v, (int)(len / sizeof(task_t)));
    }
}

/* ── bounded write queue: completes done offline, replayed on reconnect ── */
typedef struct {
    char    ids[TASK_QUEUE_MAX][TASK_ID_MAX];
    int     n;
    uint8_t tries;   /* failed replay attempts on the head entry ids[0] (poison guard) */
} task_queue_t;

static inline void task_queue_load(app_store_t *st, task_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    size_t len = sizeof(*q);
    app_store_get_blob(st, TASK_QUEUE_KEY, q, &len);   /* missing key → stays zeroed */
    if (q->n < 0 || q->n > TASK_QUEUE_MAX) q->n = 0;
}

static inline void task_queue_save(app_store_t *st, const task_queue_t *q)
{
    app_store_set_blob(st, TASK_QUEUE_KEY, q, sizeof(*q));
}

/* Enqueue a task id; false when the queue is full (the write is then refused). */
static inline bool task_queue_push(task_queue_t *q, const char *id)
{
    if (q->n >= TASK_QUEUE_MAX) return false;
    strlcpy(q->ids[q->n], id, TASK_ID_MAX);
    q->n++;
    return true;
}

static inline void task_queue_remove(task_queue_t *q, int idx)
{
    if (idx < 0 || idx >= q->n) return;
    for (int i = idx; i < q->n - 1; i++) {
        memcpy(q->ids[i], q->ids[i + 1], TASK_ID_MAX);
    }
    q->n--;
}

/* Drop the head entry (a successful — or poison — replay) and reset its try count. */
static inline void task_queue_pop_head(task_queue_t *q)
{
    if (q->n <= 0) return;
    task_queue_remove(q, 0);
    q->tries = 0;   /* the next entry becomes the head */
}

/* Record a failed replay of the head; returns true when it has failed enough times
 * to be considered poison and should be dropped (step 13 — prevents a stuck queue). */
static inline bool task_queue_fail_head(task_queue_t *q)
{
    if (q->n <= 0) return false;
    if (q->tries < 255) q->tries++;
    return q->tries >= TASK_QUEUE_TRIES;
}

/* Offline render: OFFLINE banner (with queued count) + the cached list below it.
 * Call after lv_obj_clean(content); the caller picks this path when !net_is_online(). */
static inline void task_view_render_offline(task_view_t *v, int queued)
{
    char banner[TASK_ROW_MAX];
    if (queued > 0) {
        snprintf(banner, sizeof(banner), "OFFLINE  %d queued", queued);
    } else {
        strlcpy(banner, "OFFLINE", sizeof(banner));
    }
    ui_text_row(TASK_OFFLINE_ROW, banner);
    if (v->count == 0) return;
    ui_list_set_rows(&v->list, UI_ROWS - TASK_BANNER_ROWS);   /* room for the banner */
    ui_list_draw(&v->list, TASK_BANNER_ROWS, task_row_text, v);
}

/* ── detail submenu (§8.5 step-10 polish): click a task → per-task actions ── */
#define TASK_DESC_MAX        256   /* on-demand description buffer (fetched, not stored per task) */
#define TASK_MENU_TITLE_ROW  0     /* the acted-on task's title */
#define TASK_MENU_FIRST_ROW  1     /* action list / body below the title */

enum {                             /* action rows, fixed order */
    TASK_ACT_VIEW = 0,             /* View description (on-demand fetch) */
    TASK_ACT_POSTPONE,             /* Postpone → tomorrow */
    TASK_ACT_SYNC,                 /* Sync now */
    TASK_ACT_BACK,                 /* back to the list */
    TASK_ACT_COUNT
};

typedef enum { TASK_VIEW_LIST, TASK_VIEW_MENU, TASK_VIEW_DESC } task_mode_t;

typedef struct {
    task_mode_t mode;
    int         task;                 /* index into view.items the menu acts on */
    ui_list_t   menu;                 /* the action list */
    char        desc[TASK_DESC_MAX];  /* fetched on demand */
    bool        desc_loading;
} task_detail_t;

static inline void task_detail_reset(task_detail_t *d)
{
    d->mode = TASK_VIEW_LIST;
    d->task = -1;
    d->desc[0] = '\0';
    d->desc_loading = false;
}

/* Enter the action submenu for task index `task`. */
static inline void task_detail_open(task_detail_t *d, int task)
{
    d->mode = TASK_VIEW_MENU;
    d->task = task;
    d->desc[0] = '\0';
    d->desc_loading = false;
    ui_list_init(&d->menu, UI_ROWS - TASK_MENU_FIRST_ROW);
    ui_list_set_count(&d->menu, TASK_ACT_COUNT);
}

static inline void task_menu_row_text(int index, char *buf, int buf_sz, void *ctx)
{
    (void)ctx;
    static const char *const labels[TASK_ACT_COUNT] = {
        "Details", "Postpone", "Sync now", "Back",
    };
    snprintf(buf, buf_sz, "%s", (index >= 0 && index < TASK_ACT_COUNT) ? labels[index] : "?");
}

/* The action submenu: the task title (scrolls) + the action list. */
static inline void task_menu_render(task_detail_t *d, const task_view_t *v)
{
    if (d->task < 0 || d->task >= v->count) return;
    ui_text_row_scroll(TASK_MENU_TITLE_ROW, v->items[d->task].title);
    ui_list_draw(&d->menu, TASK_MENU_FIRST_ROW, task_menu_row_text, NULL);
}

/* The task detail view: title + due date (from the cached task, no fetch) + the
 * on-demand description (or Loading / empty state). */
static inline void task_desc_render(task_detail_t *d, const task_view_t *v)
{
    if (d->task < 0 || d->task >= v->count) return;
    const task_t *t = &v->items[d->task];
    ui_text_row_scroll(TASK_MENU_TITLE_ROW, t->title);

    int row = TASK_MENU_FIRST_ROW;
    char due[TASK_DUE_MAX + 8];
    snprintf(due, sizeof(due), "Due: %s", t->due[0] ? t->due : "none");
    ui_text_row(row++, due);

    if (d->desc_loading)    { ui_text_row(row, "Loading..."); return; }
    if (d->desc[0] == '\0') { ui_text_row(row, "(no description)"); return; }
    ui_text_wrap(row, d->desc);
}

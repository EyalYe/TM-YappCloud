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

#define TASK_MAX        50   /* cap on tasks held on-device */
#define TASK_ID_MAX     24   /* id / parent_id string (incl NUL) */
#define TASK_TITLE_MAX  48   /* title (server-truncated) */
#define TASK_DUE_MAX    16   /* due date/label */
#define TASK_PRIO_MIN   1    /* lowest */
#define TASK_PRIO_MAX   4    /* highest (Todoist P1) */
#define TASK_ROW_MAX    40   /* max chars per rendered row */
#define TASK_EMPTY_ROW  0

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
    ui_list_draw(&v->list, 0, task_row_text, v);
}

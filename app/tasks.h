/*
 * tasks.h — the on-device task model + list view (PLAN §8.5 step 9).
 *
 * USERSPACE (tasks are not a core concept, §8). A bounded task_t array (no
 * per-task malloc, §6A.1) plus a view that formats tasks into the core's generic
 * ui_list — priority marker + parent nesting + title. Both source apps share this
 * thin module (copied per repo); the fetch/parse that fills `items` differs per
 * source (Local = LAN contract, step 10; Yapp = direct Todoist, step 11).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ui_list.h"

#define TASK_MAX        50   /* cap on tasks held on-device */
#define TASK_ID_MAX     24   /* id / parent_id string (incl NUL) */
#define TASK_TITLE_MAX  48   /* title (server-truncated) */
#define TASK_DUE_MAX    16   /* due date/label */
#define TASK_PRIO_MIN   1    /* lowest */
#define TASK_PRIO_MAX   4    /* highest (Todoist P1) */

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

void task_view_init(task_view_t *v, int visible_rows);
void task_view_set_count(task_view_t *v, int count);   /* call after filling items */
void task_view_move(task_view_t *v, int delta);
int  task_view_sel(const task_view_t *v);              /* selected index, or -1 if empty */

/* Render the task list into the content area: priority marker (P1..P4, P1 highest)
 * + nesting indent for subtasks + title. Empty state "No open tasks". Call after
 * lv_obj_clean(content). */
void task_view_render(task_view_t *v);

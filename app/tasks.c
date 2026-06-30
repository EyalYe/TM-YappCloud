/*
 * tasks.c — task model + list view. See tasks.h.
 */
#include "tasks.h"
#include "ui_frame.h"
#include "lvgl.h"

#include <stdio.h>

#define TASK_ROW_MAX   40   /* max chars per rendered row */
#define TASK_EMPTY_ROW 0

void task_view_init(task_view_t *v, int visible_rows)
{
    v->count = 0;
    ui_list_init(&v->list, visible_rows);
}

void task_view_set_count(task_view_t *v, int count)
{
    v->count = count < 0 ? 0 : (count > TASK_MAX ? TASK_MAX : count);
    ui_list_set_count(&v->list, v->count);
}

void task_view_move(task_view_t *v, int delta)
{
    ui_list_move(&v->list, delta);
}

int task_view_sel(const task_view_t *v)
{
    return v->count > 0 ? ui_list_sel(&v->list) : -1;
}

/* ui_list row text: "<indent>P<n> <title>"; P1 = highest (priority TASK_PRIO_MAX). */
static void task_row_text(int index, char *buf, int buf_sz, void *ctx)
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

void task_view_render(task_view_t *v)
{
    if (v->count == 0) {
        ui_text_row(TASK_EMPTY_ROW, "No open tasks");
        return;
    }
    ui_list_draw(&v->list, 0, task_row_text, v);
}

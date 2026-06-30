/*
 * app_yapp.c — the "Yapp" device app for TaskMaster-C3 (direct Todoist).
 *
 * Firmware half of the yappcloud source product: a self-registering app pulled
 * into a build via the manifest (a git: line in main/idf_component.yml). Depends
 * only on the stable core app API (core ⟂ userspace).
 *
 * Placeholder for now — it registers, declares its Todoist token as app config
 * (§9.4), and renders a stub screen. The direct-Todoist fetch/parse → task_t →
 * render lands at Phase 3 step 11.
 */
#include "app.h"
#include "ui_frame.h"
#include "app_config.h"

#define YAPP_TOKEN_MAX  64
#define YAPP_ROW_TITLE  0
#define YAPP_ROW_NOTE1  1
#define YAPP_ROW_NOTE2  2

/* Declared config (§9.4): the Todoist API token, pasted via the setup form into
 * this app's app_store namespace ("yapp"). Never hardcoded, never in core. */
static const app_cfg_field_t YAPP_CFG[] = {
    { .key = "token", .label = "Todoist token", .type = ACFG_STR, .input = ACFG_PASTE,
      .secret = true, .max_len = YAPP_TOKEN_MAX },
};
TASKMASTER_REGISTER_APP_CONFIG("yapp", "Yapp", YAPP_CFG);

static void yapp_init(void)            { }
static void yapp_on_event(uint8_t ev)  { (void)ev; }

static void yapp_render(void)
{
    lv_obj_clean(ui_frame_content());
    ui_frame_set_hints(NULL);
    ui_text_row(YAPP_ROW_TITLE, "Yapp (Todoist)");
    ui_text_row(YAPP_ROW_NOTE1, "direct cloud");
    ui_text_row(YAPP_ROW_NOTE2, "coming soon");
}

static void yapp_exit(void)            { }

static const device_app_t yapp_app = {
    .name     = "Yapp",
    .init     = yapp_init,
    .on_event = yapp_on_event,
    .render   = yapp_render,
    .exit     = yapp_exit,
};

TASKMASTER_REGISTER_APP(yapp_app);

// LCD GUI: status log, button handling, and lab-list rendering.
#pragma once

#include <stdbool.h>
#include <stdint.h>

//-----------------------------------------------------------------------------
// Button IDs (indices into the internal g_buttons[] table)
//-----------------------------------------------------------------------------
typedef enum {
    GUI_BTN_A = 0,
    GUI_BTN_B,
    GUI_BTN_X,
    GUI_BTN_Y,
    GUI_KEY_UP,
    GUI_KEY_DOWN,
    GUI_KEY_LEFT,
    GUI_KEY_RIGHT,
    GUI_KEY_CTRL,
    GUI_BTN_COUNT,
} gui_button_id_t;

//-----------------------------------------------------------------------------
// View state
//-----------------------------------------------------------------------------
typedef enum {
    VIEW_STATUS,
    VIEW_BUTTONS,
    VIEW_LABS,
} view_state_t;

//-----------------------------------------------------------------------------
// Lab list state
//-----------------------------------------------------------------------------
typedef enum {
    LAB_IDLE,
    LAB_LOADING,
    LAB_OK,
    LAB_ERR,
} lab_state_t;

#define MAX_LABS        16
#define MAX_LAB_ID_LEN  64

// Shared state — defined in gui.c, read/written from main.c core loops.
extern volatile view_state_t g_view;
extern volatile lab_state_t  g_lab_state;
extern char     g_operator_ids[MAX_LABS][MAX_LAB_ID_LEN];
extern char     g_lab_ids[MAX_LABS][MAX_LAB_ID_LEN];
extern int      g_lab_count;
extern int      g_lab_selected;
extern bool     g_timeline_active;
extern char     g_timeline_lab_id[MAX_LAB_ID_LEN];
extern volatile int  g_timeline_status_ver;
extern volatile bool g_tts_play_active;

//-----------------------------------------------------------------------------
// Status log (thread-safe, called from both cores via wifi.c / main.c)
//-----------------------------------------------------------------------------
void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Returns the monotonic version counter; caller compares to detect updates.
uint32_t gui_status_version(void);

//-----------------------------------------------------------------------------
// LCD layout constants
//-----------------------------------------------------------------------------
#define FONT_SCALE  2
#define ROW_H       22
#define ROW_Y0      28
#define TEXT_X      8
#define IND_X       160

//-----------------------------------------------------------------------------
// Functions called from core0_loop
//-----------------------------------------------------------------------------
void buttons_init(void);
void buttons_poll(void);

void render_status_view(void);
void render_buttons_view(void);
void render_lab_loading(void);
void render_lab_error(void);
void render_lab_list(void);
void render_lab_spinner(uint32_t slow_phase, bool fast_blink_on);

// Trigger a lab-list fetch (sends IC_MSG_BTN_X to core1).
void trigger_fetch(void);

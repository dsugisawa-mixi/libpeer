#include "gui.h"
#include "common.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/mutex.h"
#include "st7789.h"
#include "ic_ring.h"
#include "audio.h"

//=============================================================================
// Shared state (owned here, externed in gui.h)
//=============================================================================
volatile view_state_t g_view = VIEW_STATUS;
volatile lab_state_t  g_lab_state    = LAB_IDLE;
char     g_operator_ids[MAX_LABS][MAX_LAB_ID_LEN];
char     g_lab_ids[MAX_LABS][MAX_LAB_ID_LEN];
bool     g_lab_is_radio[MAX_LABS];
int      g_lab_count    = 0;
int      g_lab_selected = 0;

bool          g_timeline_active    = false;
char          g_timeline_lab_id[MAX_LAB_ID_LEN] = "";
volatile int  g_timeline_status_ver = 0;
volatile bool g_tts_play_active = false;

//=============================================================================
// Status log (cross-core mini log on LCD)
//=============================================================================
#define STATUS_LINES    9
#define STATUS_LINE_LEN 24
#define STATUS_FONT     2
#define STATUS_ROW_H    22
#define STATUS_ROW_Y0   28

static char g_status_lines[STATUS_LINES][STATUS_LINE_LEN];
static int  g_status_head  = 0;
static int  g_status_count = 0;
static volatile uint32_t g_status_version = 0;
auto_init_mutex(g_status_mutex);

void set_status(const char *fmt, ...) {
    mutex_enter_blocking(&g_status_mutex);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status_lines[g_status_head], STATUS_LINE_LEN, fmt, ap);
    va_end(ap);
    int written = g_status_head;
    g_status_head = (g_status_head + 1) % STATUS_LINES;
    if (g_status_count < STATUS_LINES) g_status_count++;
    g_status_version++;
    mutex_exit(&g_status_mutex);

    printf("[status] %s\n", g_status_lines[written]);
}

uint32_t gui_status_version(void) {
    return g_status_version;
}

//=============================================================================
// Status view
//=============================================================================
void render_status_view(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Boot", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);

    mutex_enter_blocking(&g_status_mutex);
    int n = g_status_count;
    int start = (g_status_head - n + STATUS_LINES) % STATUS_LINES;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % STATUS_LINES;
        int y = STATUS_ROW_Y0 + i * STATUS_ROW_H;
        lcd_draw_text(TEXT_X, y, g_status_lines[idx], STATUS_FONT, COLOR_WHITE, COLOR_BLACK);
    }
    mutex_exit(&g_status_mutex);
}

//=============================================================================
// Buttons (Waveshare Pico-LCD-1.3)
//=============================================================================
typedef struct {
    const char *name;
    uint8_t     pin;
    bool        pressed;
} button_t;

static button_t g_buttons[] = {
    {"Button-A",  15, false},
    {"Button-B",  17, false},
    {"Button-X",  19, false},
    {"Button-Y",  21, false},
    {"Key-Up",     2, false},
    {"Key-Down",  18, false},
    {"Key-Left",  16, false},
    {"Key-Right", 20, false},
    {"Key-Ctrl",   3, false},
};
#define NUM_BUTTONS (sizeof(g_buttons)/sizeof(g_buttons[0]))

void buttons_init(void) {
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(g_buttons[i].pin);
        gpio_set_dir(g_buttons[i].pin, GPIO_IN);
        gpio_pull_up(g_buttons[i].pin);
    }
}

static void draw_button_row(int row, const char *name, bool pressed) {
    int y = ROW_Y0 + row * ROW_H;
    uint16_t bg = pressed ? COLOR_GREEN : COLOR_BLACK;
    uint16_t fg = pressed ? COLOR_BLACK : COLOR_WHITE;
    lcd_fill_rect(0, y, LCD_W, ROW_H - 2, bg);
    lcd_draw_text(TEXT_X, y + 3, name, FONT_SCALE, fg, bg);
    lcd_draw_text(IND_X,  y + 3, pressed ? "ON " : "OFF", FONT_SCALE, fg, bg);
}

void render_buttons_view(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "PicoLCD-1.3 Buttons", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        draw_button_row((int)i, g_buttons[i].name, g_buttons[i].pressed);
    }
}

//=============================================================================
// Lab views
//=============================================================================
void render_lab_loading(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Labs", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    lcd_draw_text(8, ROW_Y0, "Fetching...", FONT_SCALE, COLOR_YELLOW, COLOR_BLACK);
}

void render_lab_error(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Labs", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    lcd_draw_text(8, ROW_Y0, "Fetch error", FONT_SCALE, COLOR_RED, COLOR_BLACK);
}

void render_lab_list(void) {
    lcd_fill(COLOR_BLACK);
    const char *title = g_timeline_active
        ? "Labs (Enter=stop)"
        : "Labs (Enter=start)";
    lcd_draw_text(8, 4, title, FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    if (g_lab_count == 0) {
        lcd_draw_text(8, ROW_Y0, "(no labs)", FONT_SCALE, COLOR_WHITE, COLOR_BLACK);
        return;
    }
    for (int i = 0; i < g_lab_count; i++) {
        int y = ROW_Y0 + i * ROW_H;
        bool sel = (i == g_lab_selected);
        uint16_t bg = sel
            ? (g_timeline_active ? COLOR_YELLOW : COLOR_GREEN)
            : COLOR_BLACK;
        uint16_t fg = sel ? COLOR_BLACK : COLOR_WHITE;
        lcd_fill_rect(0, y, LCD_W, ROW_H - 2, bg);
        lcd_draw_text(TEXT_X, y + 3, g_lab_ids[i], FONT_SCALE, fg, bg);
    }
}

void render_lab_spinner(uint32_t slow_phase, bool fast_blink_on) {
    if (g_view != VIEW_LABS) return;
    const char *spinner_frames[] = {"|", "/", "-", "\\"};
    int sx = LCD_W - 14;
    int ix = LCD_W - 28;

    lcd_fill_rect(sx, 4, 12, 16, COLOR_BLACK);
    if (g_timeline_active) {
        const char *frame = spinner_frames[slow_phase & 3];
        lcd_draw_text(sx, 4, frame, FONT_SCALE, COLOR_YELLOW, COLOR_BLACK);
    }

    lcd_fill_rect(ix, 4, 12, 16, COLOR_BLACK);
    if (g_tts_play_active && fast_blink_on) {
        lcd_draw_text(ix, 4, "*", FONT_SCALE, COLOR_GREEN, COLOR_BLACK);
    }
}

//=============================================================================
// Fetch trigger
//=============================================================================
void trigger_fetch(void) {
    if (g_lab_state == LAB_LOADING) return;
    g_lab_state = LAB_LOADING;
    g_view      = VIEW_LABS;
    render_lab_loading();
    ic_send(IC_MSG_BTN_X, NULL, 0);
}

//=============================================================================
// Button event dispatch
//=============================================================================
static void on_button_event(int idx, bool now_pressed) {
    if (!now_pressed) return;

    switch (idx) {
        case GUI_BTN_A:
            audio_play_sine();
            break;
        case GUI_BTN_B:
            audio_stop();
            break;
        case GUI_BTN_X:
            trigger_fetch();
            break;
        case GUI_BTN_Y:
            g_view = VIEW_BUTTONS;
            render_buttons_view();
            break;
        case GUI_KEY_UP:
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0
                && !g_timeline_active && !g_tts_play_active) {
                g_lab_selected = (g_lab_selected - 1 + g_lab_count) % g_lab_count;
                render_lab_list();
            }
            break;
        case GUI_KEY_DOWN:
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0
                && !g_timeline_active && !g_tts_play_active) {
                g_lab_selected = (g_lab_selected + 1) % g_lab_count;
                render_lab_list();
            }
            break;
        case GUI_KEY_CTRL:
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0) {
                bool radio = g_lab_is_radio[g_lab_selected];
                if (radio) {
                    // Re-press while a radio stream is playing → stop it. We
                    // use g_tts_play_active as the "stream live" signal
                    // because tts_start_playback flips it on once headers
                    // arrive, and the post-cleanup drain flips it back off.
                    if (g_tts_play_active) {
                        ic_send(IC_MSG_RADIO_STOP, NULL, 0);
                        printf("[core0] Enter: stop radio\n");
                    } else {
                        ic_send(IC_MSG_RADIO_START, NULL, 0);
                        printf("[core0] Enter: start radio op=%s\n",
                               g_operator_ids[g_lab_selected]);
                    }
                } else if (!g_timeline_active) {
                    strncpy(g_timeline_lab_id, g_lab_ids[g_lab_selected], MAX_LAB_ID_LEN - 1);
                    g_timeline_lab_id[MAX_LAB_ID_LEN - 1] = '\0';
                    const char *op = g_operator_ids[g_lab_selected];
                    ic_send(IC_MSG_TIMELINE_START, op, (uint16_t)strlen(op));
                    g_timeline_active = true;
                    printf("[core0] Enter: start timeline for lab=%s\n", g_timeline_lab_id);
                } else {
                    ic_send(IC_MSG_TIMELINE_STOP, NULL, 0);
                    g_timeline_active = false;
                    printf("[core0] Enter: stop timeline\n");
                }
                render_lab_list();
            }
            break;
        default:
            break;
    }
}

void buttons_poll(void) {
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        bool now_pressed = !gpio_get(g_buttons[i].pin);
        if (now_pressed != g_buttons[i].pressed) {
            g_buttons[i].pressed = now_pressed;
            printf("[core0] %-9s %s @ %lu ms\n",
                   g_buttons[i].name,
                   now_pressed ? "PRESS  " : "RELEASE",
                   (unsigned long)board_millis());

            if (g_view == VIEW_BUTTONS) {
                draw_button_row((int)i, g_buttons[i].name, now_pressed);
            }
            on_button_event((int)i, now_pressed);
        }
    }
}

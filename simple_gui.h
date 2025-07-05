#ifndef SIMPLE_GUI_H
#define SIMPLE_GUI_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include <time.h>

struct SGButton;

//==============================================================================
// Data Structures 
//==============================================================================

/**
 * @brief Represents the core components of a window managed by X11.
 *
 * The 'font' field has been removed as it was part of the old rendering
 * system and is no longer needed with the Xft library.
 */
typedef struct {
    Display* display;
    Window window;
    GC gc;
    int width;
    int height;
    Pixmap back_buffer;
} SGWindow;

/**
 * @brief Represents the state of a button widget.
 */
typedef struct SGButton {
    int id;
    int x, y, width, height;
    char* text;
    bool pressed;
    bool hovered;
    void (*on_click)(struct SGButton* self, void* user_data);
    void* user_data;
} SGButton;

/**
 * @brief Represents the state of a label widget.
 */
typedef struct {
    int x, y;
    char* text;
    int font_size; // 0 for default
    int alignment; // 0: left, 1: center, 2: right
} SGLabel;

/**
 * @brief A struct to manage the state of a simple vertical layout.
 */
typedef struct {
    int current_y;
    int start_x;
    int padding;
} SGLayoutState;

//==============================================================================
// Public API Functions
//==============================================================================

// --- Window Management ---
SGWindow sg_create_window(int width, int height, const char* title);
void sg_destroy_window(SGWindow* sg_window);

// --- Event Handling ---
void sg_handle_events(SGWindow* sg_window, SGButton buttons[], int button_count);

// --- Drawing ---
void sg_clear_window(SGWindow* sg_window);
void sg_draw_button(SGWindow* sg_window, const SGButton* button);
void sg_draw_label(SGWindow* sg_window, const SGLabel* label);
void sg_flush(SGWindow* sg_window);

// --- Layout Management ---
SGLayoutState sg_layout_begin(int start_x, int start_y, int padding);
void sg_layout_add_button(SGLayoutState* layout_state, SGButton* button);
void sg_layout_add_label(SGLayoutState* layout_state, SGLabel* label);
void sg_layout_add_spacing(SGLayoutState* layout_state, int space);

//==============================================================================
//Utility Functions
//==============================================================================

// --- Safe Widget Creation ---
SGButton sg_create_button(int id, int x, int y, int width, int height, const char* text);
SGLabel sg_create_label(int x, int y, const char* text, int font_size, int alignment);

// --- Safe Widget Destruction ---
void sg_destroy_button(SGButton* button);
void sg_destroy_label(SGLabel* label);

// --- Widget Property Management ---
void sg_button_set_callback(SGButton* button, void (*callback)(struct SGButton*, void*), void* user_data);
void sg_button_set_text(SGButton* button, const char* text);
void sg_label_set_text(SGLabel* label, const char* text);

// --- Window Utility Functions ---
void sg_get_window_size(SGWindow* sgw, int* width, int* height);

// --- Widget State Query Functions ---
bool sg_button_is_hovered(const SGButton* button);
bool sg_button_is_pressed(const SGButton* button);

// --- Resource Management ---
void sg_cleanup_global_resources(void);
void sg_cleanup_unused_fonts(Display* display);

// --- Performance Monitoring ---
void sg_get_font_cache_stats(int* total_fonts, int* cache_hits, int* cache_misses);

//==============================================================================
// Backward Compatibility Macros
//==============================================================================

#define SG_BUTTON_INIT(id, x, y, w, h, txt) sg_create_button(id, x, y, w, h, txt)
#define SG_LABEL_INIT(x, y, txt, size, align) sg_create_label(x, y, txt, size, align)

// Alignment constants 
#define SG_ALIGN_LEFT    0
#define SG_ALIGN_CENTER  1
#define SG_ALIGN_RIGHT   2

// Common button dimensions
#define SG_BUTTON_WIDTH_DEFAULT   100
#define SG_BUTTON_HEIGHT_DEFAULT  40
#define SG_BUTTON_WIDTH_LARGE     150
#define SG_BUTTON_HEIGHT_LARGE    50

// Common font sizes
#define SG_FONT_SIZE_SMALL   12
#define SG_FONT_SIZE_MEDIUM  16
#define SG_FONT_SIZE_LARGE   24
#define SG_FONT_SIZE_XLARGE  32

#endif
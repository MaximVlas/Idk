#include "simple_gui.h"
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

//==============================================================================
// Color Palette & Style Definitions
//==============================================================================
#define BG_COLOR_TOP         0x3B4252 // Nord Polar Night (lighter)
#define BG_COLOR_BOTTOM      0x2E3440 // Nord Polar Night (darker)
#define FG_COLOR             0xECEFF4 // Nord Snow Storm (brighter)
#define BORDER_COLOR         0x4C566A // Nord Polar Night (brighter)
#define SHADOW_COLOR         0x20242C // Darker for shadow
#define BUTTON_RADIUS        8        // Rounded corners for buttons
#define SHADOW_OFFSET        2        // Drop shadow offset

// Button colors
#define BTN_IDLE_TOP         0x434C5E
#define BTN_IDLE_BOTTOM      0x3B4252
#define BTN_HOVER_TOP        0x4C566A
#define BTN_HOVER_BOTTOM     0x434C5E
#define BTN_PRESSED_TOP      0x81A1C1 // Nord Frost
#define BTN_PRESSED_BOTTOM   0x88C0D0 // Nord Frost

// Performance and resource management constants
#define MAX_CACHED_FONTS     8        // Maximum number of cached fonts
#define FONT_NAME_MAX_LEN    64       // Maximum font name length
#define MIN_REDRAW_INTERVAL  16       // Minimum ms between redraws (60 FPS)

//==============================================================================
// Resource Management
//==============================================================================

// Font cache entry for improved memory management
typedef struct {
    XftFont* font;
    char name[FONT_NAME_MAX_LEN];
    int size;
    int ref_count;
    bool in_use;
} FontCacheEntry;

// Global resource management structure
typedef struct {
    // Font cache
    FontCacheEntry font_cache[MAX_CACHED_FONTS];
    int font_cache_size;
    
    // Xft resources
    XftDraw* draw;
    XftColor text_color;
    Pixmap last_drawable;
    Display* last_display;
    
    // Performance tracking
    unsigned long last_redraw_time;
    
    // Initialization flags
    bool xft_initialized;
    bool resources_initialized;
} SGResourceManager;

// Global resource manager instance
static SGResourceManager g_rm = {0};

//==============================================================================
// Helper Functions
//==============================================================================

// Get current time in milliseconds for performance tracking
static unsigned long get_time_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
    return 0;
}

// error handling with detailed messages
static void sg_log_error(const char* function, const char* message) {
    fprintf(stderr, "[SimpleGUI Error] %s: %s\n", function, message);
    if (errno != 0) {
        fprintf(stderr, "[SimpleGUI Error] System error: %s\n", strerror(errno));
        errno = 0;
    }
}

// Safe memory allocation with error checking
static void* sg_safe_malloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr && size > 0) {
        sg_log_error("sg_safe_malloc", "Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

// Safe string duplication
static char* sg_safe_strdup(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str) + 1;
    char* copy = sg_safe_malloc(len);
    memcpy(copy, str, len);
    return copy;
}

//==============================================================================
// Font Management
//==============================================================================

// Find cached font by name and size
static FontCacheEntry* find_cached_font(const char* name, int size) {
    for (int i = 0; i < g_rm.font_cache_size; i++) {
        if (g_rm.font_cache[i].in_use && 
            g_rm.font_cache[i].size == size &&
            strcmp(g_rm.font_cache[i].name, name) == 0) {
            return &g_rm.font_cache[i];
        }
    }
    return NULL;
}

// Get font with improved caching and error handling
static XftFont* get_cached_font(Display* display, const char* base_name, int size) {
    if (!display || !base_name || size <= 0) return NULL;
    
    char font_name[FONT_NAME_MAX_LEN];
    snprintf(font_name, sizeof(font_name), "%s-%d", base_name, size);
    
    // Check if font is already cached
    FontCacheEntry* entry = find_cached_font(font_name, size);
    if (entry) {
        entry->ref_count++;
        return entry->font;
    }
    
    // Find available cache slot
    int slot = -1;
    for (int i = 0; i < MAX_CACHED_FONTS; i++) {
        if (!g_rm.font_cache[i].in_use) {
            slot = i;
            break;
        }
    }
    
    // If no slot available, find least recently used
    if (slot == -1) {
        int min_ref = INT_MAX;
        for (int i = 0; i < MAX_CACHED_FONTS; i++) {
            if (g_rm.font_cache[i].ref_count < min_ref) {
                min_ref = g_rm.font_cache[i].ref_count;
                slot = i;
            }
        }
        
        // Clean up old font
        if (g_rm.font_cache[slot].font) {
            XftFontClose(display, g_rm.font_cache[slot].font);
        }
    }
    
    // Load new font
    XftFont* font = XftFontOpenName(display, DefaultScreen(display), font_name);
    if (!font) {
        sg_log_error("get_cached_font", "Failed to load font");
        return NULL;
    }
    
    // Cache the font
    FontCacheEntry* new_entry = &g_rm.font_cache[slot];
    new_entry->font = font;
    strncpy(new_entry->name, font_name, FONT_NAME_MAX_LEN - 1);
    new_entry->name[FONT_NAME_MAX_LEN - 1] = '\0';
    new_entry->size = size;
    new_entry->ref_count = 1;
    new_entry->in_use = true;
    
    if (slot >= g_rm.font_cache_size) {
        g_rm.font_cache_size = slot + 1;
    }
    
    return font;
}

// Cleanup font cache
static void cleanup_font_cache(Display* display) {
    for (int i = 0; i < g_rm.font_cache_size; i++) {
        if (g_rm.font_cache[i].in_use && g_rm.font_cache[i].font) {
            XftFontClose(display, g_rm.font_cache[i].font);
            g_rm.font_cache[i].in_use = false;
        }
    }
    g_rm.font_cache_size = 0;
}

//==============================================================================
// Resource Management
//==============================================================================

// Initialize Xft resources with error checking
static bool initialize_xft_resources(Display* display, Drawable drawable) {
    if (g_rm.xft_initialized && g_rm.last_drawable == drawable && g_rm.last_display == display) {
        return true; 
    }
    
    // Clean up old resources if display changed
    if (g_rm.last_display != display && g_rm.xft_initialized) {
        if (g_rm.draw) {
            XftDrawDestroy(g_rm.draw);
            g_rm.draw = NULL;
        }
        cleanup_font_cache(g_rm.last_display);
        g_rm.xft_initialized = false;
    }
    
    // Create or recreate XftDraw if drawable changed
    if (g_rm.last_drawable != drawable) {
        if (g_rm.draw) {
            XftDrawDestroy(g_rm.draw);
        }
        
        g_rm.draw = XftDrawCreate(display, drawable, 
                                 DefaultVisual(display, DefaultScreen(display)),
                                 DefaultColormap(display, DefaultScreen(display)));
        if (!g_rm.draw) {
            sg_log_error("initialize_xft_resources", "Failed to create XftDraw");
            return false;
        }
        g_rm.last_drawable = drawable;
    }
    
    // Initialize color if not done yet
    if (!g_rm.xft_initialized) {
        if (!XftColorAllocName(display, 
                              DefaultVisual(display, DefaultScreen(display)),
                              DefaultColormap(display, DefaultScreen(display)),
                              "#ECEFF4", &g_rm.text_color)) {
            sg_log_error("initialize_xft_resources", "Failed to allocate text color");
            return false;
        }
        g_rm.xft_initialized = true;
    }
    
    g_rm.last_display = display;
    return true;
}

//==============================================================================
// Drawing Functions
//==============================================================================

/**
 * @brief gradient fill with bounds checking and error handling
 */
static void fill_gradient_rect(Display* display, GC gc, Drawable d, int x, int y, int w, int h, unsigned long c_top, unsigned long c_bottom) {
    if (!display || !gc || w <= 0 || h <= 0) return;
    
    // Extract RGB components with improved bit manipulation
    const int r1 = (c_top >> 16) & 0xFF;
    const int g1 = (c_top >> 8)  & 0xFF;
    const int b1 = c_top & 0xFF;
    
    const int r_delta = ((int)((c_bottom >> 16) & 0xFF)) - r1;
    const int g_delta = ((int)((c_bottom >> 8)  & 0xFF)) - g1;
    const int b_delta = ((int)(c_bottom & 0xFF)) - b1;
    
    // Optimize for solid colors (no gradient needed)
    if (r_delta == 0 && g_delta == 0 && b_delta == 0) {
        XSetForeground(display, gc, c_top);
        XFillRectangle(display, d, gc, x, y, w, h);
        return;
    }
    
    // Draw gradient line by line with bounds checking
    for (int i = 0; i < h; i++) {
        // Use more precise color interpolation
        const unsigned char r = (unsigned char)(r1 + (r_delta * i) / h);
        const unsigned char g = (unsigned char)(g1 + (g_delta * i) / h);
        const unsigned char b = (unsigned char)(b1 + (b_delta * i) / h);
        
        const unsigned long line_color = ((unsigned long)r << 16) | ((unsigned long)g << 8) | b;
        XSetForeground(display, gc, line_color);
        XDrawLine(display, d, gc, x, y + i, x + w - 1, y + i);
    }
}

/**
 * @brief rounded rectangle with improved bounds checking
 */
static void fill_rounded_rect(Display* display, Drawable d, GC gc, int x, int y, int w, int h, int r, unsigned long color) {
    if (!display || !gc || w <= 0 || h <= 0) return;
    
    // Clamp radius to valid range
    r = (r < 0) ? 0 : r;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    
    XSetForeground(display, gc, color);
    
    if (r == 0) {
        // Simple rectangle if no rounding
        XFillRectangle(display, d, gc, x, y, w, h);
        return;
    }
    
    // Draw rounded rectangle components
    XFillRectangle(display, d, gc, x + r, y, w - 2 * r, h);
    XFillRectangle(display, d, gc, x, y + r, w, h - 2 * r);
    
    // Draw corner arcs
    XFillArc(display, d, gc, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
    XFillArc(display, d, gc, x + w - 2 * r, y, 2 * r, 2 * r, 0 * 64, 90 * 64);
    XFillArc(display, d, gc, x, y + h - 2 * r, 2 * r, 2 * r, 180 * 64, 90 * 64);
    XFillArc(display, d, gc, x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 270 * 64, 90 * 64);
}

/**
 * @brief rounded rectangle outline
 */
static void draw_rounded_rect(Display* display, Drawable d, GC gc, int x, int y, int w, int h, int r, unsigned long color) {
    if (!display || !gc || w <= 0 || h <= 0) return;
    
    // Clamp radius to valid range
    r = (r < 0) ? 0 : r;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    
    XSetForeground(display, gc, color);
    
    if (r == 0) {
        // Simple rectangle outline
        XDrawRectangle(display, d, gc, x, y, w - 1, h - 1);
        return;
    }
    
    // Draw rounded rectangle outline
    XDrawLine(display, d, gc, x + r, y, x + w - r - 1, y);
    XDrawLine(display, d, gc, x + r, y + h - 1, x + w - r - 1, y + h - 1);
    XDrawLine(display, d, gc, x, y + r, x, y + h - r - 1);
    XDrawLine(display, d, gc, x + w - 1, y + r, x + w - 1, y + h - r - 1);
    
    // Draw corner arcs
    XDrawArc(display, d, gc, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
    XDrawArc(display, d, gc, x + w - 2 * r, y, 2 * r, 2 * r, 0 * 64, 90 * 64);
    XDrawArc(display, d, gc, x, y + h - 2 * r, 2 * r, 2 * r, 180 * 64, 90 * 64);
    XDrawArc(display, d, gc, x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 270 * 64, 90 * 64);
}

// point-in-rectangle test with bounds checking
static bool is_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return (rw > 0 && rh > 0 && px >= rx && px < (rx + rw) && py >= ry && py < (ry + rh));
}

//==============================================================================
// Window Management
//==============================================================================

SGWindow sg_create_window(int width, int height, const char* title) {
    SGWindow sgw = {0}; // Initialize all fields to 0
    
    // Validate input parameters
    if (width <= 0 || height <= 0) {
        sg_log_error("sg_create_window", "Invalid window dimensions");
        exit(EXIT_FAILURE);
    }
    
    sgw.width = width;
    sgw.height = height;

    // Open X11 display
    sgw.display = XOpenDisplay(NULL);
    if (!sgw.display) {
        sg_log_error("sg_create_window", "Cannot open X11 display");
        exit(EXIT_FAILURE);
    }

    int screen = DefaultScreen(sgw.display);
    Window root = RootWindow(sgw.display, screen);

    // Create window with error checking
    sgw.window = XCreateSimpleWindow(sgw.display, root, 10, 10, width, height, 
                                    0, 0, BG_COLOR_BOTTOM);
    if (!sgw.window) {
        sg_log_error("sg_create_window", "Failed to create window");
        XCloseDisplay(sgw.display);
        exit(EXIT_FAILURE);
    }

    // Set window title safely
    if (title) {
        XStoreName(sgw.display, sgw.window, title);
    }

    // Select input events
    XSelectInput(sgw.display, sgw.window, 
                ExposureMask | KeyPressMask | ButtonPressMask | 
                ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    
    // Create graphics context
    sgw.gc = XCreateGC(sgw.display, sgw.window, 0, NULL);
    if (!sgw.gc) {
        sg_log_error("sg_create_window", "Failed to create graphics context");
        XDestroyWindow(sgw.display, sgw.window);
        XCloseDisplay(sgw.display);
        exit(EXIT_FAILURE);
    }
    
    // Create back buffer for double buffering
    sgw.back_buffer = XCreatePixmap(sgw.display, sgw.window, width, height, 
                                   DefaultDepth(sgw.display, screen));
    if (!sgw.back_buffer) {
        sg_log_error("sg_create_window", "Failed to create back buffer");
        XFreeGC(sgw.display, sgw.gc);
        XDestroyWindow(sgw.display, sgw.window);
        XCloseDisplay(sgw.display);
        exit(EXIT_FAILURE);
    }

    // Map window and set up close protocol
    XMapWindow(sgw.display, sgw.window);
    
    Atom wm_delete_window = XInternAtom(sgw.display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(sgw.display, sgw.window, &wm_delete_window, 1);

    return sgw;
}

void sg_destroy_window(SGWindow* sgw) {
    if (!sgw) return;
    
    // Clean up in reverse order of creation
    if (sgw->back_buffer) {
        XFreePixmap(sgw->display, sgw->back_buffer);
        sgw->back_buffer = 0;
    }
    
    if (sgw->gc) {
        XFreeGC(sgw->display, sgw->gc);
        sgw->gc = NULL;
    }
    
    if (sgw->window) {
        XDestroyWindow(sgw->display, sgw->window);
        sgw->window = 0;
    }
    
    if (sgw->display) {
        // Clean up global resources if this was the last window
        if (g_rm.last_display == sgw->display) {
            cleanup_font_cache(sgw->display);
            if (g_rm.draw) {
                XftDrawDestroy(g_rm.draw);
                g_rm.draw = NULL;
            }
            if (g_rm.xft_initialized) {
                XftColorFree(sgw->display, 
                           DefaultVisual(sgw->display, DefaultScreen(sgw->display)),
                           DefaultColormap(sgw->display, DefaultScreen(sgw->display)),
                           &g_rm.text_color);
                g_rm.xft_initialized = false;
            }
        }
        
        XCloseDisplay(sgw->display);
        sgw->display = NULL;
    }
}

//==============================================================================
// Event Handling
//==============================================================================

void sg_handle_events(SGWindow* sgw, SGButton buttons[], int button_count) {
    if (!sgw || !sgw->display) return;
    
    // Process all pending events
    while (XPending(sgw->display) > 0) {
        XEvent event;
        XNextEvent(sgw->display, &event);

        switch (event.type) {
            case ClientMessage: {
                Atom wm_delete_window = XInternAtom(sgw->display, "WM_DELETE_WINDOW", False);
                if ((Atom)event.xclient.data.l[0] == wm_delete_window) {
                    printf("Window closed by user.\n");
                    exit(0);
                }
                break;
            }
            
            case ConfigureNotify: {
                XConfigureEvent xce = event.xconfigure;
                if (xce.width != sgw->width || xce.height != sgw->height) {
                    sgw->width = xce.width;
                    sgw->height = xce.height;
                    
                    // Recreate back buffer with new dimensions
                    if (sgw->back_buffer) {
                        XFreePixmap(sgw->display, sgw->back_buffer);
                    }
                    sgw->back_buffer = XCreatePixmap(sgw->display, sgw->window, 
                                                   sgw->width, sgw->height, 
                                                   DefaultDepth(sgw->display, DefaultScreen(sgw->display)));
                    if (!sgw->back_buffer) {
                        sg_log_error("sg_handle_events", "Failed to recreate back buffer");
                    }
                }
                break;
            }
            
            case MotionNotify: {
                // Update hover state with bounds checking
                if (buttons && button_count > 0) {
                    for (int i = 0; i < button_count; i++) {
                        buttons[i].hovered = is_point_in_rect(event.xmotion.x, event.xmotion.y, 
                                                            buttons[i].x, buttons[i].y, 
                                                            buttons[i].width, buttons[i].height);
                    }
                }
                break;
            }
            
            case ButtonPress: {
                if (event.xbutton.button == Button1 && buttons && button_count > 0) {
                    for (int i = 0; i < button_count; i++) {
                        if (buttons[i].hovered) {
                            buttons[i].pressed = true;
                        }
                    }
                }
                break;
            }
            
            case ButtonRelease: {
                if (event.xbutton.button == Button1 && buttons && button_count > 0) {
                    for (int i = 0; i < button_count; i++) {
                        if (buttons[i].pressed && buttons[i].hovered && buttons[i].on_click) {
                            buttons[i].on_click(&buttons[i], buttons[i].user_data);
                        }
                        buttons[i].pressed = false;
                    }
                }
                break;
            }
        }
    }
}

//==============================================================================
// Drawing Implementation
//==============================================================================

void sg_clear_window(SGWindow* sgw) {
    if (!sgw || !sgw->display || !sgw->back_buffer) return;
    
    fill_gradient_rect(sgw->display, sgw->gc, sgw->back_buffer, 
                      0, 0, sgw->width, sgw->height, 
                      BG_COLOR_TOP, BG_COLOR_BOTTOM);
}

/**
 * @brief Vutton drawing
 */
void sg_draw_button(SGWindow* sgw, const SGButton* button) {
    if (!sgw || !sgw->display || !sgw->back_buffer || !button || !button->text) return;
    
    const int x = button->x;
    const int y = button->y;
    const int w = button->width;
    const int h = button->height;
    const int r = BUTTON_RADIUS;
    
    // Validate button dimensions
    if (w <= 0 || h <= 0) return;
    
    unsigned long top_color, bottom_color;
    int draw_y = y;

    // Determine button appearance based on state
    if (button->pressed) {
        top_color = BTN_PRESSED_TOP;
        bottom_color = BTN_PRESSED_BOTTOM;
        draw_y += SHADOW_OFFSET / 2; // Inset effect
    } else {
        if (button->hovered) {
            top_color = BTN_HOVER_TOP;
            bottom_color = BTN_HOVER_BOTTOM;
        } else {
            top_color = BTN_IDLE_TOP;
            bottom_color = BTN_IDLE_BOTTOM;
        }
        // Draw drop shadow for non-pressed states
        fill_rounded_rect(sgw->display, sgw->back_buffer, sgw->gc, 
                         x + SHADOW_OFFSET, y + SHADOW_OFFSET, w, h, r, SHADOW_COLOR);
    }

    // Create clipping mask for rounded gradient
    Pixmap mask = XCreatePixmap(sgw->display, sgw->back_buffer, w, h, 1);
    if (!mask) {
        sg_log_error("sg_draw_button", "Failed to create clipping mask");
        return;
    }
    
    GC mask_gc = XCreateGC(sgw->display, mask, 0, NULL);
    if (!mask_gc) {
        XFreePixmap(sgw->display, mask);
        sg_log_error("sg_draw_button", "Failed to create mask GC");
        return;
    }
    
    // Set up clipping mask
    XSetForeground(sgw->display, mask_gc, 0);
    XFillRectangle(sgw->display, mask, mask_gc, 0, 0, w, h);
    fill_rounded_rect(sgw->display, mask, mask_gc, 0, 0, w, h, r, 1);

    XSetClipMask(sgw->display, sgw->gc, mask);
    XSetClipOrigin(sgw->display, sgw->gc, x, draw_y);

    // Draw gradient background
    fill_gradient_rect(sgw->display, sgw->gc, sgw->back_buffer, 
                      x, draw_y, w, h, top_color, bottom_color);

    // Clean up clipping
    XSetClipMask(sgw->display, sgw->gc, None);
    XFreePixmap(sgw->display, mask);
    XFreeGC(sgw->display, mask_gc);

    // Draw border
    draw_rounded_rect(sgw->display, sgw->back_buffer, sgw->gc, 
                     x, draw_y, w - 1, h - 1, r, BORDER_COLOR);

    // Draw text with enhanced font management
    if (initialize_xft_resources(sgw->display, sgw->back_buffer)) {
        XftFont* font = get_cached_font(sgw->display, "sans:bold", 12);
        if (font && g_rm.draw) {
            XGlyphInfo extents;
            XftTextExtentsUtf8(sgw->display, font, (const FcChar8*)button->text, 
                              strlen(button->text), &extents);

            const int text_x = x + (w - extents.width) / 2;
            const int text_y = draw_y + (h - extents.height) / 2 + font->ascent;

            XftDrawStringUtf8(g_rm.draw, &g_rm.text_color, font, text_x, text_y, 
                             (const FcChar8*)button->text, strlen(button->text));
        }
    }
}

/**
 * @brief Enhanced label drawing with improved font management
 */
void sg_draw_label(SGWindow* sgw, const SGLabel* label) {
    if (!sgw || !sgw->display || !sgw->back_buffer || !label || !label->text) return;
    
    const int font_size = (label->font_size > 0) ? label->font_size : 24;
    
    // Initialize Xft resources
    if (!initialize_xft_resources(sgw->display, sgw->back_buffer)) return;
    
    // Get font with caching
    XftFont* font = get_cached_font(sgw->display, "sans", font_size);
    if (!font || !g_rm.draw) return;
    
    // Calculate text metrics
    XGlyphInfo extents;
    XftTextExtentsUtf8(sgw->display, font, (const FcChar8*)label->text, 
                      strlen(label->text), &extents);
    
    // Calculate text position based on alignment
    int text_x = label->x;
    switch (label->alignment) {
        case 1: // Center
            text_x = (sgw->width - extents.width) / 2;
            break;
        case 2: // Right
            text_x = sgw->width - extents.width - label->x;
            break;
        default: // Left (0) or invalid
            break;
    }
    
    const int text_y = label->y + font->ascent;
    
    // Draw text
    XftDrawStringUtf8(g_rm.draw, &g_rm.text_color, font, text_x, text_y, 
                     (const FcChar8*)label->text, strlen(label->text));
}

void sg_flush(SGWindow* sgw) {
    if (!sgw || !sgw->display || !sgw->back_buffer || !sgw->window) return;
    
    // Throttle rendering to prevent excessive CPU usage
    unsigned long current_time = get_time_ms();
    if (current_time - g_rm.last_redraw_time < MIN_REDRAW_INTERVAL) {
        return; // Skip this frame to maintain reasonable framerate
    }
    g_rm.last_redraw_time = current_time;
    
    // Copy back buffer to window
    XCopyArea(sgw->display, sgw->back_buffer, sgw->window, sgw->gc, 
              0, 0, sgw->width, sgw->height, 0, 0);
    XFlush(sgw->display);
}
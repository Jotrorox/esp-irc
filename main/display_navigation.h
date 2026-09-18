#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DISPLAY_VIEW_OVERVIEW,
    DISPLAY_VIEW_NETWORK,
    DISPLAY_VIEW_SYSTEM,
    DISPLAY_VIEW_COUNT,
} display_view_t;

typedef struct {
    bool raw_pressed;
    bool pressed;
    int64_t changed_at_ms;
} display_button_t;

typedef struct {
    display_view_t view;
    display_button_t previous;
    display_button_t next;
} display_navigation_t;

void display_navigation_init(display_navigation_t *navigation,
                             bool previous_pressed, bool next_pressed,
                             int64_t now_ms);

/* Returns true when a debounced press changes the view. No hold repeat. */
bool display_navigation_update(display_navigation_t *navigation,
                               bool previous_pressed, bool next_pressed,
                               int64_t now_ms);

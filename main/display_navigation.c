#include "display_navigation.h"

#define BUTTON_DEBOUNCE_MS 30

void display_navigation_init(display_navigation_t *navigation,
                             bool previous_pressed, bool next_pressed,
                             int64_t now_ms)
{
    /* A button held at startup must be released before it can navigate. */
    *navigation = (display_navigation_t) {
        .view = DISPLAY_VIEW_OVERVIEW,
        .previous = {previous_pressed, previous_pressed, now_ms},
        .next = {next_pressed, next_pressed, now_ms},
    };
}

static bool button_pressed(display_button_t *button, bool pressed, int64_t now_ms)
{
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->changed_at_ms = now_ms;
    }
    if (pressed != button->pressed &&
        now_ms - button->changed_at_ms >= BUTTON_DEBOUNCE_MS) {
        button->pressed = pressed;
        return pressed;
    }
    return false;
}

bool display_navigation_update(display_navigation_t *navigation,
                               bool previous_pressed, bool next_pressed,
                               int64_t now_ms)
{
    bool previous = button_pressed(&navigation->previous, previous_pressed, now_ms);
    bool next = button_pressed(&navigation->next, next_pressed, now_ms);
    /* Pressing both buttons consumes the presses without changing the view. */
    if ((previous_pressed && next_pressed) || previous == next) {
        return false;
    }
    navigation->view = (navigation->view + (next ? 1 : DISPLAY_VIEW_COUNT - 1))
                      % DISPLAY_VIEW_COUNT;
    return true;
}

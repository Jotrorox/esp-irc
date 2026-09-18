#include <assert.h>
#include <stdio.h>

#include "display_navigation.h"

static void sample(display_navigation_t *navigation, bool previous, bool next,
                   int64_t now, bool changed, display_view_t view)
{
    assert(display_navigation_update(navigation, previous, next, now) == changed);
    assert(navigation->view == view);
}

int main(void)
{
    display_navigation_t navigation;
    display_navigation_init(&navigation, false, false, 0);

    /* Bounce and a short tap must not navigate. */
    sample(&navigation, false, true, 10, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, false, 20, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, true, 30, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, true, 59, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, true, 60, true, DISPLAY_VIEW_NETWORK);
    /* Holding, including across a long gap, must not repeat. */
    sample(&navigation, false, true, 10000, false, DISPLAY_VIEW_NETWORK);
    /* Release bounce must not rearm the button prematurely. */
    sample(&navigation, false, false, 10010, false, DISPLAY_VIEW_NETWORK);
    sample(&navigation, false, true, 10020, false, DISPLAY_VIEW_NETWORK);
    sample(&navigation, false, true, 10060, false, DISPLAY_VIEW_NETWORK);
    sample(&navigation, false, false, 10070, false, DISPLAY_VIEW_NETWORK);
    sample(&navigation, false, false, 10100, false, DISPLAY_VIEW_NETWORK);
    sample(&navigation, false, true, 10110, false, DISPLAY_VIEW_NETWORK);
    sample(&navigation, false, true, 10140, true, DISPLAY_VIEW_SYSTEM);
    sample(&navigation, false, false, 10150, false, DISPLAY_VIEW_SYSTEM);
    sample(&navigation, false, false, 10180, false, DISPLAY_VIEW_SYSTEM);
    sample(&navigation, false, true, 10190, false, DISPLAY_VIEW_SYSTEM);
    sample(&navigation, false, true, 10220, true, DISPLAY_VIEW_OVERVIEW);

    display_navigation_init(&navigation, false, false, 0);
    sample(&navigation, true, false, 10, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, true, false, 40, true, DISPLAY_VIEW_SYSTEM);
    sample(&navigation, false, false, 50, false, DISPLAY_VIEW_SYSTEM);
    sample(&navigation, false, false, 80, false, DISPLAY_VIEW_SYSTEM);
    sample(&navigation, true, false, 90, false, DISPLAY_VIEW_SYSTEM);
    sample(&navigation, true, false, 120, true, DISPLAY_VIEW_NETWORK);

    /* Simultaneous presses and their releases must not change the view. */
    display_navigation_init(&navigation, false, false, 0);
    sample(&navigation, true, true, 10, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, true, true, 40, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, true, false, 50, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, true, false, 80, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, false, 90, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, false, 120, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, true, 130, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, true, 160, true, DISPLAY_VIEW_NETWORK);

    /* A startup-held button is ignored until released and pressed again. */
    display_navigation_init(&navigation, true, true, 0);
    sample(&navigation, true, true, 1000, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, false, 1010, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, false, false, 1040, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, true, false, 1050, false, DISPLAY_VIEW_OVERVIEW);
    sample(&navigation, true, false, 1080, true, DISPLAY_VIEW_SYSTEM);

    puts("Display navigation tests passed");
    return 0;
}

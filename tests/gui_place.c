/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The boxes of a screen (place.c): where each goes on a computer's window and on a phone held upright, the title
   beside the tabs or under them, the user at the head's right, the tips under the head or in the foot, and how a list
   of items fits its box. */
#include "gui/place.h"
#include <assert.h>
#include <stdio.h>

static int empty(GuiRect r) { return !r.w || !r.h; }
static int inside(GuiRect r, int width, int height) {
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= width && r.y + r.h <= height;
}
static int apart(GuiRect a, GuiRect b) {
    return empty(a) || empty(b) || a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
}
static int holds(GuiRect outer, GuiRect r) {
    return empty(r) ||
           (r.x >= outer.x && r.y >= outer.y && r.x + r.w <= outer.x + outer.w && r.y + r.h <= outer.y + outer.h);
}
/* every box in the area, none over another, the head round the tabs and the title */
static void sound(const GuiPlace* p, int width, int height) {
    const GuiRect all[] = {p->tabs, p->title, p->badge, p->tips, p->list, p->side, p->status, p->keys};
    size_t i, j, n = sizeof all / sizeof *all;
    for (i = 0; i < n; ++i) {
        assert(inside(all[i], width, height));
        for (j = i + 1; j < n; ++j) assert(apart(all[i], all[j]));
    }
    assert(holds(p->head, p->tabs) && holds(p->head, p->title) && holds(p->head, p->badge) && apart(p->head, p->list));
}

int main(void) {
    GuiPlaceIn in = {.width = 1280,
                     .height = 800,
                     .cw = 16,
                     .ch = 32,
                     .tabs = 40,
                     .tabs_w = 400,
                     .title = 64,
                     .title_w = 300,
                     .foot = 72,
                     .list_cols = 34,
                     .status = 96};
    GuiPlace p;
    int first;
    /* a computer's window, a column in from either side, top to bottom: the tabs at the left, the title under them,
       the list and the box beside it, the status box at the foot of the side's column; the foot under them, the tips
       under the list's column and the keys under the side box's */
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.tabs.x == 16 && p.tabs.y == 16 && p.tabs.w == 400 && p.tabs.h == 40);
    assert(p.title.x == 16 && p.title.y == p.tabs.y + 40 + 16 && p.title.h == 64);
    assert(p.head.y == 16 && p.head.y + p.head.h == p.title.y + 64 && p.head.w == 1248);
    assert(p.list.y == p.title.y + 64 + 16 && p.list.x == 16 && p.list.w == 34 * 16);
    assert(!p.stacked && p.side.y == p.list.y && p.side.x == p.list.x + p.list.w + 16);
    assert(p.side.x + p.side.w == 1280 - 16);
    assert(p.keys.y + p.keys.h == 800 - 16 && p.keys.h == 72 && p.list.y + p.list.h == p.keys.y - 16);
    assert(p.tips.y == p.keys.y && p.tips.h == 72 && p.tips.x == p.list.x && p.tips.w == p.list.w);
    assert(p.keys.x == p.side.x && p.keys.w == p.side.w);
    assert(p.status.h == 96 && p.status.y + p.status.h == p.list.y + p.list.h && p.status.x == p.side.x);
    assert(p.side.y + p.side.h == p.status.y - 16);
    /* the title beside the tabs: on their row against its right, the row as high as the taller, both at its foot */
    in.title_beside = 1;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.tabs.x == 16 && p.tabs.y == 16 + 64 - 40 && p.title.y == 16 && p.title.x + p.title.w == 1280 - 16);
    assert(p.head.y + p.head.h == 16 + 64 && p.list.y == 16 + 64 + 16);
    /* where both do not fit on the row, the title goes under the tabs; with no tabs, it is on a row of its own */
    in.title_w = 1248 - 400 - 2 * 16 + 1;
    gui_place(&in, &p);
    assert(p.title.y == p.tabs.y + 40 + 16 && p.title.x == 16);
    in.title_w = 300;
    in.tabs = in.tabs_w = 0;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(empty(p.tabs) && p.title.x == 16 && p.title.y == 16 && p.list.y == 16 + 64 + 16);
    in.tabs = 40;
    in.tabs_w = 400;
    /* the user, 64 high and 200 wide: against the right of the tabs' row past the title beside them, two columns
       between */
    in.badge = 64;
    in.badge_w = 200;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.badge.x + p.badge.w == 1280 - 16 && p.badge.y == 16 && p.badge.w == 200 && p.badge.h == 64);
    assert(p.title.x + p.title.w == p.badge.x - 32 && p.title.y == 16 && p.tabs.y == 16 + 64 - 40);
    assert(p.head.y + p.head.h == 16 + 64 && p.list.y == 16 + 64 + 16);
    /* taller than the title: the row is its height, the title and the tabs at its foot */
    in.badge = 80;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.badge.y == 16 && p.title.y == 16 + 80 - 64 && p.tabs.y == 16 + 80 - 40 && p.list.y == 16 + 80 + 16);
    in.badge = 64;
    /* the three do not fit on the row: the title goes under the tabs, the user at that row's right */
    in.title_w = 1248 - 400 - 2 * 16 - 200 - 32 + 1;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.title.y == p.tabs.y + 40 + 16 && p.title.x == 16 && p.title.w == 1248 - 200 - 32);
    assert(p.badge.y == p.title.y && p.badge.x + p.badge.w == 1280 - 16);
    in.title_w = 300;
    /* the title on its own row (below): the user at its right; with no title, at the right of the tabs */
    in.title_beside = 0;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.title.y == p.tabs.y + 40 + 16 && p.badge.y == p.title.y && p.title.w == 1248 - 200 - 32);
    in.title = in.title_w = 0;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(empty(p.title) && p.badge.y == 16 && p.tabs.y == 16 + 64 - 40 && p.badge.x + p.badge.w == 1280 - 16);
    /* a phone held upright, 360 wide: nothing fits beside the tabs, so the user has a row of its own under them */
    in.width = 360;
    in.tabs_w = 300;
    gui_place(&in, &p);
    sound(&p, 360, 800);
    assert(p.badge.y == p.tabs.y + 40 + 16 && p.badge.x + p.badge.w == 360 - 16 && p.list.y == p.badge.y + 64 + 16);
    /* wider than the screen: no user at all, the rest as without one */
    in.badge_w = 360;
    gui_place(&in, &p);
    sound(&p, 360, 800);
    assert(empty(p.badge) && p.list.y == p.tabs.y + 40 + 16);
    in.width = 1280;
    in.tabs_w = 400;
    in.title = 64;
    in.title_w = 300;
    in.badge = in.badge_w = 0;
    /* the tips on a row of their own under the head, the whole width: the keys alone in the foot, the whole width */
    in.tips = 40;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.tips.x == 16 && p.tips.w == 1248 && p.tips.y == p.title.y + 64 + 16 && p.tips.h == 40);
    assert(p.list.y == p.tips.y + 40 + 16 && p.keys.x == 16 && p.keys.w == 1248);
    in.tips = 0;
    /* nothing asked for, nothing given: no tabs, title, foot or status box, half a row from the top and the bottom */
    {
        GuiPlaceIn bare = {.width = 640, .height = 400, .cw = 8, .ch = 16, .list_cols = 30};
        gui_place(&bare, &p);
        sound(&p, 640, 400);
        assert(empty(p.head) && empty(p.tabs) && empty(p.title) && empty(p.keys) && empty(p.tips) && empty(p.status));
        assert(p.list.y == 8 && p.list.h == 400 - 16 && p.side.h == p.list.h);
    }
    /* one box, the whole width: no side box, the foot in halves */
    in.list_cols = 0;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.list.w == 1248 && empty(p.side) && empty(p.status));
    assert(p.tips.w == (1248 - 16) / 2 && p.keys.x == p.tips.x + p.tips.w + 16 && p.keys.x + p.keys.w == 1280 - 16);
    /* a phone held upright: too narrow for the list and a side box of GUI_PLACE_SIDE_MIN columns, so the side box goes
       under the list, the status box under it; the foot in halves */
    {
        GuiPlaceIn phone = {.width = 720,
                            .height = 1500,
                            .cw = 24,
                            .ch = 48,
                            .tabs = 56,
                            .tabs_w = 400,
                            .title = 96,
                            .title_w = 400,
                            .foot = 120,
                            .list_cols = 20,
                            .status = 144};
        assert(720 / 24 - 2 < GUI_PLACE_LIST_MIN + 1 + GUI_PLACE_SIDE_MIN);
        gui_place(&phone, &p);
        sound(&p, 720, 1500);
        assert(p.stacked && p.list.w == 720 - 48 && p.side.w == p.list.w && p.side.y > p.list.y + p.list.h);
        assert(p.list.h > p.side.h && p.status.y > p.side.y + p.side.h);
        assert(p.tips.w == (720 - 48 - 24) / 2 && p.keys.y == p.tips.y);
    }
    /* a list asking for more than leaves the side box its least: it gives way */
    {
        GuiPlaceIn narrow = {.width = 50 * 16, .height = 800, .cw = 16, .ch = 32, .list_cols = 30};
        gui_place(&narrow, &p);
        sound(&p, 50 * 16, 800);
        assert(!p.stacked && p.list.w == (48 - 1 - GUI_PLACE_SIDE_MIN) * 16 && p.side.w == GUI_PLACE_SIDE_MIN * 16);
    }
    /* a status box taller than its column takes half of it, the side box the rest */
    in.list_cols = 34;
    in.status = 2000;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.status.h == p.list.h / 2 && p.side.h == p.list.h - p.status.h - 16);
    /* a window too small for its foot: boxes of no size, never of a negative one */
    {
        GuiPlaceIn tiny = {.width = 100,
                           .height = 60,
                           .cw = 8,
                           .ch = 16,
                           .tabs = 20,
                           .tabs_w = 60,
                           .title = 32,
                           .title_w = 40,
                           .foot = 72,
                           .list_cols = 10,
                           .status = 16};
        gui_place(&tiny, &p);
        assert(p.list.h == 0 && p.list.w >= 0 && p.side.w >= 0 && p.status.h == 0);
    }
    /* a list: as many items as fit, the focus among them, from the first while it is in view */
    assert(gui_place_list(100, 20, 30, 10, -1, &first) == 3 && first == 0);
    assert(gui_place_list(100, 20, 30, 10, 2, &first) == 3 && first == 0);
    assert(gui_place_list(100, 20, 30, 10, 5, &first) == 3 && first == 3);
    assert(gui_place_list(100, 20, 30, 10, 9, &first) == 3 && first == 7);
    assert(gui_place_list(100, 20, 30, 2, 1, &first) == 2 && first == 0);
    assert(gui_place_list(10, 20, 30, 5, 3, &first) == 0 && first == 0); /* not even one fits */
    /* the pitch spreads few items, keeps many to the least */
    assert(gui_place_pitch(300, 10, 20, 50) == 30);
    assert(gui_place_pitch(300, 2, 20, 50) == 50 && gui_place_pitch(300, 30, 20, 50) == 20);
    assert(gui_place_pitch(300, 0, 20, 50) == 50);
    puts("gui place ok");
    /* fixed: the list two fifths of the width whatever it asks, the same on every screen; too narrow for both, the
       list alone */
    {
        GuiPlaceIn f = in;
        GuiPlace a, b;
        f.fixed = 1;
        f.list_cols = 20;
        gui_place(&f, &a);
        f.list_cols = 60;
        gui_place(&f, &b);
        sound(&a, 1280, 800);
        assert(a.list.w == (1248 / 16) * 2 / 5 * 16 && b.list.w == a.list.w && b.side.x == a.side.x);
        f.width = 600; /* 37 columns: under a list's least and a side box's */
        gui_place(&f, &a);
        sound(&a, 600, 800);
        assert(!a.stacked && empty(a.side) && empty(a.status) && a.list.w == 600 - 32);
        f.fixed = 0;
        gui_place(&f, &b);
        assert(b.stacked && !empty(b.side)); /* adaptive: under it */
    }
    return 0;
}

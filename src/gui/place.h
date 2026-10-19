/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Where a screen's boxes go, in pixels of the area it is drawn in: the row of tabs, the title after them or on a row
   of its own, the user against the head's right after the title, the tips under the head when they go there, the list and the box beside it (under it on a narrow
   screen), the status box under that one, in half its column at most, and the foot: the keys' box, and the tips' on
   its left under the list when they go there. A column of margin either side and half a row between boxes; nothing
   overlaps. */
#ifndef SNX_GUI_PLACE_H
#define SNX_GUI_PLACE_H

typedef struct {
    int x, y, w, h;
} GuiRect;
typedef struct {
    int width, height;  /* the area */
    int cw, ch;         /* a cell of the text */
    int tabs, tabs_w;   /* the row of tabs: its height and the tabs' width */
    int title, title_w; /* the title's height and width, 0 for none */
    int title_beside;   /* the title in the row of tabs, after them, when both fit */
    int badge, badge_w; /* the user's nickname and picture: their height and width, 0 for none */
    int tips;           /* the tips' height on a row under the head; 0: they are in the foot */
    int foot;           /* the foot's height: the keys' box, and the tips' beside it unless they are under the head */
    int keys_w;         /* the keys' box at least this wide, the tips' giving way */
    int list_cols;      /* the columns the list asks for; 0: one box, the whole width */
    int fixed;          /* the list two fifths of the width whatever it asks, the side box gone where both do not fit */
    int status;         /* the status box's height, 0 for none */
} GuiPlaceIn;
typedef struct {
    GuiRect head; /* the tabs and the title, the whole width */
    GuiRect tabs, title, badge, tips, list, side, status, keys;
    int stacked; /* the side box under the list: too narrow for both side by side */
} GuiPlace;
#define GUI_PLACE_SIDE_MIN 24 /* the side box's columns at least, beside the list */
#define GUI_PLACE_LIST_MIN 20 /* and the list's: narrower than both, the side box goes under the list */
void gui_place(const GuiPlaceIn* in, GuiPlace* out);
/* a box at x, y, w by h, of no size rather than a negative one */
GuiRect gui_rect(int x, int y, int w, int h);
/* count items item pixels high, pitch apart from the top of a box height high: as many as fit, the first of them
   chosen so focus (-1: none) is among them */
int gui_place_list(int height, int item, int pitch, int count, int focus, int* first);
/* the pitch that spreads count items over height, from least to most */
int gui_place_pitch(int height, int count, int least, int most);
#endif

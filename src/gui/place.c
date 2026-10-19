/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/place.h"
#include <string.h>

GuiRect gui_rect(int x, int y, int w, int h) {
    GuiRect r;
    r.x = x;
    r.y = y;
    r.w = w > 0 ? w : 0;
    r.h = h > 0 ? h : 0;
    return r;
}
void gui_place(const GuiPlaceIn* in, GuiPlace* out) {
    int mx = in->cw, gap = in->ch / 2, width = in->width - 2 * mx, y = gap, bottom, foot_y, cols;
    GuiRect content;
    memset(out, 0, sizeof *out);
    /* the head: the tabs at its left, the title after them against its right when both fit, else under them; the user
       against the right of the title's row, two columns past it as the title is past the tabs, or of the tabs' when there is no title; on a row of its
       own when it does not fit there */
    int badge = in->badge && in->badge_w ? in->badge_w + 2 * in->cw : 0;
    if (in->title && in->title_beside && in->tabs && in->tabs_w + 2 * in->cw + in->title_w + badge <= width) {
        int row = in->tabs > in->title ? in->tabs : in->title;
        if (badge && in->badge > row) row = in->badge;
        out->tabs = gui_rect(mx, y + row - in->tabs, in->tabs_w, in->tabs);
        out->title = gui_rect(mx + width - badge - in->title_w, y + row - in->title, in->title_w, in->title);
        if (badge) out->badge = gui_rect(mx + width - in->badge_w, y + row - in->badge, in->badge_w, in->badge);
        y += row + gap;
    } else {
        int beside = in->tabs && !in->title && in->tabs_w + badge <= width;
        if (in->tabs) {
            int row = beside && in->badge > in->tabs ? in->badge : in->tabs;
            out->tabs = gui_rect(mx, y + row - in->tabs, in->tabs_w, in->tabs);
            if (beside && badge)
                out->badge = gui_rect(mx + width - in->badge_w, y + row - in->badge, in->badge_w, in->badge);
            y += row + gap;
        }
        if (in->title) {
            int room = badge && in->title_w + badge <= width, row = in->title;
            if (room && in->badge > row) row = in->badge;
            out->title = gui_rect(mx, y + row - in->title, room ? width - badge : width, in->title);
            if (room) out->badge = gui_rect(mx + width - in->badge_w, y + row - in->badge, in->badge_w, in->badge);
            y += row + gap;
        }
        if (badge && !out->badge.h) { /* too narrow beside either: its own row, or none */
            if (in->badge_w <= width) out->badge = gui_rect(mx + width - in->badge_w, y, in->badge_w, in->badge);
            if (out->badge.h) y += in->badge + gap;
        }
    }
    out->head = gui_rect(mx, gap, width, y - 2 * gap);
    if (in->tips) {
        out->tips = gui_rect(mx, y, width, in->tips);
        y += in->tips + gap;
    }
    bottom = in->height - gap;
    foot_y = bottom - in->foot;
    if (in->foot) bottom -= in->foot + gap;
    content = gui_rect(mx, y, width, bottom - y);
    out->list = content;
    if (in->list_cols) {
        cols = content.w / in->cw;
        if (cols >= GUI_PLACE_LIST_MIN + 1 + GUI_PLACE_SIDE_MIN) { /* the list gives way to the side box's least */
            int asks =
                in->fixed ? (cols * 2 / 5 > GUI_PLACE_LIST_MIN ? cols * 2 / 5 : GUI_PLACE_LIST_MIN) : in->list_cols;
            int w = (asks < cols - 1 - GUI_PLACE_SIDE_MIN ? asks : cols - 1 - GUI_PLACE_SIDE_MIN) * in->cw;
            out->list = gui_rect(content.x, content.y, w, content.h);
            out->side = gui_rect(content.x + w + in->cw, content.y, content.w - w - in->cw, content.h);
        } else if (in->fixed) { /* a phone held upright: the list alone */
            out->list = content;
        } else { /* the list over three fifths, the side box under it */
            int h = (content.h - gap) * 3 / 5;
            out->stacked = 1;
            out->list = gui_rect(content.x, content.y, content.w, h);
            out->side = gui_rect(content.x, content.y + h + gap, content.w, content.h - h - gap);
        }
        if (in->status) { /* at most half the column: the side box shows what the focus is about */
            int h = in->status < out->side.h / 2 ? in->status : out->side.h / 2;
            out->status = gui_rect(out->side.x, out->side.y + out->side.h - h, out->side.w, h);
            out->side.h = out->side.h - h - gap > 0 ? out->side.h - h - gap : 0;
        }
    }
    /* the foot: the tips under the list's column when they go there, the keys under the side box's */
    if (!in->foot) return;
    if (in->tips)
        out->keys = gui_rect(mx, foot_y, width, in->foot);
    else {
        int w = in->list_cols && !out->stacked ? out->list.w : (width - in->cw) / 2;
        if (width - w - in->cw < in->keys_w) w = width - in->cw - in->keys_w;
        if (w < 0) w = 0;
        out->tips = gui_rect(mx, foot_y, w, in->foot);
        out->keys = gui_rect(mx + w + in->cw, foot_y, width - w - in->cw, in->foot);
    }
}
int gui_place_list(int height, int item, int pitch, int count, int focus, int* first) {
    int fit = height >= item && pitch > 0 ? (height - item) / pitch + 1 : 0, shown = fit < count ? fit : count;
    *first = 0;
    if (shown > 0 && focus >= shown) *first = focus - shown + 1;
    if (*first > count - shown) *first = count - shown;
    if (*first < 0) *first = 0;
    return shown;
}
int gui_place_pitch(int height, int count, int least, int most) {
    int pitch = count > 0 ? height / count : most;
    return pitch < least ? least : pitch > most ? most : pitch;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_OVERVIEW_H
#define LABWC_OVERVIEW_H

#include <stdbool.h>
#include <time.h>
#include <wayland-server-core.h>

struct lab_scene_rect;
struct output;
struct scaled_font_buffer;
struct view;
struct wl_event_source;
struct wlr_scene_buffer;
struct wlr_scene_node;
struct wlr_scene_rect;
struct wlr_scene_tree;

struct overview_item {
	struct view *view;
	struct wlr_scene_tree *tree;
	struct lab_scene_rect *border;          /* border rect for resize */
	struct wlr_scene_rect *hitbox;          /* invisible hitbox for resize */
	struct wlr_scene_buffer *thumbnail;     /* thumbnail for dest_size */
	struct lab_scene_rect *hover_border;    /* highlight on hover (hidden by default) */
	struct scaled_font_buffer *title_label; /* window title (shown after animation) */
	struct wl_list link;

	/* Start position: window's current position relative to content_tree */
	int normal_x, normal_y, normal_w, normal_h;
	/* End position: calculated overview position relative to content_tree */
	int overview_x, overview_y, overview_w, overview_h;
};

struct overview_state {
	bool active;
	struct wl_list items; /* struct overview_item.link */
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *background; /* full-screen overlay for fade */
	struct output *output;

	/* Animation */
	bool animating;
	bool closing;           /* true = animating toward t=0 (normal) */
	bool focus_on_finish;   /* focus selected view when close completes */
	struct view *selected_view;      /* view to focus when close animation ends */
	struct overview_item *hovered;   /* currently hovered item */
	double current_t;       /* current visual progress [0=normal, 1=overview] */
	double anim_from_t;
	double anim_to_t;
	struct timespec anim_start;
	uint32_t anim_duration_ms;
	struct wl_event_source *anim_timer;
};

/* Begin overview mode */
void overview_begin(void);

/* End overview mode */
void overview_finish(bool focus_selected);

/* Toggle overview mode */
void overview_toggle(void);

/* Focus the clicked window and close overview */
void overview_on_cursor_release(struct wlr_scene_node *node);

/* Update hover highlight based on node under cursor (NULL = no item hovered) */
void overview_on_cursor_motion(struct wlr_scene_node *node);

/* Get overview item from scene node */
struct overview_item *node_overview_item_from_node(
	struct wlr_scene_node *wlr_scene_node);

#endif /* LABWC_OVERVIEW_H */

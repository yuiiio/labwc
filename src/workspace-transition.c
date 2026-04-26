// SPDX-License-Identifier: GPL-2.0-only
#include "workspace-transition.h"
#include <math.h>
#include <time.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "common/scene-helpers.h"
#include "labwc.h"
#include "output.h"
#include "workspaces.h"

#define TRANSITION_MS 250

struct workspace_transition {
	bool active;
	struct wl_event_source *timer;
	struct wlr_scene_tree *from_overlay;
	struct workspace *from_ws;
	struct workspace *to_ws;
	struct output *output;
	int direction;
	int width;
	struct timespec start;
};

static struct workspace_transition trans;

static double
ease_in_out_cubic(double t)
{
	if (t < 0.5) {
		return 4.0 * t * t * t;
	}
	double f = 2.0 * t - 2.0;
	return 0.5 * f * f * f + 1.0;
}

static void
finish_transition(void)
{
	trans.active = false;
	if (trans.timer) {
		wl_event_source_remove(trans.timer);
		trans.timer = NULL;
	}
	if (trans.from_overlay) {
		wlr_scene_node_reparent(&trans.from_ws->tree->node,
			server.workspace_tree);
		wlr_scene_node_set_position(&trans.from_ws->tree->node, 0, 0);
		wlr_scene_node_set_enabled(&trans.from_ws->tree->node, false);
		wlr_scene_node_destroy(&trans.from_overlay->node);
		trans.from_overlay = NULL;
	}
	if (trans.to_ws) {
		wlr_scene_node_set_position(&trans.to_ws->tree->node, 0, 0);
		trans.to_ws = NULL;
	}
	trans.from_ws = NULL;
}

static int
transition_tick(void *data)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed_ms =
		(double)(now.tv_sec - trans.start.tv_sec) * 1000.0
		+ (double)(now.tv_nsec - trans.start.tv_nsec) / 1.0e6;
	double raw = fmin(1.0, elapsed_ms / (double)TRANSITION_MS);
	double eased = ease_in_out_cubic(raw);

	int slide = (int)(eased * trans.width);
	int from_x = -trans.direction * slide;
	int to_x = trans.direction * (trans.width - slide);

	wlr_scene_node_set_position(&trans.from_overlay->node, from_x, 0);
	wlr_scene_node_set_position(&trans.to_ws->tree->node, to_x, 0);
	wlr_output_schedule_frame(trans.output->wlr_output);

	if (raw < 1.0) {
		wl_event_source_timer_update(trans.timer, 8);
		return 0;
	}

	finish_transition();
	return 0;
}

void
workspace_transition_begin(struct workspace *target,
		int direction, bool update_focus)
{
	if (trans.active) {
		finish_transition();
	}

	struct output *output = output_nearest_to_cursor();
	if (!output) {
		workspaces_switch_to(target, update_focus);
		return;
	}

	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output, &box);
	int width = box.width;

	if (width <= 0) {
		workspaces_switch_to(target, update_focus);
		return;
	}

	struct workspace *from = server.workspaces.current;

	/*
	 * Reparent from->tree into an overlay raised above workspace_tree so
	 * it can slide out independently while layer_tree[0] (wallpaper) stays
	 * stationary at scene root.
	 */
	struct wlr_scene_tree *from_overlay =
		lab_wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_raise_to_top(&from_overlay->node);
	wlr_scene_node_reparent(&from->tree->node, from_overlay);

	workspaces_switch_to(target, update_focus);

	/* workspaces_switch_to() disabled from->tree; re-enable for animation */
	wlr_scene_node_set_enabled(&from->tree->node, true);

	/* Start to->tree off-screen in the incoming direction */
	wlr_scene_node_set_position(&target->tree->node, direction * width, 0);

	trans.active = true;
	trans.from_overlay = from_overlay;
	trans.from_ws = from;
	trans.to_ws = target;
	trans.output = output;
	trans.direction = direction;
	trans.width = width;
	clock_gettime(CLOCK_MONOTONIC, &trans.start);

	trans.timer = wl_event_loop_add_timer(
		server.wl_event_loop, transition_tick, NULL);
	wl_event_source_timer_update(trans.timer, 1);
}

bool
workspace_transition_is_active(void)
{
	return trans.active;
}

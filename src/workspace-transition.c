// SPDX-License-Identifier: GPL-2.0-only
#include "workspace-transition.h"
#include <math.h>
#include <time.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "common/scene-helpers.h"
#include "labwc.h"
#include "output.h"
#include "workspaces.h"

#define TRANSITION_MS 250

struct workspace_transition {
	bool active;
	struct wl_event_source *timer;
	struct wlr_scene_tree *overlay;
	struct wlr_scene_buffer *from_buf;
	struct wlr_scene_buffer *to_buf;
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
render_node(struct wlr_render_pass *pass, struct wlr_scene_node *node,
		int x, int y)
{
	if (!node->enabled) {
		return;
	}
	x += node->x;
	y += node->y;

	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node(pass, child, x, y);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		if (!sb->buffer) {
			break;
		}
		struct wlr_client_buffer *cb = wlr_client_buffer_get(sb->buffer);
		if (!cb || !cb->texture) {
			break;
		}
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = cb->texture,
			.src_box = sb->src_box,
			.dst_box = {
				.x = x,
				.y = y,
				.width = sb->dst_width,
				.height = sb->dst_height,
			},
			.transform = sb->transform,
		});
		break;
	}
	case WLR_SCENE_NODE_RECT:
		break;
	}
}

static struct wlr_buffer *
capture_workspace(struct workspace *workspace, struct output *output,
		int width, int height, int ox, int oy)
{
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server.allocator, width, height,
		&output->wlr_output->swapchain->format);
	if (!buf) {
		wlr_log(WLR_ERROR, "workspace transition: failed to allocate buffer");
		return NULL;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, buf, NULL);
	if (!pass) {
		wlr_log(WLR_ERROR, "workspace transition: failed to begin render pass");
		wlr_buffer_drop(buf);
		return NULL;
	}

	render_node(pass, &output->layer_tree[0]->node, -ox, -oy);
	render_node(pass, &workspace->tree->node, -ox, -oy);

	if (!wlr_render_pass_submit(pass)) {
		wlr_log(WLR_ERROR, "workspace transition: failed to submit render pass");
		wlr_buffer_drop(buf);
		return NULL;
	}
	return buf;
}

static void
finish_transition(void)
{
	trans.active = false;
	if (trans.timer) {
		wl_event_source_remove(trans.timer);
		trans.timer = NULL;
	}
	if (trans.overlay) {
		wlr_scene_node_destroy(&trans.overlay->node);
		trans.overlay = NULL;
	}
	trans.from_buf = NULL;
	trans.to_buf = NULL;
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

	wlr_scene_node_set_position(&trans.from_buf->node, from_x, 0);
	wlr_scene_node_set_position(&trans.to_buf->node, to_x, 0);
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
	int height = box.height;

	if (width <= 0 || height <= 0) {
		workspaces_switch_to(target, update_focus);
		return;
	}

	struct workspace *from = server.workspaces.current;

	struct wlr_buffer *from_buffer = capture_workspace(
		from, output, width, height, box.x, box.y);

	workspaces_switch_to(target, update_focus);

	struct wlr_buffer *to_buffer = capture_workspace(
		target, output, width, height, box.x, box.y);

	if (!from_buffer || !to_buffer) {
		if (from_buffer) {
			wlr_buffer_drop(from_buffer);
		}
		if (to_buffer) {
			wlr_buffer_drop(to_buffer);
		}
		return;
	}

	trans.overlay = lab_wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_set_position(&trans.overlay->node, box.x, box.y);
	wlr_scene_node_raise_to_top(&trans.overlay->node);

	trans.from_buf = lab_wlr_scene_buffer_create(trans.overlay, from_buffer);
	wlr_buffer_drop(from_buffer);
	wlr_scene_buffer_set_dest_size(trans.from_buf, width, height);
	wlr_scene_node_set_position(&trans.from_buf->node, 0, 0);

	trans.to_buf = lab_wlr_scene_buffer_create(trans.overlay, to_buffer);
	wlr_buffer_drop(to_buffer);
	wlr_scene_buffer_set_dest_size(trans.to_buf, width, height);
	wlr_scene_node_set_position(&trans.to_buf->node, direction * width, 0);

	trans.active = true;
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

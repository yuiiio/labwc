// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "common/array.h"
#include "common/lab-scene-rect.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "node.h"
#include "output.h"
#include "overview.h"
#include "theme.h"
#include "view.h"

#define OVERVIEW_GAP 15
#define OVERVIEW_PADDING 40

static struct overview_state overview;

/* Temporary struct for layout calculation */
struct layout_item {
	struct view *view;
	/* Original position (window center) for sorting */
	int orig_x, orig_y;
	/* Final position (top-left) */
	int x, y;
	/* Size after scaling */
	int width, height;
};

static void
render_node(struct wlr_render_pass *pass,
		struct wlr_scene_node *node, int x, int y)
{
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node(pass, child, x + node->x, y + node->y);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);
		if (!scene_buffer->buffer) {
			break;
		}
		struct wlr_texture *texture = NULL;
		struct wlr_client_buffer *client_buffer =
			wlr_client_buffer_get(scene_buffer->buffer);
		if (client_buffer) {
			texture = client_buffer->texture;
		}
		if (!texture) {
			break;
		}
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.src_box = scene_buffer->src_box,
			.dst_box = {
				.x = x,
				.y = y,
				.width = scene_buffer->dst_width,
				.height = scene_buffer->dst_height,
			},
			.transform = scene_buffer->transform,
		});
		break;
	}
	case WLR_SCENE_NODE_RECT:
		break;
	}
}

static struct wlr_buffer *
render_thumb(struct output *output, struct view *view)
{
	if (!view->content_tree) {
		return NULL;
	}
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(server.allocator,
		view->current.width, view->current.height,
		&output->wlr_output->swapchain->format);
	if (!buffer) {
		wlr_log(WLR_ERROR, "failed to allocate buffer for overview thumbnail");
		return NULL;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, buffer, NULL);
	render_node(pass, &view->content_tree->node, 0, 0);
	if (!wlr_render_pass_submit(pass)) {
		wlr_log(WLR_ERROR, "failed to submit render pass");
		wlr_buffer_drop(buffer);
		return NULL;
	}
	return buffer;
}

struct overview_item *
node_overview_item_from_node(struct wlr_scene_node *wlr_scene_node)
{
	assert(wlr_scene_node->data);
	struct node_descriptor *node_descriptor = wlr_scene_node->data;
	assert(node_descriptor->type == LAB_NODE_OVERVIEW_ITEM);
	return (struct overview_item *)node_descriptor->data;
}

static struct overview_item *
create_item_at(struct wlr_scene_tree *parent, struct view *view,
		struct output *output, int x, int y, int width, int height)
{
	struct overview_item *item = znew(*item);
	wl_list_append(&overview.items, &item->link);

	struct wlr_scene_tree *tree = lab_wlr_scene_tree_create(parent);
	node_descriptor_create(&tree->node, LAB_NODE_OVERVIEW_ITEM, view, item);
	item->tree = tree;
	item->view = view;

	wlr_scene_node_set_position(&tree->node, x, y);

	/* Border/highlight for item */
	struct theme *theme = rc.theme;
	struct lab_scene_rect_options opts = {
		.border_colors = (float *[1]) {
			theme->osd_window_switcher_thumbnail.item_active_border_color
		},
		.nr_borders = 1,
		.border_width = 2,
		.bg_color = theme->overview_bg_color,
		.width = width,
		.height = height,
	};
	lab_scene_rect_create(tree, &opts);

	/* Invisible hitbox for mouse clicks */
	lab_wlr_scene_rect_create(tree, width, height, (float[4]) {0});

	/* Thumbnail */
	struct wlr_buffer *thumb_buffer = render_thumb(output, view);
	if (thumb_buffer) {
		struct wlr_scene_buffer *thumb_scene_buffer =
			lab_wlr_scene_buffer_create(tree, thumb_buffer);
		wlr_buffer_drop(thumb_buffer);
		wlr_scene_buffer_set_dest_size(thumb_scene_buffer,
			width, height);
	}

	return item;
}

/* Row structure for layout packing (stack-allocated) */
struct row_info {
	int start_idx;
	int count;
	int width;
	int height;
};

/* Compare by height (ascending) for row packing */
static int
compare_by_height(const void *a, const void *b)
{
	const struct layout_item *ia = a;
	const struct layout_item *ib = b;

	if (ia->height != ib->height) {
		return ia->height - ib->height;
	}
	if (ia->orig_y != ib->orig_y) {
		return ia->orig_y - ib->orig_y;
	}
	/* Stability: fall back to creation_id */
	if (ia->view->creation_id != ib->view->creation_id) {
		return (ia->view->creation_id < ib->view->creation_id) ? 1 : -1;
	}
	return 0;
}

/* Compare by original X position for row-internal sorting */
static int
compare_by_orig_x(const void *a, const void *b)
{
	const struct layout_item *ia = a;
	const struct layout_item *ib = b;

	if (ia->orig_x != ib->orig_x) {
		return ia->orig_x - ib->orig_x;
	}
	return 0;
}

void
overview_begin(void)
{
	if (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH) {
		return;
	}

	struct output *output = output_nearest_to_cursor();
	if (!output || !output_is_usable(output)) {
		return;
	}

	/* Get output geometry */
	struct wlr_box output_box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output,
		&output_box);

	/* Collect all views */
	struct wl_array views;
	wl_array_init(&views);
	view_array_append(&views,
		LAB_VIEW_CRITERIA_CURRENT_WORKSPACE
		| LAB_VIEW_CRITERIA_NO_SKIP_WINDOW_SWITCHER);

	int count = wl_array_len(&views);
	if (count == 0) {
		wl_array_release(&views);
		return;
	}

	/* Available area for layout */
	int avail_width = output_box.width - 2 * OVERVIEW_PADDING;
	int avail_height = output_box.height - 2 * OVERVIEW_PADDING;

	/* Allocate layout items */
	struct layout_item *items = calloc(count, sizeof(*items));
	if (!items) {
		wl_array_release(&views);
		return;
	}

	/* Initialize items and calculate total area for scaling */
	struct view **view_ptr;
	int idx = 0;
	double total_area = 0;
	wl_array_for_each(view_ptr, &views) {
		struct view *v = *view_ptr;
		items[idx].view = v;
		items[idx].orig_x = v->current.x + v->current.width / 2;
		items[idx].orig_y = v->current.y + v->current.height / 2;
		total_area += (double)v->current.width * v->current.height;
		idx++;
	}

	/* Calculate common scale based on area */
	double avail_area = (double)avail_width * avail_height;
	double scale = sqrt(avail_area * 0.7 / total_area);
	scale = fmin(scale, 1.0);
	scale = fmax(scale, 0.05);

	/* Apply scale to all items */
	for (int i = 0; i < count; i++) {
		items[i].width = (int)(items[i].view->current.width * scale);
		items[i].height = (int)(items[i].view->current.height * scale);
	}

	/* Sort by height for row packing */
	qsort(items, count, sizeof(*items), compare_by_height);

	/* Row packing (stack-allocated, max 32 rows) */
	struct row_info rows[32];
	int row_count = 0;
	int current_row_width = 0;

	rows[0] = (struct row_info){0, 0, 0, 0};

	for (int i = 0; i < count; i++) {
		int w = items[i].width;
		int h = items[i].height;

		/* Start new row if this item doesn't fit */
		if (current_row_width + w + OVERVIEW_GAP > avail_width
				&& rows[row_count].count > 0
				&& row_count < 31) {
			row_count++;
			rows[row_count] = (struct row_info){i, 0, 0, 0};
			current_row_width = 0;
		}

		rows[row_count].count++;
		rows[row_count].width += w + OVERVIEW_GAP;
		if (h > rows[row_count].height) {
			rows[row_count].height = h;
		}
		current_row_width = rows[row_count].width;
	}
	row_count++;

	/* Sort items within each row by original X position */
	for (int r = 0; r < row_count; r++) {
		qsort(&items[rows[r].start_idx], rows[r].count,
			sizeof(*items), compare_by_orig_x);
	}

	/* Calculate total height */
	int total_height = 0;
	for (int r = 0; r < row_count; r++) {
		total_height += rows[r].height;
		if (r < row_count - 1) {
			total_height += OVERVIEW_GAP;
		}
	}

	/* Calculate positions with centering */
	int y_offset = (avail_height - total_height) / 2;
	int y = y_offset;
	for (int r = 0; r < row_count; r++) {
		int row_width = rows[r].width - OVERVIEW_GAP; /* Remove trailing gap */
		int x = (avail_width - row_width) / 2;

		for (int j = rows[r].start_idx;
				j < rows[r].start_idx + rows[r].count; j++) {
			items[j].x = x;
			items[j].y = y + (rows[r].height - items[j].height) / 2;
			x += items[j].width + OVERVIEW_GAP;
		}
		y += rows[r].height + OVERVIEW_GAP;
	}

	/* Initialize overview state */
	overview.active = true;
	overview.output = output;
	wl_list_init(&overview.items);

	/* Create overview tree */
	overview.tree = lab_wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_raise_to_top(&overview.tree->node);

	/* Background overlay */
	struct theme *theme = rc.theme;
	float bg_color[4] = {
		theme->overview_bg_color[0],
		theme->overview_bg_color[1],
		theme->overview_bg_color[2],
		theme->overview_bg_color[3],
	};
	struct wlr_scene_rect *bg = lab_wlr_scene_rect_create(overview.tree,
		output_box.width, output_box.height, bg_color);
	wlr_scene_node_set_position(&bg->node, output_box.x, output_box.y);

	/* Create content tree */
	struct wlr_scene_tree *content_tree =
		lab_wlr_scene_tree_create(overview.tree);
	wlr_scene_node_set_position(&content_tree->node,
		output_box.x + OVERVIEW_PADDING,
		output_box.y + OVERVIEW_PADDING);

	/* Create items at calculated positions */
	for (int i = 0; i < count; i++) {
		create_item_at(content_tree, items[i].view, output,
			items[i].x, items[i].y,
			items[i].width, items[i].height);
	}

	free(items);
	wl_array_release(&views);

	seat_focus_override_begin(&server.seat,
		LAB_INPUT_STATE_OVERVIEW, LAB_CURSOR_DEFAULT);

	cursor_update_focus();
}

void
overview_finish(bool focus_selected)
{
	if (!overview.active) {
		return;
	}

	if (overview.tree) {
		wlr_scene_node_destroy(&overview.tree->node);
	}

	struct overview_item *item, *tmp;
	wl_list_for_each_safe(item, tmp, &overview.items, link) {
		wl_list_remove(&item->link);
		free(item);
	}

	overview = (struct overview_state){0};
	wl_list_init(&overview.items);

	seat_focus_override_end(&server.seat, /*restore_focus*/ !focus_selected);
	cursor_update_focus();
}

void
overview_on_cursor_release(struct wlr_scene_node *node)
{
	assert(server.input_mode == LAB_INPUT_STATE_OVERVIEW);

	struct overview_item *item = node_overview_item_from_node(node);
	struct view *view = item->view;

	overview_finish(/*focus_selected*/ true);

	if (view) {
		desktop_focus_view(view, /*raise*/ true);
	}
}

void
overview_toggle(void)
{
	if (overview.active) {
		overview_finish(/*focus_selected*/ false);
	} else {
		overview_begin();
	}
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Overview layout algorithm based on KWin's expolayout
 * Original: SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>
 * Original: SPDX-FileCopyrightText: 2024 Yifan Zhu <fanzhuyifan@gmail.com>
 */
#include <assert.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "common/lab-scene-rect.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "common/string-helpers.h"
#include "config/rcxml.h"
#include "input/cursor.h"
#include "labwc.h"
#include "node.h"
#include "output.h"
#include "overview.h"
#include "scaled-buffer/scaled-font-buffer.h"
#include "theme.h"
#include "view.h"
#include "workspaces.h"

/* Layout parameters (matching KWin defaults) */
#define SEARCH_TOLERANCE 0.2

/* Animation duration in milliseconds for full open/close */
#define OVERVIEW_ANIM_MS 250
#define IDEAL_WIDTH_RATIO 0.8
#define RELATIVE_MARGIN 0.07
#define RELATIVE_MIN_LENGTH 0.15
#define MAX_GAP_RATIO 1.5
#define MAX_SCALE 1.0

static struct view *pending_selected_view;

/* Window rect with margins for layout calculation */
struct window_rect {
	double x, y, width, height;
	double center_x, center_y;
	int id;
	struct view *view;
};

/* A layer (row) of windows */
struct layer {
	double max_width;
	double max_height;
	double width;
	int *ids;
	int count;
};

/* Layered packing result */
struct layered_packing {
	double max_width;
	double width;
	double height;
	struct layer *layers;
	int layer_count;
	int *all_ids;  /* Single allocation for all layer ids */
};

/* Final layout result */
struct layout_result {
	double x, y, width, height;
};

static void output_overview_finish_immediate(struct output *output);
static void output_overview_show_labels(struct output *output);
static int ws_slide_tick(void *data);
static void overview_create_items(struct output *output,
	struct wlr_box *output_box);

static double
ease_in_out_cubic(double t)
{
	if (t < 0.5) {
		return 4.0 * t * t * t;
	}
	double f = 2.0 * t - 2.0;
	return 0.5 * f * f * f + 1.0;
}

static int
lerp_int(int a, int b, double t)
{
	return a + (int)((double)(b - a) * t);
}

static void
apply_visual_t(struct overview_state *ov, double t)
{
	struct overview_item *item;
	wl_list_for_each(item, &ov->items, link) {
		int x = lerp_int(item->normal_x, item->overview_x, t);
		int y = lerp_int(item->normal_y, item->overview_y, t);
		int w = lerp_int(item->normal_w, item->overview_w, t);
		int h = lerp_int(item->normal_h, item->overview_h, t);

		wlr_scene_node_set_position(&item->tree->node, x, y);
		lab_scene_rect_set_size(item->border, w, h);
		wlr_scene_rect_set_size(item->hitbox, w, h);
		if (item->thumbnail) {
			wlr_scene_buffer_set_dest_size(item->thumbnail, w, h);
		}
	}
}

static int
animation_tick(void *data)
{
	struct output *output = data;
	struct overview_state *ov = &output->overview;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed_ms =
		(double)(now.tv_sec - ov->anim_start.tv_sec) * 1000.0
		+ (double)(now.tv_nsec - ov->anim_start.tv_nsec) / 1.0e6;
	double raw = fmin(1.0, elapsed_ms / (double)ov->anim_duration_ms);
	double eased = ease_in_out_cubic(raw);
	double t = ov->anim_from_t + (ov->anim_to_t - ov->anim_from_t) * eased;

	ov->current_t = t;
	apply_visual_t(ov, t);
	wlr_output_schedule_frame(output->wlr_output);

	if (raw < 1.0) {
		wl_event_source_timer_update(ov->anim_timer, 8);
		return 0;
	}

	/* Animation complete */
	ov->animating = false;
	wl_event_source_remove(ov->anim_timer);
	ov->anim_timer = NULL;

	if (ov->closing) {
		output_overview_finish_immediate(output);
	} else {
		output_overview_show_labels(output);
	}
	return 0;
}

static void
output_overview_show_labels(struct output *output)
{
	struct overview_state *ov = &output->overview;
	struct theme *theme = rc.theme;
	float *border_color =
		theme->osd_window_switcher_thumbnail.item_active_border_color;

	struct overview_item *item;
	wl_list_for_each(item, &ov->items, link) {
		int w = item->overview_w;
		int h = item->overview_h;

		/* Hover border: shown on demand, initially hidden */
		struct lab_scene_rect_options hover_opts = {
			.border_colors = (float *[1]){border_color},
			.nr_borders = 1,
			.border_width = 3,
			.bg_color = NULL,
			.width = w,
			.height = h,
		};
		item->hover_border = lab_scene_rect_create(item->tree, &hover_opts);
		wlr_scene_node_set_enabled(
			&item->hover_border->tree->node, false);

		/* Title label below thumbnail */
		if (!string_null_or_empty(item->view->title)) {
			item->title_label = scaled_font_buffer_create(item->tree);
			wlr_scene_node_set_position(
				&item->title_label->scene_buffer->node,
				0, h + 2);
			scaled_font_buffer_update(item->title_label,
				item->view->title, w,
				&rc.font_osd,
				theme->osd_label_text_color, theme->osd_bg_color);
		}
	}

	/* Update hover state for whatever is already under the cursor */
	struct cursor_context ctx = get_cursor_context();
	overview_on_cursor_motion(
		ctx.type == LAB_NODE_OVERVIEW_ITEM ? ctx.node : NULL);
}

void
overview_on_cursor_motion(struct wlr_scene_node *node)
{
	struct overview_item *hovered = NULL;
	if (node) {
		hovered = node_overview_item_from_node(node);
	}

	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		struct overview_state *ov = &output->overview;
		if (!ov->active || ov->animating || ov->ws_sliding) {
			continue;
		}
		struct overview_item *new_hov =
			(hovered && hovered->output == output) ? hovered : NULL;
		if (ov->hovered == new_hov) {
			continue;
		}
		if (ov->hovered && ov->hovered->hover_border) {
			wlr_scene_node_set_enabled(
				&ov->hovered->hover_border->tree->node, false);
		}
		ov->hovered = new_hov;
		if (ov->hovered && ov->hovered->hover_border) {
			wlr_scene_node_set_enabled(
				&ov->hovered->hover_border->tree->node, true);
		}
	}
}

static void
start_animation(struct output *output, double from_t, double to_t)
{
	struct overview_state *ov = &output->overview;
	double distance = fabs(to_t - from_t);
	if (distance < 0.001) {
		ov->current_t = to_t;
		apply_visual_t(ov, to_t);
		if (ov->closing) {
			output_overview_finish_immediate(output);
		}
		return;
	}

	ov->animating = true;
	ov->anim_from_t = from_t;
	ov->anim_to_t = to_t;
	ov->anim_duration_ms = (uint32_t)(distance * OVERVIEW_ANIM_MS);
	clock_gettime(CLOCK_MONOTONIC, &ov->anim_start);

	if (!ov->anim_timer) {
		ov->anim_timer = wl_event_loop_add_timer(
			server.wl_event_loop, animation_tick, output);
	}
	wl_event_source_timer_update(ov->anim_timer, 1);
}

static void
render_node(struct wlr_render_pass *pass,
		struct wlr_scene_node *node, int x, int y)
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
	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = {
				.x = x,
				.y = y,
				.width = rect->width,
				.height = rect->height,
			},
			.color = {
				.r = rect->color[0],
				.g = rect->color[1],
				.b = rect->color[2],
				.a = rect->color[3],
			},
			.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		});
		break;
	}
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
	if (!pass) {
		wlr_log(WLR_ERROR, "failed to begin render pass for overview thumbnail");
		wlr_buffer_drop(buffer);
		return NULL;
	}
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
create_item_at(struct output *output, struct view *view,
		int x, int y, int width, int height)
{
	struct overview_state *ov = &output->overview;
	struct overview_item *item = znew(*item);
	wl_list_append(&ov->items, &item->link);

	struct wlr_scene_tree *tree = lab_wlr_scene_tree_create(ov->content_tree);
	node_descriptor_create(&tree->node, LAB_NODE_OVERVIEW_ITEM, view, item);
	item->tree = tree;
	item->view = view;
	item->output = output;

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
	item->border = lab_scene_rect_create(tree, &opts);

	/* Invisible hitbox for mouse clicks */
	item->hitbox = lab_wlr_scene_rect_create(tree, width, height,
		(float[4]){0});

	/* Thumbnail */
	struct wlr_buffer *thumb_buffer = render_thumb(output, view);
	if (thumb_buffer) {
		item->thumbnail = lab_wlr_scene_buffer_create(tree, thumb_buffer);
		wlr_buffer_drop(thumb_buffer);
		wlr_scene_buffer_set_dest_size(item->thumbnail, width, height);
	}

	return item;
}

/* Compare windows by height (ascending), then by y position */
static int
compare_by_height(const void *a, const void *b)
{
	const struct window_rect *wa = a;
	const struct window_rect *wb = b;

	if (wa->height != wb->height) {
		return (wa->height < wb->height) ? -1 : 1;
	}
	if (wa->center_y != wb->center_y) {
		return (wa->center_y < wb->center_y) ? -1 : 1;
	}

	/* Stability: fall back to creation_id */
	if (wa->view->creation_id != wb->view->creation_id) {
		return (wa->view->creation_id < wb->view->creation_id) ? 1 : -1;
	}

	return 0;
}

/* Temporary struct for sorting layer windows by x position */
struct sort_item {
	int id;
	double center_x;
};

/* Compare sort_items by x position */
static int
compare_by_x(const void *a, const void *b)
{
	const struct sort_item *sa = a;
	const struct sort_item *sb = b;
	if (sa->center_x != sb->center_x) {
		return (sa->center_x < sb->center_x) ? -1 : 1;
	}
	return 0;
}

/*
 * Weight function for LWS algorithm
 * Penalizes non-uniform row widths
 */
static double
layer_weight(double ideal_width, double max_width,
		double *cum_widths, int start, int end, int count)
{
	double width = cum_widths[end] - cum_widths[start];

	if (width < ideal_width) {
		double diff = (width - ideal_width) / ideal_width;
		return diff * diff;
	} else {
		double penalty_factor = count;
		double diff = (width - ideal_width) / (max_width - ideal_width);
		return penalty_factor * diff * diff;
	}
}

/*
 * Simplified LWS algorithm to find optimal layer boundaries
 * Returns array of layer start positions
 */
static void
get_layer_start_positions(double max_width, double ideal_width,
		int count, double *cum_widths, int *out_layer_count, int *layer_positions,
		double *least_weight, int *layer_start)
{
	/* least_weight[i] = minimum weight to arrange first i windows */
	/* layer_start[i] = where last layer starts for first i windows */

	least_weight[0] = 0;

	for (int i = 1; i <= count; i++) {
		double best_weight = DBL_MAX;
		int best_start = 0;

		for (int j = 0; j < i; j++) {
			double w = least_weight[j] +
				layer_weight(ideal_width, max_width,
					cum_widths, j, i, count);
			if (w < best_weight) {
				best_weight = w;
				best_start = j;
			}
		}

		least_weight[i] = best_weight;
		layer_start[i] = best_start;
	}

	/* Reconstruct layer boundaries */
	int pos_count = 0;

	int current = count;
	while (current > 0) {
		layer_positions[pos_count++] = current;
		current = layer_start[current];
	}
	layer_positions[pos_count++] = 0;

	/* Reverse in place */
	for (int i = 0; i < pos_count / 2; i++) {
		int tmp = layer_positions[i];
		layer_positions[i] = layer_positions[pos_count - 1 - i];
		layer_positions[pos_count - 1 - i] = tmp;
	}

	*out_layer_count = pos_count;
}

/*
 * Create a layered packing from window sizes
 */
static struct layered_packing
create_packing(double max_width, struct window_rect *sorted_windows,
		int count, int *layer_positions, int layer_count)
{
	struct layered_packing packing = {
		.max_width = max_width,
		.width = 0,
		.height = 0,
		.layers = znew_n(struct layer, layer_count - 1),
		.layer_count = layer_count - 1,
		.all_ids = znew_n(int, count),
	};

	int id_offset = 0;
	for (int l = 0; l < layer_count - 1; l++) {
		int start = layer_positions[l];
		int end = layer_positions[l + 1];
		int layer_window_count = end - start;

		struct layer *layer = &packing.layers[l];
		layer->max_width = max_width;
		layer->max_height = sorted_windows[end - 1].height;
		layer->width = 0;
		layer->ids = packing.all_ids + id_offset;
		layer->count = layer_window_count;
		id_offset += layer_window_count;

		for (int i = start; i < end; i++) {
			layer->ids[i - start] = sorted_windows[i].id;
			layer->width += sorted_windows[i].width;
		}

		if (layer->width > packing.width) {
			packing.width = layer->width;
		}
		packing.height += layer->max_height;
	}

	return packing;
}

static void
evaluate_packing(struct window_rect *sorted_windows,
		int count, int *layer_positions, int layer_count,
		double *out_width, double *out_height)
{
	double max_width = 0.0;
	double total_height = 0.0;

	for (int l = 0; l < layer_count - 1; l++) {
		int start = layer_positions[l];
		int end = layer_positions[l + 1];

		double layer_width = 0.0;

		for (int i = start; i < end; i++) {
			layer_width += sorted_windows[i].width;
		}

		if (layer_width > max_width) {
			max_width = layer_width;
		}
		total_height += sorted_windows[end - 1].height;
	}

	*out_width = max_width;
	*out_height = total_height;
}

static void
free_packing(struct layered_packing *packing)
{
	free(packing->all_ids);
	free(packing->layers);
}

/*
 * Find good packing using binary search on strip width
 */
static struct layered_packing
find_good_packing(double area_width, double area_height,
		struct window_rect *windows, int count)
{
	/*
	 * Allocate all temporary arrays in a single malloc.
	 * Layout (aligned for double first, then int):
	 *   sorted:             count * sizeof(window_rect)
	 *   cum_widths:         (count+1) * sizeof(double)
	 *   tmp_least_weight:   (count+1) * sizeof(double)
	 *   min_layer_positions:(count+2) * sizeof(int)
	 *   max_layer_positions:(count+2) * sizeof(int)
	 *   mid_layer_positions:(count+2) * sizeof(int)
	 *   tmp_layer_start:    (count+1) * sizeof(int)
	 */
	size_t sorted_size = count * sizeof(struct window_rect);
	size_t cum_widths_size = (count + 1) * sizeof(double);
	size_t least_weight_size = (count + 1) * sizeof(double);
	size_t layer_pos_size = (count + 2) * sizeof(int);
	size_t layer_start_size = (count + 1) * sizeof(int);

	size_t total_size = sorted_size + cum_widths_size + least_weight_size
		+ 3 * layer_pos_size + layer_start_size;

	char *buf = xmalloc(total_size);
	char *ptr = buf;
	struct window_rect *sorted = (struct window_rect *)ptr;
	ptr += sorted_size;
	double *cum_widths = (double *)ptr;
	ptr += cum_widths_size;
	double *tmp_least_weight = (double *)ptr;
	ptr += least_weight_size;
	int *min_layer_positions = (int *)ptr;
	ptr += layer_pos_size;
	int *max_layer_positions = (int *)ptr;
	ptr += layer_pos_size;
	int *mid_layer_positions = (int *)ptr;
	ptr += layer_pos_size;
	int *tmp_layer_start = (int *)ptr;

	/* Sort windows by height */
	memcpy(sorted, windows, count * sizeof(struct window_rect));
	qsort(sorted, count, sizeof(struct window_rect), compare_by_height);

	/* Calculate cumulative widths */
	double strip_width_min = 0;
	double strip_width_max = 0;

	cum_widths[0] = 0;
	for (int i = 0; i < count; i++) {
		cum_widths[i + 1] = cum_widths[i] + sorted[i].width;
		if (sorted[i].width > strip_width_min) {
			strip_width_min = sorted[i].width;
		}
		strip_width_max += sorted[i].width;
	}

	strip_width_min /= IDEAL_WIDTH_RATIO;
	strip_width_max /= IDEAL_WIDTH_RATIO;

	double target_ratio = area_height / area_width;

	/* Binary search for optimal strip width */
	struct layered_packing best_packing = {0};

	int min_layer_count;
	double min_width;
	double min_height;
	int max_layer_count;
	double max_width;
	double max_height;
	int mid_layer_count;
	double mid_width;
	double mid_height;

	int *best_layer_positions;
	int best_layer_count;
	double best_strip_width;

	/* Try minimum width */
	get_layer_start_positions(strip_width_min,
		strip_width_min * IDEAL_WIDTH_RATIO, count, cum_widths,
		&min_layer_count, min_layer_positions,
		tmp_least_weight, tmp_layer_start);
	evaluate_packing(sorted, count, min_layer_positions, min_layer_count,
			&min_width, &min_height);

	double ratio_high = min_height / min_width;
	if (ratio_high <= target_ratio) {
		best_layer_positions = min_layer_positions;
		best_layer_count = min_layer_count;
		best_strip_width = strip_width_min;
		goto result;
	}

	/* Try maximum width */
	get_layer_start_positions(strip_width_max,
		strip_width_max * IDEAL_WIDTH_RATIO, count, cum_widths,
		&max_layer_count, max_layer_positions,
		tmp_least_weight, tmp_layer_start);
	evaluate_packing(sorted, count, max_layer_positions, max_layer_count,
			&max_width, &max_height);

	double ratio_low = max_height / max_width;
	if (ratio_low >= target_ratio) {
		best_layer_positions = max_layer_positions;
		best_layer_count = max_layer_count;
		best_strip_width = strip_width_max;
		goto result;
	}

	/* Binary search (max 10 iterations to guarantee termination) */
	for (int iter = 0; iter < 10 &&
			strip_width_max / strip_width_min > 1 + SEARCH_TOLERANCE; iter++) {
		double strip_width_mid = sqrt(strip_width_min * strip_width_max);

		get_layer_start_positions(strip_width_mid,
			strip_width_mid * IDEAL_WIDTH_RATIO, count, cum_widths,
			&mid_layer_count, mid_layer_positions,
			tmp_least_weight, tmp_layer_start);

		evaluate_packing(sorted, count, mid_layer_positions, mid_layer_count,
				&mid_width, &mid_height);

		double ratio_mid = mid_height / mid_width;

		if (ratio_mid > target_ratio) {
			strip_width_min = strip_width_mid;
			// swap
			int *temp = min_layer_positions;
			min_layer_positions = mid_layer_positions;
			mid_layer_positions = temp;

			min_layer_count = mid_layer_count;
			min_width = mid_width;
			min_height = mid_height;
			ratio_high = ratio_mid;
		} else {
			strip_width_max = strip_width_mid;
			// swap
			int *temp = max_layer_positions;
			max_layer_positions = mid_layer_positions;
			mid_layer_positions = temp;

			max_layer_count = mid_layer_count;
			max_width = mid_width;
			max_height = mid_height;
			ratio_low = ratio_mid;
		}
	}

	/* Choose packing with better scale */
	double scale_min = fmin(area_width / min_width,
		area_height / min_height);
	double scale_max = fmin(area_width / max_width,
		area_height / max_height);

	if (scale_min > scale_max) {
		best_layer_positions = min_layer_positions;
		best_layer_count = min_layer_count;
		best_strip_width = strip_width_min;
	} else {
		best_layer_positions = max_layer_positions;
		best_layer_count = max_layer_count;
		best_strip_width = strip_width_max;
	}

result:
	best_packing = create_packing(best_strip_width,
		sorted, count, best_layer_positions, best_layer_count);
	free(buf);

	return best_packing;
}

/*
 * Apply packing and compute final window positions
 */
static void
apply_packing(double area_x, double area_y, double area_width, double area_height,
		double margin, struct layered_packing *packing,
		struct window_rect *windows, int count,
		struct layout_result *results)
{
	/* Scale packing to fit area */
	double scale = fmin(area_width / packing->width,
		area_height / packing->height);
	scale = fmin(scale, MAX_SCALE);

	double scaled_margin = margin * scale;

	/* Maximum additional gap */
	double max_gap_y = MAX_GAP_RATIO * 2 * scaled_margin;
	double max_gap_x = MAX_GAP_RATIO * 2 * scaled_margin;

	/* Center align vertically */
	double extra_y = area_height - packing->height * scale;
	double gap_y = fmin(max_gap_y, extra_y / (packing->layer_count + 1));
	double y = area_y + (extra_y - gap_y * (packing->layer_count - 1)) / 2;

	/* Find max layer count for sort_items allocation */
	int max_layer_window_count = 0;
	for (int l = 0; l < packing->layer_count; l++) {
		if (packing->layers[l].count > max_layer_window_count) {
			max_layer_window_count = packing->layers[l].count;
		}
	}
	struct sort_item *sort_items = xmalloc(
		max_layer_window_count * sizeof(*sort_items));

	for (int l = 0; l < packing->layer_count; l++) {
		struct layer *layer = &packing->layers[l];

		/* Sort windows within layer by x position */
		for (int i = 0; i < layer->count; i++) {
			int id = layer->ids[i];
			sort_items[i].id = id;
			sort_items[i].center_x = windows[id].center_x;
		}
		qsort(sort_items, layer->count, sizeof(*sort_items), compare_by_x);
		for (int i = 0; i < layer->count; i++) {
			layer->ids[i] = sort_items[i].id;
		}

		double extra_x = area_width - layer->width * scale;
		double gap_x = fmin(max_gap_x, extra_x / (layer->count + 1));
		double x = area_x + (extra_x - gap_x * (layer->count - 1)) / 2;

		for (int i = 0; i < layer->count; i++) {
			int id = layer->ids[i];
			struct window_rect *win = &windows[id];

			/* Center align vertically within layer */
			double new_y = y + (layer->max_height - win->height) * scale / 2;

			/* Apply scaling and margins */
			results[id].x = x + scaled_margin;
			results[id].y = new_y + scaled_margin;
			results[id].width = (win->width - 2 * margin) * scale;
			results[id].height = (win->height - 2 * margin) * scale;

			x += win->width * scale + gap_x;
		}

		y += layer->max_height * scale + gap_y;
	}

	free(sort_items);
}

static void
overview_create_items(struct output *output, struct wlr_box *output_box)
{
	struct overview_state *ov = &output->overview;

	struct wl_array all_views;
	wl_array_init(&all_views);
	view_array_append(&all_views,
		LAB_VIEW_CRITERIA_CURRENT_WORKSPACE
		| LAB_VIEW_CRITERIA_NO_SKIP_WINDOW_SWITCHER);

	/* Count views whose primary output matches this output */
	int count = 0;
	struct view **view_ptr;
	wl_array_for_each(view_ptr, &all_views) {
		if ((*view_ptr)->output == output) {
			count++;
		}
	}
	if (count == 0) {
		wl_array_release(&all_views);
		return;
	}

	double short_side = fmin(output_box->width, output_box->height);
	double margin = RELATIVE_MARGIN * short_side;
	double min_length = RELATIVE_MIN_LENGTH * short_side;
	double area_width = output_box->width - 2 * margin;
	double area_height = output_box->height - 2 * margin;

	struct window_rect *windows = znew_n(*windows, count);
	int idx = 0;
	wl_array_for_each(view_ptr, &all_views) {
		struct view *v = *view_ptr;
		if (v->output != output) {
			continue;
		}
		double w = fmax(v->current.width, min_length) + 2 * margin;
		double h = fmax(v->current.height, min_length) + 2 * margin;
		w = fmin(w, 4 * area_width);
		h = fmin(h, 4 * area_height);
		windows[idx].x = v->current.x;
		windows[idx].y = v->current.y;
		windows[idx].width = w;
		windows[idx].height = h;
		windows[idx].center_x = v->current.x + v->current.width / 2.0;
		windows[idx].center_y = v->current.y + v->current.height / 2.0;
		windows[idx].id = idx;
		windows[idx].view = v;
		idx++;
	}

	struct layered_packing packing = find_good_packing(
		area_width, area_height, windows, count);
	struct layout_result *results = znew_n(*results, count);
	apply_packing(0, 0, area_width, area_height, margin, &packing,
		windows, count, results);

	/*
	 * Create items in reverse focus order so that the frontmost window
	 * is added last and rendered on top.
	 */
	for (int i = count - 1; i >= 0; i--) {
		struct view *v = windows[i].view;
		int norm_x = v->current.x - ov->content_x;
		int norm_y = v->current.y - ov->content_y;
		int norm_w = v->current.width;
		int norm_h = v->current.height;

		struct overview_item *item = create_item_at(output, v,
			norm_x, norm_y, norm_w, norm_h);

		item->normal_x = norm_x;
		item->normal_y = norm_y;
		item->normal_w = norm_w;
		item->normal_h = norm_h;
		item->overview_x = (int)results[i].x;
		item->overview_y = (int)results[i].y;
		item->overview_w = (int)results[i].width;
		item->overview_h = (int)results[i].height;
	}

	free_packing(&packing);
	free(results);
	free(windows);
	wl_array_release(&all_views);
}

static void
output_overview_finish_immediate(struct output *output)
{
	struct overview_state *ov = &output->overview;

	if (ov->anim_timer) {
		wl_event_source_remove(ov->anim_timer);
		ov->anim_timer = NULL;
	}

	if (ov->ws_slide_timer) {
		wl_event_source_remove(ov->ws_slide_timer);
		ov->ws_slide_timer = NULL;
	}

	/*
	 * ws_slide_old_content is a child of ov->tree and will be
	 * destroyed with it below.  Only free the item structs here.
	 */
	struct overview_item *old_item, *old_tmp;
	wl_list_for_each_safe(old_item, old_tmp,
			&ov->ws_slide_old_items, link) {
		wl_list_remove(&old_item->link);
		free(old_item);
	}

	if (ov->tree) {
		/* Restore wallpaper to scene root before destroying overview tree */
		wlr_scene_node_reparent(&output->layer_tree[0]->node,
			&server.scene->tree);
		wlr_scene_node_lower_to_bottom(
			&output->layer_tree[1]->node);
		wlr_scene_node_lower_to_bottom(
			&output->layer_tree[0]->node);
		wlr_scene_node_destroy(&ov->tree->node);
	}

	struct overview_item *item, *tmp;
	wl_list_for_each_safe(item, tmp, &ov->items, link) {
		wl_list_remove(&item->link);
		free(item);
	}

	*ov = (struct overview_state){0};
	/* active is now false */

	if (!overview_is_active()) {
		seat_focus_override_end(&server.seat,
			/*restore_focus*/ !pending_selected_view);
		if (pending_selected_view) {
			desktop_focus_view(pending_selected_view, /*raise*/ true);
		}
		pending_selected_view = NULL;
		cursor_update_focus();
	}
}

static void
output_overview_start_close(struct output *output)
{
	struct overview_state *ov = &output->overview;

	ov->closing = true;

	/* Hide labels and borders immediately when close starts */
	struct overview_item *item;
	wl_list_for_each(item, &ov->items, link) {
		if (item->hover_border) {
			wlr_scene_node_set_enabled(
				&item->hover_border->tree->node, false);
		}
		if (item->title_label) {
			wlr_scene_node_set_enabled(
				&item->title_label->scene_buffer->node, false);
		}
	}

	if (ov->animating) {
		/*
		 * Reverse mid-open animation. Mirror the raw progress so the
		 * visual position stays continuous (approximate for cubic easing
		 * but imperceptible at this timescale).
		 */
		double midpoint = ov->current_t;
		if (ov->anim_timer) {
			wl_event_source_remove(ov->anim_timer);
			ov->anim_timer = NULL;
		}
		ov->animating = false;
		start_animation(output, midpoint, 0.0);
	} else {
		start_animation(output, ov->current_t, 0.0);
	}
}

static void
output_overview_begin_one(struct output *output)
{
	struct overview_state *ov = &output->overview;

	struct wlr_box output_box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output,
		&output_box);

	ov->active = true;
	wl_list_init(&ov->items);
	wl_list_init(&ov->ws_slide_old_items);

	/* Create overview tree */
	ov->tree = lab_wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_raise_to_top(&ov->tree->node);

	/* Move wallpaper into overview tree as bottommost layer */
	wlr_scene_node_reparent(&output->layer_tree[0]->node, ov->tree);
	wlr_scene_node_lower_to_bottom(&output->layer_tree[0]->node);

	/* Background overlay */
	struct theme *theme = rc.theme;
	float bg[4] = {
		theme->overview_bg_color[0],
		theme->overview_bg_color[1],
		theme->overview_bg_color[2],
		theme->overview_bg_color[3],
	};
	ov->background = lab_wlr_scene_rect_create(ov->tree,
		output_box.width, output_box.height, bg);
	wlr_scene_node_set_position(&ov->background->node,
		output_box.x, output_box.y);

	/* Create content tree and store its position for slide use */
	double short_side = fmin(output_box.width, output_box.height);
	double margin = RELATIVE_MARGIN * short_side;
	ov->content_x = output_box.x + (int)margin;
	ov->content_y = output_box.y + (int)margin;
	ov->content_tree = lab_wlr_scene_tree_create(ov->tree);
	wlr_scene_node_set_position(&ov->content_tree->node,
		ov->content_x, ov->content_y);

	/* Create items for current workspace */
	overview_create_items(output, &output_box);

	if (wl_list_empty(&ov->items)) {
		/* Empty workspace: show background only, no animation needed */
		ov->current_t = 1.0;
		return;
	}

	/* Animate from normal window positions to overview positions */
	ov->closing = false;
	ov->current_t = 0.0;
	start_animation(output, 0.0, 1.0);
}

void
overview_begin(void)
{
	if (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH) {
		return;
	}

	bool any = false;
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output)) {
			output_overview_begin_one(output);
			any = true;
		}
	}
	if (!any) {
		return;
	}

	seat_focus_override_begin(&server.seat,
		LAB_INPUT_STATE_OVERVIEW, LAB_CURSOR_DEFAULT);
	cursor_update_focus();
}

void
overview_finish(bool focus_selected)
{
	if (!overview_is_active()) {
		return;
	}

	pending_selected_view = NULL;
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		struct overview_state *ov = &output->overview;
		if (!ov->active) {
			continue;
		}
		if (ov->closing || ov->ws_sliding) {
			return;
		}
		if (focus_selected && !pending_selected_view) {
			pending_selected_view = ov->selected_view;
		}
	}

	wl_list_for_each(output, &server.outputs, link) {
		if (output->overview.active) {
			output_overview_start_close(output);
		}
	}
}

void
overview_on_cursor_release(struct wlr_scene_node *node)
{
	assert(server.input_mode == LAB_INPUT_STATE_OVERVIEW);

	struct overview_item *item = node_overview_item_from_node(node);

	/*
	 * Raise the selected item's scene tree to the top so it stays
	 * visually in the foreground during the close animation.
	 */
	wlr_scene_node_raise_to_top(&item->tree->node);
	item->output->overview.selected_view = item->view;

	overview_finish(/*focus_selected*/ true);
}

void
overview_toggle(void)
{
	if (overview_is_active()) {
		overview_finish(/*focus_selected*/ false);
	} else {
		overview_begin();
	}
}

bool
overview_is_active(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->overview.active) {
			return true;
		}
	}
	return false;
}

static int
ws_slide_tick(void *data)
{
	struct output *output = data;
	struct overview_state *ov = &output->overview;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed_ms =
		(double)(now.tv_sec - ov->ws_slide_start.tv_sec) * 1000.0
		+ (double)(now.tv_nsec - ov->ws_slide_start.tv_nsec) / 1.0e6;
	double raw = fmin(1.0, elapsed_ms / (double)OVERVIEW_ANIM_MS);
	double eased = ease_in_out_cubic(raw);

	int slide = (int)(eased * ov->ws_slide_width);
	int old_x = ov->content_x - ov->ws_slide_direction * slide;
	int new_x = ov->content_x
		+ ov->ws_slide_direction * (ov->ws_slide_width - slide);

	wlr_scene_node_set_position(&ov->ws_slide_old_content->node,
		old_x, ov->content_y);
	wlr_scene_node_set_position(&ov->content_tree->node,
		new_x, ov->content_y);
	wlr_output_schedule_frame(output->wlr_output);

	if (raw < 1.0) {
		wl_event_source_timer_update(ov->ws_slide_timer, 8);
		return 0;
	}

	/* Slide complete: snap new content to final position and clean up */
	wlr_scene_node_set_position(&ov->content_tree->node,
		ov->content_x, ov->content_y);
	wlr_scene_node_destroy(&ov->ws_slide_old_content->node);
	ov->ws_slide_old_content = NULL;
	struct overview_item *item, *tmp;
	wl_list_for_each_safe(item, tmp, &ov->ws_slide_old_items, link) {
		wl_list_remove(&item->link);
		free(item);
	}
	wl_event_source_remove(ov->ws_slide_timer);
	ov->ws_slide_timer = NULL;
	ov->ws_sliding = false;

	if (!wl_list_empty(&ov->items)) {
		output_overview_show_labels(output);
	}
	return 0;
}

void
overview_goto_workspace(struct workspace *target, int direction)
{
	if (!overview_is_active()) {
		return;
	}

	/*
	 * Phase 1: for each active output, snap animation, hide labels,
	 * move items to ws_slide_old_items, save old content_tree.
	 */
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		struct overview_state *ov = &output->overview;
		if (!ov->active || ov->ws_sliding) {
			continue;
		}

		struct wlr_box box;
		wlr_output_layout_get_box(server.output_layout,
			output->wlr_output, &box);
		if (box.width <= 0) {
			continue;
		}

		/* Ensure items are at t=1 (overview positions) before transitioning */
		if (ov->animating) {
			wl_event_source_remove(ov->anim_timer);
			ov->anim_timer = NULL;
			ov->animating = false;
			ov->current_t = 1.0;
			apply_visual_t(ov, 1.0);
		}

		/* Hide hover/label UI from outgoing items */
		struct overview_item *item;
		wl_list_for_each(item, &ov->items, link) {
			if (item->hover_border) {
				wlr_scene_node_set_enabled(
					&item->hover_border->tree->node, false);
			}
			if (item->title_label) {
				wlr_scene_node_set_enabled(
					&item->title_label->scene_buffer->node, false);
			}
		}
		ov->hovered = NULL;

		/*
		 * Move current items to the old-items list so they can be freed
		 * when the slide completes. ov->items is cleared for new items.
		 */
		wl_list_init(&ov->ws_slide_old_items);
		if (!wl_list_empty(&ov->items)) {
			struct wl_list *h = &ov->ws_slide_old_items;
			h->next = ov->items.next;
			h->prev = ov->items.prev;
			h->next->prev = h;
			h->prev->next = h;
		}
		wl_list_init(&ov->items);

		/* Save outgoing content_tree reference */
		ov->ws_slide_old_content = ov->content_tree;
		ov->content_tree = NULL;
	}

	/* Phase transition: switch workspace */
	seat_focus_override_end(&server.seat, /*restore_focus*/ false);
	workspaces_switch_to(target, /*update_focus*/ true);
	seat_focus_override_begin(&server.seat,
		LAB_INPUT_STATE_OVERVIEW, LAB_CURSOR_DEFAULT);
	cursor_update_focus();

	/*
	 * Phase 2: for each active output that has a saved old_content,
	 * create new content_tree at offset position, populate items,
	 * and start the slide timer.
	 */
	wl_list_for_each(output, &server.outputs, link) {
		struct overview_state *ov = &output->overview;
		if (!ov->active || !ov->ws_slide_old_content) {
			continue;
		}

		struct wlr_box box;
		wlr_output_layout_get_box(server.output_layout,
			output->wlr_output, &box);

		/*
		 * Create a new content_tree for the new workspace, initially
		 * placed off-screen in the direction of travel within ov->tree.
		 * ov->tree (with wallpaper and background) remains stationary.
		 */
		ov->content_tree = lab_wlr_scene_tree_create(ov->tree);
		wlr_scene_node_set_position(&ov->content_tree->node,
			ov->content_x + direction * box.width, ov->content_y);

		overview_create_items(output, &box);
		apply_visual_t(ov, 1.0);

		ov->ws_slide_direction = direction;
		ov->ws_slide_width = box.width;
		ov->ws_sliding = true;
		clock_gettime(CLOCK_MONOTONIC, &ov->ws_slide_start);
		ov->ws_slide_timer = wl_event_loop_add_timer(
			server.wl_event_loop, ws_slide_tick, output);
		wl_event_source_timer_update(ov->ws_slide_timer, 1);
	}
}

void
overview_on_view_destroy(struct view *view)
{
	if (pending_selected_view == view) {
		pending_selected_view = NULL;
	}

	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		struct overview_state *ov = &output->overview;
		if (!ov->active) {
			continue;
		}

		if (ov->selected_view == view) {
			ov->selected_view = NULL;
		}
		if (ov->hovered && ov->hovered->view == view) {
			ov->hovered = NULL;
		}

		struct overview_item *item, *tmp;
		wl_list_for_each_safe(item, tmp, &ov->items, link) {
			if (item->view == view) {
				wlr_scene_node_destroy(&item->tree->node);
				wl_list_remove(&item->link);
				free(item);
				goto next_output;
			}
		}
		wl_list_for_each_safe(item, tmp, &ov->ws_slide_old_items, link) {
			if (item->view == view) {
				wlr_scene_node_destroy(&item->tree->node);
				wl_list_remove(&item->link);
				free(item);
				goto next_output;
			}
		}
next_output:
		;
	}
}

void
overview_on_output_destroy(struct output *output)
{
	if (!output->overview.active) {
		return;
	}
	output_overview_finish_immediate(output);
}

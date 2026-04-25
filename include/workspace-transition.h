/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_WORKSPACE_TRANSITION_H
#define LABWC_WORKSPACE_TRANSITION_H
#include <stdbool.h>

struct workspace;

/*
 * Animate a workspace switch with a sliding transition.
 * direction: +1 = slide right (going to next), -1 = slide left (going to prev)
 */
void workspace_transition_begin(struct workspace *target,
	int direction, bool update_focus);

bool workspace_transition_is_active(void);

#endif /* LABWC_WORKSPACE_TRANSITION_H */

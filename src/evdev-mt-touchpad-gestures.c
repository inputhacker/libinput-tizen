/*
 * Copyright © 2015 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>

#include "evdev-mt-touchpad.h"

#define DEFAULT_GESTURE_SWITCH_TIMEOUT 100 /* ms */

static void
tp_get_average_touches_delta(struct tp_dispatch *tp, double *dx, double *dy)
{
	struct tp_touch *t;
	int nchanged = 0;
	double tmpx, tmpy;

	*dx = 0.0;
	*dy = 0.0;

	tp_for_each_touch(tp, t) {
		if (tp_touch_active(tp, t) && t->dirty) {
			nchanged++;
			tp_get_delta(t, &tmpx, &tmpy);

			*dx += tmpx;
			*dy += tmpy;
		}
	}

	if (nchanged == 0)
		return;

	*dx /= nchanged;
	*dy /= nchanged;
}

static void
tp_get_combined_touches_delta(struct tp_dispatch *tp, double *dx, double *dy)
{
	struct tp_touch *t;
	double tdx, tdy;
	unsigned int i;

	for (i = 0; i < tp->real_touches; i++) {
		t = &tp->touches[i];

		if (!tp_touch_active(tp, t) || !t->dirty)
			continue;

		tp_get_delta(t, &tdx, &tdy);
		*dx += tdx;
		*dy += tdy;
	}
}

static void
tp_gesture_start(struct tp_dispatch *tp, uint64_t time)
{
	if (tp->gesture.started)
		return;

	switch (tp->gesture.finger_count) {
	case 2:
		/* NOP */
		break;
	}
	tp->gesture.started = true;
}

static void
tp_gesture_post_pointer_motion(struct tp_dispatch *tp, uint64_t time)
{
	double dx = 0.0, dy = 0.0;
	double dx_unaccel, dy_unaccel;

	/* When a clickpad is clicked, combine motion of all active touches */
	if (tp->buttons.is_clickpad && tp->buttons.state)
		tp_get_combined_touches_delta(tp, &dx, &dy);
	else
		tp_get_average_touches_delta(tp, &dx, &dy);

	tp_filter_motion(tp, &dx, &dy, &dx_unaccel, &dy_unaccel, time);

	if (dx != 0.0 || dy != 0.0 || dx_unaccel != 0.0 || dy_unaccel != 0.0) {
		pointer_notify_motion(&tp->device->base, time,
				      dx, dy, dx_unaccel, dy_unaccel);
	}
}

static void
tp_gesture_post_twofinger_scroll(struct tp_dispatch *tp, uint64_t time)
{
	double dx = 0, dy =0;

	tp_get_average_touches_delta(tp, &dx, &dy);
	tp_filter_motion(tp, &dx, &dy, NULL, NULL, time);

	if (dx == 0.0 && dy == 0.0)
		return;

	tp_gesture_start(tp, time);
	evdev_post_scroll(tp->device,
			  time,
			  LIBINPUT_POINTER_AXIS_SOURCE_FINGER,
			  dx, dy);
}

void
tp_gesture_post_events(struct tp_dispatch *tp, uint64_t time)
{
	if (tp->gesture.finger_count == 0)
		return;

	/* When tap-and-dragging, or a clickpad is clicked force 1fg mode */
	if (tp_tap_dragging(tp) || (tp->buttons.is_clickpad && tp->buttons.state)) {
		tp_gesture_stop(tp, time);
		tp->gesture.finger_count = 1;
		tp->gesture.finger_count_pending = 0;
	}

	/* Don't send events when we're unsure in which mode we are */
	if (tp->gesture.finger_count_pending)
		return;

	switch (tp->gesture.finger_count) {
	case 1:
		tp_gesture_post_pointer_motion(tp, time);
		break;
	case 2:
		tp_gesture_post_twofinger_scroll(tp, time);
		break;
	}
}

void
tp_gesture_stop_twofinger_scroll(struct tp_dispatch *tp, uint64_t time)
{
	evdev_stop_scroll(tp->device,
			  time,
			  LIBINPUT_POINTER_AXIS_SOURCE_FINGER);
}

void
tp_gesture_stop(struct tp_dispatch *tp, uint64_t time)
{
	if (!tp->gesture.started)
		return;

	switch (tp->gesture.finger_count) {
	case 2:
		tp_gesture_stop_twofinger_scroll(tp, time);
		break;
	}
	tp->gesture.started = false;
}

static void
tp_gesture_finger_count_switch_timeout(uint64_t now, void *data)
{
	struct tp_dispatch *tp = data;

	if (!tp->gesture.finger_count_pending)
		return;

	tp_gesture_stop(tp, now); /* End current gesture */
	tp->gesture.finger_count = tp->gesture.finger_count_pending;
	tp->gesture.finger_count_pending = 0;
}

void
tp_gesture_handle_state(struct tp_dispatch *tp, uint64_t time)
{
	unsigned int active_touches = 0;
	struct tp_touch *t;

	tp_for_each_touch(tp, t)
		if (tp_touch_active(tp, t))
			active_touches++;

	if (active_touches != tp->gesture.finger_count) {
		/* If all fingers are lifted immediately end the gesture */
		if (active_touches == 0) {
			tp_gesture_stop(tp, time);
			tp->gesture.finger_count = 0;
			tp->gesture.finger_count_pending = 0;
		/* Immediately switch to new mode to avoid initial latency */
		} else if (!tp->gesture.started) {
			tp->gesture.finger_count = active_touches;
			tp->gesture.finger_count_pending = 0;
		/* Else debounce finger changes */
		} else if (active_touches != tp->gesture.finger_count_pending) {
			tp->gesture.finger_count_pending = active_touches;
			libinput_timer_set(&tp->gesture.finger_count_switch_timer,
				time + DEFAULT_GESTURE_SWITCH_TIMEOUT);
		}
	} else {
		 tp->gesture.finger_count_pending = 0;
	}
}

int
tp_init_gesture(struct tp_dispatch *tp)
{
	libinput_timer_init(&tp->gesture.finger_count_switch_timer,
			    tp->device->base.seat->libinput,
			    tp_gesture_finger_count_switch_timeout, tp);
	return 0;
}

void
tp_remove_gesture(struct tp_dispatch *tp)
{
	libinput_timer_cancel(&tp->gesture.finger_count_switch_timer);
}
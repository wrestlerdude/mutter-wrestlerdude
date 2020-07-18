/*
 * Copyright (C) 2019 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "clutter-build-config.h"

#include "clutter/clutter-frame-clock.h"

#include "clutter/clutter-main.h"
#include "clutter/clutter-private.h"
#include "clutter/clutter-timeline-private.h"
#include "clutter/clutter-clock-driver.h"
#include "cogl/cogl-trace.h"

enum
{
  DESTROY,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Wait 2ms after vblank before starting to draw next frame */
#define SYNC_DELAY_US ms2us (2)

typedef struct _ClutterFrameListener
{
  const ClutterFrameListenerIface *iface;
  gpointer user_data;
} ClutterFrameListener;

typedef enum _ClutterFrameClockState
{
  CLUTTER_FRAME_CLOCK_STATE_IDLE,
  CLUTTER_FRAME_CLOCK_STATE_SCHEDULED,
  CLUTTER_FRAME_CLOCK_STATE_DISPATCHING,
  CLUTTER_FRAME_CLOCK_STATE_PENDING_PRESENTED,
} ClutterFrameClockState;

struct _ClutterFrameClock
{
  GObject parent;

  float refresh_rate;
  int64_t frame_interval_duration_us;

  ClutterFrameListener listener;

  ClutterClockDriver *clock_driver;

  GSource *source;

  int64_t frame_count;

  ClutterFrameClockState state;

  int64_t last_presentation_time_us;

  int64_t last_expected_presentation_time_us;

  gboolean pending_reschedule;
  gboolean pending_reschedule_now;

  int inhibit_count;

  GList *timelines;
};

G_DEFINE_TYPE (ClutterFrameClock, clutter_frame_clock,
               G_TYPE_OBJECT)

float
clutter_frame_clock_get_refresh_rate (ClutterFrameClock *frame_clock)
{
  return frame_clock->refresh_rate;
}

void
clutter_frame_clock_add_timeline (ClutterFrameClock *frame_clock,
                                  ClutterTimeline   *timeline)
{
  gboolean is_first;

  if (g_list_find (frame_clock->timelines, timeline))
    return;

  is_first = !frame_clock->timelines;

  frame_clock->timelines = g_list_prepend (frame_clock->timelines, timeline);

  if (is_first)
    clutter_frame_clock_schedule_update (frame_clock);
}

void
clutter_frame_clock_remove_timeline (ClutterFrameClock *frame_clock,
                                     ClutterTimeline   *timeline)
{
  frame_clock->timelines = g_list_remove (frame_clock->timelines, timeline);
}

static void
advance_timelines (ClutterFrameClock *frame_clock,
                   int64_t            time_us)
{
  GList *timelines;
  GList *l;

  /* we protect ourselves from timelines being removed during
   * the advancement by other timelines by copying the list of
   * timelines, taking a reference on them, iterating over the
   * copied list and then releasing the reference.
   *
   * we cannot simply take a reference on the timelines and still
   * use the list held by the master clock because the do_tick()
   * might result in the creation of a new timeline, which gets
   * added at the end of the list with no reference increase and
   * thus gets disposed at the end of the iteration.
   *
   * this implies that a newly added timeline will not be advanced
   * by this clock iteration, which is perfectly fine since we're
   * in its first cycle.
   *
   * we also cannot steal the frame clock timelines list because
   * a timeline might be removed as the direct result of do_tick()
   * and remove_timeline() would not find the timeline, failing
   * and leaving a dangling pointer behind.
   */

  timelines = g_list_copy (frame_clock->timelines);
  g_list_foreach (timelines, (GFunc) g_object_ref, NULL);

  for (l = timelines; l; l = l->next)
    {
      ClutterTimeline *timeline = l->data;

      _clutter_timeline_do_tick (timeline, time_us / 1000);
    }

  g_list_free_full (timelines, g_object_unref);
}

static void
maybe_reschedule_update (ClutterFrameClock *frame_clock)
{
  if (frame_clock->pending_reschedule ||
      frame_clock->timelines)
    {
      frame_clock->pending_reschedule = FALSE;

      if (frame_clock->pending_reschedule_now)
        {
          frame_clock->pending_reschedule_now = FALSE;
          clutter_frame_clock_schedule_update_now (frame_clock);
        }
      else
        {
          clutter_frame_clock_schedule_update (frame_clock);
        }
    }
}

void
clutter_frame_clock_notify_presented (ClutterFrameClock *frame_clock,
                                      ClutterFrameInfo  *frame_info)
{
  int64_t presentation_time_us = frame_info->presentation_time;

  if (presentation_time_us > frame_clock->last_presentation_time_us ||
      ((presentation_time_us - frame_clock->last_presentation_time_us) >
       INT64_MAX / 2))
    {
      frame_clock->last_presentation_time_us = presentation_time_us;
    }
  else
    {
      g_warning_once ("Bogus presentation time %" G_GINT64_FORMAT
                      " travelled back in time, using current time.",
                      presentation_time_us);
      frame_clock->last_presentation_time_us = g_get_monotonic_time ();
    }

  switch (frame_clock->state)
    {
    case CLUTTER_FRAME_CLOCK_STATE_IDLE:
    case CLUTTER_FRAME_CLOCK_STATE_SCHEDULED:
      g_warn_if_reached ();
      break;
    case CLUTTER_FRAME_CLOCK_STATE_DISPATCHING:
    case CLUTTER_FRAME_CLOCK_STATE_PENDING_PRESENTED:
      frame_clock->state = CLUTTER_FRAME_CLOCK_STATE_IDLE;
      maybe_reschedule_update (frame_clock);
      break;
    }
}

void
clutter_frame_clock_inhibit (ClutterFrameClock *frame_clock)
{
  frame_clock->inhibit_count++;

  if (frame_clock->inhibit_count == 1)
    {
      switch (frame_clock->state)
        {
        case CLUTTER_FRAME_CLOCK_STATE_IDLE:
          break;
        case CLUTTER_FRAME_CLOCK_STATE_SCHEDULED:
          frame_clock->pending_reschedule = TRUE;
          frame_clock->state = CLUTTER_FRAME_CLOCK_STATE_IDLE;
          break;
        case CLUTTER_FRAME_CLOCK_STATE_DISPATCHING:
        case CLUTTER_FRAME_CLOCK_STATE_PENDING_PRESENTED:
          break;
        }

      clutter_clock_driver_cancel_tick (frame_clock->clock_driver);
    }
}

void
clutter_frame_clock_uninhibit (ClutterFrameClock *frame_clock)
{
  g_return_if_fail (frame_clock->inhibit_count > 0);

  frame_clock->inhibit_count--;

  if (frame_clock->inhibit_count == 0)
    maybe_reschedule_update (frame_clock);
}

void
clutter_frame_clock_schedule_update_now (ClutterFrameClock *frame_clock)
{
  if (frame_clock->inhibit_count > 0)
    {
      frame_clock->pending_reschedule = TRUE;
      frame_clock->pending_reschedule_now = TRUE;
      return;
    }

  switch (frame_clock->state)
    {
    case CLUTTER_FRAME_CLOCK_STATE_IDLE:
      break;
    case CLUTTER_FRAME_CLOCK_STATE_SCHEDULED:
      return;
    case CLUTTER_FRAME_CLOCK_STATE_DISPATCHING:
    case CLUTTER_FRAME_CLOCK_STATE_PENDING_PRESENTED:
      frame_clock->pending_reschedule = TRUE;
      frame_clock->pending_reschedule_now = TRUE;
      return;
    }

  g_source_set_ready_time (frame_clock->source, g_get_monotonic_time ());
  frame_clock->last_expected_presentation_time_us = -1;
  frame_clock->state = CLUTTER_FRAME_CLOCK_STATE_SCHEDULED;
}

void
clutter_frame_clock_schedule_update (ClutterFrameClock *frame_clock)
{
  if (frame_clock->inhibit_count > 0)
    {
      frame_clock->pending_reschedule = TRUE;
      return;
    }

  switch (frame_clock->state)
    {
    case CLUTTER_FRAME_CLOCK_STATE_IDLE:
      break;
    case CLUTTER_FRAME_CLOCK_STATE_SCHEDULED:
      return;
    case CLUTTER_FRAME_CLOCK_STATE_DISPATCHING:
    case CLUTTER_FRAME_CLOCK_STATE_PENDING_PRESENTED:
      frame_clock->pending_reschedule = TRUE;
      return;
    }

  clutter_clock_driver_schedule_tick (frame_clock->clock_driver,
                                      frame_clock->last_presentation_time_us,
                                      frame_clock->last_expected_presentation_time_us,
                                      &frame_clock->last_expected_presentation_time_us);
  frame_clock->state = CLUTTER_FRAME_CLOCK_STATE_SCHEDULED;
}

static void
frame_clock_tick (ClutterClockDriver *clock_driver,
                  int64_t             time_us,
                  gpointer            user_data)
{
  ClutterFrameClock *frame_clock = CLUTTER_FRAME_CLOCK (user_data);
  int64_t frame_count;
  ClutterFrameResult result;

  COGL_TRACE_BEGIN_SCOPED (ClutterFrameCLockDispatch, "Frame Clock (dispatch)");

  frame_clock->state = CLUTTER_FRAME_CLOCK_STATE_DISPATCHING;

  frame_count = frame_clock->frame_count++;

  COGL_TRACE_BEGIN (ClutterFrameClockEvents, "Frame Clock (before frame)");
  if (frame_clock->listener.iface->before_frame)
    {
      frame_clock->listener.iface->before_frame (frame_clock,
                                                 frame_count,
                                                 frame_clock->listener.user_data);
    }
  COGL_TRACE_END (ClutterFrameClockEvents);

  COGL_TRACE_BEGIN (ClutterFrameClockTimelines, "Frame Clock (timelines)");
  advance_timelines (frame_clock, time_us);
  COGL_TRACE_END (ClutterFrameClockTimelines);

  COGL_TRACE_BEGIN (ClutterFrameClockFrame, "Frame Clock (frame)");
  result = frame_clock->listener.iface->frame (frame_clock,
                                               frame_count,
                                               time_us,
                                               frame_clock->listener.user_data);
  COGL_TRACE_END (ClutterFrameClockFrame);

  switch (frame_clock->state)
    {
    case CLUTTER_FRAME_CLOCK_STATE_PENDING_PRESENTED:
      g_warn_if_reached ();
      break;
    case CLUTTER_FRAME_CLOCK_STATE_IDLE:
    case CLUTTER_FRAME_CLOCK_STATE_SCHEDULED:
      break;
    case CLUTTER_FRAME_CLOCK_STATE_DISPATCHING:
      switch (result)
        {
        case CLUTTER_FRAME_RESULT_PENDING_PRESENTED:
          frame_clock->state = CLUTTER_FRAME_CLOCK_STATE_PENDING_PRESENTED;
          break;
        case CLUTTER_FRAME_RESULT_IDLE:
          frame_clock->state = CLUTTER_FRAME_CLOCK_STATE_IDLE;
          maybe_reschedule_update (frame_clock);
          break;
        }
      break;
    }
}

static void
init_frame_clock_source (ClutterFrameClock *frame_clock)
{
  GSource *source;

  source = clutter_clock_driver_create_source (frame_clock->clock_driver);

  g_source_set_priority (source, CLUTTER_PRIORITY_REDRAW);
  g_source_set_callback (source,
                         G_SOURCE_FUNC (frame_clock_tick),
                         frame_clock,
                         NULL);

  frame_clock->source = source;
  g_source_attach (source, NULL);
}

ClutterFrameClock *
clutter_frame_clock_new (float                            refresh_rate,
                         const ClutterFrameListenerIface *iface,
                         gpointer                         user_data)
{
  ClutterFrameClock *frame_clock;

  frame_clock = g_object_new (CLUTTER_TYPE_FRAME_CLOCK, NULL);

  frame_clock->listener.iface = iface;
  frame_clock->listener.user_data = user_data;

  frame_clock->frame_interval_duration_us =
    (int64_t) (0.5 + G_USEC_PER_SEC / refresh_rate);

  frame_clock->clock_driver =
    clutter_clock_driver_new (frame_clock->frame_interval_duration_us,
                              frame_clock->frame_interval_duration_us / 2,
                              frame_clock->frame_interval_duration_us - SYNC_DELAY_US);

  init_frame_clock_source (frame_clock);

  frame_clock->refresh_rate = refresh_rate;

  return frame_clock;
}

void
clutter_frame_clock_destroy (ClutterFrameClock *frame_clock)
{
  g_object_run_dispose (G_OBJECT (frame_clock));
  g_object_unref (frame_clock);
}

static void
clutter_frame_clock_dispose (GObject *object)
{
  ClutterFrameClock *frame_clock = CLUTTER_FRAME_CLOCK (object);

  if (frame_clock->source)
    {
      g_signal_emit (frame_clock, signals[DESTROY], 0);
      g_source_destroy (frame_clock->source);
      g_clear_pointer (&frame_clock->source, g_source_unref);
      g_clear_pointer (&frame_clock->clock_driver, g_object_unref);
    }

  G_OBJECT_CLASS (clutter_frame_clock_parent_class)->dispose (object);
}

static void
clutter_frame_clock_init (ClutterFrameClock *frame_clock)
{
  frame_clock->last_presentation_time_us = -1;
  frame_clock->last_expected_presentation_time_us = -1;
  frame_clock->state = CLUTTER_FRAME_CLOCK_STATE_IDLE;
}

static void
clutter_frame_clock_class_init (ClutterFrameClockClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = clutter_frame_clock_dispose;

  signals[DESTROY] =
    g_signal_new (I_("destroy"),
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  0);
}

/*
 * Copyright (C) 2020 Dor Askayo
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

#include "clutter/clutter-clock-driver.h"

#include "clutter/clutter-main.h"
#include "clutter/clutter-private.h"

typedef void (* ClutterClockDriverSourceFunc) (ClutterClockDriver *clock_driver,
                                               int64_t             tick_time_us,
                                               gpointer            user_data);

typedef struct _ClutterClockDriverSource
{
  GSource source;

  ClutterClockDriver *clock_driver;
} ClutterClockDriverSource;

struct _ClutterClockDriver
{
  GObject parent;

  int64_t interval_duration_us;
  int64_t minimum_tick_duration_us;
  int64_t maximum_tick_duration_us;

  GSource *source;
};

G_DEFINE_TYPE (ClutterClockDriver, clutter_clock_driver,
               G_TYPE_OBJECT)

static void
calculate_tick_start_time_us (ClutterClockDriver *clock_driver,
                              int64_t             last_interval_time_us,
                              int64_t             last_target_interval_time_us,
                              int64_t            *out_tick_start_time_us,
                              int64_t            *out_target_interval_time_us)
{
  int64_t now_us;
  int64_t interval_duration_us;
  int64_t minimum_tick_duration_us;
  int64_t maximum_tick_duration_us;
  int64_t target_interval_time_us;
  int64_t tick_start_time_us;

  now_us = g_get_monotonic_time ();

  interval_duration_us = clock_driver->interval_duration_us;
  minimum_tick_duration_us = clock_driver->minimum_tick_duration_us;
  maximum_tick_duration_us = clock_driver->maximum_tick_duration_us;

  target_interval_time_us = last_interval_time_us + interval_duration_us;

  g_debug ("calculate_tick_start_time_us: %p, step 1, target_interval_time_us: %" G_GINT64_FORMAT, clock_driver, target_interval_time_us);

  /* Skip ahead to get close to the actual target interval time. */
  if (target_interval_time_us < now_us)
    {
      int64_t logical_clock_offset_us;
      int64_t logical_clock_phase_us;
      int64_t hw_clock_offset_us;

      logical_clock_offset_us = now_us % interval_duration_us;
      logical_clock_phase_us = now_us - logical_clock_offset_us;
      hw_clock_offset_us = last_interval_time_us % interval_duration_us;

      target_interval_time_us = logical_clock_phase_us + hw_clock_offset_us;

      g_debug ("calculate_tick_start_time_us: %p, step 2, target_interval_time_us: %" G_GINT64_FORMAT, clock_driver, target_interval_time_us);
    }

  if (last_target_interval_time_us != -1)
  {
    int64_t time_since_last_target_interval_time_us =
        target_interval_time_us - last_target_interval_time_us;

    /* Skip one interval in case the last interval time is unreliable. */
    if (time_since_last_target_interval_time_us < minimum_tick_duration_us)
      {
        target_interval_time_us =
          last_target_interval_time_us + interval_duration_us;

        g_debug ("calculate_tick_start_time_us: %p, step 3, target_interval_time_us: %" G_GINT64_FORMAT, clock_driver, target_interval_time_us);
      }
  }

  while (target_interval_time_us < now_us + minimum_tick_duration_us)
    target_interval_time_us += interval_duration_us;

  g_debug ("calculate_tick_start_time_us: %p, step 4, target_interval_time_us: %" G_GINT64_FORMAT, clock_driver, target_interval_time_us);

  tick_start_time_us = target_interval_time_us - maximum_tick_duration_us;

  *out_tick_start_time_us = tick_start_time_us;
  *out_target_interval_time_us = target_interval_time_us;
}

void
clutter_clock_driver_schedule_tick (ClutterClockDriver *clock_driver,
                                    int64_t             last_interval_time_us,
                                    int64_t             last_target_interval_time_us,
                                    int64_t            *out_target_interval_time_us)
{
  int64_t tick_start_time_us = -1;

  if (last_interval_time_us >= 0)
    {
      calculate_tick_start_time_us (clock_driver,
                                    last_interval_time_us,
                                    last_target_interval_time_us,
                                    &tick_start_time_us,
                                    out_target_interval_time_us);
    }
  else
    {
      tick_start_time_us = g_get_monotonic_time ();
      *out_target_interval_time_us = -1;
    }

  g_warn_if_fail (tick_start_time_us != -1);

  g_debug ("clutter_clock_driver_schedule_tick: %p: dispatching in: %" G_GINT64_FORMAT, clock_driver, tick_start_time_us);

  g_source_set_ready_time (clock_driver->source, tick_start_time_us);
}

void
clutter_clock_driver_cancel_tick (ClutterClockDriver *clock_driver)
{
  g_debug ("clutter_clock_driver_cancel_tick: %p", clock_driver);

  g_source_set_ready_time (clock_driver->source, -1);
}

static gboolean
clock_driver_source_dispatch (GSource     *source,
                              GSourceFunc  callback,
                              gpointer     user_data)
{
  ClutterClockDriverSource *driver_source = (ClutterClockDriverSource *) source;
  ClutterClockDriverSourceFunc driver_source_callback =
    (ClutterClockDriverSourceFunc) callback;

  g_source_set_ready_time (source, -1);

  if (driver_source_callback)
    {
      ClutterClockDriver *clock_driver = driver_source->clock_driver;
      int64_t tick_time_us;

      tick_time_us = g_source_get_time (source);
      driver_source_callback (clock_driver,
                              tick_time_us,
                              user_data);
    }

  return G_SOURCE_CONTINUE;
}

static void
clock_driver_source_finalize (GSource *source)
{
  ClutterClockDriverSource *driver_source = (ClutterClockDriverSource *) source;

  g_clear_pointer (&driver_source->clock_driver, g_object_unref);
}

static GSourceFuncs clock_driver_source_funcs = {
  NULL,
  NULL,
  clock_driver_source_dispatch,
  clock_driver_source_finalize
};

GSource *
clutter_clock_driver_create_source (ClutterClockDriver *clock_driver)
{
  GSource *source;
  ClutterClockDriverSource *clock_source;
  g_autofree char *name = NULL;

  source = g_source_new (&clock_driver_source_funcs,
                         sizeof (ClutterClockDriverSource));
  clock_source = (ClutterClockDriverSource *) source;

  name = g_strdup_printf ("Clutter clock driver (%p)", clock_driver);
  g_source_set_name (source, name);
  g_source_set_can_recurse (source, FALSE);

  g_object_ref (clock_driver);
  clock_source->clock_driver = clock_driver;

  g_source_ref (source);
  clock_driver->source = source;

  return source;
}

ClutterClockDriver *
clutter_clock_driver_new (int64_t interval_duration_us,
                          int64_t minimum_tick_duration_us,
                          int64_t maximum_tick_duration_us)
{
  ClutterClockDriver *clock_driver;

  g_assert_cmpint (interval_duration_us, >, 0);

  clock_driver = g_object_new (CLUTTER_TYPE_CLOCK_DRIVER, NULL);

  clock_driver->interval_duration_us = interval_duration_us;

  if (minimum_tick_duration_us > maximum_tick_duration_us)
    minimum_tick_duration_us = maximum_tick_duration_us;

  clock_driver->minimum_tick_duration_us = minimum_tick_duration_us;
  clock_driver->maximum_tick_duration_us = maximum_tick_duration_us;

  return clock_driver;
}

static void
clutter_clock_driver_dispose (GObject *object)
{
  ClutterClockDriver *clock_driver = CLUTTER_CLOCK_DRIVER (object);

  if (clock_driver->source)
    {
      g_clear_pointer (&clock_driver->source, g_source_unref);
    }

  G_OBJECT_CLASS (clutter_clock_driver_parent_class)->dispose (object);
}

static void
clutter_clock_driver_init (ClutterClockDriver *clock_driver)
{
}

static void
clutter_clock_driver_class_init (ClutterClockDriverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = clutter_clock_driver_dispose;
}

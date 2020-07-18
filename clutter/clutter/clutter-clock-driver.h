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

#ifndef CLUTTER_CLOCK_DRIVER_H
#define CLUTTER_CLOCK_DRIVER_H

#if !defined(__CLUTTER_H_INSIDE__) && !defined(CLUTTER_COMPILATION)
#error "Only <clutter/clutter.h> can be included directly."
#endif

#include <glib.h>
#include <glib-object.h>
#include <stdint.h>

#include "clutter/clutter-types.h"

#define CLUTTER_TYPE_CLOCK_DRIVER (clutter_clock_driver_get_type ())
CLUTTER_EXPORT
G_DECLARE_FINAL_TYPE (ClutterClockDriver, clutter_clock_driver,
                      CLUTTER, CLOCK_DRIVER,
                      GObject)

CLUTTER_EXPORT
GSource *clutter_clock_driver_create_source (ClutterClockDriver *clock_driver);

CLUTTER_EXPORT
ClutterClockDriver *clutter_clock_driver_new (int64_t interval_period_us,
                                              int64_t minimum_tick_period_us,
                                              int64_t maximum_tick_period_us);

CLUTTER_EXPORT
void clutter_clock_driver_schedule_tick (ClutterClockDriver *clock_driver,
                                         int64_t             last_interval_time_us,
                                         int64_t             last_target_interval_time_us,
                                         int64_t            *out_target_interval_time_us);

CLUTTER_EXPORT
void clutter_clock_driver_cancel_tick (ClutterClockDriver *clock_driver);

#endif /* CLUTTER_CLOCK_DRIVER_H */

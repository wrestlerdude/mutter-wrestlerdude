/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2020 Dor Askayo
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Dor Askayo <dor.askayo@gmail.com>
 */

#include "backends/native/meta-renderer-view-native.h"

#include "clutter/clutter.h"
#include "backends/meta-output.h"
#include "backends/native/meta-onscreen-native.h"
#include "backends/native/meta-renderer-native.h"

struct _MetaRendererViewNative
{
  MetaRendererView parent;

  ClutterActor *frame_sync_actor;
  gulong frame_sync_actor_frozen_id;
  gulong frame_sync_actor_destroy_id;
};

G_DEFINE_TYPE (MetaRendererViewNative, meta_renderer_view_native,
               META_TYPE_RENDERER_VIEW);

static void
meta_renderer_view_native_update_vrr_mode (MetaRendererViewNative *view_native,
                                           gboolean                vrr_requested)
{
  MetaRendererView *view = META_RENDERER_VIEW (view_native);
  MetaOutput *output;

  output = meta_renderer_view_get_output (view);

  if (vrr_requested != meta_output_is_vrr_requested (output))
    {
      ClutterStageView *stage_view = CLUTTER_STAGE_VIEW (view);

      meta_output_set_vrr_requested (output, vrr_requested);

      if (meta_output_is_vrr_enabled (output))
        {
          CoglFramebuffer *framebuffer =
            clutter_stage_view_get_onscreen (stage_view);
          CoglOnscreen *onscreen = COGL_ONSCREEN (framebuffer);

          meta_onscreen_native_queue_modeset (onscreen);
        }
    }
}

static void
meta_renderer_view_native_update_sync_mode (MetaRendererViewNative *view_native)
{
  ClutterFrameClock *frame_clock =
    clutter_stage_view_get_frame_clock (CLUTTER_STAGE_VIEW (view_native));
  gboolean sync_requested;
  ClutterFrameClockMode clock_mode;

  sync_requested = view_native->frame_sync_actor != NULL;

  if (sync_requested)
    clock_mode = CLUTTER_FRAME_CLOCK_MODE_VARIABLE;
  else
    clock_mode = CLUTTER_FRAME_CLOCK_MODE_FIXED;

  clutter_frame_clock_set_mode (frame_clock, clock_mode);

  meta_renderer_view_native_update_vrr_mode (view_native,
                                             sync_requested);
}

static void
on_frame_sync_actor_frozen (ClutterActor           *actor,
                            MetaRendererViewNative *view_native)
{
  meta_renderer_view_native_set_frame_sync_actor (view_native, NULL);
}

static void
on_frame_sync_actor_destroyed (ClutterActor           *actor,
                               MetaRendererViewNative *view_native)
{
  meta_renderer_view_native_set_frame_sync_actor (view_native, NULL);
}

static void
meta_renderer_view_native_schedule_actor_update (ClutterStageView *stage_view,
                                                 ClutterActor     *actor)
{
  MetaRendererViewNative *view_native = META_RENDERER_VIEW_NATIVE (stage_view);
  ClutterFrameClock *frame_clock;

  g_return_if_fail (actor != NULL);

  frame_clock = clutter_stage_view_get_frame_clock (stage_view);

  if (actor == view_native->frame_sync_actor)
    clutter_frame_clock_schedule_update_now (frame_clock);
  else
    clutter_frame_clock_schedule_update (frame_clock);
}

void
meta_renderer_view_native_set_frame_sync_actor (MetaRendererViewNative *view_native,
                                                ClutterActor           *actor)
{
  if (G_LIKELY (actor == view_native->frame_sync_actor))
    return;

  if (view_native->frame_sync_actor)
    {
      g_clear_signal_handler (&view_native->frame_sync_actor_frozen_id,
                              view_native->frame_sync_actor);
      g_clear_signal_handler (&view_native->frame_sync_actor_destroy_id,
                              view_native->frame_sync_actor);
    }

  if (actor)
    {
        view_native->frame_sync_actor_frozen_id =
        g_signal_connect (actor, "frozen",
                          G_CALLBACK (on_frame_sync_actor_frozen),
                          view_native);
        view_native->frame_sync_actor_destroy_id =
        g_signal_connect (actor, "destroy",
                          G_CALLBACK (on_frame_sync_actor_destroyed),
                          view_native);
    }

  view_native->frame_sync_actor = actor;

  meta_renderer_view_native_update_sync_mode (view_native);
}

static void
meta_renderer_view_native_dispose (GObject *object)
{
  MetaRendererViewNative *view_native = META_RENDERER_VIEW_NATIVE (object);

  if (view_native->frame_sync_actor)
    {
      g_clear_signal_handler (&view_native->frame_sync_actor_destroy_id,
                              view_native->frame_sync_actor);
      g_clear_signal_handler (&view_native->frame_sync_actor_frozen_id,
                              view_native->frame_sync_actor);
    }

  G_OBJECT_CLASS (meta_renderer_view_native_parent_class)->dispose (object);
}

static void
meta_renderer_view_native_init (MetaRendererViewNative *view_native)
{
  view_native->frame_sync_actor = NULL;
  view_native->frame_sync_actor_frozen_id = 0;
  view_native->frame_sync_actor_destroy_id = 0;
}

static void
meta_renderer_view_native_class_init (MetaRendererViewNativeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterStageViewClass *clutter_stage_view_class = CLUTTER_STAGE_VIEW_CLASS (klass);

  object_class->dispose = meta_renderer_view_native_dispose;

  clutter_stage_view_class->schedule_actor_update = meta_renderer_view_native_schedule_actor_update;
}

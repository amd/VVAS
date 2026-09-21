/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/vvas/gstvvastilecompositioncoordinator.h>
#include <gst/vvas/gstvvastilecompositionleasemeta.h>

#define TILE_COUNT 4
#define EPOCH_COUNT 4

typedef struct
{
  GMutex lock;
  GCond condition;
  gboolean received;
  guint handoff_count;
  GstVvasTileCompositionIdentity expected;
  GstVvasTileCompositionIdentity actual;
  GstVvasTileCompositionResult complete_result;
} HandoffContext;

static GstFlowReturn
push_buffer (GstElement * appsrc, GstBuffer * buffer)
{
  GstFlowReturn flow = GST_FLOW_ERROR;

  g_signal_emit_by_name (appsrc, "push-buffer", buffer, &flow);
  return flow;
}

static GstFlowReturn
end_stream (GstElement * appsrc)
{
  GstFlowReturn flow = GST_FLOW_ERROR;

  g_signal_emit_by_name (appsrc, "end-of-stream", &flow);
  return flow;
}

static void
handoff_cb (GstElement * sink, GstBuffer * buffer, GstPad * pad,
    gpointer user_data)
{
  HandoffContext *context = user_data;
  GstVvasTileCompositionLeaseMeta *meta;

  (void) sink;
  (void) pad;
  meta = gst_buffer_get_vvas_tile_composition_lease_meta (buffer);

  g_mutex_lock (&context->lock);
  context->handoff_count++;
  if (meta &&
      gst_vvas_tile_composition_lease_get_role (meta->lease) ==
      GST_VVAS_TILE_COMPOSITION_LEASE_OUTPUT) {
    gst_vvas_tile_composition_lease_get_identity (meta->lease,
        &context->actual);
    context->complete_result =
        gst_vvas_tile_composition_coordinator_complete_processing (meta->lease);
    context->received = TRUE;
  }
  g_cond_broadcast (&context->condition);
  g_mutex_unlock (&context->lock);
}

static gboolean
wait_for_handoff (HandoffContext * context, GstClockTime timeout)
{
  gint64 deadline = g_get_monotonic_time () + timeout / GST_USECOND;
  gboolean received;

  g_mutex_lock (&context->lock);
  while (!context->received) {
    if (!g_cond_wait_until (&context->condition, &context->lock, deadline))
      break;
  }
  received = context->received;
  g_mutex_unlock (&context->lock);
  return received;
}

static GstBufferPool *
query_tile_pool (GstElement * appsrc, GstCaps * caps)
{
  GstPad *srcpad;
  GstQuery *query;
  GstBufferPool *pool = NULL;

  srcpad = gst_element_get_static_pad (appsrc, "src");
  g_assert_nonnull (srcpad);
  query = gst_query_new_allocation (caps, TRUE);
  g_assert_true (gst_pad_peer_query (srcpad, query));
  g_assert_cmpuint (gst_query_get_n_allocation_pools (query), >, 0);
  gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
  g_assert_nonnull (pool);
  gst_query_unref (query);
  gst_object_unref (srcpad);
  return pool;
}

static GstBuffer *
acquire_epoch_buffer (GstBufferPool * pool, guint epoch_index,
    GstVvasTileCompositionIdentity * identity)
{
  GstBuffer *buffer = NULL;
  GstVvasTileCompositionLeaseMeta *meta;

  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &buffer, NULL), ==,
      GST_FLOW_OK);
  g_assert_nonnull (buffer);
  meta = gst_buffer_get_vvas_tile_composition_lease_meta (buffer);
  g_assert_nonnull (meta);
  gst_vvas_tile_composition_lease_get_identity (meta->lease, identity);
  GST_BUFFER_PTS (buffer) = epoch_index * GST_SECOND / 30;
  GST_BUFFER_DURATION (buffer) = GST_SECOND / 30;
  return buffer;
}

static void
run_mixed_front_test (gboolean delayed_pad)
{
  const gchar *xclbin = g_getenv ("VVAS_TEST_XCLBIN");
  GstElement *pipeline;
  GstElement *compositor;
  GstElement *capsfilter;
  GstElement *sink;
  GstElement *sources[TILE_COUNT] = { NULL };
  GstPad *sinkpads[TILE_COUNT] = { NULL };
  GstBufferPool *pools[TILE_COUNT] = { NULL };
  GstBuffer *buffers[TILE_COUNT][EPOCH_COUNT] = { {NULL} };
  GstBuffer *eos_queued[TILE_COUNT] = { NULL };
  GstVvasTileCompositionIdentity identities[TILE_COUNT][EPOCH_COUNT];
  GstBuffer *delayed = NULL;
  GstCaps *tile_caps;
  GstCaps *master_caps;
  GstBus *bus;
  GstMessage *message;
  HandoffContext context = { 0 };
  gboolean failed = FALSE;
  guint epoch_count = delayed_pad ? 2 : EPOCH_COUNT;
  guint tile;
  guint epoch;

  if (!xclbin || !*xclbin) {
    g_test_skip ("VVAS_TEST_XCLBIN is not set");
    return;
  }
  pipeline = gst_pipeline_new ("tile-compositor-liveness");
  compositor = gst_element_factory_make ("vvas_xtilecompositor", "compositor");
  capsfilter = gst_element_factory_make ("capsfilter", "master-caps");
  sink = gst_element_factory_make ("fakesink", "sink");
  g_assert_nonnull (pipeline);
  g_assert_nonnull (compositor);
  g_assert_nonnull (capsfilter);
  g_assert_nonnull (sink);

  tile_caps = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, "BGR",
      "width", G_TYPE_INT, 960,
      "height", G_TYPE_INT, 536, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
  master_caps = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, "BGR",
      "width", G_TYPE_INT, 1920,
      "height", G_TYPE_INT, 1080, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);

  g_object_set (compositor,
      "need-dma", TRUE,
      "max-pool-size", EPOCH_COUNT + 1,
      "probe-downstream-layout", FALSE, "xclbin-location", xclbin, NULL);
  g_object_set (capsfilter, "caps", master_caps, NULL);
  g_object_set (sink, "signal-handoffs", TRUE, "sync", FALSE, "async", FALSE,
      NULL);
  g_signal_connect (sink, "handoff", G_CALLBACK (handoff_cb), &context);
  g_mutex_init (&context.lock);
  g_cond_init (&context.condition);
  context.complete_result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  gst_bin_add_many (GST_BIN (pipeline), compositor, capsfilter, sink, NULL);
  g_assert_true (gst_element_link_many (compositor, capsfilter, sink, NULL));

  for (tile = 0; tile < TILE_COUNT; tile++) {
    gchar *name = g_strdup_printf ("source-%u", tile);
    gchar *pad_name = g_strdup_printf ("sink_%u", tile);
    GstPad *srcpad;

    sources[tile] = gst_element_factory_make ("appsrc", name);
    g_free (name);
    g_assert_nonnull (sources[tile]);
    g_object_set (sources[tile],
        "caps", tile_caps,
        "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE, NULL);
    gst_bin_add (GST_BIN (pipeline), sources[tile]);
    sinkpads[tile] = gst_element_request_pad_simple (compositor, pad_name);
    g_free (pad_name);
    g_assert_nonnull (sinkpads[tile]);
    srcpad = gst_element_get_static_pad (sources[tile], "src");
    g_assert_nonnull (srcpad);
    g_assert_cmpint (gst_pad_link (srcpad, sinkpads[tile]), ==,
        GST_PAD_LINK_OK);
    gst_object_unref (srcpad);
  }

  g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PAUSED), !=,
      GST_STATE_CHANGE_FAILURE);
  gst_element_get_state (pipeline, NULL, NULL, GST_SECOND);

  for (tile = 0; tile < TILE_COUNT; tile++) {
    pools[tile] = query_tile_pool (sources[tile], tile_caps);
    g_assert_true (gst_buffer_pool_set_active (pools[tile], TRUE));
  }

  for (epoch = 0; epoch < epoch_count; epoch++) {
    for (tile = 0; tile < TILE_COUNT; tile++) {
      buffers[tile][epoch] =
          acquire_epoch_buffer (pools[tile], epoch, &identities[tile][epoch]);
      if (tile)
        g_assert_cmpuint (identities[tile][epoch].epoch, ==,
            identities[0][epoch].epoch);
    }
  }
  context.expected = identities[0][epoch_count - 1];
  if (delayed_pad) {
    gst_clear_buffer (&buffers[0][0]);
    delayed = g_steal_pointer (&buffers[3][1]);
  } else {
    gst_clear_buffer (&buffers[1][0]);
    gst_clear_buffer (&buffers[2][0]);
    gst_clear_buffer (&buffers[2][1]);
    gst_clear_buffer (&buffers[3][0]);
    gst_clear_buffer (&buffers[3][1]);
    gst_clear_buffer (&buffers[3][2]);
  }

  for (tile = 0; tile < TILE_COUNT; tile++) {
    for (epoch = 0; epoch < epoch_count; epoch++) {
      GstBuffer *buffer = buffers[tile][epoch];

      if (!buffer)
        continue;
      buffers[tile][epoch] = NULL;
      g_assert_cmpint (push_buffer (sources[tile], buffer), ==, GST_FLOW_OK);
      gst_buffer_unref (buffer);
    }
  }

  g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=,
      GST_STATE_CHANGE_FAILURE);
  if (delayed_pad) {
    g_usleep (200 * G_TIME_SPAN_MILLISECOND);
    g_assert_false (context.received);
    g_assert_cmpint (push_buffer (sources[3], delayed), ==, GST_FLOW_OK);
    gst_clear_buffer (&delayed);
  }
  if (!wait_for_handoff (&context, 3 * GST_SECOND)) {
    g_test_message ("no common epoch was published after stale fronts drained");
    failed = TRUE;
    goto cleanup;
  }

  g_assert_cmpuint (context.handoff_count, ==, 1);
  g_assert_cmpuint (context.actual.slot_id, ==, context.expected.slot_id);
  g_assert_cmpuint (context.actual.epoch, ==, context.expected.epoch);
  g_assert_cmpuint (context.actual.layout_version, ==,
      context.expected.layout_version);
  g_assert_cmpint (context.complete_result, ==,
      GST_VVAS_TILE_COMPOSITION_RESULT_OK);
  bus = gst_element_get_bus (pipeline);
  for (tile = 0; tile < TILE_COUNT; tile++) {
    GstVvasTileCompositionIdentity queued_identity;

    eos_queued[tile] =
        acquire_epoch_buffer (pools[tile], epoch_count, &queued_identity);
    g_assert_cmpuint (queued_identity.tile_id, ==, tile);
  }
  g_assert_cmpint (end_stream (sources[0]), ==, GST_FLOW_OK);
  for (tile = 1; tile < TILE_COUNT; tile++) {
    g_assert_cmpint (push_buffer (sources[tile], eos_queued[tile]), ==,
        GST_FLOW_OK);
    gst_clear_buffer (&eos_queued[tile]);
  }
  message = gst_bus_timed_pop_filtered (bus, 200 * GST_MSECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_null (message);
  for (tile = 1; tile < TILE_COUNT; tile++)
    g_assert_cmpint (end_stream (sources[tile]), ==, GST_FLOW_OK);
  message = gst_bus_timed_pop_filtered (bus, 3 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (message);
  g_assert_cmpuint (GST_MESSAGE_TYPE (message), ==, GST_MESSAGE_EOS);
  gst_message_unref (message);
  gst_object_unref (bus);

cleanup:
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_element_get_state (pipeline, NULL, NULL, GST_SECOND);
  for (tile = 0; tile < TILE_COUNT; tile++) {
    for (epoch = 0; epoch < EPOCH_COUNT; epoch++)
      gst_clear_buffer (&buffers[tile][epoch]);
    gst_clear_buffer (&eos_queued[tile]);
    if (pools[tile]) {
      gst_buffer_pool_set_active (pools[tile], FALSE);
      gst_object_unref (pools[tile]);
    }
    if (sinkpads[tile]) {
      gst_element_release_request_pad (compositor, sinkpads[tile]);
      gst_object_unref (sinkpads[tile]);
    }
  }
  gst_clear_buffer (&delayed);
  gst_caps_unref (master_caps);
  gst_caps_unref (tile_caps);
  gst_object_unref (pipeline);
  g_cond_clear (&context.condition);
  g_mutex_clear (&context.lock);
  g_assert_false (failed);
}

static void
test_prequeued_staircase_drains (void)
{
  run_mixed_front_test (FALSE);
}

static void
test_delayed_pad_reschedules (void)
{
  run_mixed_front_test (TRUE);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vvas/xtilecompositor/prequeued-staircase",
      test_prequeued_staircase_drains);
  g_test_add_func ("/vvas/xtilecompositor/delayed-pad",
      test_delayed_pad_reschedules);
  return g_test_run ();
}

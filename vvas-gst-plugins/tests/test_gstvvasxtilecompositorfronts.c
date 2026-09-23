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
#include "gstvvas_xtilecompositorfronts.h"

#define TILE_COUNT GST_VVAS_TILE_COMPOSITION_TILE_COUNT
#define MAX_DEPTH 4

typedef struct
{
  GstVvasTileCompositionIdentity queues[TILE_COUNT][MAX_DEPTH];
  guint lengths[TILE_COUNT];
  guint positions[TILE_COUNT];
  guint pop_count;
  guint abort_count;
  GstVvasTileCompositionIdentity aborted[MAX_DEPTH];
  GstVvasTileCompositionResult abort_result;
  gboolean fail_pop;
  guint fail_pop_tile;
  guint mismatch_count;
} FakeFronts;

static GstVvasTileCompositionIdentity
make_identity (guint tile_id, guint64 epoch)
{
  GstVvasTileCompositionIdentity identity = { 0 };

  identity.slot_id = epoch;
  identity.epoch = epoch;
  identity.layout_version = 1;
  identity.role = GST_VVAS_TILE_COMPOSITION_LEASE_TILE;
  identity.tile_id = tile_id;
  return identity;
}

static gboolean
fake_peek (gpointer user_data, guint tile_id,
    GstVvasTileCompositionIdentity * identity)
{
  FakeFronts *fronts = user_data;

  if (fronts->positions[tile_id] >= fronts->lengths[tile_id])
    return FALSE;
  *identity = fronts->queues[tile_id][fronts->positions[tile_id]];
  return TRUE;
}

static gboolean
fake_pop (gpointer user_data, guint tile_id)
{
  FakeFronts *fronts = user_data;

  if (fronts->fail_pop && tile_id == fronts->fail_pop_tile)
    return FALSE;
  if (fronts->positions[tile_id] >= fronts->lengths[tile_id])
    return FALSE;
  fronts->positions[tile_id]++;
  fronts->pop_count++;
  return TRUE;
}

static GstVvasTileCompositionResult
fake_abort (gpointer user_data, const GstVvasTileCompositionIdentity * identity)
{
  FakeFronts *fronts = user_data;

  g_assert_cmpuint (fronts->abort_count, <, MAX_DEPTH);
  fronts->aborted[fronts->abort_count++] = *identity;
  return fronts->abort_result;
}

static void
fake_mismatch (gpointer user_data,
    const GstVvasTileCompositionIdentity identities[TILE_COUNT])
{
  FakeFronts *fronts = user_data;

  g_assert_nonnull (identities);
  fronts->mismatch_count++;
}

static void
append_epoch (FakeFronts * fronts, guint tile_id, guint64 epoch)
{
  guint position = fronts->lengths[tile_id]++;

  g_assert_cmpuint (position, <, MAX_DEPTH);
  fronts->queues[tile_id][position] = make_identity (tile_id, epoch);
}

static void
test_staircase_drains_to_common_epoch (void)
{
  FakeFronts fronts = { 0 };
  GstVvasTileCompositionIdentity aligned[TILE_COUNT];
  GstVvasXTileCompositorFrontResult result;
  guint tile;

  append_epoch (&fronts, 0, 1);
  append_epoch (&fronts, 0, 2);
  append_epoch (&fronts, 0, 3);
  append_epoch (&fronts, 0, 4);
  append_epoch (&fronts, 1, 2);
  append_epoch (&fronts, 1, 3);
  append_epoch (&fronts, 1, 4);
  append_epoch (&fronts, 2, 3);
  append_epoch (&fronts, 2, 4);
  append_epoch (&fronts, 3, 4);

  result = gst_vvas_xtilecompositor_align_fronts (fake_peek, fake_pop,
      fake_abort, fake_mismatch, &fronts, aligned);

  g_assert_cmpint (result, ==, GST_VVAS_XTILECOMPOSITOR_FRONTS_ALIGNED);
  for (tile = 0; tile < TILE_COUNT; tile++) {
    g_assert_cmpuint (aligned[tile].epoch, ==, 4);
    g_assert_cmpuint (aligned[tile].slot_id, ==, 4);
    g_assert_cmpuint (aligned[tile].layout_version, ==, 1);
  }
  g_assert_cmpuint (fronts.pop_count, ==, 6);
  g_assert_cmpuint (fronts.abort_count, ==, 3);
  g_assert_cmpuint (fronts.aborted[0].epoch, ==, 1);
  g_assert_cmpuint (fronts.aborted[1].epoch, ==, 2);
  g_assert_cmpuint (fronts.aborted[2].epoch, ==, 3);
  g_assert_cmpuint (fronts.mismatch_count, ==, 3);
}

static void
test_empty_pad_returns_need_data (void)
{
  FakeFronts fronts = { 0 };
  GstVvasTileCompositionIdentity aligned[TILE_COUNT];
  GstVvasXTileCompositorFrontResult result;

  append_epoch (&fronts, 0, 1);
  append_epoch (&fronts, 1, 1);
  append_epoch (&fronts, 2, 1);

  result = gst_vvas_xtilecompositor_align_fronts (fake_peek, fake_pop,
      fake_abort, NULL, &fronts, aligned);

  g_assert_cmpint (result, ==, GST_VVAS_XTILECOMPOSITOR_FRONTS_NEED_DATA);
  g_assert_cmpuint (fronts.pop_count, ==, 0);
  g_assert_cmpuint (fronts.abort_count, ==, 0);
}

static void
test_identity_fields_are_compared (void)
{
  FakeFronts fronts = { 0 };
  GstVvasTileCompositionIdentity aligned[TILE_COUNT];
  GstVvasXTileCompositorFrontResult result;
  guint tile;

  for (tile = 0; tile < TILE_COUNT; tile++)
    append_epoch (&fronts, tile, 4);
  fronts.queues[3][0].slot_id = 3;
  fronts.queues[3][0].layout_version = 2;

  result = gst_vvas_xtilecompositor_align_fronts (fake_peek, fake_pop,
      fake_abort, fake_mismatch, &fronts, aligned);

  g_assert_cmpint (result, ==, GST_VVAS_XTILECOMPOSITOR_FRONTS_NEED_DATA);
  g_assert_cmpuint (fronts.pop_count, ==, TILE_COUNT);
  g_assert_cmpuint (fronts.abort_count, ==, 2);
  g_assert_cmpuint (fronts.aborted[0].slot_id, ==, 4);
  g_assert_cmpuint (fronts.aborted[0].layout_version, ==, 1);
  g_assert_cmpuint (fronts.aborted[1].slot_id, ==, 3);
  g_assert_cmpuint (fronts.aborted[1].layout_version, ==, 2);
  g_assert_cmpuint (fronts.mismatch_count, ==, 1);
}

static void
test_abort_failure_stops_before_pop (void)
{
  FakeFronts fronts = { 0 };
  GstVvasTileCompositionIdentity aligned[TILE_COUNT];
  GstVvasXTileCompositorFrontResult result;
  guint tile;

  for (tile = 0; tile < TILE_COUNT; tile++)
    append_epoch (&fronts, tile, tile ? 2 : 1);
  fronts.abort_result = GST_VVAS_TILE_COMPOSITION_RESULT_INVALID;

  result = gst_vvas_xtilecompositor_align_fronts (fake_peek, fake_pop,
      fake_abort, fake_mismatch, &fronts, aligned);

  g_assert_cmpint (result, ==, GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR);
  g_assert_cmpuint (fronts.abort_count, ==, 1);
  g_assert_cmpuint (fronts.pop_count, ==, 0);
}

static void
test_pop_failure_returns_error (void)
{
  FakeFronts fronts = { 0 };
  GstVvasTileCompositionIdentity aligned[TILE_COUNT];
  GstVvasXTileCompositorFrontResult result;
  guint tile;

  for (tile = 0; tile < TILE_COUNT; tile++)
    append_epoch (&fronts, tile, tile ? 2 : 1);
  fronts.fail_pop = TRUE;
  fronts.fail_pop_tile = 0;

  result = gst_vvas_xtilecompositor_align_fronts (fake_peek, fake_pop,
      fake_abort, fake_mismatch, &fronts, aligned);

  g_assert_cmpint (result, ==, GST_VVAS_XTILECOMPOSITOR_FRONTS_ERROR);
  g_assert_cmpuint (fronts.abort_count, ==, 1);
  g_assert_cmpuint (fronts.pop_count, ==, 0);
}

static void
test_stale_abort_is_safe_to_pop (void)
{
  FakeFronts fronts = { 0 };
  GstVvasTileCompositionIdentity aligned[TILE_COUNT];
  GstVvasXTileCompositorFrontResult result;
  guint tile;

  for (tile = 0; tile < TILE_COUNT; tile++)
    append_epoch (&fronts, tile, tile ? 2 : 1);
  fronts.abort_result = GST_VVAS_TILE_COMPOSITION_RESULT_STALE;

  result = gst_vvas_xtilecompositor_align_fronts (fake_peek, fake_pop,
      fake_abort, fake_mismatch, &fronts, aligned);

  g_assert_cmpint (result, ==, GST_VVAS_XTILECOMPOSITOR_FRONTS_NEED_DATA);
  g_assert_cmpuint (fronts.abort_count, ==, 1);
  g_assert_cmpuint (fronts.pop_count, ==, 1);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vvas/xtilecompositor-fronts/staircase",
      test_staircase_drains_to_common_epoch);
  g_test_add_func ("/vvas/xtilecompositor-fronts/need-data",
      test_empty_pad_returns_need_data);
  g_test_add_func ("/vvas/xtilecompositor-fronts/identity-fields",
      test_identity_fields_are_compared);
  g_test_add_func ("/vvas/xtilecompositor-fronts/abort-error",
      test_abort_failure_stops_before_pop);
  g_test_add_func ("/vvas/xtilecompositor-fronts/pop-error",
      test_pop_failure_returns_error);
  g_test_add_func ("/vvas/xtilecompositor-fronts/stale-abort",
      test_stale_abort_is_safe_to_pop);
  return g_test_run ();
}

/* GStreamer
 * Copyright (C) 2026 Ludeo
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include "gstnvencobject.h"
#include <string>

enum class GstNvH264PtdErrorCode
{
  None,
  InvalidTemporalLayers,
  InvalidGopLength,
  InvalidPoc,
  InvalidReferencePattern,
  MissingDecision,
};

struct GstNvH264PtdConfig
{
  guint temporal_layers = 1;
  guint gop_length = 0;
  gboolean enable_temporal_svc = FALSE;
  gboolean repeat_sps_pps = FALSE;
  gboolean use_non_ref_p_type = FALSE;
  gboolean strict_validation = TRUE;
};

struct GstNvH264FrameInput
{
  guint system_frame_number = 0;
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  GstClockTime duration = GST_CLOCK_TIME_NONE;
  gboolean force_keyframe = FALSE;
};

struct GstNvH264LtrRequest
{
  gboolean mark_frame = FALSE;
  gboolean use_frames = FALSE;
  guint mark_frame_idx = 0;
  guint use_frame_bitmap = 0;
};

struct GstNvH264PtdCounters
{
  guint64 abs_frame_idx_before = 0;
  guint64 gop_frame_idx_before = 0;
  guint64 abs_frame_idx_after = 0;
  guint64 gop_frame_idx_after = 0;
};

struct GstNvH264PtdResult
{
  GstNvEncH264PtdDecision decision = {};
  GstNvH264PtdErrorCode error_code = GstNvH264PtdErrorCode::None;
  const gchar *error_message = "";
};

class GstNvH264PtdController final
{
public:
  explicit GstNvH264PtdController (const GstNvH264PtdConfig & config);

  void reset ();
  void updateConfig (const GstNvH264PtdConfig & config);
  GstNvH264PtdResult decide (const GstNvH264FrameInput & frame,
      const GstNvH264LtrRequest & ltr);
  GstNvH264PtdResult acceptUpstreamDecision (const GstNvH264FrameInput & frame,
      const GstNvEncH264PtdDecision & upstream_decision,
      const GstNvH264LtrRequest & ltr);
  GstNvH264PtdCounters counters () const;

private:
  GstNvH264PtdResult decidePipelineCalculated (
      const GstNvH264FrameInput & frame, const GstNvH264LtrRequest & ltr);
  GstNvH264PtdErrorCode validate (
      const GstNvEncH264PtdDecision & decision) const;
  void advance (GstNvEncH264PtdDecision * decision);
  void applyLtr (GstNvEncH264PtdDecision * decision,
      const GstNvH264LtrRequest & ltr) const;
  const gchar *errorMessage (GstNvH264PtdErrorCode error_code) const;

  GstNvH264PtdConfig config_;
  guint64 abs_frame_idx_ = 0;
  guint64 gop_frame_idx_ = 0;
};

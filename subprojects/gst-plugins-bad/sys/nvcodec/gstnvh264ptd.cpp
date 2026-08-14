/* GStreamer
 * Copyright (C) 2026 Ludeo
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstnvh264ptd.h"

namespace
{

struct TemporalPattern
{
  guint temporal_layer;
  guint ref_pic_flag;
  guint pattern_idx;
};

TemporalPattern
temporal_pattern_for_frame (guint temporal_layers, guint64 gop_frame_idx)
{
  static const guint l1t4_tid[] = { 0, 3, 2, 3, 1, 3, 2, 3 };
  static const guint l1t4_ref[] = { 1, 0, 1, 0, 1, 0, 1, 0 };
  static const guint l1t3_tid[] = { 0, 2, 1, 2 };
  static const guint l1t3_ref[] = { 1, 0, 1, 0 };
  static const guint l1t2_tid[] = { 0, 1 };
  static const guint l1t2_ref[] = { 1, 0 };

  if (temporal_layers >= 4) {
    guint idx = (guint) (gop_frame_idx % G_N_ELEMENTS (l1t4_tid));
    return { l1t4_tid[idx], l1t4_ref[idx], idx };
  }

  if (temporal_layers == 3) {
    guint idx = (guint) (gop_frame_idx % G_N_ELEMENTS (l1t3_tid));
    return { l1t3_tid[idx], l1t3_ref[idx], idx };
  }

  if (temporal_layers == 2) {
    guint idx = (guint) (gop_frame_idx % G_N_ELEMENTS (l1t2_tid));
    return { l1t2_tid[idx], l1t2_ref[idx], idx };
  }

  return { 0, 1, 0 };
}

bool
has_finite_gop (guint gop_length)
{
  return gop_length > 0 && gop_length != NVENC_INFINITE_GOPLENGTH;
}

}

GstNvH264PtdController::GstNvH264PtdController (
    const GstNvH264PtdConfig & config)
    : config_ (config)
{
}

void
GstNvH264PtdController::reset ()
{
  abs_frame_idx_ = 0;
  gop_frame_idx_ = 0;
  ltr_live_bitmap_ = 0;
}

void
GstNvH264PtdController::updateConfig (const GstNvH264PtdConfig & config)
{
  config_ = config;
}

GstNvH264PtdCounters
GstNvH264PtdController::counters () const
{
  return { abs_frame_idx_, gop_frame_idx_, abs_frame_idx_, gop_frame_idx_ };
}

GstNvH264PtdResult
GstNvH264PtdController::decide (const GstNvH264FrameInput & frame,
    const GstNvH264LtrRequest & ltr)
{
  return decidePipelineCalculated (frame, ltr);
}

GstNvH264PtdResult
GstNvH264PtdController::acceptUpstreamDecision (
    const GstNvH264FrameInput & frame,
    const GstNvEncH264PtdDecision & upstream_decision,
    const GstNvH264LtrRequest & ltr)
{
  GstNvH264PtdResult result;

  result.decision = upstream_decision;
  result.decision.valid = TRUE;
  result.decision.owner = GST_NV_H264_PTD_OWNER_UPSTREAM_PROVIDED;
  result.decision.is_idr =
      upstream_decision.picture_type == NV_ENC_PIC_TYPE_IDR;
  result.decision.gop_boundary = has_finite_gop (config_.gop_length) &&
      gop_frame_idx_ >= config_.gop_length;
  result.decision.abs_frame_idx_before = abs_frame_idx_;
  result.decision.gop_frame_idx_before = gop_frame_idx_;
  result.decision.temporal_layers = config_.temporal_layers;
  result.decision.temporal_svc_enabled = config_.enable_temporal_svc;
  result.decision.pattern_idx =
      temporal_pattern_for_frame (MAX (config_.temporal_layers, 1),
      gop_frame_idx_).pattern_idx;

  applyLtr (&result.decision, ltr);

  result.error_code = validate (result.decision);
  if (result.error_code != GstNvH264PtdErrorCode::None) {
    result.decision.valid = FALSE;
    result.error_message = errorMessage (result.error_code);
    return result;
  }

  commitLtrDecision (result.decision);
  advance (&result.decision);
  result.error_message = errorMessage (GstNvH264PtdErrorCode::None);

  GST_TRACE ("Accepted upstream H264 PTD frame=%u abs=%" G_GUINT64_FORMAT
      " gop=%" G_GUINT64_FORMAT " type=%d ref=%u poc=%u tl=%u",
      frame.system_frame_number, result.decision.abs_frame_idx_before,
      result.decision.gop_frame_idx_before,
      (gint) result.decision.picture_type, result.decision.ref_pic_flag,
      result.decision.display_poc_syntax, result.decision.temporal_layer);

  return result;
}

GstNvH264PtdResult
GstNvH264PtdController::decidePipelineCalculated (
    const GstNvH264FrameInput & frame, const GstNvH264LtrRequest & ltr)
{
  GstNvH264PtdResult result;
  GstNvEncH264PtdDecision *decision = &result.decision;
  TemporalPattern pattern = { 0, 1, 0 };
  gboolean gop_boundary = has_finite_gop (config_.gop_length) &&
      gop_frame_idx_ >= config_.gop_length;
  gboolean is_idr = frame.force_keyframe || abs_frame_idx_ == 0 ||
      gop_boundary;
  guint64 display_poc = 0;
  guint temporal_layers = MAX (config_.temporal_layers, 1);

  if (temporal_layers > 4) {
    result.error_code = GstNvH264PtdErrorCode::InvalidTemporalLayers;
    result.error_message = errorMessage (result.error_code);
    return result;
  }

  if (has_finite_gop (config_.gop_length) && config_.gop_length < 1) {
    result.error_code = GstNvH264PtdErrorCode::InvalidGopLength;
    result.error_message = errorMessage (result.error_code);
    return result;
  }

  if (!is_idr && config_.enable_temporal_svc)
    pattern = temporal_pattern_for_frame (temporal_layers, gop_frame_idx_);

  if (!is_idr) {
    display_poc = gop_frame_idx_ * 2;
    if (display_poc > G_MAXUINT32) {
      result.error_code = GstNvH264PtdErrorCode::InvalidPoc;
      result.error_message = errorMessage (result.error_code);
      return result;
    }
  }

  decision->valid = TRUE;
  decision->owner = GST_NV_H264_PTD_OWNER_PIPELINE_CALCULATED;
  decision->picture_type = is_idr ? NV_ENC_PIC_TYPE_IDR :
      (config_.use_non_ref_p_type && pattern.ref_pic_flag == 0 ?
      NV_ENC_PIC_TYPE_NONREF_P : NV_ENC_PIC_TYPE_P);
  decision->display_poc_syntax = (guint32) display_poc;
  decision->ref_pic_flag = is_idr ? 1 : pattern.ref_pic_flag;
  decision->temporal_layer = is_idr ? 0 : pattern.temporal_layer;
  decision->encode_pic_flags = 0;
  decision->is_idr = is_idr;
  decision->gop_boundary = gop_boundary;
  decision->abs_frame_idx_before = abs_frame_idx_;
  decision->gop_frame_idx_before = gop_frame_idx_;
  decision->pattern_idx = is_idr ? 0 : pattern.pattern_idx;
  decision->temporal_layers = temporal_layers;
  decision->temporal_svc_enabled = config_.enable_temporal_svc;

  if (is_idr) {
    decision->encode_pic_flags |= NV_ENC_PIC_FLAG_FORCEIDR;
    if (config_.repeat_sps_pps)
      decision->encode_pic_flags |= NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
  }

  applyLtr (decision, ltr);

  result.error_code = validate (*decision);
  if (result.error_code != GstNvH264PtdErrorCode::None) {
    decision->valid = FALSE;
    result.error_message = errorMessage (result.error_code);
    return result;
  }

  commitLtrDecision (*decision);
  advance (decision);
  result.error_message = errorMessage (GstNvH264PtdErrorCode::None);

  return result;
}

GstNvH264PtdErrorCode
GstNvH264PtdController::validate (
    const GstNvEncH264PtdDecision & decision) const
{
  if (!decision.valid)
    return GstNvH264PtdErrorCode::MissingDecision;

  if (config_.temporal_layers == 0 || config_.temporal_layers > 4)
    return GstNvH264PtdErrorCode::InvalidTemporalLayers;

  if (decision.is_idr && decision.display_poc_syntax != 0)
    return GstNvH264PtdErrorCode::InvalidPoc;

  if (!decision.is_idr && decision.owner ==
      GST_NV_H264_PTD_OWNER_PIPELINE_CALCULATED) {
    guint64 expected_poc = decision.gop_frame_idx_before * 2;
    if (expected_poc > G_MAXUINT32 ||
        decision.display_poc_syntax != (guint32) expected_poc)
      return GstNvH264PtdErrorCode::InvalidPoc;

    if (config_.enable_temporal_svc) {
      TemporalPattern expected = temporal_pattern_for_frame (
          MAX (config_.temporal_layers, 1), decision.gop_frame_idx_before);
      if (decision.temporal_layer != expected.temporal_layer ||
          decision.ref_pic_flag != expected.ref_pic_flag)
        return GstNvH264PtdErrorCode::InvalidReferencePattern;
    }
  }

  return GstNvH264PtdErrorCode::None;
}

void
GstNvH264PtdController::advance (GstNvEncH264PtdDecision * decision)
{
  g_return_if_fail (decision != nullptr);

  abs_frame_idx_++;
  if (decision->is_idr)
    gop_frame_idx_ = 1;
  else
    gop_frame_idx_++;

  decision->abs_frame_idx_after = abs_frame_idx_;
  decision->gop_frame_idx_after = gop_frame_idx_;
}

void
GstNvH264PtdController::applyLtr (GstNvEncH264PtdDecision * decision,
    const GstNvH264LtrRequest & ltr) const
{
  g_return_if_fail (decision != nullptr);

  decision->ltr_mark_frame = FALSE;
  decision->ltr_use_frames = FALSE;
  decision->ltr_mark_frame_idx = 0;
  decision->ltr_use_frame_bitmap = 0;
  decision->ltr_reset = decision->is_idr;
  decision->ltr_slot_count = ltr.slot_count;
  decision->ltr_confirmed_bitmap = ltr.confirmed_bitmap;
  decision->ltr_mark_candidate = ltr.mark_candidate;

  if (decision->is_idr || decision->picture_type == NV_ENC_PIC_TYPE_IDR)
    return;

  if (decision->ref_pic_flag == 0 || ltr.slot_count == 0)
    return;

  guint slot_count = MIN (ltr.slot_count, 32);
  guint32 slot_mask = slot_count >= 32 ? G_MAXUINT32 :
      ((1u << slot_count) - 1u);
  gboolean markable = !decision->temporal_svc_enabled ||
      decision->temporal_layer == 0;

  if (!markable)
    return;

  guint32 use_bitmap = ltr.confirmed_bitmap & ltr_live_bitmap_ & slot_mask;

  if (ltr.mark_candidate >= 0 &&
      (guint) ltr.mark_candidate < slot_count) {
    decision->ltr_mark_frame = TRUE;
    decision->ltr_mark_frame_idx = (guint32) ltr.mark_candidate;
    use_bitmap &= ~(1u << decision->ltr_mark_frame_idx);
  }

  if (use_bitmap != 0) {
    decision->ltr_use_frames = TRUE;
    decision->ltr_use_frame_bitmap = use_bitmap;
  }
}

void
GstNvH264PtdController::commitLtrDecision (
    const GstNvEncH264PtdDecision & decision)
{
  guint slot_count = MIN (decision.ltr_slot_count, 32);
  guint32 slot_mask = slot_count >= 32 ? G_MAXUINT32 :
      (slot_count == 0 ? 0 : ((1u << slot_count) - 1u));

  ltr_live_bitmap_ &= slot_mask;

  if (decision.ltr_reset) {
    ltr_live_bitmap_ = 0;
    return;
  }

  if (decision.ltr_mark_frame && decision.ltr_mark_frame_idx < slot_count)
    ltr_live_bitmap_ |= (1u << decision.ltr_mark_frame_idx);
}

const gchar *
GstNvH264PtdController::errorMessage (
    GstNvH264PtdErrorCode error_code) const
{
  switch (error_code) {
    case GstNvH264PtdErrorCode::None:
      return "none";
    case GstNvH264PtdErrorCode::InvalidTemporalLayers:
      return "invalid temporal layer count";
    case GstNvH264PtdErrorCode::InvalidGopLength:
      return "invalid GOP length";
    case GstNvH264PtdErrorCode::InvalidPoc:
      return "invalid H.264 display POC";
    case GstNvH264PtdErrorCode::InvalidReferencePattern:
      return "invalid temporal reference pattern";
    case GstNvH264PtdErrorCode::MissingDecision:
      return "missing H.264 picture type decision";
    default:
      return "unknown H.264 PTD error";
  }
}

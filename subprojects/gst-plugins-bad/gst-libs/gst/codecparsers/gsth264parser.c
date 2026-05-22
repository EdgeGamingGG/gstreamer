/* Gstreamer
 * Copyright (C) <2011> Intel Corporation
 * Copyright (C) <2011> Collabora Ltd.
 * Copyright (C) <2011> Thibault Saunier <thibault.saunier@collabora.com>
 *
 * Some bits C-c,C-v'ed and s/4/3 from h264parse and videoparsers/h264parse.c:
 *    Copyright (C) <2010> Mark Nauwelaerts <mark.nauwelaerts@collabora.co.uk>
 *    Copyright (C) <2010> Collabora Multimedia
 *    Copyright (C) <2010> Nokia Corporation
 *
 *    (C) 2005 Michal Benes <michal.benes@itonis.tv>
 *    (C) 2008 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:gsth264parser
 * @title: GstH264Parser
 * @short_description: Convenience library for h264 video
 * bitstream parsing.
 *
 * It offers bitstream parsing in both AVC (length-prefixed) and Annex B
 * (0x000001 start code prefix) format. To identify a NAL unit in a bitstream
 * and parse its headers, first call:
 *
 *   * #gst_h264_parser_identify_nalu to identify a NAL unit in an Annex B type bitstream
 *
 *   * #gst_h264_parser_identify_nalu_avc to identify a NAL unit in an AVC type bitstream
 *
 * The following functions are then available for parsing the structure of the
 * #GstH264NalUnit, depending on the #GstH264NalUnitType:
 *
 *   * From %GST_H264_NAL_SLICE to %GST_H264_NAL_SLICE_IDR: #gst_h264_parser_parse_slice_hdr
 *
 *   * %GST_H264_NAL_SEI: #gst_h264_parser_parse_sei
 *
 *   * %GST_H264_NAL_SPS: #gst_h264_parser_parse_sps
 *
 *   * %GST_H264_NAL_PPS: #gst_h264_parser_parse_pps
 *
 *   * Any other: #gst_h264_parser_parse_nal
 *
 * One of these functions *must* be called on every NAL unit in the bitstream,
 * in order to keep the internal structures of the #GstH264NalParser up to
 * date. It is legal to call #gst_h264_parser_parse_nal on NAL units of any
 * type, if no special parsing of the current NAL unit is required by the
 * application.
 *
 * For more details about the structures, look at the ITU-T H.264 and ISO/IEC 14496-10 â€“ MPEG-4
 * Part 10 specifications, available at:
 *
 *   * ITU-T H.264: http://www.itu.int/rec/T-REC-H.264
 *
 *   * ISO/IEC 14496-10: http://www.iso.org/iso/iso_catalogue/catalogue_tc/catalogue_detail.htm?csnumber=56538
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "nalutils.h"
#include "gsth264parser.h"

#include <gst/base/gstbytereader.h>
#include <gst/base/gstbitreader.h>

#ifndef GST_DISABLE_GST_DEBUG
#define GST_CAT_DEFAULT gst_h264_debug_category_get()
static GstDebugCategory *
gst_h264_debug_category_get (void)
{
  static gsize cat_gonce = 0;

  if (g_once_init_enter (&cat_gonce)) {
    GstDebugCategory *cat = NULL;

    GST_DEBUG_CATEGORY_INIT (cat, "codecparsers_h264", 0, "h264 parse library");

    g_once_init_leave (&cat_gonce, (gsize) cat);
  }

  return (GstDebugCategory *) cat_gonce;
}
#endif /* GST_DISABLE_GST_DEBUG */

/**** Default scaling_lists according to Table 7-2 *****/
static const guint8 default_4x4_intra[16] = {
  6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32,
  32, 37, 37, 42
};

static const guint8 default_4x4_inter[16] = {
  10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27,
  27, 30, 30, 34
};

static const guint8 default_8x8_intra[64] = {
  6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18,
  18, 18, 18, 23, 23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27,
  27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31, 31, 33,
  33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42
};

static const guint8 default_8x8_inter[64] = {
  9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19,
  19, 19, 19, 21, 21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24,
  24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 28,
  28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35
};

static const guint8 zigzag_8x8[64] = {
  0, 1, 8, 16, 9, 2, 3, 10,
  17, 24, 32, 25, 18, 11, 4, 5,
  12, 19, 26, 33, 40, 48, 41, 34,
  27, 20, 13, 6, 7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36,
  29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46,
  53, 60, 61, 54, 47, 55, 62, 63
};

static const guint8 zigzag_4x4[16] = {
  0, 1, 4, 8,
  5, 2, 3, 6,
  9, 12, 13, 10,
  7, 11, 14, 15,
};

typedef struct
{
  guint par_n, par_d;
} PAR;

/* Table E-1 - Meaning of sample aspect ratio indicator (1..16) */
static const PAR aspect_ratios[17] = {
  {0, 0},
  {1, 1},
  {12, 11},
  {10, 11},
  {16, 11},
  {40, 33},
  {24, 11},
  {20, 11},
  {32, 11},
  {80, 33},
  {18, 11},
  {15, 11},
  {64, 33},
  {160, 99},
  {4, 3},
  {3, 2},
  {2, 1}
};

/*****  Utils ****/
#define EXTENDED_SAR 255

static GstH264SPS *
gst_h264_parser_get_sps (GstH264NalParser * nalparser, guint8 sps_id)
{
  GstH264SPS *sps;

  sps = &nalparser->sps[sps_id];

  if (sps->valid)
    return sps;

  return NULL;
}

static GstH264PPS *
gst_h264_parser_get_pps (GstH264NalParser * nalparser, guint8 pps_id)
{
  GstH264PPS *pps;

  pps = &nalparser->pps[pps_id];

  if (pps->valid)
    return pps;

  return NULL;
}

static gboolean
gst_h264_parse_nalu_header (GstH264NalUnit * nalu)
{
  guint8 *data = nalu->data + nalu->offset;
  guint8 svc_extension_flag;
  GstBitReader br;

  if (nalu->size < 1)
    return FALSE;

  nalu->type = (data[0] & 0x1f);
  nalu->ref_idc = (data[0] & 0x60) >> 5;
  nalu->idr_pic_flag = (nalu->type == 5 ? 1 : 0);
  nalu->header_bytes = 1;

  nalu->extension_type = GST_H264_NAL_EXTENSION_NONE;

  switch (nalu->type) {
    case GST_H264_NAL_PREFIX_UNIT:
    case GST_H264_NAL_SLICE_EXT:
      if (nalu->size < 4)
        return FALSE;
      gst_bit_reader_init (&br, nalu->data + nalu->offset + nalu->header_bytes,
          nalu->size - nalu->header_bytes);

      svc_extension_flag = gst_bit_reader_get_bits_uint8_unchecked (&br, 1);
      if (svc_extension_flag) { /* SVC */
        GstH264NalUnitExtensionSVC *const svc = &nalu->extension.svc;

        nalu->extension_type = GST_H264_NAL_EXTENSION_SVC;
        svc->idr_flag = gst_bit_reader_get_bits_uint8_unchecked (&br, 1);
        svc->priority_id = gst_bit_reader_get_bits_uint8_unchecked (&br, 6);
        svc->no_inter_layer_pred_flag =
            gst_bit_reader_get_bits_uint8_unchecked (&br, 1);
        svc->dependency_id = gst_bit_reader_get_bits_uint8_unchecked (&br, 3);
        svc->quality_id = gst_bit_reader_get_bits_uint8_unchecked (&br, 4);
        svc->temporal_id = gst_bit_reader_get_bits_uint8_unchecked (&br, 3);
        svc->use_ref_base_pic_flag =
            gst_bit_reader_get_bits_uint8_unchecked (&br, 1);
        svc->discardable_flag = gst_bit_reader_get_bits_uint8_unchecked (&br, 1);
        svc->output_flag = gst_bit_reader_get_bits_uint8_unchecked (&br, 1);
        svc->reserved_three_2bits =
            gst_bit_reader_get_bits_uint8_unchecked (&br, 2);

        /* Update IdrPicFlag (G.7.4.1.1) */
        nalu->idr_pic_flag = svc->idr_flag;

      } else {                  /* MVC */
        GstH264NalUnitExtensionMVC *const mvc = &nalu->extension.mvc;

        nalu->extension_type = GST_H264_NAL_EXTENSION_MVC;
        mvc->non_idr_flag = gst_bit_reader_get_bits_uint8_unchecked (&br, 1);
        mvc->priority_id = gst_bit_reader_get_bits_uint8_unchecked (&br, 6);
        mvc->view_id = gst_bit_reader_get_bits_uint16_unchecked (&br, 10);
        mvc->temporal_id = gst_bit_reader_get_bits_uint8_unchecked (&br, 3);
        mvc->anchor_pic_flag = gst_bit_reader_get_bits_uint8_unchecked (&br, 1);
        mvc->inter_view_flag = gst_bit_reader_get_bits_uint8_unchecked (&br, 1);

        /* Update IdrPicFlag (H.7.4.1.1) */
        nalu->idr_pic_flag = !mvc->non_idr_flag;
      }
      nalu->header_bytes += 3;
      break;
    default:
      break;
  }

  GST_DEBUG ("Nal type %u, ref_idc %u", nalu->type, nalu->ref_idc);
  return TRUE;
}

/*
 * gst_h264_pps_copy:
 * @dst_pps: The destination #GstH264PPS to copy into
 * @src_pps: The source #GstH264PPS to copy from
 *
 * Copies @src_pps into @dst_pps.
 *
 * Returns: %TRUE if everything went fine, %FALSE otherwise
 */
static gboolean
gst_h264_pps_copy (GstH264PPS * dst_pps, const GstH264PPS * src_pps)
{
  g_return_val_if_fail (dst_pps != NULL, FALSE);
  g_return_val_if_fail (src_pps != NULL, FALSE);

  gst_h264_pps_clear (dst_pps);

  *dst_pps = *src_pps;

  if (src_pps->slice_group_id)
    dst_pps->slice_group_id = g_memdup2 (src_pps->slice_group_id,
        src_pps->pic_size_in_map_units_minus1 + 1);

  return TRUE;
}

/* Copy MVC-specific data for subset SPS header */
static gboolean
gst_h264_sps_mvc_copy (GstH264SPS * dst_sps, const GstH264SPS * src_sps)
{
  GstH264SPSExtMVC *const dst_mvc = &dst_sps->extension.mvc;
  const GstH264SPSExtMVC *const src_mvc = &src_sps->extension.mvc;
  guint i, j, k;

  g_assert (dst_sps->extension_type == GST_H264_NAL_EXTENSION_MVC);

  dst_mvc->num_views_minus1 = src_mvc->num_views_minus1;
  dst_mvc->view = g_new0 (GstH264SPSExtMVCView, dst_mvc->num_views_minus1 + 1);
  if (!dst_mvc->view)
    return FALSE;

  dst_mvc->view[0].view_id = src_mvc->view[0].view_id;

  for (i = 1; i <= dst_mvc->num_views_minus1; i++) {
    GstH264SPSExtMVCView *const dst_view = &dst_mvc->view[i];
    const GstH264SPSExtMVCView *const src_view = &src_mvc->view[i];

    dst_view->view_id = src_view->view_id;

    dst_view->num_anchor_refs_l0 = src_view->num_anchor_refs_l0;
    for (j = 0; j < dst_view->num_anchor_refs_l0; j++)
      dst_view->anchor_ref_l0[j] = src_view->anchor_ref_l0[j];

    dst_view->num_anchor_refs_l1 = src_view->num_anchor_refs_l1;
    for (j = 0; j < dst_view->num_anchor_refs_l1; j++)
      dst_view->anchor_ref_l1[j] = src_view->anchor_ref_l1[j];

    dst_view->num_non_anchor_refs_l0 = src_view->num_non_anchor_refs_l0;
    for (j = 0; j < dst_view->num_non_anchor_refs_l0; j++)
      dst_view->non_anchor_ref_l0[j] = src_view->non_anchor_ref_l0[j];

    dst_view->num_non_anchor_refs_l1 = src_view->num_non_anchor_refs_l1;
    for (j = 0; j < dst_view->num_non_anchor_refs_l1; j++)
      dst_view->non_anchor_ref_l1[j] = src_view->non_anchor_ref_l1[j];
  }

  dst_mvc->num_level_values_signalled_minus1 =
      src_mvc->num_level_values_signalled_minus1;
  dst_mvc->level_value = g_new0 (GstH264SPSExtMVCLevelValue,
      dst_mvc->num_level_values_signalled_minus1 + 1);
  if (!dst_mvc->level_value)
    return FALSE;

  for (i = 0; i <= dst_mvc->num_level_values_signalled_minus1; i++) {
    GstH264SPSExtMVCLevelValue *const dst_value = &dst_mvc->level_value[i];
    const GstH264SPSExtMVCLevelValue *const src_value =
        &src_mvc->level_value[i];

    dst_value->level_idc = src_value->level_idc;

    dst_value->num_applicable_ops_minus1 = src_value->num_applicable_ops_minus1;
    dst_value->applicable_op = g_new0 (GstH264SPSExtMVCLevelValueOp,
        dst_value->num_applicable_ops_minus1 + 1);
    if (!dst_value->applicable_op)
      return FALSE;

    for (j = 0; j <= dst_value->num_applicable_ops_minus1; j++) {
      GstH264SPSExtMVCLevelValueOp *const dst_op = &dst_value->applicable_op[j];
      const GstH264SPSExtMVCLevelValueOp *const src_op =
          &src_value->applicable_op[j];

      dst_op->temporal_id = src_op->temporal_id;
      dst_op->num_target_views_minus1 = src_op->num_target_views_minus1;
      dst_op->target_view_id =
          g_new (guint16, dst_op->num_target_views_minus1 + 1);
      if (!dst_op->target_view_id)
        return FALSE;

      for (k = 0; k <= dst_op->num_target_views_minus1; k++)
        dst_op->target_view_id[k] = src_op->target_view_id[k];
      dst_op->num_views_minus1 = src_op->num_views_minus1;
    }
  }
  return TRUE;
}

/*
 * gst_h264_sps_copy:
 * @dst_sps: The destination #GstH264SPS to copy into
 * @src_sps: The source #GstH264SPS to copy from
 *
 * Copies @src_sps into @dst_sps.
 *
 * Returns: %TRUE if everything went fine, %FALSE otherwise
 */
static gboolean
gst_h264_sps_copy (GstH264SPS * dst_sps, const GstH264SPS * src_sps)
{
  g_return_val_if_fail (dst_sps != NULL, FALSE);
  g_return_val_if_fail (src_sps != NULL, FALSE);

  gst_h264_sps_clear (dst_sps);

  *dst_sps = *src_sps;

  switch (dst_sps->extension_type) {
    case GST_H264_NAL_EXTENSION_MVC:
      if (!gst_h264_sps_mvc_copy (dst_sps, src_sps))
        return FALSE;
      break;
  }
  return TRUE;
}

/****** Parsing functions *****/

static gboolean
gst_h264_parse_hrd_parameters (GstH264HRDParams * hrd, NalReader * nr)
{
  guint sched_sel_idx;

  GST_DEBUG ("parsing \"HRD Parameters\"");

  READ_UE_MAX (nr, hrd->cpb_cnt_minus1, 31);
  READ_UINT8 (nr, hrd->bit_rate_scale, 4);
  READ_UINT8 (nr, hrd->cpb_size_scale, 4);

  for (sched_sel_idx = 0; sched_sel_idx <= hrd->cpb_cnt_minus1; sched_sel_idx++) {
    READ_UE (nr, hrd->bit_rate_value_minus1[sched_sel_idx]);
    READ_UE (nr, hrd->cpb_size_value_minus1[sched_sel_idx]);
    READ_UINT8 (nr, hrd->cbr_flag[sched_sel_idx], 1);
  }

  READ_UINT8 (nr, hrd->initial_cpb_removal_delay_length_minus1, 5);
  READ_UINT8 (nr, hrd->cpb_removal_delay_length_minus1, 5);
  READ_UINT8 (nr, hrd->dpb_output_delay_length_minus1, 5);
  READ_UINT8 (nr, hrd->time_offset_length, 5);

  return TRUE;

error:
  GST_WARNING ("error parsing \"HRD Parameters\"");
  return FALSE;
}

static gboolean
gst_h264_parse_vui_parameters (GstH264SPS * sps, NalReader * nr)
{
  GstH264VUIParams *vui = &sps->vui_parameters;

  GST_DEBUG ("parsing \"VUI Parameters\"");

  /* set default values for fields that might not be present in the bitstream
     and have valid defaults */
  vui->video_format = 5;
  vui->colour_primaries = 2;
  vui->transfer_characteristics = 2;
  vui->matrix_coefficients = 2;

  READ_UINT8 (nr, vui->aspect_ratio_info_present_flag, 1);
  if (vui->aspect_ratio_info_present_flag) {
    READ_UINT8 (nr, vui->aspect_ratio_idc, 8);
    if (vui->aspect_ratio_idc == EXTENDED_SAR) {
      READ_UINT16 (nr, vui->sar_width, 16);
      READ_UINT16 (nr, vui->sar_height, 16);
      vui->par_n = vui->sar_width;
      vui->par_d = vui->sar_height;
    } else if (vui->aspect_ratio_idc <= 16) {
      vui->par_n = aspect_ratios[vui->aspect_ratio_idc].par_n;
      vui->par_d = aspect_ratios[vui->aspect_ratio_idc].par_d;
    }
  }

  READ_UINT8 (nr, vui->overscan_info_present_flag, 1);
  if (vui->overscan_info_present_flag)
    READ_UINT8 (nr, vui->overscan_appropriate_flag, 1);

  READ_UINT8 (nr, vui->video_signal_type_present_flag, 1);
  if (vui->video_signal_type_present_flag) {

    READ_UINT8 (nr, vui->video_format, 3);
    READ_UINT8 (nr, vui->video_full_range_flag, 1);
    READ_UINT8 (nr, vui->colour_description_present_flag, 1);
    if (vui->colour_description_present_flag) {
      READ_UINT8 (nr, vui->colour_primaries, 8);
      READ_UINT8 (nr, vui->transfer_characteristics, 8);
      READ_UINT8 (nr, vui->matrix_coefficients, 8);
    }
  }

  READ_UINT8 (nr, vui->chroma_loc_info_present_flag, 1);
  if (vui->chroma_loc_info_present_flag) {
    READ_UE_MAX (nr, vui->chroma_sample_loc_type_top_field, 5);
    READ_UE_MAX (nr, vui->chroma_sample_loc_type_bottom_field, 5);
  }

  READ_UINT8 (nr, vui->timing_info_present_flag, 1);
  if (vui->timing_info_present_flag) {
    READ_UINT32 (nr, vui->num_units_in_tick, 32);
    if (vui->num_units_in_tick == 0)
      GST_WARNING ("num_units_in_tick = 0 detected in stream "
          "(incompliant to H.264 E.2.1).");

    READ_UINT32 (nr, vui->time_scale, 32);
    if (vui->time_scale == 0)
      GST_WARNING ("time_scale = 0 detected in stream "
          "(incompliant to H.264 E.2.1).");

    READ_UINT8 (nr, vui->fixed_frame_rate_flag, 1);
  }

  READ_UINT8 (nr, vui->nal_hrd_parameters_present_flag, 1);
  if (vui->nal_hrd_parameters_present_flag) {
    if (!gst_h264_parse_hrd_parameters (&vui->nal_hrd_parameters, nr))
      goto error;
  }

  READ_UINT8 (nr, vui->vcl_hrd_parameters_present_flag, 1);
  if (vui->vcl_hrd_parameters_present_flag) {
    if (!gst_h264_parse_hrd_parameters (&vui->vcl_hrd_parameters, nr))
      goto error;
  }

  if (vui->nal_hrd_parameters_present_flag ||
      vui->vcl_hrd_parameters_present_flag)
    READ_UINT8 (nr, vui->low_delay_hrd_flag, 1);

  READ_UINT8 (nr, vui->pic_struct_present_flag, 1);
  READ_UINT8 (nr, vui->bitstream_restriction_flag, 1);
  if (vui->bitstream_restriction_flag) {
    READ_UINT8 (nr, vui->motion_vectors_over_pic_boundaries_flag, 1);
    READ_UE (nr, vui->max_bytes_per_pic_denom);
    READ_UE_MAX (nr, vui->max_bits_per_mb_denom, 16);
    READ_UE_MAX (nr, vui->log2_max_mv_length_horizontal, 16);
    READ_UE_MAX (nr, vui->log2_max_mv_length_vertical, 16);
    READ_UE (nr, vui->num_reorder_frames);
    READ_UE (nr, vui->max_dec_frame_buffering);
  }

  return TRUE;

error:
  GST_WARNING ("error parsing \"VUI Parameters\"");
  return FALSE;
}

static gboolean
gst_h264_parser_parse_scaling_list (NalReader * nr,
    guint8 scaling_lists_4x4[6][16], guint8 scaling_lists_8x8[6][64],
    const guint8 fallback_4x4_inter[16], const guint8 fallback_4x4_intra[16],
    const guint8 fallback_8x8_inter[64], const guint8 fallback_8x8_intra[64],
    guint8 n_lists)
{
  guint i;

  static const guint8 *default_lists[12] = {
    default_4x4_intra, default_4x4_intra, default_4x4_intra,
    default_4x4_inter, default_4x4_inter, default_4x4_inter,
    default_8x8_intra, default_8x8_inter,
    default_8x8_intra, default_8x8_inter,
    default_8x8_intra, default_8x8_inter
  };

  GST_DEBUG ("parsing scaling lists");

  for (i = 0; i < 12; i++) {
    gboolean use_default = FALSE;

    if (i < n_lists) {
      guint8 scaling_list_present_flag;

      READ_UINT8 (nr, scaling_list_present_flag, 1);
      if (scaling_list_present_flag) {
        guint8 *scaling_list;
        guint size;
        guint j;
        guint8 last_scale, next_scale;

        if (i < 6) {
          scaling_list = scaling_lists_4x4[i];
          size = 16;
        } else {
          scaling_list = scaling_lists_8x8[i - 6];
          size = 64;
        }

        last_scale = 8;
        next_scale = 8;
        for (j = 0; j < size; j++) {
          if (next_scale != 0) {
            gint32 delta_scale;

            READ_SE (nr, delta_scale);
            next_scale = (last_scale + delta_scale) & 0xff;
          }
          if (j == 0 && next_scale == 0) {
            /* Use default scaling lists (7.4.2.1.1.1) */
            memcpy (scaling_list, default_lists[i], size);
            break;
          }
          last_scale = scaling_list[j] =
              (next_scale == 0) ? last_scale : next_scale;
        }
      } else
        use_default = TRUE;
    } else
      use_default = TRUE;

    if (use_default) {
      switch (i) {
        case 0:
          memcpy (scaling_lists_4x4[0], fallback_4x4_intra, 16);
          break;
        case 1:
          memcpy (scaling_lists_4x4[1], scaling_lists_4x4[0], 16);
          break;
        case 2:
          memcpy (scaling_lists_4x4[2], scaling_lists_4x4[1], 16);
          break;
        case 3:
          memcpy (scaling_lists_4x4[3], fallback_4x4_inter, 16);
          break;
        case 4:
          memcpy (scaling_lists_4x4[4], scaling_lists_4x4[3], 16);
          break;
        case 5:
          memcpy (scaling_lists_4x4[5], scaling_lists_4x4[4], 16);
          break;
        case 6:
          memcpy (scaling_lists_8x8[0], fallback_8x8_intra, 64);
          break;
        case 7:
          memcpy (scaling_lists_8x8[1], fallback_8x8_inter, 64);
          break;
        case 8:
          memcpy (scaling_lists_8x8[2], scaling_lists_8x8[0], 64);
          break;
        case 9:
          memcpy (scaling_lists_8x8[3], scaling_lists_8x8[1], 64);
          break;
        case 10:
          memcpy (scaling_lists_8x8[4], scaling_lists_8x8[2], 64);
          break;
        case 11:
          memcpy (scaling_lists_8x8[5], scaling_lists_8x8[3], 64);
          break;

        default:
          break;
      }
    }
  }

  return TRUE;

error:
  GST_WARNING ("error parsing scaling lists");
  return FALSE;
}

static gboolean
slice_parse_ref_pic_list_modification_1 (GstH264SliceHdr * slice,
    NalReader * nr, guint list, gboolean is_mvc)
{
  GstH264RefPicListModification *entries;
  guint8 *ref_pic_list_modification_flag, *n_ref_pic_list_modification;
  guint32 modification_of_pic_nums_idc;
  gsize max_entries;
  guint i = 0;

  if (list == 0) {
    entries = slice->ref_pic_list_modification_l0;
    max_entries = G_N_ELEMENTS (slice->ref_pic_list_modification_l0);
    ref_pic_list_modification_flag = &slice->ref_pic_list_modification_flag_l0;
    n_ref_pic_list_modification = &slice->n_ref_pic_list_modification_l0;
  } else {
    entries = slice->ref_pic_list_modification_l1;
    max_entries = G_N_ELEMENTS (slice->ref_pic_list_modification_l1);
    ref_pic_list_modification_flag = &slice->ref_pic_list_modification_flag_l1;
    n_ref_pic_list_modification = &slice->n_ref_pic_list_modification_l1;
  }

  READ_UINT8 (nr, *ref_pic_list_modification_flag, 1);
  if (*ref_pic_list_modification_flag) {
    while (1) {
      READ_UE (nr, modification_of_pic_nums_idc);
      if (modification_of_pic_nums_idc == 0 ||
          modification_of_pic_nums_idc == 1) {
        READ_UE_MAX (nr, entries[i].value.abs_diff_pic_num_minus1,
            slice->max_pic_num - 1);
      } else if (modification_of_pic_nums_idc == 2) {
        READ_UE (nr, entries[i].value.long_term_pic_num);
      } else if (is_mvc && (modification_of_pic_nums_idc == 4 ||
              modification_of_pic_nums_idc == 5)) {
        READ_UE (nr, entries[i].value.abs_diff_view_idx_minus1);
      }
      entries[i++].modification_of_pic_nums_idc = modification_of_pic_nums_idc;
      if (modification_of_pic_nums_idc == 3)
        break;
      if (i >= max_entries)
        goto error;
    }
  }
  *n_ref_pic_list_modification = i;
  return TRUE;

error:
  GST_WARNING ("error parsing \"Reference picture list %u modification\"",
      list);
  return FALSE;
}

static gboolean
slice_parse_ref_pic_list_modification (GstH264SliceHdr * slice, NalReader * nr,
    gboolean is_mvc)
{
  if (!GST_H264_IS_I_SLICE (slice) && !GST_H264_IS_SI_SLICE (slice)) {
    if (!slice_parse_ref_pic_list_modification_1 (slice, nr, 0, is_mvc))
      return FALSE;
  }

  if (GST_H264_IS_B_SLICE (slice)) {
    if (!slice_parse_ref_pic_list_modification_1 (slice, nr, 1, is_mvc))
      return FALSE;
  }
  return TRUE;
}

static gboolean
gst_h264_slice_parse_dec_ref_pic_marking (GstH264SliceHdr * slice,
    GstH264NalUnit * nalu, NalReader * nr)
{
  GstH264DecRefPicMarking *dec_ref_pic_m;
  guint start_pos, start_epb;

  GST_DEBUG ("parsing \"Decoded reference picture marking\"");

  start_pos = nal_reader_get_pos (nr);
  start_epb = nal_reader_get_epb_count (nr);

  dec_ref_pic_m = &slice->dec_ref_pic_marking;

  if (nalu->idr_pic_flag) {
    READ_UINT8 (nr, dec_ref_pic_m->no_output_of_prior_pics_flag, 1);
    READ_UINT8 (nr, dec_ref_pic_m->long_term_reference_flag, 1);
  } else {
    READ_UINT8 (nr, dec_ref_pic_m->adaptive_ref_pic_marking_mode_flag, 1);
    if (dec_ref_pic_m->adaptive_ref_pic_marking_mode_flag) {
      guint32 mem_mgmt_ctrl_op;
      GstH264RefPicMarking *refpicmarking;

      dec_ref_pic_m->n_ref_pic_marking = 0;
      while (1) {
        READ_UE_MAX (nr, mem_mgmt_ctrl_op, 6);
        if (mem_mgmt_ctrl_op == 0)
          break;

        if (dec_ref_pic_m->n_ref_pic_marking >=
            G_N_ELEMENTS (dec_ref_pic_m->ref_pic_marking))
          goto error;

        refpicmarking =
            &dec_ref_pic_m->ref_pic_marking[dec_ref_pic_m->n_ref_pic_marking];

        refpicmarking->memory_management_control_operation = mem_mgmt_ctrl_op;

        if (mem_mgmt_ctrl_op == 1 || mem_mgmt_ctrl_op == 3)
          READ_UE (nr, refpicmarking->difference_of_pic_nums_minus1);

        if (mem_mgmt_ctrl_op == 2)
          READ_UE (nr, refpicmarking->long_term_pic_num);

        if (mem_mgmt_ctrl_op == 3 || mem_mgmt_ctrl_op == 6)
          READ_UE (nr, refpicmarking->long_term_frame_idx);

        if (mem_mgmt_ctrl_op == 4)
          READ_UE (nr, refpicmarking->max_long_term_frame_idx_plus1);

        dec_ref_pic_m->n_ref_pic_marking++;
      }
    }
  }

  dec_ref_pic_m->bit_size = (nal_reader_get_pos (nr) - start_pos) -
      (8 * (nal_reader_get_epb_count (nr) - start_epb));

  return TRUE;

error:
  GST_WARNING ("error parsing \"Decoded reference picture marking\"");
  return FALSE;
}

static gboolean
gst_h264_slice_parse_pred_weight_table (GstH264SliceHdr * slice,
    NalReader * nr, guint8 chroma_array_type)
{
  GstH264PredWeightTable *p;
  gint16 default_luma_weight, default_chroma_weight;
  gint i;

  GST_DEBUG ("parsing \"Prediction weight table\"");

  p = &slice->pred_weight_table;

  READ_UE_MAX (nr, p->luma_log2_weight_denom, 7);
  /* set default values */
  default_luma_weight = 1 << p->luma_log2_weight_denom;
  for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++)
    p->luma_weight_l0[i] = default_luma_weight;
  if (GST_H264_IS_B_SLICE (slice)) {
    for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++)
      p->luma_weight_l1[i] = default_luma_weight;
  }

  if (chroma_array_type != 0) {
    READ_UE_MAX (nr, p->chroma_log2_weight_denom, 7);
    /* set default values */
    default_chroma_weight = 1 << p->chroma_log2_weight_denom;
    for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++) {
      p->chroma_weight_l0[i][0] = default_chroma_weight;
      p->chroma_weight_l0[i][1] = default_chroma_weight;
    }
    if (GST_H264_IS_B_SLICE (slice)) {
      for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++) {
        p->chroma_weight_l1[i][0] = default_chroma_weight;
        p->chroma_weight_l1[i][1] = default_chroma_weight;
      }
    }
  }

  for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++) {
    guint8 luma_weight_l0_flag;

    READ_UINT8 (nr, luma_weight_l0_flag, 1);
    if (luma_weight_l0_flag) {
      READ_SE_ALLOWED (nr, p->luma_weight_l0[i], -128, 127);
      READ_SE_ALLOWED (nr, p->luma_offset_l0[i], -128, 127);
    }
    if (chroma_array_type != 0) {
      guint8 chroma_weight_l0_flag;
      gint j;

      READ_UINT8 (nr, chroma_weight_l0_flag, 1);
      if (chroma_weight_l0_flag) {
        for (j = 0; j < 2; j++) {
          READ_SE_ALLOWED (nr, p->chroma_weight_l0[i][j], -128, 127);
          READ_SE_ALLOWED (nr, p->chroma_offset_l0[i][j], -128, 127);
        }
      }
    }
  }

  if (GST_H264_IS_B_SLICE (slice)) {
    for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++) {
      guint8 luma_weight_l1_flag;

      READ_UINT8 (nr, luma_weight_l1_flag, 1);
      if (luma_weight_l1_flag) {
        READ_SE_ALLOWED (nr, p->luma_weight_l1[i], -128, 127);
        READ_SE_ALLOWED (nr, p->luma_offset_l1[i], -128, 127);
      }
      if (chroma_array_type != 0) {
        guint8 chroma_weight_l1_flag;
        gint j;

        READ_UINT8 (nr, chroma_weight_l1_flag, 1);
        if (chroma_weight_l1_flag) {
          for (j = 0; j < 2; j++) {
            READ_SE_ALLOWED (nr, p->chroma_weight_l1[i][j], -128, 127);
            READ_SE_ALLOWED (nr, p->chroma_offset_l1[i][j], -128, 127);
          }
        }
      }
    }
  }

  return TRUE;

error:
  GST_WARNING ("error parsing \"Prediction weight table\"");
  return FALSE;
}

static GstH264ParserResult
gst_h264_parser_parse_buffering_period (GstH264NalParser * nalparser,
    GstH264BufferingPeriod * per, NalReader * nr)
{
  GstH264SPS *sps;
  guint8 sps_id;

  GST_DEBUG ("parsing \"Buffering period\"");

  READ_UE_MAX (nr, sps_id, GST_H264_MAX_SPS_COUNT - 1);
  sps = gst_h264_parser_get_sps (nalparser, sps_id);
  if (!sps) {
    GST_WARNING ("couldn't find associated sequence parameter set with id: %d",
        sps_id);
    return GST_H264_PARSER_BROKEN_LINK;
  }
  per->sps = sps;

  if (sps->vui_parameters_present_flag) {
    GstH264VUIParams *vui = &sps->vui_parameters;

    if (vui->nal_hrd_parameters_present_flag) {
      GstH264HRDParams *hrd = &vui->nal_hrd_parameters;
      const guint8 nbits = hrd->initial_cpb_removal_delay_length_minus1 + 1;
      guint8 sched_sel_idx;

      for (sched_sel_idx = 0; sched_sel_idx <= hrd->cpb_cnt_minus1;
          sched_sel_idx++) {
        READ_UINT32 (nr, per->nal_initial_cpb_removal_delay[sched_sel_idx],
            nbits);
        READ_UINT32 (nr,
            per->nal_initial_cpb_removal_delay_offset[sched_sel_idx], nbits);
      }
    }

    if (vui->vcl_hrd_parameters_present_flag) {
      GstH264HRDParams *hrd = &vui->vcl_hrd_parameters;
      const guint8 nbits = hrd->initial_cpb_removal_delay_length_minus1 + 1;
      guint8 sched_sel_idx;

      for (sched_sel_idx = 0; sched_sel_idx <= hrd->cpb_cnt_minus1;
          sched_sel_idx++) {
        READ_UINT32 (nr, per->vcl_initial_cpb_removal_delay[sched_sel_idx],
            nbits);
        READ_UINT32 (nr,
            per->vcl_initial_cpb_removal_delay_offset[sched_sel_idx], nbits);
      }
    }
  }

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Buffering period\"");
  return GST_H264_PARSER_ERROR;
}

static gboolean
gst_h264_parse_clock_timestamp (GstH264ClockTimestamp * tim,
    guint8 time_offset_length, NalReader * nr)
{
  GST_DEBUG ("parsing \"Clock timestamp\"");

  /* default values */
  tim->time_offset = 0;

  READ_UINT8 (nr, tim->ct_type, 2);
  READ_UINT8 (nr, tim->nuit_field_based_flag, 1);
  READ_UINT8 (nr, tim->counting_type, 5);
  READ_UINT8 (nr, tim->full_timestamp_flag, 1);
  READ_UINT8 (nr, tim->discontinuity_flag, 1);
  READ_UINT8 (nr, tim->cnt_dropped_flag, 1);
  READ_UINT8 (nr, tim->n_frames, 8);

  if (tim->full_timestamp_flag) {
    tim->seconds_flag = TRUE;
    READ_UINT8 (nr, tim->seconds_value, 6);

    tim->minutes_flag = TRUE;
    READ_UINT8 (nr, tim->minutes_value, 6);

    tim->hours_flag = TRUE;
    READ_UINT8 (nr, tim->hours_value, 5);
  } else {
    READ_UINT8 (nr, tim->seconds_flag, 1);
    if (tim->seconds_flag) {
      READ_UINT8 (nr, tim->seconds_value, 6);
      READ_UINT8 (nr, tim->minutes_flag, 1);
      if (tim->minutes_flag) {
        READ_UINT8 (nr, tim->minutes_value, 6);
        READ_UINT8 (nr, tim->hours_flag, 1);
        if (tim->hours_flag)
          READ_UINT8 (nr, tim->hours_value, 5);
      }
    }
  }

  if (time_offset_length > 0)
    READ_UINT32 (nr, tim->time_offset, time_offset_length);

  return TRUE;

error:
  GST_WARNING ("error parsing \"Clock timestamp\"");
  return FALSE;
}

static GstH264ParserResult
gst_h264_parser_parse_pic_timing (GstH264NalParser * nalparser,
    GstH264PicTiming * tim, NalReader * nr)
{
  GstH264ParserResult error = GST_H264_PARSER_ERROR;

  GST_DEBUG ("parsing \"Picture timing\"");
  if (!nalparser->last_sps || !nalparser->last_sps->valid) {
    GST_WARNING ("didn't get the associated sequence parameter set for the "
        "current access unit");
    error = GST_H264_PARSER_BROKEN_LINK;
    goto error;
  }

  if (nalparser->last_sps->vui_parameters_present_flag) {
    GstH264VUIParams *vui = &nalparser->last_sps->vui_parameters;
    GstH264HRDParams *hrd = NULL;

    if (vui->nal_hrd_parameters_present_flag) {
      hrd = &vui->nal_hrd_parameters;
    } else if (vui->vcl_hrd_parameters_present_flag) {
      hrd = &vui->vcl_hrd_parameters;
    }

    tim->CpbDpbDelaysPresentFlag = !!hrd;
    tim->pic_struct_present_flag = vui->pic_struct_present_flag;

    if (tim->CpbDpbDelaysPresentFlag) {
      tim->cpb_removal_delay_length_minus1 =
          hrd->cpb_removal_delay_length_minus1;
      tim->dpb_output_delay_length_minus1 = hrd->dpb_output_delay_length_minus1;

      READ_UINT32 (nr, tim->cpb_removal_delay,
          tim->cpb_removal_delay_length_minus1 + 1);
      READ_UINT32 (nr, tim->dpb_output_delay,
          tim->dpb_output_delay_length_minus1 + 1);
    }

    if (tim->pic_struct_present_flag) {
      const guint8 num_clock_ts_table[9] = {
        1, 1, 1, 2, 2, 3, 3, 2, 3
      };
      guint8 num_clock_num_ts;
      guint i;

      READ_UINT8 (nr, tim->pic_struct, 4);
      CHECK_ALLOWED ((gint8) tim->pic_struct, 0, 8);

      tim->time_offset_length = 24;
      if (hrd)
        tim->time_offset_length = hrd->time_offset_length;

      num_clock_num_ts = num_clock_ts_table[tim->pic_struct];
      for (i = 0; i < num_clock_num_ts; i++) {
        READ_UINT8 (nr, tim->clock_timestamp_flag[i], 1);
        if (tim->clock_timestamp_flag[i]) {
          if (!gst_h264_parse_clock_timestamp (&tim->clock_timestamp[i],
                  tim->time_offset_length, nr))
            goto error;
        }
      }
    }
  }

  if (!tim->CpbDpbDelaysPresentFlag && !tim->pic_struct_present_flag) {
    GST_WARNING
        ("Invalid pic_timing SEI NAL with neither CpbDpbDelays nor pic_struct");
    return GST_H264_PARSER_BROKEN_DATA;
  }

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Picture timing\"");
  return error;
}

static GstH264ParserResult
gst_h264_parser_parse_registered_user_data (GstH264NalParser * nalparser,
    GstH264RegisteredUserData * rud, NalReader * nr, guint payload_size)
{
  guint8 *data = NULL;
  guint i;

  rud->data = NULL;
  rud->size = 0;

  if (payload_size < 2) {
    GST_WARNING ("Too small payload size %d", payload_size);
    return GST_H264_PARSER_BROKEN_DATA;
  }

  READ_UINT8 (nr, rud->country_code, 8);
  --payload_size;

  if (rud->country_code == 0xFF) {
    READ_UINT8 (nr, rud->country_code_extension, 8);
    --payload_size;
  } else {
    rud->country_code_extension = 0;
  }

  if (payload_size < 1) {
    GST_WARNING ("No more remaining payload data to store");
    return GST_H264_PARSER_BROKEN_DATA;
  }

  data = g_malloc (payload_size);
  for (i = 0; i < payload_size; ++i) {
    READ_UINT8 (nr, data[i], 8);
  }

  GST_MEMDUMP ("SEI user data", data, payload_size);

  rud->data = data;
  rud->size = payload_size;
  return GST_H264_PARSER_OK;

error:
  {
    GST_WARNING ("error parsing \"Registered User Data\"");
    g_free (data);
    return GST_H264_PARSER_ERROR;
  }
}

static GstH264ParserResult
gst_h264_parser_parse_user_data_unregistered (GstH264NalParser * nalparser,
    GstH264UserDataUnregistered * urud, NalReader * nr, guint payload_size)
{
  guint8 *data = NULL;
  gint i;

  if (payload_size < 16) {
    GST_WARNING ("Too small payload size %d", payload_size);
    return GST_H264_PARSER_BROKEN_DATA;
  }

  for (int i = 0; i < 16; i++) {
    READ_UINT8 (nr, urud->uuid[i], 8);
  }
  payload_size -= 16;

  urud->size = payload_size;

  data = g_malloc0 (payload_size);
  for (i = 0; i < payload_size; ++i) {
    READ_UINT8 (nr, data[i], 8);
  }

  urud->data = data;
  GST_MEMDUMP ("SEI user data unregistered", data, payload_size);
  return GST_H264_PARSER_OK;

error:
  {
    GST_WARNING ("error parsing \"User Data Unregistered\"");
    g_clear_pointer (&data, g_free);
    return GST_H264_PARSER_ERROR;
  }
}

static GstH264ParserResult
gst_h264_parser_parse_recovery_point (GstH264NalParser * nalparser,
    GstH264RecoveryPoint * rp, NalReader * nr)
{
  GstH264SPS *const sps = nalparser->last_sps;

  GST_DEBUG ("parsing \"Recovery point\"");
  if (!sps || !sps->valid) {
    GST_WARNING ("didn't get the associated sequence parameter set for the "
        "current access unit");
    goto error;
  }

  READ_UE_MAX (nr, rp->recovery_frame_cnt, sps->max_frame_num - 1);
  READ_UINT8 (nr, rp->exact_match_flag, 1);
  READ_UINT8 (nr, rp->broken_link_flag, 1);
  READ_UINT8 (nr, rp->changing_slice_group_idc, 2);

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Recovery point\"");
  return GST_H264_PARSER_ERROR;
}

/* Parse SEI stereo_video_info() message */
static GstH264ParserResult
gst_h264_parser_parse_stereo_video_info (GstH264NalParser * nalparser,
    GstH264StereoVideoInfo * info, NalReader * nr)
{
  GST_DEBUG ("parsing \"Stereo Video info\"");

  READ_UINT8 (nr, info->field_views_flag, 1);
  if (info->field_views_flag) {
    READ_UINT8 (nr, info->top_field_is_left_view_flag, 1);
  } else {
    READ_UINT8 (nr, info->current_frame_is_left_view_flag, 1);
    READ_UINT8 (nr, info->next_frame_is_second_view_flag, 1);
  }
  READ_UINT8 (nr, info->left_view_self_contained_flag, 1);
  READ_UINT8 (nr, info->right_view_self_contained_flag, 1);

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Stereo Video info\"");
  return GST_H264_PARSER_ERROR;
}

/* Parse SEI frame_packing_arrangement() message */
static GstH264ParserResult
gst_h264_parser_parse_frame_packing (GstH264NalParser * nalparser,
    GstH264FramePacking * frame_packing, NalReader * nr, guint payload_size)
{
  guint8 frame_packing_extension_flag;
  guint start_pos;

  GST_DEBUG ("parsing \"Frame Packing Arrangement\"");

  start_pos = nal_reader_get_pos (nr);
  READ_UE (nr, frame_packing->frame_packing_id);
  READ_UINT8 (nr, frame_packing->frame_packing_cancel_flag, 1);

  if (!frame_packing->frame_packing_cancel_flag) {
    READ_UINT8 (nr, frame_packing->frame_packing_type, 7);
    READ_UINT8 (nr, frame_packing->quincunx_sampling_flag, 1);
    READ_UINT8 (nr, frame_packing->content_interpretation_type, 6);
    READ_UINT8 (nr, frame_packing->spatial_flipping_flag, 1);
    READ_UINT8 (nr, frame_packing->frame0_flipped_flag, 1);
    READ_UINT8 (nr, frame_packing->field_views_flag, 1);
    READ_UINT8 (nr, frame_packing->current_frame_is_frame0_flag, 1);
    READ_UINT8 (nr, frame_packing->frame0_self_contained_flag, 1);
    READ_UINT8 (nr, frame_packing->frame1_self_contained_flag, 1);

    if (!frame_packing->quincunx_sampling_flag &&
        frame_packing->frame_packing_type !=
        GST_H264_FRAME_PACKING_TEMPORAL_INTERLEAVING) {
      READ_UINT8 (nr, frame_packing->frame0_grid_position_x, 4);
      READ_UINT8 (nr, frame_packing->frame0_grid_position_y, 4);
      READ_UINT8 (nr, frame_packing->frame1_grid_position_x, 4);
      READ_UINT8 (nr, frame_packing->frame1_grid_position_y, 4);
    }

    /* Skip frame_packing_arrangement_reserved_byte */
    if (!nal_reader_skip (nr, 8))
      goto error;

    READ_UE_MAX (nr, frame_packing->frame_packing_repetition_period, 16384);
  }

  READ_UINT8 (nr, frame_packing_extension_flag, 1);

  /* All data that follows within a frame packing arrangement SEI message
     after the value 1 for frame_packing_arrangement_extension_flag shall
     be ignored (D.2.25) */
  if (frame_packing_extension_flag) {
    nal_reader_skip_long (nr,
        payload_size - (nal_reader_get_pos (nr) - start_pos));
  }

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Frame Packing Arrangement\"");
  return GST_H264_PARSER_ERROR;
}

static GstH264ParserResult
gst_h264_parser_parse_mastering_display_colour_volume (GstH264NalParser *
    parser, GstH264MasteringDisplayColourVolume * mdcv, NalReader * nr)
{
  guint i;

  GST_DEBUG ("parsing \"Mastering display colour volume\"");

  for (i = 0; i < 3; i++) {
    READ_UINT16 (nr, mdcv->display_primaries_x[i], 16);
    READ_UINT16 (nr, mdcv->display_primaries_y[i], 16);
  }

  READ_UINT16 (nr, mdcv->white_point_x, 16);
  READ_UINT16 (nr, mdcv->white_point_y, 16);
  READ_UINT32 (nr, mdcv->max_display_mastering_luminance, 32);
  READ_UINT32 (nr, mdcv->min_display_mastering_luminance, 32);

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Mastering display colour volume\"");
  return GST_H264_PARSER_ERROR;
}

static GstH264ParserResult
gst_h264_parser_parse_content_light_level_info (GstH264NalParser * parser,
    GstH264ContentLightLevel * cll, NalReader * nr)
{
  GST_DEBUG ("parsing \"Content light level\"");

  READ_UINT16 (nr, cll->max_content_light_level, 16);
  READ_UINT16 (nr, cll->max_pic_average_light_level, 16);

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Content light level\"");
  return GST_H264_PARSER_ERROR;
}

static GstH264ParserResult
gst_h264_parser_parse_sei_unhandled_payload (GstH264NalParser * parser,
    GstH264SEIUnhandledPayload * payload, NalReader * nr, guint payload_type,
    guint payload_size)
{
  guint8 *data = NULL;
  gint i;

  payload->payloadType = payload_type;

  data = g_malloc0 (payload_size);
  for (i = 0; i < payload_size; ++i) {
    READ_UINT8 (nr, data[i], 8);
  }

  payload->size = payload_size;
  payload->data = data;

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Unhandled payload\"");
  g_free (data);

  return GST_H264_PARSER_ERROR;
}

static GstH264ParserResult
gst_h264_parser_parse_sei_message (GstH264NalParser * nalparser,
    NalReader * nr, GstH264SEIMessage * sei)
{
  guint32 payloadSize;
  guint8 payload_type_byte, payload_size_byte;
  guint remaining, payload_size, next;
  GstH264ParserResult res;

  GST_DEBUG ("parsing \"SEI message\"");

  memset (sei, 0, sizeof (*sei));

  do {
    READ_UINT8 (nr, payload_type_byte, 8);
    sei->payloadType += payload_type_byte;
  } while (payload_type_byte == 0xff);

  payloadSize = 0;
  do {
    READ_UINT8 (nr, payload_size_byte, 8);
    payloadSize += payload_size_byte;
  }
  while (payload_size_byte == 0xff);

  remaining = nal_reader_get_remaining (nr);
  payload_size = payloadSize * 8 < remaining ? payloadSize * 8 : remaining;
  next = nal_reader_get_pos (nr) + payload_size;

  GST_DEBUG ("SEI message received: payloadType  %u, payloadSize = %u bits",
      sei->payloadType, payload_size);

  switch (sei->payloadType) {
    case GST_H264_SEI_BUF_PERIOD:
      /* size not set; might depend on emulation_prevention_three_byte */
      res = gst_h264_parser_parse_buffering_period (nalparser,
          &sei->payload.buffering_period, nr);
      break;
    case GST_H264_SEI_PIC_TIMING:
      /* size not set; might depend on emulation_prevention_three_byte */
      res = gst_h264_parser_parse_pic_timing (nalparser,
          &sei->payload.pic_timing, nr);
      break;
    case GST_H264_SEI_REGISTERED_USER_DATA:
      res = gst_h264_parser_parse_registered_user_data (nalparser,
          &sei->payload.registered_user_data, nr, payload_size >> 3);
      break;
    case GST_H264_SEI_USER_DATA_UNREGISTERED:
      res = gst_h264_parser_parse_user_data_unregistered (nalparser,
          &sei->payload.user_data_unregistered, nr, payload_size >> 3);
      break;
    case GST_H264_SEI_RECOVERY_POINT:
      res = gst_h264_parser_parse_recovery_point (nalparser,
          &sei->payload.recovery_point, nr);
      break;
    case GST_H264_SEI_STEREO_VIDEO_INFO:
      res = gst_h264_parser_parse_stereo_video_info (nalparser,
          &sei->payload.stereo_video_info, nr);
      break;
    case GST_H264_SEI_FRAME_PACKING:
      res = gst_h264_parser_parse_frame_packing (nalparser,
          &sei->payload.frame_packing, nr, payload_size);
      break;
    case GST_H264_SEI_MASTERING_DISPLAY_COLOUR_VOLUME:
      res = gst_h264_parser_parse_mastering_display_colour_volume (nalparser,
          &sei->payload.mastering_display_colour_volume, nr);
      break;
    case GST_H264_SEI_CONTENT_LIGHT_LEVEL:
      res = gst_h264_parser_parse_content_light_level_info (nalparser,
          &sei->payload.content_light_level, nr);
      break;
    default:
      res = gst_h264_parser_parse_sei_unhandled_payload (nalparser,
          &sei->payload.unhandled_payload, nr, sei->payloadType,
          payload_size >> 3);
      sei->payloadType = GST_H264_SEI_UNHANDLED_PAYLOAD;
      break;
  }

  /* When SEI message doesn't end at byte boundary,
   * check remaining bits fit the specification.
   */
  if (!nal_reader_is_byte_aligned (nr)) {
    guint8 bit_equal_to_one;
    READ_UINT8 (nr, bit_equal_to_one, 1);
    if (!bit_equal_to_one)
      GST_WARNING ("Bit non equal to one.");

    while (!nal_reader_is_byte_aligned (nr)) {
      guint8 bit_equal_to_zero;
      READ_UINT8 (nr, bit_equal_to_zero, 1);
      if (bit_equal_to_zero)
        GST_WARNING ("Bit non equal to zero.");
    }
  }

  /* Always make sure all the advertised SEI bits
   * were consumed during parsing */
  if (next > nal_reader_get_pos (nr)) {
    GST_LOG ("Skipping %u unused SEI bits", next - nal_reader_get_pos (nr));

    if (!nal_reader_skip_long (nr, next - nal_reader_get_pos (nr)))
      goto error;
  }

  return res;

error:
  GST_WARNING ("error parsing \"Sei message\"");
  gst_h264_sei_clear (sei);
  return GST_H264_PARSER_ERROR;
}

/******** API *************/

/**
 * gst_h264_nal_parser_new:
 *
 * Creates a new #GstH264NalParser. It should be freed with
 * gst_h264_nal_parser_free after use.
 *
 * Returns: a new #GstH264NalParser
 */
GstH264NalParser *
gst_h264_nal_parser_new (void)
{
  GstH264NalParser *nalparser;

  nalparser = g_new0 (GstH264NalParser, 1);

  return nalparser;
}

/**
 * gst_h264_nal_parser_free:
 * @nalparser: the #GstH264NalParser to free
 *
 * Frees @nalparser
 */
void
gst_h264_nal_parser_free (GstH264NalParser * nalparser)
{
  guint i;

  for (i = 0; i < GST_H264_MAX_SPS_COUNT; i++)
    gst_h264_sps_clear (&nalparser->sps[i]);
  for (i = 0; i < GST_H264_MAX_PPS_COUNT; i++)
    gst_h264_pps_clear (&nalparser->pps[i]);
  g_free (nalparser);
}

/**
 * gst_h264_parser_identify_nalu_unchecked:
 * @nalparser: a #GstH264NalParser
 * @data: The data to parse
 * @offset: the offset from which to parse @data
 * @size: the size of @data
 * @nalu: The #GstH264NalUnit where to store parsed nal headers
 *
 * Parses @data and fills @nalu from the next nalu data from @data.
 *
 * This differs from @gst_h264_parser_identify_nalu in that it doesn't
 * check whether the packet is complete or not.
 *
 * Note: Only use this function if you already know the provided @data
 * is a complete NALU, else use @gst_h264_parser_identify_nalu.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parser_identify_nalu_unchecked (GstH264NalParser * nalparser,
    const guint8 * data, guint offset, gsize size, GstH264NalUnit * nalu)
{
  gint off1;

  memset (nalu, 0, sizeof (*nalu));

  if (size < offset + 4) {
    GST_DEBUG ("Can't parse, buffer has too small size %" G_GSIZE_FORMAT
        ", offset %u", size, offset);
    return GST_H264_PARSER_ERROR;
  }

  off1 = scan_for_start_codes (data + offset, size - offset);

  if (off1 < 0) {
    GST_DEBUG ("No start code prefix in this buffer");
    return GST_H264_PARSER_NO_NAL;
  }

  nalu->sc_offset = offset + off1;

  /* sc might have 2 or 3 0-bytes */
  if (nalu->sc_offset > 0 && data[nalu->sc_offset - 1] == 00)
    nalu->sc_offset--;

  nalu->offset = offset + off1 + 3;
  nalu->data = (guint8 *) data;
  nalu->size = size - nalu->offset;

  if (!gst_h264_parse_nalu_header (nalu)) {
    GST_DEBUG ("not enough data to parse \"NAL unit header\"");
    nalu->size = 0;
    return GST_H264_PARSER_NO_NAL;
  }

  nalu->valid = TRUE;

  if (nalu->type == GST_H264_NAL_SEQ_END ||
      nalu->type == GST_H264_NAL_STREAM_END) {
    GST_DEBUG ("end-of-seq or end-of-stream nal found");
    nalu->size = 1;
    return GST_H264_PARSER_OK;
  }

  return GST_H264_PARSER_OK;
}

/**
 * gst_h264_parser_identify_nalu:
 * @nalparser: a #GstH264NalParser
 * @data: The data to parse, containing an Annex B coded NAL unit
 * @offset: the offset in @data from which to parse the NAL unit
 * @size: the size of @data
 * @nalu: The #GstH264NalUnit to store the identified NAL unit in
 *
 * Parses the headers of an Annex B coded NAL unit from @data and puts the
 * result into @nalu.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parser_identify_nalu (GstH264NalParser * nalparser,
    const guint8 * data, guint offset, gsize size, GstH264NalUnit * nalu)
{
  GstH264ParserResult res;
  gint off2;

  res =
      gst_h264_parser_identify_nalu_unchecked (nalparser, data, offset, size,
      nalu);

  if (res != GST_H264_PARSER_OK)
    goto beach;

  /* The two NALs are exactly 1 byte size and are placed at the end of an AU,
   * there is no need to wait for the following */
  if (nalu->type == GST_H264_NAL_SEQ_END ||
      nalu->type == GST_H264_NAL_STREAM_END)
    goto beach;

  off2 = scan_for_start_codes (data + nalu->offset, size - nalu->offset);
  if (off2 < 0) {
    GST_DEBUG ("Nal start %d, No end found", nalu->offset);

    return GST_H264_PARSER_NO_NAL_END;
  }

  /* Mini performance improvement:
   * We could have a way to store how many 0s were skipped to avoid
   * parsing them again on the next NAL */
  while (off2 > 0 && data[nalu->offset + off2 - 1] == 00)
    off2--;

  nalu->size = off2;
  if (nalu->size < 2)
    return GST_H264_PARSER_BROKEN_DATA;

  GST_DEBUG ("Complete nal found. Off: %d, Size: %d", nalu->offset, nalu->size);

beach:
  return res;
}


/**
 * gst_h264_parser_identify_nalu_avc:
 * @nalparser: a #GstH264NalParser
 * @data: The data to parse, containing an AVC coded NAL unit
 * @offset: the offset in @data from which to parse the NAL unit
 * @size: the size of @data
 * @nal_length_size: the size in bytes of the AVC nal length prefix.
 * @nalu: The #GstH264NalUnit to store the identified NAL unit in
 *
 * Parses the headers of an AVC coded NAL unit from @data and puts the result
 * into @nalu.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parser_identify_nalu_avc (GstH264NalParser * nalparser,
    const guint8 * data, guint offset, gsize size, guint8 nal_length_size,
    GstH264NalUnit * nalu)
{
  GstBitReader br;

  memset (nalu, 0, sizeof (*nalu));

  /* Would overflow guint below otherwise: the callers needs to ensure that
   * this never happens */
  if (offset > G_MAXUINT32 - nal_length_size) {
    GST_WARNING ("offset + nal_length_size overflow");
    nalu->size = 0;
    return GST_H264_PARSER_BROKEN_DATA;
  }

  if (size < offset + nal_length_size) {
    GST_DEBUG ("Can't parse, buffer has too small size %" G_GSIZE_FORMAT
        ", offset %u", size, offset);
    return GST_H264_PARSER_ERROR;
  }

  size = size - offset;
  gst_bit_reader_init (&br, data + offset, size);

  nalu->size = gst_bit_reader_get_bits_uint32_unchecked (&br,
      nal_length_size * 8);
  nalu->sc_offset = offset;
  nalu->offset = offset + nal_length_size;

  if (nalu->size > G_MAXUINT32 - nal_length_size) {
    GST_WARNING ("NALU size + nal_length_size overflow");
    nalu->size = 0;
    return GST_H264_PARSER_BROKEN_DATA;
  }

  if (size < (gsize) nalu->size + nal_length_size) {
    nalu->size = 0;

    return GST_H264_PARSER_NO_NAL_END;
  }

  nalu->data = (guint8 *) data;

  if (!gst_h264_parse_nalu_header (nalu)) {
    GST_WARNING ("error parsing \"NAL unit header\"");
    nalu->size = 0;
    return GST_H264_PARSER_BROKEN_DATA;
  }

  nalu->valid = TRUE;

  return GST_H264_PARSER_OK;
}

/**
 * gst_h264_parser_identify_and_split_nalu_avc:
 * @nalparser: a #GstH264NalParser
 * @data: The data to parse, containing an AVC coded NAL unit
 * @offset: the offset in @data from which to parse the NAL unit
 * @size: the size of @data
 * @nal_length_size: the size in bytes of the AVC nal length prefix.
 * @nalus: a caller allocated GArray of #GstH264NalUnit where to store parsed nal headers
 * @consumed: (out): the size of consumed bytes
 *
 * Parses @data for packetized (e.g., avc/avc3) bitstream and
 * sets @nalus. In addition to nal identifying process,
 * this method scans start-code prefix to split malformed packet into
 * actual nal chunks.
 *
 * Returns: a #GstH264ParserResult
 *
 * Since: 1.22.9
 */
GstH264ParserResult
gst_h264_parser_identify_and_split_nalu_avc (GstH264NalParser * nalparser,
    const guint8 * data, guint offset, gsize size, guint8 nal_length_size,
    GArray * nalus, gsize * consumed)
{
  GstBitReader br;
  guint nalu_size;
  guint remaining;
  guint off;
  guint sc_size;

  g_return_val_if_fail (data != NULL, GST_H264_PARSER_ERROR);
  g_return_val_if_fail (nalus != NULL, GST_H264_PARSER_ERROR);
  g_return_val_if_fail (nal_length_size > 0 && nal_length_size < 5,
      GST_H264_PARSER_ERROR);

  g_array_set_size (nalus, 0);

  if (consumed)
    *consumed = 0;

  /* Would overflow guint below otherwise: the callers needs to ensure that
   * this never happens */
  if (offset > G_MAXUINT32 - nal_length_size) {
    GST_WARNING ("offset + nal_length_size overflow");
    return GST_H264_PARSER_BROKEN_DATA;
  }

  if (size < offset + nal_length_size) {
    GST_DEBUG ("Can't parse, buffer has too small size %" G_GSIZE_FORMAT
        ", offset %u", size, offset);
    return GST_H264_PARSER_ERROR;
  }

  /* Read nal unit size and unwrap the size field */
  gst_bit_reader_init (&br, data + offset, size - offset);
  nalu_size = gst_bit_reader_get_bits_uint32_unchecked (&br,
      nal_length_size * 8);

  if (nalu_size < 1) {
    GST_WARNING ("too small nal size %d", nalu_size);
    return GST_H264_PARSER_BROKEN_DATA;
  }

  if (size < (gsize) nalu_size + nal_length_size) {
    GST_WARNING ("larger nalu size %d than data size %" G_GSIZE_FORMAT,
        nalu_size + nal_length_size, size);
    return GST_H264_PARSER_BROKEN_DATA;
  }

  if (consumed)
    *consumed = nalu_size + nal_length_size;

  off = offset + nal_length_size;
  remaining = nalu_size;
  sc_size = nal_length_size;

  /* Drop trailing start-code since it will not be scanned */
  if (remaining >= 3) {
    if (data[off + remaining - 1] == 0x01 && data[off + remaining - 2] == 0x00
        && data[off + remaining - 3] == 0x00) {
      remaining -= 3;

      /* 4 bytes start-code */
      if (remaining > 0 && data[off + remaining - 1] == 0x00)
        remaining--;
    }
  }

  /* Looping to split malformed nal units. nal-length field was dropped above
   * so expected bitstream structure are:
   *
   * <complete nalu>
   * | nalu |
   * sc scan result will be -1 and handled in CONDITION-A
   *
   * <nalu with startcode prefix>
   * | SC | nalu |
   * Hit CONDITION-C first then terminated in CONDITION-A
   *
   * <first nal has no startcode but others have>
   * | nalu | SC | nalu | ...
   * CONDITION-B handles those cases
   */
  do {
    GstH264NalUnit nalu;
    gint sc_offset = -1;
    guint skip_size = 0;

    memset (&nalu, 0, sizeof (GstH264NalUnit));

    /* startcode 3 bytes + minimum nal size 1 */
    if (remaining >= 4)
      sc_offset = scan_for_start_codes (data + off, remaining);

    if (sc_offset < 0) {
      if (remaining >= 1) {
        /* CONDITION-A */
        /* Last chunk */
        nalu.size = remaining;
        nalu.sc_offset = off - sc_size;
        nalu.offset = off;
        nalu.data = (guint8 *) data;
        nalu.valid = TRUE;

        gst_h264_parse_nalu_header (&nalu);
        g_array_append_val (nalus, nalu);
      }
      break;
    } else if ((sc_offset == 2 && data[off + sc_offset - 1] != 0)
        || sc_offset > 2) {
      /* CONDITION-B */
      /* Found trailing startcode prefix */

      nalu.size = sc_offset;
      if (data[off + sc_offset - 1] == 0) {
        /* 4 bytes start code */
        nalu.size--;
      }

      nalu.sc_offset = off - sc_size;
      nalu.offset = off;
      nalu.data = (guint8 *) data;
      nalu.valid = TRUE;

      gst_h264_parse_nalu_header (&nalu);
      g_array_append_val (nalus, nalu);
    } else {
      /* CONDITION-C */
      /* startcode located at beginning of this chunk without actual nal data.
       * skip this start code */
    }

    skip_size = sc_offset + 3;
    if (skip_size >= remaining)
      break;

    /* no more nal-length bytes but 3bytes startcode */
    sc_size = 3;
    if (sc_offset > 0 && data[off + sc_offset - 1] == 0)
      sc_size++;

    remaining -= skip_size;
    off += skip_size;
  } while (remaining >= 1);

  if (nalus->len > 0)
    return GST_H264_PARSER_OK;

  GST_WARNING ("No nal found");

  return GST_H264_PARSER_BROKEN_DATA;
}

/**
 * gst_h264_parser_parse_nal:
 * @nalparser: a #GstH264NalParser
 * @nalu: The #GstH264NalUnit to parse
 *
 * This function should be called in the case one doesn't need to
 * parse a specific structure. It is necessary to do so to make
 * sure @nalparser is up to date.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parser_parse_nal (GstH264NalParser * nalparser, GstH264NalUnit * nalu)
{
  GstH264ParserResult res = GST_H264_PARSER_OK;

  switch (nalu->type) {
    case GST_H264_NAL_SPS:{
      GstH264SPS sps;

      res = gst_h264_parser_parse_sps (nalparser, nalu, &sps);
      gst_h264_sps_clear (&sps);
      break;
    }
    case GST_H264_NAL_PPS:{
      GstH264PPS pps;

      res = gst_h264_parser_parse_pps (nalparser, nalu, &pps);
      gst_h264_pps_clear (&pps);
      break;
    }
  }

  return res;
}

/**
 * gst_h264_parser_parse_sps:
 * @nalparser: a #GstH264NalParser
 * @nalu: The #GST_H264_NAL_SPS #GstH264NalUnit to parse
 * @sps: The #GstH264SPS to fill.
 *
 * Parses @nalu containing a Sequence Parameter Set, and fills @sps.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parser_parse_sps (GstH264NalParser * nalparser, GstH264NalUnit * nalu,
    GstH264SPS * sps)
{
  GstH264ParserResult res = gst_h264_parse_sps (nalu, sps);

  if (res == GST_H264_PARSER_OK) {
    if (nalparser->temporal_sps_fixup && sps->num_ref_frames == 3)
      sps->num_ref_frames = 5;

    GST_DEBUG ("adding sequence parameter set with id: %d to array", sps->id);

    if (!gst_h264_sps_copy (&nalparser->sps[sps->id], sps))
      return GST_H264_PARSER_ERROR;
    nalparser->last_sps = &nalparser->sps[sps->id];
  }
  return res;
}

/* Parse seq_parameter_set_data() */
static gboolean
gst_h264_parse_sps_data (NalReader * nr, GstH264SPS * sps)
{
  gint width, height;
  guint subwc[] = { 1, 2, 2, 1 };
  guint subhc[] = { 1, 2, 1, 1 };

  memset (sps, 0, sizeof (*sps));

  /* set default values for fields that might not be present in the bitstream
     and have valid defaults */
  sps->extension_type = GST_H264_NAL_EXTENSION_NONE;
  sps->chroma_format_idc = 1;
  memset (sps->scaling_lists_4x4, 16, 96);
  memset (sps->scaling_lists_8x8, 16, 384);

  READ_UINT8 (nr, sps->profile_idc, 8);
  READ_UINT8 (nr, sps->constraint_set0_flag, 1);
  READ_UINT8 (nr, sps->constraint_set1_flag, 1);
  READ_UINT8 (nr, sps->constraint_set2_flag, 1);
  READ_UINT8 (nr, sps->constraint_set3_flag, 1);
  READ_UINT8 (nr, sps->constraint_set4_flag, 1);
  READ_UINT8 (nr, sps->constraint_set5_flag, 1);

  /* skip reserved_zero_2bits */
  if (!nal_reader_skip (nr, 2))
    goto error;

  READ_UINT8 (nr, sps->level_idc, 8);

  READ_UE_MAX (nr, sps->id, GST_H264_MAX_SPS_COUNT - 1);

  if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
      sps->profile_idc == 122 || sps->profile_idc == 244 ||
      sps->profile_idc == 44 || sps->profile_idc == 83 ||
      sps->profile_idc == 86 || sps->profile_idc == 118 ||
      sps->profile_idc == 128 || sps->profile_idc == 138 ||
      sps->profile_idc == 139 || sps->profile_idc == 134 ||
      sps->profile_idc == 135) {
    READ_UE_MAX (nr, sps->chroma_format_idc, 3);
    if (sps->chroma_format_idc == 3)
      READ_UINT8 (nr, sps->separate_colour_plane_flag, 1);

    READ_UE_MAX (nr, sps->bit_depth_luma_minus8, 6);
    READ_UE_MAX (nr, sps->bit_depth_chroma_minus8, 6);
    READ_UINT8 (nr, sps->qpprime_y_zero_transform_bypass_flag, 1);

    READ_UINT8 (nr, sps->scaling_matrix_present_flag, 1);
    if (sps->scaling_matrix_present_flag) {
      guint8 n_lists;

      n_lists = (sps->chroma_format_idc != 3) ? 8 : 12;
      if (!gst_h264_parser_parse_scaling_list (nr,
              sps->scaling_lists_4x4, sps->scaling_lists_8x8,
              default_4x4_inter, default_4x4_intra,
              default_8x8_inter, default_8x8_intra, n_lists))
        goto error;
    }
  }

  READ_UE_MAX (nr, sps->log2_max_frame_num_minus4, 12);

  sps->max_frame_num = 1 << (sps->log2_max_frame_num_minus4 + 4);

  READ_UE_MAX (nr, sps->pic_order_cnt_type, 2);
  if (sps->pic_order_cnt_type == 0) {
    READ_UE_MAX (nr, sps->log2_max_pic_order_cnt_lsb_minus4, 12);
  } else if (sps->pic_order_cnt_type == 1) {
    guint i;

    READ_UINT8 (nr, sps->delta_pic_order_always_zero_flag, 1);
    READ_SE (nr, sps->offset_for_non_ref_pic);
    READ_SE (nr, sps->offset_for_top_to_bottom_field);
    READ_UE_MAX (nr, sps->num_ref_frames_in_pic_order_cnt_cycle, 255);

    for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
      READ_SE (nr, sps->offset_for_ref_frame[i]);
  }

  READ_UE (nr, sps->num_ref_frames);
  READ_UINT8 (nr, sps->gaps_in_frame_num_value_allowed_flag, 1);
  READ_UE (nr, sps->pic_width_in_mbs_minus1);
  READ_UE (nr, sps->pic_height_in_map_units_minus1);
  READ_UINT8 (nr, sps->frame_mbs_only_flag, 1);

  if (!sps->frame_mbs_only_flag)
    READ_UINT8 (nr, sps->mb_adaptive_frame_field_flag, 1);

  READ_UINT8 (nr, sps->direct_8x8_inference_flag, 1);
  READ_UINT8 (nr, sps->frame_cropping_flag, 1);
  if (sps->frame_cropping_flag) {
    READ_UE (nr, sps->frame_crop_left_offset);
    READ_UE (nr, sps->frame_crop_right_offset);
    READ_UE (nr, sps->frame_crop_top_offset);
    READ_UE (nr, sps->frame_crop_bottom_offset);
  }

  READ_UINT8 (nr, sps->vui_parameters_present_flag, 1);
  if (sps->vui_parameters_present_flag)
    if (!gst_h264_parse_vui_parameters (sps, nr))
      goto error;

  /* calculate ChromaArrayType */
  if (!sps->separate_colour_plane_flag)
    sps->chroma_array_type = sps->chroma_format_idc;

  /* Calculate  width and height */
  width = (sps->pic_width_in_mbs_minus1 + 1);
  width *= 16;
  height = (sps->pic_height_in_map_units_minus1 + 1);
  height *= 16 * (2 - sps->frame_mbs_only_flag);
  GST_LOG ("initial width=%d, height=%d", width, height);
  if (width < 0 || height < 0) {
    GST_WARNING ("invalid width/height in SPS");
    goto error;
  }

  sps->width = width;
  sps->height = height;

  if (sps->frame_cropping_flag) {
    const guint crop_unit_x = subwc[sps->chroma_format_idc];
    const guint crop_unit_y =
        subhc[sps->chroma_format_idc] * (2 - sps->frame_mbs_only_flag);

    width -= (sps->frame_crop_left_offset + sps->frame_crop_right_offset)
        * crop_unit_x;
    height -= (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset)
        * crop_unit_y;

    sps->crop_rect_width = width;
    sps->crop_rect_height = height;
    sps->crop_rect_x = sps->frame_crop_left_offset * crop_unit_x;
    sps->crop_rect_y = sps->frame_crop_top_offset * crop_unit_y;

    GST_LOG ("crop_rectangle x=%u y=%u width=%u, height=%u", sps->crop_rect_x,
        sps->crop_rect_y, width, height);
  }

  sps->fps_num_removed = 0;
  sps->fps_den_removed = 1;

  return TRUE;

error:
  return FALSE;
}

/* Parse subset_seq_parameter_set() data for MVC */
static gboolean
gst_h264_parse_sps_mvc_data (NalReader * nr, GstH264SPS * sps)
{
  GstH264SPSExtMVC *const mvc = &sps->extension.mvc;
  guint8 bit_equal_to_one;
  guint i, j, k;

  READ_UINT8 (nr, bit_equal_to_one, 1);
  if (!bit_equal_to_one)
    return FALSE;

  sps->extension_type = GST_H264_NAL_EXTENSION_MVC;

  READ_UE_MAX (nr, mvc->num_views_minus1, GST_H264_MAX_VIEW_COUNT - 1);

  mvc->view = g_new0 (GstH264SPSExtMVCView, mvc->num_views_minus1 + 1);
  if (!mvc->view)
    goto error_allocation_failed;

  for (i = 0; i <= mvc->num_views_minus1; i++)
    READ_UE_MAX (nr, mvc->view[i].view_id, GST_H264_MAX_VIEW_ID);

  for (i = 1; i <= mvc->num_views_minus1; i++) {
    /* for RefPicList0 */
    READ_UE_MAX (nr, mvc->view[i].num_anchor_refs_l0, 15);
    for (j = 0; j < mvc->view[i].num_anchor_refs_l0; j++) {
      READ_UE_MAX (nr, mvc->view[i].anchor_ref_l0[j], GST_H264_MAX_VIEW_ID);
    }

    /* for RefPicList1 */
    READ_UE_MAX (nr, mvc->view[i].num_anchor_refs_l1, 15);
    for (j = 0; j < mvc->view[i].num_anchor_refs_l1; j++) {
      READ_UE_MAX (nr, mvc->view[i].anchor_ref_l1[j], GST_H264_MAX_VIEW_ID);
    }
  }

  for (i = 1; i <= mvc->num_views_minus1; i++) {
    /* for RefPicList0 */
    READ_UE_MAX (nr, mvc->view[i].num_non_anchor_refs_l0, 15);
    for (j = 0; j < mvc->view[i].num_non_anchor_refs_l0; j++) {
      READ_UE_MAX (nr, mvc->view[i].non_anchor_ref_l0[j], GST_H264_MAX_VIEW_ID);
    }

    /* for RefPicList1 */
    READ_UE_MAX (nr, mvc->view[i].num_non_anchor_refs_l1, 15);
    for (j = 0; j < mvc->view[i].num_non_anchor_refs_l1; j++) {
      READ_UE_MAX (nr, mvc->view[i].non_anchor_ref_l1[j], GST_H264_MAX_VIEW_ID);
    }
  }

  READ_UE_MAX (nr, mvc->num_level_values_signalled_minus1, 63);

  mvc->level_value =
      g_new0 (GstH264SPSExtMVCLevelValue,
      mvc->num_level_values_signalled_minus1 + 1);
  if (!mvc->level_value)
    goto error_allocation_failed;

  for (i = 0; i <= mvc->num_level_values_signalled_minus1; i++) {
    GstH264SPSExtMVCLevelValue *const level_value = &mvc->level_value[i];

    READ_UINT8 (nr, level_value->level_idc, 8);

    READ_UE_MAX (nr, level_value->num_applicable_ops_minus1, 1023);
    level_value->applicable_op =
        g_new0 (GstH264SPSExtMVCLevelValueOp,
        level_value->num_applicable_ops_minus1 + 1);
    if (!level_value->applicable_op)
      goto error_allocation_failed;

    for (j = 0; j <= level_value->num_applicable_ops_minus1; j++) {
      GstH264SPSExtMVCLevelValueOp *const op = &level_value->applicable_op[j];

      READ_UINT8 (nr, op->temporal_id, 3);

      READ_UE_MAX (nr, op->num_target_views_minus1, 1023);
      op->target_view_id = g_new (guint16, op->num_target_views_minus1 + 1);
      if (!op->target_view_id)
        goto error_allocation_failed;

      for (k = 0; k <= op->num_target_views_minus1; k++)
        READ_UE_MAX (nr, op->target_view_id[k], GST_H264_MAX_VIEW_ID);
      READ_UE_MAX (nr, op->num_views_minus1, 1023);
    }
  }
  return TRUE;

error_allocation_failed:
  GST_WARNING ("failed to allocate memory");
  gst_h264_sps_clear (sps);
  return FALSE;

error:
  gst_h264_sps_clear (sps);
  return FALSE;
}

/**
 * gst_h264_parse_sps:
 * @nalu: The #GST_H264_NAL_SPS #GstH264NalUnit to parse
 * @sps: The #GstH264SPS to fill.
 *
 * Parses @data, and fills the @sps structure.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parse_sps (GstH264NalUnit * nalu, GstH264SPS * sps)
{
  NalReader nr;

  GST_DEBUG ("parsing SPS");

  nal_reader_init (&nr, nalu->data + nalu->offset + nalu->header_bytes,
      nalu->size - nalu->header_bytes);

  if (!gst_h264_parse_sps_data (&nr, sps))
    goto error;

  sps->valid = TRUE;

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Sequence parameter set\"");
  sps->valid = FALSE;
  return GST_H264_PARSER_ERROR;
}

/**
 * gst_h264_parser_parse_subset_sps:
 * @nalparser: a #GstH264NalParser
 * @nalu: The #GST_H264_NAL_SUBSET_SPS #GstH264NalUnit to parse
 * @sps: The #GstH264SPS to fill.
 *
 * Parses @data, and fills in the @sps structure.
 *
 * This function fully parses @data and allocates all the necessary
 * data structures needed for MVC extensions. The resulting @sps
 * structure shall be deallocated with gst_h264_sps_clear() when it is
 * no longer needed.
 *
 * Note: if the caller doesn't need any of the MVC-specific data, then
 * gst_h264_parser_parse_sps() is more efficient because those extra
 * syntax elements are not parsed and no extra memory is allocated.
 *
 * Returns: a #GstH264ParserResult
 *
 * Since: 1.6
 */
GstH264ParserResult
gst_h264_parser_parse_subset_sps (GstH264NalParser * nalparser,
    GstH264NalUnit * nalu, GstH264SPS * sps)
{
  GstH264ParserResult res;

  res = gst_h264_parse_subset_sps (nalu, sps);
  if (res == GST_H264_PARSER_OK) {
    if (nalparser->temporal_sps_fixup && sps->num_ref_frames == 3)
      sps->num_ref_frames = 5;

    GST_DEBUG ("adding sequence parameter set with id: %d to array", sps->id);

    if (!gst_h264_sps_copy (&nalparser->sps[sps->id], sps)) {
      gst_h264_sps_clear (sps);
      return GST_H264_PARSER_ERROR;
    }
    nalparser->last_sps = &nalparser->sps[sps->id];
  }
  return res;
}

/**
 * gst_h264_parse_subset_sps:
 * @nalu: The #GST_H264_NAL_SUBSET_SPS #GstH264NalUnit to parse
 * @sps: The #GstH264SPS to fill.
 *
 * Parses @data, and fills in the @sps structure.
 *
 * This function fully parses @data and allocates all the necessary
 * data structures needed for MVC extensions. The resulting @sps
 * structure shall be deallocated with gst_h264_sps_clear() when it is
 * no longer needed.
 *
 * Note: if the caller doesn't need any of the MVC-specific data, then
 * gst_h264_parser_parse_sps() is more efficient because those extra
 * syntax elements are not parsed and no extra memory is allocated.
 *
 * Returns: a #GstH264ParserResult
 *
 * Since: 1.6
 */
GstH264ParserResult
gst_h264_parse_subset_sps (GstH264NalUnit * nalu, GstH264SPS * sps)
{
  NalReader nr;

  GST_DEBUG ("parsing Subset SPS");

  nal_reader_init (&nr, nalu->data + nalu->offset + nalu->header_bytes,
      nalu->size - nalu->header_bytes);

  if (!gst_h264_parse_sps_data (&nr, sps))
    goto error;

  if (sps->profile_idc == GST_H264_PROFILE_MULTIVIEW_HIGH ||
      sps->profile_idc == GST_H264_PROFILE_STEREO_HIGH) {
    if (!gst_h264_parse_sps_mvc_data (&nr, sps))
      goto error;
  }

  sps->valid = TRUE;
  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Subset sequence parameter set\"");
  gst_h264_sps_clear (sps);
  sps->valid = FALSE;
  return GST_H264_PARSER_ERROR;
}

/**
 * gst_h264_parse_pps:
 * @nalparser: a #GstH264NalParser
 * @nalu: The %GST_H264_NAL_PPS #GstH264NalUnit to parse
 * @pps: The #GstH264PPS to fill.
 *
 * Parses @data, and fills the @pps structure.
 *
 * The resulting @pps data structure shall be deallocated with the
 * gst_h264_pps_clear() function when it is no longer needed, or prior
 * to parsing a new PPS NAL unit.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parse_pps (GstH264NalParser * nalparser, GstH264NalUnit * nalu,
    GstH264PPS * pps)
{
  NalReader nr;
  GstH264SPS *sps;
  gint sps_id;
  gint qp_bd_offset;

  GST_DEBUG ("parsing PPS");

  nal_reader_init (&nr, nalu->data + nalu->offset + nalu->header_bytes,
      nalu->size - nalu->header_bytes);

  memset (pps, 0, sizeof (*pps));

  READ_UE_MAX (&nr, pps->id, GST_H264_MAX_PPS_COUNT - 1);
  READ_UE_MAX (&nr, sps_id, GST_H264_MAX_SPS_COUNT - 1);

  sps = gst_h264_parser_get_sps (nalparser, sps_id);
  if (!sps) {
    GST_WARNING ("couldn't find associated sequence parameter set with id: %d",
        sps_id);
    return GST_H264_PARSER_BROKEN_LINK;
  }
  pps->sequence = sps;
  qp_bd_offset = 6 * (sps->bit_depth_luma_minus8 +
      sps->separate_colour_plane_flag);

  /* set default values for fields that might not be present in the bitstream
     and have valid defaults */
  memcpy (&pps->scaling_lists_4x4, &sps->scaling_lists_4x4, 96);
  memcpy (&pps->scaling_lists_8x8, &sps->scaling_lists_8x8, 384);

  READ_UINT8 (&nr, pps->entropy_coding_mode_flag, 1);
  READ_UINT8 (&nr, pps->pic_order_present_flag, 1);
  READ_UE_MAX (&nr, pps->num_slice_groups_minus1, 7);
  if (pps->num_slice_groups_minus1 > 0) {
    READ_UE_MAX (&nr, pps->slice_group_map_type, 6);

    if (pps->slice_group_map_type == 0) {
      gint i;

      for (i = 0; i <= pps->num_slice_groups_minus1; i++)
        READ_UE (&nr, pps->run_length_minus1[i]);
    } else if (pps->slice_group_map_type == 2) {
      gint i;

      for (i = 0; i < pps->num_slice_groups_minus1; i++) {
        READ_UE (&nr, pps->top_left[i]);
        READ_UE (&nr, pps->bottom_right[i]);
      }
    } else if (pps->slice_group_map_type >= 3 && pps->slice_group_map_type <= 5) {
      READ_UINT8 (&nr, pps->slice_group_change_direction_flag, 1);
      READ_UE (&nr, pps->slice_group_change_rate_minus1);
    } else if (pps->slice_group_map_type == 6) {
      gint bits;
      gint i;

      READ_UE (&nr, pps->pic_size_in_map_units_minus1);
      /* 7.4.2.2 7-23 slice_group_id */
      bits = gst_util_ceil_log2 (pps->num_slice_groups_minus1 + 1);

      pps->slice_group_id =
          g_new (guint8, pps->pic_size_in_map_units_minus1 + 1);
      for (i = 0; i <= pps->pic_size_in_map_units_minus1; i++)
        READ_UINT8 (&nr, pps->slice_group_id[i], bits);
    }
  }

  READ_UE_MAX (&nr, pps->num_ref_idx_l0_active_minus1, 31);
  READ_UE_MAX (&nr, pps->num_ref_idx_l1_active_minus1, 31);
  READ_UINT8 (&nr, pps->weighted_pred_flag, 1);
  READ_UINT8 (&nr, pps->weighted_bipred_idc, 2);
  READ_SE_ALLOWED (&nr, pps->pic_init_qp_minus26, -(26 + qp_bd_offset), 25);
  READ_SE_ALLOWED (&nr, pps->pic_init_qs_minus26, -26, 25);
  READ_SE_ALLOWED (&nr, pps->chroma_qp_index_offset, -12, 12);
  pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
  READ_UINT8 (&nr, pps->deblocking_filter_control_present_flag, 1);
  READ_UINT8 (&nr, pps->constrained_intra_pred_flag, 1);
  READ_UINT8 (&nr, pps->redundant_pic_cnt_present_flag, 1);

  if (!nal_reader_has_more_data (&nr))
    goto done;

  READ_UINT8 (&nr, pps->transform_8x8_mode_flag, 1);

  READ_UINT8 (&nr, pps->pic_scaling_matrix_present_flag, 1);
  if (pps->pic_scaling_matrix_present_flag) {
    guint8 n_lists;

    n_lists = 6 + ((sps->chroma_format_idc != 3) ? 2 : 6) *
        pps->transform_8x8_mode_flag;

    if (sps->scaling_matrix_present_flag) {
      if (!gst_h264_parser_parse_scaling_list (&nr,
              pps->scaling_lists_4x4, pps->scaling_lists_8x8,
              sps->scaling_lists_4x4[3], sps->scaling_lists_4x4[0],
              sps->scaling_lists_8x8[3], sps->scaling_lists_8x8[0], n_lists))
        goto error;
    } else {
      if (!gst_h264_parser_parse_scaling_list (&nr,
              pps->scaling_lists_4x4, pps->scaling_lists_8x8,
              default_4x4_inter, default_4x4_intra,
              default_8x8_inter, default_8x8_intra, n_lists))
        goto error;
    }
  }

  READ_SE_ALLOWED (&nr, pps->second_chroma_qp_index_offset, -12, 12);

done:
  pps->valid = TRUE;
  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Picture parameter set\"");
  pps->valid = FALSE;
  gst_h264_pps_clear (pps);
  return GST_H264_PARSER_ERROR;
}

/**
 * gst_h264_parser_parse_pps:
 * @nalparser: a #GstH264NalParser
 * @nalu: The %GST_H264_NAL_PPS #GstH264NalUnit to parse
 * @pps: The #GstH264PPS to fill.
 *
 * Parses @nalu containing a Picture Parameter Set, and fills @pps.
 *
 * The resulting @pps data structure must be deallocated by the caller using
 * gst_h264_pps_clear().
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parser_parse_pps (GstH264NalParser * nalparser,
    GstH264NalUnit * nalu, GstH264PPS * pps)
{
  GstH264ParserResult res = gst_h264_parse_pps (nalparser, nalu, pps);

  if (res == GST_H264_PARSER_OK) {
    GST_DEBUG ("adding picture parameter set with id: %d to array", pps->id);

    if (!gst_h264_pps_copy (&nalparser->pps[pps->id], pps))
      return GST_H264_PARSER_ERROR;
    nalparser->last_pps = &nalparser->pps[pps->id];
  }

  return res;
}

/**
 * gst_h264_pps_clear:
 * @pps: The #GstH264PPS to free
 *
 * Clears all @pps internal resources.
 *
 * Since: 1.4
 */
void
gst_h264_pps_clear (GstH264PPS * pps)
{
  g_return_if_fail (pps != NULL);

  g_free (pps->slice_group_id);
  pps->slice_group_id = NULL;
}

/**
 * gst_h264_parser_parse_slice_hdr:
 * @nalparser: a #GstH264NalParser
 * @nalu: The %GST_H264_NAL_SLICE to %GST_H264_NAL_SLICE_IDR #GstH264NalUnit to parse
 * @slice: The #GstH264SliceHdr to fill.
 * @parse_pred_weight_table: Whether to parse the pred_weight_table or not
 * @parse_dec_ref_pic_marking: Whether to parse the dec_ref_pic_marking or not
 *
 * Parses @nalu containing a coded slice, and fills @slice.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parser_parse_slice_hdr (GstH264NalParser * nalparser,
    GstH264NalUnit * nalu, GstH264SliceHdr * slice,
    gboolean parse_pred_weight_table, gboolean parse_dec_ref_pic_marking)
{
  NalReader nr;
  gint pps_id;
  GstH264PPS *pps;
  GstH264SPS *sps;
  guint start_pos, start_epb;

  memset (slice, 0, sizeof (*slice));

  if (!nalu->size) {
    GST_DEBUG ("Invalid Nal Unit");
    return GST_H264_PARSER_ERROR;
  }

  nal_reader_init (&nr, nalu->data + nalu->offset + nalu->header_bytes,
      nalu->size - nalu->header_bytes);

  READ_UE (&nr, slice->first_mb_in_slice);
  READ_UE (&nr, slice->type);

  GST_DEBUG ("parsing \"Slice header\", slice type %u", slice->type);

  READ_UE_MAX (&nr, pps_id, GST_H264_MAX_PPS_COUNT - 1);
  pps = gst_h264_parser_get_pps (nalparser, pps_id);

  if (!pps) {
    GST_WARNING ("couldn't find associated picture parameter set with id: %d",
        pps_id);

    return GST_H264_PARSER_BROKEN_LINK;
  }

  slice->pps = pps;
  sps = pps->sequence;
  if (!sps) {
    GST_WARNING ("couldn't find associated sequence parameter set with id: %d",
        pps->id);
    return GST_H264_PARSER_BROKEN_LINK;
  }

  /* Check we can actually parse this slice. The parser handles AVC, MVC, and
   * SVC extension NAL units. Other extension types remain unsupported. */
  if (sps->extension_type &&
      sps->extension_type != GST_H264_NAL_EXTENSION_MVC &&
      sps->extension_type != GST_H264_NAL_EXTENSION_SVC) {
    GST_WARNING ("failed to parse unsupported slice header");
    return GST_H264_PARSER_BROKEN_DATA;
  }

  /* set default values for fields that might not be present in the bitstream
     and have valid defaults */
  if (GST_H264_IS_I_SLICE (slice)) {
    slice->num_ref_idx_l0_active_minus1 = 0;
    slice->num_ref_idx_l1_active_minus1 = 0;
  } else {
    slice->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_active_minus1;

    if (GST_H264_IS_B_SLICE (slice))
      slice->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_active_minus1;
    else
      slice->num_ref_idx_l1_active_minus1 = 0;
  }

  if (sps->separate_colour_plane_flag)
    READ_UINT8 (&nr, slice->colour_plane_id, 2);

  READ_UINT16 (&nr, slice->frame_num, sps->log2_max_frame_num_minus4 + 4);

  if (!sps->frame_mbs_only_flag) {
    READ_UINT8 (&nr, slice->field_pic_flag, 1);
    if (slice->field_pic_flag)
      READ_UINT8 (&nr, slice->bottom_field_flag, 1);
  }

  /* calculate MaxPicNum */
  if (slice->field_pic_flag)
    slice->max_pic_num = 2 * sps->max_frame_num;
  else
    slice->max_pic_num = sps->max_frame_num;

  if (nalu->idr_pic_flag)
    READ_UE_MAX (&nr, slice->idr_pic_id, G_MAXUINT16);

  start_pos = nal_reader_get_pos (&nr);
  start_epb = nal_reader_get_epb_count (&nr);

  if (sps->pic_order_cnt_type == 0) {
    READ_UINT16 (&nr, slice->pic_order_cnt_lsb,
        sps->log2_max_pic_order_cnt_lsb_minus4 + 4);

    if (pps->pic_order_present_flag && !slice->field_pic_flag)
      READ_SE (&nr, slice->delta_pic_order_cnt_bottom);
  }

  if (sps->pic_order_cnt_type == 1 && !sps->delta_pic_order_always_zero_flag) {
    READ_SE (&nr, slice->delta_pic_order_cnt[0]);
    if (pps->pic_order_present_flag && !slice->field_pic_flag)
      READ_SE (&nr, slice->delta_pic_order_cnt[1]);
  }

  slice->pic_order_cnt_bit_size = (nal_reader_get_pos (&nr) - start_pos) -
      (8 * (nal_reader_get_epb_count (&nr) - start_epb));

  if (pps->redundant_pic_cnt_present_flag)
    READ_UE_MAX (&nr, slice->redundant_pic_cnt, G_MAXINT8);

  if (GST_H264_IS_B_SLICE (slice))
    READ_UINT8 (&nr, slice->direct_spatial_mv_pred_flag, 1);

  if (GST_H264_IS_P_SLICE (slice) || GST_H264_IS_SP_SLICE (slice) ||
      GST_H264_IS_B_SLICE (slice)) {
    READ_UINT8 (&nr, slice->num_ref_idx_active_override_flag, 1);
    if (slice->num_ref_idx_active_override_flag) {
      READ_UE_MAX (&nr, slice->num_ref_idx_l0_active_minus1, 31);

      if (GST_H264_IS_B_SLICE (slice))
        READ_UE_MAX (&nr, slice->num_ref_idx_l1_active_minus1, 31);
    }
  }

  if (!slice_parse_ref_pic_list_modification (slice, &nr,
          GST_H264_IS_MVC_NALU (nalu)))
    goto error;

  if ((pps->weighted_pred_flag && (GST_H264_IS_P_SLICE (slice)
              || GST_H264_IS_SP_SLICE (slice)))
      || (pps->weighted_bipred_idc == 1 && GST_H264_IS_B_SLICE (slice))) {
    if (!gst_h264_slice_parse_pred_weight_table (slice, &nr,
            sps->chroma_array_type))
      goto error;
  }

  if (nalu->ref_idc != 0) {
    if (!gst_h264_slice_parse_dec_ref_pic_marking (slice, nalu, &nr))
      goto error;
  }

  if (pps->entropy_coding_mode_flag && !GST_H264_IS_I_SLICE (slice) &&
      !GST_H264_IS_SI_SLICE (slice))
    READ_UE_MAX (&nr, slice->cabac_init_idc, 2);

  READ_SE_ALLOWED (&nr, slice->slice_qp_delta, -87, 77);

  if (GST_H264_IS_SP_SLICE (slice) || GST_H264_IS_SI_SLICE (slice)) {
    if (GST_H264_IS_SP_SLICE (slice))
      READ_UINT8 (&nr, slice->sp_for_switch_flag, 1);
    READ_SE_ALLOWED (&nr, slice->slice_qs_delta, -51, 51);
  }

  if (pps->deblocking_filter_control_present_flag) {
    READ_UE_MAX (&nr, slice->disable_deblocking_filter_idc, 2);
    if (slice->disable_deblocking_filter_idc != 1) {
      READ_SE_ALLOWED (&nr, slice->slice_alpha_c0_offset_div2, -6, 6);
      READ_SE_ALLOWED (&nr, slice->slice_beta_offset_div2, -6, 6);
    }
  }

  if (pps->num_slice_groups_minus1 > 0 &&
      pps->slice_group_map_type >= 3 && pps->slice_group_map_type <= 5) {

    guint32 PicWidthInMbs = sps->pic_width_in_mbs_minus1 + 1;
    guint32 PicHeightInMapUnits = sps->pic_height_in_map_units_minus1 + 1;
    guint32 PicSizeInMapUnits = PicWidthInMbs * PicHeightInMapUnits;
    guint32 SliceGroupChangeRate = pps->slice_group_change_rate_minus1 + 1;
    /* Ceil(Log2(PicSizeInMapUnits / SliceGroupChangeRate + 1))  [7-35] */
    const guint n =
        gst_util_ceil_log2 (PicSizeInMapUnits / SliceGroupChangeRate + 1);
    READ_UINT16 (&nr, slice->slice_group_change_cycle, n);
  }

  slice->header_size = nal_reader_get_pos (&nr);
  slice->n_emulation_prevention_bytes = nal_reader_get_epb_count (&nr);

  return GST_H264_PARSER_OK;

error:
  GST_WARNING ("error parsing \"Slice header\"");
  return GST_H264_PARSER_ERROR;
}

// Alex: too much details:
typedef struct
{
  gint low;
  gint range;
  const guint8 *bytestream_start;
  const guint8 *bytestream;
  const guint8 *bytestream_end;
} GstH264CabacContext;

/* FFmpeg-derived H.264 CABAC tables. This minimal parser-side port currently
 * only uses them for exact all-skip detection in CABAC P/SP slices. */
static const guint8 gst_h264_ff_cabac_lps_range[512] = {
  128, 128, 128, 128, 128, 128, 123, 123, 116, 116, 111, 111, 105, 105, 100,
  100, 95, 95, 90, 90, 85, 85, 81, 81, 77, 77, 73, 73, 69, 69, 66, 66, 62, 62,
  59, 59, 56, 56, 53, 53, 51, 51, 48, 48, 46, 46, 43, 43, 41, 41, 39, 39, 37,
  37, 35, 35, 33, 33, 32, 32, 30, 30, 29, 29, 27, 27, 26, 26, 24, 24, 23, 23,
  22, 22, 21, 21, 20, 20, 19, 19, 18, 18, 17, 17, 16, 16, 15, 15, 14, 14, 14,
  14, 13, 13, 12, 12, 12, 12, 11, 11, 11, 11, 10, 10, 10, 10, 9, 9, 9, 9, 8, 8,
  8, 8, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 2, 2, 176, 176, 167, 167, 158, 158,
  150, 150, 142, 142, 135, 135, 128, 128, 122, 122, 116, 116, 110, 110, 104,
  104, 99, 99, 94, 94, 89, 89, 85, 85, 80, 80, 76, 76, 72, 72, 69, 69, 65, 65,
  62, 62, 59, 59, 56, 56, 53, 53, 50, 50, 48, 48, 45, 45, 43, 43, 41, 41, 39,
  39, 37, 37, 35, 35, 33, 33, 31, 31, 30, 30, 28, 28, 27, 27, 26, 26, 24, 24,
  23, 23, 22, 22, 21, 21, 20, 20, 19, 19, 18, 18, 17, 17, 16, 16, 15, 15, 14,
  14, 14, 14, 13, 13, 12, 12, 12, 12, 11, 11, 11, 11, 10, 10, 9, 9, 9, 9, 9, 9,
  8, 8, 8, 8, 7, 7, 7, 7, 2, 2, 208, 208, 197, 197, 187, 187, 178, 178, 169,
  169, 160, 160, 152, 152, 144, 144, 137, 137, 130, 130, 123, 123, 117, 117,
  111, 111, 105, 105, 100, 100, 95, 95, 90, 90, 86, 86, 81, 81, 77, 77, 73, 73,
  69, 69, 66, 66, 63, 63, 59, 59, 56, 56, 54, 54, 51, 51, 48, 48, 46, 46, 43,
  43, 41, 41, 39, 39, 37, 37, 35, 35, 33, 33, 32, 32, 30, 30, 29, 29, 27, 27,
  26, 26, 25, 25, 23, 23, 22, 22, 21, 21, 20, 20, 19, 19, 18, 18, 17, 17, 16,
  16, 15, 15, 15, 15, 14, 14, 13, 13, 12, 12, 12, 12, 11, 11, 11, 11, 10, 10,
  10, 10, 9, 9, 9, 9, 8, 8, 2, 2, 240, 240, 227, 227, 216, 216, 205, 205, 195,
  195, 185, 185, 175, 175, 166, 166, 158, 158, 150, 150, 142, 142, 135, 135,
  128, 128, 122, 122, 116, 116, 110, 110, 104, 104, 99, 99, 94, 94, 89, 89, 85,
  85, 80, 80, 76, 76, 72, 72, 69, 69, 65, 65, 62, 62, 59, 59, 56, 56, 53, 53,
  50, 50, 48, 48, 45, 45, 43, 43, 41, 41, 39, 39, 37, 37, 35, 35, 33, 33, 31,
  31, 30, 30, 28, 28, 27, 27, 25, 25, 24, 24, 23, 23, 22, 22, 21, 21, 20, 20,
  19, 19, 18, 18, 17, 17, 16, 16, 15, 15, 14, 14, 14, 14, 13, 13, 12, 12, 12,
  12, 11, 11, 11, 11, 10, 10, 9, 9, 2, 2
};

static const guint8 gst_h264_ff_cabac_mlps_state[256] = {
  127, 126, 77, 76, 77, 76, 75, 74, 75, 74, 75, 74, 73, 72, 73, 72, 73, 72, 71,
  70, 71, 70, 71, 70, 69, 68, 69, 68, 67, 66, 67, 66, 67, 66, 65, 64, 65, 64,
  63, 62, 61, 60, 61, 60, 61, 60, 59, 58, 59, 58, 57, 56, 55, 54, 55, 54, 53,
  52, 53, 52, 51, 50, 49, 48, 49, 48, 47, 46, 45, 44, 45, 44, 43, 42, 43, 42,
  39, 38, 39, 38, 37, 36, 37, 36, 33, 32, 33, 32, 31, 30, 31, 30, 27, 26, 27,
  26, 25, 24, 23, 22, 23, 22, 19, 18, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9,
  8, 9, 8, 5, 4, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
  13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
  32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
  51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
  70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88,
  89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106,
  107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
  122, 123, 124, 125, 124, 125, 126, 127
};

static const gint8 gst_h264_p_skip_cabac_init[3][2] = {
  {23, 33},
  {22, 25},
  {29, 16}
};

static const gint8 gst_h264_p_mb_type_cabac_init[3][7][2] = {
  {
        { 1, 9}, { 0, 49}, {-37, 118}, { 5, 57},
        {12, 49}, {-4, 73}, {17, 50}
      },
  {
        {-2, 9}, { 4, 41}, {-29, 118}, { 2, 65},
        { 9, 50}, {-3, 70}, {10, 54}
      },
  {
        {-10, 51}, {-3, 62}, {-27, 99}, {26, 16},
        { 6, 57}, {-17, 73}, {14, 57}
      }
};

static const gint8 gst_h264_p_mvd_cabac_init[3][8][2] = {
  {
        {-3, 69}, {-6, 81}, {-11, 96}, { 6, 55},
        {-3, 76}, {-10, 94}, { 5, 54}, { 4, 69}
      },
  {
        {-2, 69}, {-5, 82}, {-10, 96}, { 2, 59},
        {-3, 74}, { -6, 85}, { 0, 59}, {-3, 81}
      },
  {
        {-11, 89}, {-15, 103}, {-21, 116}, {19, 57},
        { -5, 85}, {-13, 106}, { 5, 63}, { 6, 75}
      }
};

static const gint8 gst_h264_p_ref_cabac_init[3][6][2] = {
  {
        {-7, 67}, {-5, 74}, {-4, 74},
        {-5, 80}, {-7, 72}, { 1, 58}
      },
  {
        {-1, 66}, {-1, 77}, { 1, 70},
        {-2, 86}, {-5, 72}, { 0, 61}
      },
  {
        { 3, 55}, {-4, 79}, {-2, 75},
        {-12, 97}, {-7, 50}, { 1, 60}
      }
};

static const gint8 gst_h264_p_cbp_cabac_init[3][11][2] = {
  {
        {-27, 126}, {-28, 98}, {-25, 101}, {-23, 67}, {-28, 82},
        {-20, 94}, {-16, 83}, {-22, 110}, {-21, 91}, {-18, 102},
        {-13, 93}
      },
  {
        {-39, 127}, {-18, 91}, {-17, 96}, {-26, 81}, {-35, 98},
        {-24, 102}, {-23, 97}, {-27, 119}, {-24, 99}, {-21, 110},
        {-18, 102}
      },
  {
        {-36, 127}, {-17, 91}, {-14, 95}, {-25, 84}, {-25, 86},
        {-12, 89}, {-17, 91}, {-31, 127}, {-14, 76}, {-18, 103},
        {-13, 90}
      }
};

static const gint8 gst_h264_p_qp_delta_cabac_init[3][4][2] = {
  {
        {0, 41}, {0, 63}, {0, 63}, {0, 63}
      },
  {
        {0, 41}, {0, 63}, {0, 63}, {0, 63}
      },
  {
        {0, 41}, {0, 63}, {0, 63}, {0, 63}
      }
};

static const gint8 gst_h264_p_intra_chroma_pred_cabac_init[3][6][2] = {
  {
        {-9, 83}, {4, 86}, {0, 97}, {-7, 72}, {13, 41}, {3, 62}
      },
  {
        {-9, 83}, {4, 86}, {0, 97}, {-7, 72}, {13, 41}, {3, 62}
      },
  {
        {-9, 83}, {4, 86}, {0, 97}, {-7, 72}, {13, 41}, {3, 62}
      }
};

static const gint8 gst_h264_p_residual_cbf_cabac_init[3][12][2] = {
  {
        {-3, 74}, {-9, 92}, {-8, 87}, {-23, 126}, {5, 54}, {6, 60},
        {6, 59}, {6, 69}, {-1, 48}, {0, 68}, {-4, 69}, {-8, 88}
      },
  {
        {-2, 73}, {-12, 104}, {-9, 91}, {-31, 127}, {3, 55}, {7, 56},
        {7, 55}, {8, 61}, {-3, 53}, {0, 68}, {-7, 74}, {-9, 88}
      },
  {
        {-5, 79}, {-11, 104}, {-11, 91}, {-30, 127}, {0, 65}, {-2, 79},
        {0, 72}, {-4, 92}, {-6, 56}, {3, 68}, {-8, 71}, {-13, 98}
      }
};

static const gint8 gst_h264_p_residual_sig_cabac_init[3][33][2] = {
  {
        {9, 53}, {2, 53}, {5, 53}, {-2, 61}, {0, 56}, {0, 56}, {-13, 63},
        {-5, 60}, {-1, 62}, {4, 57}, {-6, 69}, {4, 57}, {14, 39}, {4, 51},
        {13, 68}, {3, 64}, {1, 61}, {9, 63}, {7, 50}, {16, 39}, {5, 44},
        {4, 52}, {11, 48}, {-5, 60}, {-1, 59}, {0, 59}, {22, 33}, {5, 44},
        {14, 43}, {-1, 78}, {0, 60}, {9, 69}, {11, 28}
      },
  {
        {0, 54}, {-5, 61}, {0, 58}, {-1, 60}, {-3, 61}, {-8, 67},
        {-25, 84}, {-14, 74}, {-5, 65}, {5, 52}, {2, 57}, {0, 61},
        {-9, 69}, {-11, 70}, {18, 55}, {-4, 71}, {0, 58}, {7, 61},
        {9, 41}, {18, 25}, {9, 32}, {5, 43}, {9, 47}, {0, 44}, {0, 51},
        {2, 46}, {19, 38}, {-4, 66}, {15, 38}, {12, 42}, {9, 34}, {0, 89},
        {4, 45}
      },
  {
        {1, 67}, {-15, 72}, {-5, 75}, {-8, 80}, {-21, 83}, {-21, 64},
        {-13, 31}, {-25, 64}, {-29, 94}, {9, 75}, {17, 63}, {-8, 74},
        {-5, 35}, {-2, 27}, {13, 91}, {3, 65}, {-7, 69}, {8, 77},
        {-10, 66}, {3, 62}, {-3, 68}, {-20, 81}, {0, 30}, {1, 7},
        {-3, 23}, {-21, 74}, {16, 66}, {-23, 124}, {17, 37}, {44, -18},
        {50, -34}, {-22, 127}, {4, 39}
      }
};

static const gint8 gst_h264_p_residual_last_cabac_init[3][33][2] = {
  {
        {25, 7}, {30, -7}, {28, 3}, {28, 4}, {32, 0}, {34, -1}, {30, 6},
        {30, 6}, {32, 9}, {31, 19}, {26, 27}, {26, 30}, {37, 20}, {28, 34},
        {17, 70}, {1, 67}, {5, 59}, {9, 67}, {16, 30}, {18, 32}, {18, 35},
        {22, 29}, {24, 31}, {23, 38}, {18, 43}, {20, 41}, {11, 63}, {9, 59},
        {9, 64}, {-1, 94}, {-2, 89}, {-9, 108}, {-6, 76}
      },
  {
        {33, -25}, {34, -30}, {36, -28}, {38, -28}, {38, -27}, {34, -18},
        {35, -16}, {34, -14}, {32, -8}, {37, -6}, {35, 0}, {30, 10},
        {28, 18}, {26, 25}, {29, 41}, {0, 75}, {2, 72}, {8, 77}, {14, 35},
        {18, 31}, {17, 35}, {21, 30}, {17, 45}, {20, 42}, {18, 45},
        {27, 26}, {16, 54}, {7, 66}, {16, 56}, {11, 73}, {10, 67},
        {-10, 116}, {-23, 112}
      },
  {
        {35, -18}, {33, -25}, {28, -3}, {24, 10}, {27, 0}, {34, -14},
        {52, -44}, {39, -24}, {19, 17}, {31, 25}, {36, 29}, {24, 33},
        {34, 15}, {30, 20}, {22, 73}, {20, 34}, {19, 31}, {27, 44},
        {19, 16}, {15, 36}, {15, 36}, {21, 28}, {25, 21}, {30, 20},
        {31, 12}, {27, 16}, {24, 42}, {0, 93}, {14, 56}, {15, 57},
        {26, 38}, {-24, 127}, {-24, 115}
      }
};

static const gint8 gst_h264_p_residual_abs_cabac_init[3][29][2] = {
  {
        {1, 58}, {-3, 29}, {-1, 36}, {1, 38}, {2, 43}, {-6, 55}, {0, 58},
        {0, 64}, {-3, 74}, {-10, 90}, {0, 70}, {-4, 29}, {5, 31}, {7, 42},
        {1, 59}, {-2, 58}, {-3, 72}, {-3, 81}, {-11, 97}, {0, 58}, {8, 5},
        {10, 14}, {14, 18}, {13, 27}, {2, 40}, {0, 58}, {-3, 70}, {-6, 79},
        {-8, 85}
      },
  {
        {-11, 76}, {-10, 44}, {-10, 52}, {-10, 57}, {-9, 58}, {-16, 72},
        {-7, 69}, {-4, 69}, {-5, 74}, {-9, 86}, {2, 66}, {-9, 34}, {1, 32},
        {11, 31}, {5, 52}, {-2, 55}, {-2, 67}, {0, 73}, {-8, 89}, {3, 52},
        {7, 4}, {10, 8}, {17, 8}, {16, 19}, {3, 37}, {-1, 61}, {-5, 73},
        {-1, 70}, {-4, 78}
      },
  {
        {-10, 82}, {-8, 48}, {-8, 61}, {-8, 66}, {-7, 70}, {-14, 75},
        {-10, 79}, {-9, 83}, {-12, 92}, {-18, 108}, {-4, 79}, {-22, 69},
        {-16, 75}, {-2, 58}, {1, 58}, {-13, 78}, {-9, 83}, {-4, 81},
        {-13, 99}, {-13, 81}, {-6, 38}, {-13, 62}, {-6, 58}, {-2, 59},
        {-16, 73}, {-10, 76}, {-13, 86}, {-9, 83}, {-10, 87}
      }
};

static const gint8 gst_h264_p_transform8x8_cabac_init[3][37][2] = {
  {
        {12, 40}, {11, 51}, {14, 59}, {-4, 79}, {-7, 71}, {-5, 69},
        {-9, 70}, {-8, 66}, {-10, 68}, {-19, 73}, {-12, 69}, {-16, 70},
        {-15, 67}, {-20, 62}, {-19, 70}, {-16, 66}, {-22, 65}, {-20, 63},
        {9, -2}, {26, -9}, {33, -9}, {39, -7}, {41, -2}, {45, 3}, {49, 9},
        {45, 27}, {36, 59}, {-6, 66}, {-7, 35}, {-7, 42}, {-8, 45},
        {-5, 48}, {-12, 56}, {-6, 60}, {-5, 62}, {-8, 66}, {-8, 76}
      },
  {
        {25, 32}, {21, 49}, {21, 54}, {-5, 85}, {-6, 81}, {-10, 77},
        {-7, 81}, {-17, 80}, {-18, 73}, {-4, 74}, {-10, 83}, {-9, 71},
        {-9, 67}, {-1, 61}, {-8, 66}, {-14, 66}, {0, 59}, {2, 59},
        {17, -10}, {32, -13}, {42, -9}, {49, -5}, {53, 0}, {64, 3},
        {68, 10}, {66, 27}, {47, 57}, {-5, 71}, {0, 24}, {-1, 36},
        {-2, 42}, {-2, 52}, {-9, 57}, {-6, 63}, {-4, 65}, {-4, 67},
        {-7, 82}
      },
  {
        {21, 33}, {19, 50}, {17, 61}, {-3, 78}, {-8, 74}, {-9, 72},
        {-10, 72}, {-18, 75}, {-12, 71}, {-11, 63}, {-5, 70}, {-17, 75},
        {-14, 72}, {-16, 67}, {-8, 53}, {-14, 59}, {-9, 52}, {-11, 68},
        {9, -2}, {30, -10}, {31, -4}, {33, -1}, {33, 7}, {31, 12},
        {37, 23}, {31, 38}, {20, 64}, {-9, 71}, {-7, 37}, {-8, 44},
        {-11, 49}, {-10, 56}, {-12, 59}, {-8, 63}, {-9, 67}, {-6, 68},
        {-10, 79}
      }
};

static const guint8 gst_h264_last_coeff_flag_offset_8x8[63] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
  5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8
};

static const guint8 gst_h264_significant_coeff_flag_offset_8x8[63] = {
  0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5,
  4, 4, 4, 4, 3, 3, 6, 7, 7, 7, 8, 9, 10, 9, 8, 7,
  7, 6, 11, 12, 13, 11, 6, 7, 8, 9, 14, 10, 9, 8, 6, 11,
  12, 13, 11, 6, 9, 14, 10, 9, 11, 12, 13, 11, 14, 10, 12
};

static const guint8 gst_h264_sig_coeff_offset_dc[7] = { 0, 0, 1, 1, 2, 2, 2 };
static const guint8 gst_h264_coeff_abs_level1_ctx[8] = { 1, 2, 3, 4, 0, 0, 0,
  0 };
static const guint8 gst_h264_coeff_abs_levelgt1_ctx[8] = { 5, 5, 5, 5, 6, 7, 8,
  9 };
static const guint8 gst_h264_coeff_abs_level_transition0[8] = { 1, 2, 3, 3, 4,
  5, 6, 7 };
static const guint8 gst_h264_coeff_abs_level_transition1[8] = { 4, 4, 4, 4, 5,
  6, 7, 7 };

typedef enum
{
  GST_H264_CABAC_P_MB_16X16,
  GST_H264_CABAC_P_MB_16X8,
  GST_H264_CABAC_P_MB_8X16,
  GST_H264_CABAC_P_MB_8X8,
  GST_H264_CABAC_P_MB_UNSUPPORTED
} GstH264CabacPMbType;

typedef enum
{
  GST_H264_CABAC_P_SUB_MB_8X8,
  GST_H264_CABAC_P_SUB_MB_8X4,
  GST_H264_CABAC_P_SUB_MB_4X8,
  GST_H264_CABAC_P_SUB_MB_4X4
} GstH264CabacPSubMbType;

static gboolean
gst_h264_parser_extract_rbsp_suffix (GstH264NalUnit * nalu, guint start_bit,
    guint8 ** rbsp_data, guint * rbsp_size)
{
  NalReader nr;
  guint aligned_start_bit;
  guint remaining_bits;
  guint size;
  guint i;

  *rbsp_data = NULL;
  *rbsp_size = 0;

  nal_reader_init (&nr, nalu->data + nalu->offset + nalu->header_bytes,
      nalu->size - nalu->header_bytes);

  aligned_start_bit = (start_bit + 7u) & ~7u;
  if (!nal_reader_skip_long (&nr, aligned_start_bit))
    return FALSE;
  if (!nal_reader_is_byte_aligned (&nr))
    return FALSE;

  remaining_bits = nal_reader_get_remaining (&nr);
  size = remaining_bits / 8;
  if (size == 0)
    return FALSE;

  *rbsp_data = g_malloc (size);
  for (i = 0; i < size; i++) {
    if (!nal_reader_get_bits_uint8 (&nr, &(*rbsp_data)[i], 8)) {
      g_free (*rbsp_data);
      *rbsp_data = NULL;
      return FALSE;
    }
  }

  *rbsp_size = size;
  return TRUE;
}

static void
gst_h264_parser_cabac_refill (GstH264CabacContext * cabac)
{
  guint next0 = 0;
  guint next1 = 0;

  if (cabac->bytestream < cabac->bytestream_end)
    next0 = cabac->bytestream[0];
  if (cabac->bytestream + 1 < cabac->bytestream_end)
    next1 = cabac->bytestream[1];

  cabac->low += (next0 << 9) + (next1 << 1);
  cabac->low -= 0xFFFF;

  if (cabac->bytestream < cabac->bytestream_end)
    cabac->bytestream++;
  if (cabac->bytestream < cabac->bytestream_end)
    cabac->bytestream++;
}

static gboolean
gst_h264_parser_init_cabac_decoder (GstH264CabacContext * cabac,
    const guint8 * data, guint size)
{
  if (size < 3)
    return FALSE;

  cabac->bytestream_start = data;
  cabac->bytestream = data;
  cabac->bytestream_end = data + size;

  cabac->low = (*cabac->bytestream++) << 18;
  cabac->low += (*cabac->bytestream++) << 10;
  if ((((guintptr) cabac->bytestream) & 1u) == 0) {
    cabac->low += (1 << 9);
  } else {
    if (cabac->bytestream >= cabac->bytestream_end)
      return FALSE;
    cabac->low += ((*cabac->bytestream++) << 2) + 2;
  }

  cabac->range = 0x1FE;
  if ((cabac->range << 17) < cabac->low)
    return FALSE;

  return TRUE;
}

static guint8
gst_h264_parser_init_cabac_state (gint qp, gint m, gint n)
{
  gint pre;

  pre = 2 * (((m * qp) >> 4) + n) - 127;
  pre = ABS (pre);
  if (pre > 124)
    pre = 124 + (pre & 1);

  return (guint8) pre;
}

static guint8
gst_h264_parser_get_cabac (GstH264CabacContext * cabac, guint8 * state)
{
  gint s = *state;
  gint range_lps;
  gint lps_mask;
  guint8 bit;

  range_lps = gst_h264_ff_cabac_lps_range[2 * (cabac->range & 0xC0) + s];
  cabac->range -= range_lps;
  lps_mask = ((cabac->range << 17) - cabac->low) >> 31;

  cabac->low -= (cabac->range << 17) & lps_mask;
  cabac->range += (range_lps - cabac->range) & lps_mask;

  s ^= lps_mask;
  *state = gst_h264_ff_cabac_mlps_state[128 + s];
  bit = s & 1;

  while (cabac->range < 0x100) {
    cabac->range <<= 1;
    cabac->low <<= 1;
    if (!(cabac->low & 0xFFFF))
      gst_h264_parser_cabac_refill (cabac);
  }

  return bit;
}

static gboolean
gst_h264_parser_get_cabac_bypass (GstH264CabacContext * cabac)
{
  gint range;

  cabac->low += cabac->low;
  if (!(cabac->low & 0xFFFF))
    gst_h264_parser_cabac_refill (cabac);

  range = cabac->range << 17;
  if (cabac->low < range)
    return 0;

  cabac->low -= range;
  return 1;
}

static gint
gst_h264_parser_get_cabac_bypass_sign (GstH264CabacContext * cabac, gint value)
{
  gint range;
  gint mask;

  cabac->low += cabac->low;
  if (!(cabac->low & 0xFFFF))
    gst_h264_parser_cabac_refill (cabac);

  range = cabac->range << 17;
  cabac->low -= range;
  mask = cabac->low >> 31;
  range &= mask;
  cabac->low += range;
  return (value ^ mask) - mask;
}

static gboolean
gst_h264_parser_get_cabac_terminate (GstH264CabacContext * cabac)
{
  cabac->range -= 2;
  if (cabac->low < (cabac->range << 17)) {
    gint shift = (guint32) (cabac->range - 0x100) >> 31;
    cabac->range <<= shift;
    cabac->low <<= shift;
    if (!(cabac->low & 0xFFFF))
      gst_h264_parser_cabac_refill (cabac);
    return FALSE;
  }

  return TRUE;
}

static void
gst_h264_parser_fill_ref_rect (gint8 ref_grid[4][4], gint x, gint y, gint w,
    gint h, gint8 ref)
{
  gint xx;
  gint yy;

  for (yy = y; yy < y + h; yy++) {
    for (xx = x; xx < x + w; xx++)
      ref_grid[yy][xx] = ref;
  }
}

static void
gst_h264_parser_fill_mvd_rect (gint16 mvd_grid[4][4][2], gint x, gint y, gint w,
    gint h, gint16 mvd_x, gint16 mvd_y)
{
  gint xx;
  gint yy;

  for (yy = y; yy < y + h; yy++) {
    for (xx = x; xx < x + w; xx++) {
      mvd_grid[yy][xx][0] = mvd_x;
      mvd_grid[yy][xx][1] = mvd_y;
    }
  }
}

static gint8
gst_h264_parser_left_ref (gint8 ref_grid[4][4], const gint8 left_ref[4], gint x,
    gint y)
{
  return x > 0 ? ref_grid[y][x - 1] : left_ref[y];
}

static gint8
gst_h264_parser_top_ref (gint8 ref_grid[4][4], const gint8 top_ref[4], gint x,
    gint y)
{
  return y > 0 ? ref_grid[y - 1][x] : top_ref[x];
}

static gint16
gst_h264_parser_left_mvd (gint16 mvd_grid[4][4][2], const gint16 left_mvd[4][2],
    gint x, gint y, gint comp)
{
  return x > 0 ? mvd_grid[y][x - 1][comp] : left_mvd[y][comp];
}

static gint16
gst_h264_parser_top_mvd (gint16 mvd_grid[4][4][2], const gint16 top_mvd[4][2],
    gint x, gint y, gint comp)
{
  return y > 0 ? mvd_grid[y - 1][x][comp] : top_mvd[x][comp];
}

static guint8
gst_h264_parser_init_cabac_state_mm (gint qp, const gint8 init_mm[2])
{
  return gst_h264_parser_init_cabac_state (qp, init_mm[0], init_mm[1]);
}

static void
gst_h264_parser_init_p_slice_cabac_states (guint8 * state, gint qp,
    guint8 cabac_init_idc)
{
  gint i;

  memset (state, 0, 1024);
  state[11] = gst_h264_parser_init_cabac_state_mm (qp,
      gst_h264_p_skip_cabac_init[cabac_init_idc]);
  for (i = 0; i < 7; i++)
    state[14 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_mb_type_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 8; i++)
    state[40 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_mvd_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 6; i++)
    state[54 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_ref_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 11; i++)
    state[73 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_cbp_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 4; i++)
    state[60 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_qp_delta_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 6; i++)
    state[64 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_intra_chroma_pred_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 12; i++)
    state[93 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_residual_cbf_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 33; i++) {
    state[134 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_residual_sig_cabac_init[cabac_init_idc][i]);
    state[195 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_residual_last_cabac_init[cabac_init_idc][i]);
  }
  for (i = 0; i < 29; i++)
    state[247 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_residual_abs_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 37; i++)
    state[399 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_transform8x8_cabac_init[cabac_init_idc][i]);
  for (i = 0; i < 4; i++)
    state[1012 + i] = gst_h264_parser_init_cabac_state_mm (qp,
        gst_h264_p_residual_cbf_cabac_init[cabac_init_idc][i]);
}

static gboolean
gst_h264_parser_decode_cabac_mb_skip (GstH264CabacContext * cabac,
    guint8 * state, gboolean left_non_skip, gboolean top_non_skip)
{
  gint ctx = (left_non_skip ? 1 : 0) + (top_non_skip ? 1 : 0);

  return gst_h264_parser_get_cabac (cabac, &state[11 + ctx]) != 0;
}

static GstH264CabacPMbType
gst_h264_parser_decode_cabac_p_mb_type (GstH264CabacContext * cabac,
    guint8 * state)
{
  gint type;

  if (gst_h264_parser_get_cabac (cabac, &state[14]) != 0)
    return GST_H264_CABAC_P_MB_UNSUPPORTED;

  if (gst_h264_parser_get_cabac (cabac, &state[15]) == 0)
    type = 3 * gst_h264_parser_get_cabac (cabac, &state[16]);
  else
    type = 2 - gst_h264_parser_get_cabac (cabac, &state[17]);

  switch (type) {
    case 0:
      return GST_H264_CABAC_P_MB_16X16;
    case 1:
      return GST_H264_CABAC_P_MB_16X8;
    case 2:
      return GST_H264_CABAC_P_MB_8X16;
    case 3:
      return GST_H264_CABAC_P_MB_8X8;
    default:
      return GST_H264_CABAC_P_MB_UNSUPPORTED;
  }
}

static gint
gst_h264_parser_decode_cabac_intra_mb_type (GstH264CabacContext * cabac,
    guint8 * state)
{
  gint mb_type;

  if (gst_h264_parser_get_cabac (cabac, &state[17]) == 0)
    return 0;
  if (gst_h264_parser_get_cabac_terminate (cabac))
    return 25;

  mb_type = 1;
  mb_type += 12 * gst_h264_parser_get_cabac (cabac, &state[18]);
  if (gst_h264_parser_get_cabac (cabac, &state[19]) != 0)
    mb_type += 4 + 4 * gst_h264_parser_get_cabac (cabac, &state[19]);
  mb_type += 2 * gst_h264_parser_get_cabac (cabac, &state[20]);
  mb_type += gst_h264_parser_get_cabac (cabac, &state[20]);
  return mb_type;
}

static GstH264CabacPSubMbType
gst_h264_parser_decode_cabac_p_sub_mb_type (GstH264CabacContext * cabac,
    guint8 * state)
{
  if (gst_h264_parser_get_cabac (cabac, &state[21]) != 0)
    return GST_H264_CABAC_P_SUB_MB_8X8;
  if (gst_h264_parser_get_cabac (cabac, &state[22]) == 0)
    return GST_H264_CABAC_P_SUB_MB_8X4;
  if (gst_h264_parser_get_cabac (cabac, &state[23]) != 0)
    return GST_H264_CABAC_P_SUB_MB_4X8;
  return GST_H264_CABAC_P_SUB_MB_4X4;
}

static gboolean
gst_h264_parser_decode_cabac_mb_ref (GstH264CabacContext * cabac,
    guint8 * state, gint8 left_ref, gint8 top_ref, guint8 ref_count,
    guint8 * ref_idx)
{
  guint8 ref = 0;
  gint ctx = 0;

  if (ref_count <= 1) {
    *ref_idx = 0;
    return TRUE;
  }

  if (left_ref > 0)
    ctx++;
  if (top_ref > 0)
    ctx += 2;

  while (gst_h264_parser_get_cabac (cabac, &state[54 + ctx]) != 0) {
    ref++;
    ctx = (ctx >> 2) + 4;
    if (ref >= ref_count)
      return FALSE;
  }

  *ref_idx = ref;
  return TRUE;
}

static gboolean
gst_h264_parser_decode_cabac_mb_mvd_component (GstH264CabacContext * cabac,
    guint8 * state, gint ctxbase, gint amvd, gint16 * out)
{
  gint mvd;
  gint clipped;

  if (gst_h264_parser_get_cabac (cabac,
          &state[ctxbase + ((amvd - 3) >> 31) + ((amvd - 33) >> 31) + 2]) == 0) {
    *out = 0;
    return TRUE;
  }

  mvd = 1;
  ctxbase += 3;
  while (mvd < 9 && gst_h264_parser_get_cabac (cabac, &state[ctxbase]) != 0) {
    if (mvd < 4)
      ctxbase++;
    mvd++;
  }

  if (mvd >= 9) {
    gint k = 3;

    while (gst_h264_parser_get_cabac_bypass (cabac) != 0) {
      mvd += 1 << k;
      k++;
      if (k > 24)
        return FALSE;
    }
    while (k-- > 0)
      mvd += gst_h264_parser_get_cabac_bypass (cabac) << k;
  }

  clipped = mvd < 70 ? mvd : 70;
  *out = (gint16) gst_h264_parser_get_cabac_bypass_sign (cabac, -clipped);
  return TRUE;
}

static gboolean
gst_h264_parser_decode_cabac_mb_mvd (GstH264CabacContext * cabac,
    guint8 * state, gint16 mvd_grid[4][4][2], const gint16 left_mvd[4][2],
    const gint16 top_mvd[4][2], gint x, gint y, gint16 * out_x, gint16 * out_y)
{
  gint amvd_x;
  gint amvd_y;

  amvd_x =
      gst_h264_parser_left_mvd (mvd_grid, left_mvd, x, y, 0) +
      gst_h264_parser_top_mvd (mvd_grid, top_mvd, x, y, 0);
  amvd_y =
      gst_h264_parser_left_mvd (mvd_grid, left_mvd, x, y, 1) +
      gst_h264_parser_top_mvd (mvd_grid, top_mvd, x, y, 1);

  if (!gst_h264_parser_decode_cabac_mb_mvd_component (cabac, state, 40, amvd_x,
          out_x))
    return FALSE;
  if (!gst_h264_parser_decode_cabac_mb_mvd_component (cabac, state, 47, amvd_y,
          out_y))
    return FALSE;
  return TRUE;
}

static guint16
gst_h264_parser_decode_cabac_mb_cbp_luma (GstH264CabacContext * cabac,
    guint8 * state, guint16 left_cbp, guint16 top_cbp)
{
  guint16 cbp = 0;
  gint ctx;

  ctx = !(left_cbp & 0x02) + 2 * !(top_cbp & 0x04);
  cbp += gst_h264_parser_get_cabac (cabac, &state[73 + ctx]);
  ctx = !(cbp & 0x01) + 2 * !(top_cbp & 0x08);
  cbp += gst_h264_parser_get_cabac (cabac, &state[73 + ctx]) << 1;
  ctx = !(left_cbp & 0x08) + 2 * !(cbp & 0x01);
  cbp += gst_h264_parser_get_cabac (cabac, &state[73 + ctx]) << 2;
  ctx = !(cbp & 0x04) + 2 * !(cbp & 0x02);
  cbp += gst_h264_parser_get_cabac (cabac, &state[73 + ctx]) << 3;
  return cbp;
}

static guint16
gst_h264_parser_decode_cabac_mb_cbp_chroma (GstH264CabacContext * cabac,
    guint8 * state, guint16 left_cbp, guint16 top_cbp)
{
  gint ctx = 0;
  gint cbp_a = (left_cbp >> 4) & 0x03;
  gint cbp_b = (top_cbp >> 4) & 0x03;

  if (cbp_a > 0)
    ctx++;
  if (cbp_b > 0)
    ctx += 2;
  if (gst_h264_parser_get_cabac (cabac, &state[77 + ctx]) == 0)
    return 0;

  ctx = 4;
  if (cbp_a == 2)
    ctx++;
  if (cbp_b == 2)
    ctx += 2;
  return 1 + gst_h264_parser_get_cabac (cabac, &state[77 + ctx]);
}

static guint8
gst_h264_parser_decode_cabac_mb_chroma_pre_mode (GstH264CabacContext * cabac,
    guint8 * state, guint8 left_chroma_pred_mode, guint8 top_chroma_pred_mode)
{
  gint ctx = 0;

  if (left_chroma_pred_mode != 0)
    ctx++;
  if (top_chroma_pred_mode != 0)
    ctx++;

  if (gst_h264_parser_get_cabac (cabac, &state[64 + ctx]) == 0)
    return 0;
  if (gst_h264_parser_get_cabac (cabac, &state[67]) == 0)
    return 1;
  if (gst_h264_parser_get_cabac (cabac, &state[67]) == 0)
    return 2;
  return 3;
}

static guint8
gst_h264_parser_get_luma_left_nnz (const guint8 nnz[4][4],
    const guint8 left_luma_nnz[4], gint x, gint y)
{
  return x > 0 ? nnz[y][x - 1] : left_luma_nnz[y];
}

static guint8
gst_h264_parser_get_luma_top_nnz (const guint8 nnz[4][4],
    const guint8 * top_luma_nnz, gint x, gint y)
{
  return y > 0 ? nnz[y - 1][x] : top_luma_nnz[x];
}

static guint8
gst_h264_parser_get_chroma_left_nnz (const guint8 nnz[2][2],
    const guint8 left_chroma_nnz[2], gint x, gint y)
{
  return x > 0 ? nnz[y][x - 1] : left_chroma_nnz[y];
}

static guint8
gst_h264_parser_get_chroma_top_nnz (const guint8 nnz[2][2],
    const guint8 * top_chroma_nnz, gint x, gint y)
{
  return y > 0 ? nnz[y - 1][x] : top_chroma_nnz[x];
}

static gint
gst_h264_parser_decode_cabac_residual_cbf_ctx (guint16 left_cbp, guint16 top_cbp,
    const guint8 luma_nnz[4][4], const guint8 * top_luma_nnz,
    const guint8 left_luma_nnz[4], const guint8 chroma_nnz[2][2],
    const guint8 * top_chroma_nnz, const guint8 left_chroma_nnz[2],
    gint cat, gint block_idx, gint plane)
{
  gint ctx = 0;
  guint8 left;
  guint8 top;

  switch (cat) {
    case 0:
      if (left_cbp & (0x100u << plane))
        ctx++;
      if (top_cbp & (0x100u << plane))
        ctx += 2;
      return 85 + ctx;
    case 1:
      left = gst_h264_parser_get_luma_left_nnz (luma_nnz, left_luma_nnz,
          block_idx & 3, block_idx >> 2);
      top = gst_h264_parser_get_luma_top_nnz (luma_nnz, top_luma_nnz,
          block_idx & 3, block_idx >> 2);
      if (left > 0)
        ctx++;
      if (top > 0)
        ctx += 2;
      return 89 + ctx;
    case 2:
      left = gst_h264_parser_get_luma_left_nnz (luma_nnz, left_luma_nnz,
          block_idx & 3, block_idx >> 2);
      top = gst_h264_parser_get_luma_top_nnz (luma_nnz, top_luma_nnz,
          block_idx & 3, block_idx >> 2);
      if (left > 0)
        ctx++;
      if (top > 0)
        ctx += 2;
      return 93 + ctx;
    case 3:
      if (left_cbp & (0x40u << plane))
        ctx++;
      if (top_cbp & (0x40u << plane))
        ctx += 2;
      return 97 + ctx;
    case 4:
      left = gst_h264_parser_get_chroma_left_nnz (chroma_nnz, left_chroma_nnz,
          block_idx & 1, block_idx >> 1);
      top = gst_h264_parser_get_chroma_top_nnz (chroma_nnz, top_chroma_nnz,
          block_idx & 1, block_idx >> 1);
      if (left > 0)
        ctx++;
      if (top > 0)
        ctx += 2;
      return 101 + ctx;
    case 5:
      left = gst_h264_parser_get_luma_left_nnz (luma_nnz, left_luma_nnz,
          (block_idx & 1) * 2, (block_idx >> 1) * 2);
      top = gst_h264_parser_get_luma_top_nnz (luma_nnz, top_luma_nnz,
          (block_idx & 1) * 2, (block_idx >> 1) * 2);
      if (left > 0)
        ctx++;
      if (top > 0)
        ctx += 2;
      return 1012 + ctx;
    default:
      return -1;
  }
}

static gboolean
gst_h264_parser_skip_cabac_coeff_levels (GstH264CabacContext * cabac,
    guint8 * state, gint abs_level_base, guint coeff_count)
{
  gint node_ctx = 0;

  while (coeff_count-- > 0) {
    if (gst_h264_parser_get_cabac (cabac,
            &state[abs_level_base +
                gst_h264_coeff_abs_level1_ctx[node_ctx]]) == 0) {
      node_ctx = gst_h264_coeff_abs_level_transition0[node_ctx];
      (void) gst_h264_parser_get_cabac_bypass_sign (cabac, -1);
    } else {
      guint coeff_abs = 2;

      node_ctx = gst_h264_coeff_abs_level_transition1[node_ctx];
      while (coeff_abs < 15 && gst_h264_parser_get_cabac (cabac,
              &state[abs_level_base +
                  gst_h264_coeff_abs_levelgt1_ctx[node_ctx]]) != 0)
        coeff_abs++;
      if (coeff_abs >= 15) {
        gint j = 0;

        while (gst_h264_parser_get_cabac_bypass (cabac) != 0 && j < 23)
          j++;
        coeff_abs = 1;
        while (j-- > 0)
          coeff_abs += coeff_abs + gst_h264_parser_get_cabac_bypass (cabac);
        coeff_abs += 14;
      }
      (void) gst_h264_parser_get_cabac_bypass_sign (cabac, -(gint) coeff_abs);
    }
  }

  return TRUE;
}

static gboolean
gst_h264_parser_skip_cabac_residual_4x4 (GstH264CabacContext * cabac,
    guint8 * state, gint sig_base, gint last_base, gint abs_level_base,
    guint max_coeff, guint8 * coeff_count)
{
  guint count = 0;
  guint last;

  for (last = 0; last + 1 < max_coeff; last++) {
    if (gst_h264_parser_get_cabac (cabac, &state[sig_base + last]) == 0)
      continue;
    count++;
    if (gst_h264_parser_get_cabac (cabac, &state[last_base + last]) != 0) {
      last = max_coeff;
      break;
    }
  }

  if (last == max_coeff - 1)
    count++;

  if (count == 0) {
    *coeff_count = 0;
    return TRUE;
  }

  if (!gst_h264_parser_skip_cabac_coeff_levels (cabac, state, abs_level_base,
          count))
    return FALSE;

  *coeff_count = (guint8) count;
  return TRUE;
}

static gboolean
gst_h264_parser_skip_cabac_residual_8x8 (GstH264CabacContext * cabac,
    guint8 * state, gint sig_base, gint last_base, gint abs_level_base,
    guint8 * coeff_count)
{
  guint count = 0;
  guint last;

  for (last = 0; last < 63; last++) {
    if (gst_h264_parser_get_cabac (cabac,
            &state[sig_base + gst_h264_significant_coeff_flag_offset_8x8[last]])
        == 0)
      continue;
    count++;
    if (gst_h264_parser_get_cabac (cabac,
            &state[last_base + gst_h264_last_coeff_flag_offset_8x8[last]]) != 0) {
      last = 64;
      break;
    }
  }

  if (last == 63)
    count++;

  if (count == 0) {
    *coeff_count = 0;
    return TRUE;
  }

  if (!gst_h264_parser_skip_cabac_coeff_levels (cabac, state, abs_level_base,
          count))
    return FALSE;

  *coeff_count = (guint8) count;
  return TRUE;
}

static gboolean
gst_h264_parser_skip_cabac_chroma_dc (GstH264CabacContext * cabac,
    guint8 * state, guint16 left_cbp, guint16 top_cbp, guint plane,
    guint16 * cbp_io)
{
  gint ctx_idx;
  guint count = 0;
  guint last;

  ctx_idx = gst_h264_parser_decode_cabac_residual_cbf_ctx (left_cbp, top_cbp,
      NULL, NULL, NULL, NULL, NULL, NULL, 3, 0, plane);
  if (ctx_idx < 0)
    return FALSE;
  if (gst_h264_parser_get_cabac (cabac, &state[ctx_idx]) == 0)
    return TRUE;

  for (last = 0; last < 3; last++) {
    guint off = gst_h264_sig_coeff_offset_dc[last];

    if (gst_h264_parser_get_cabac (cabac, &state[149 + off]) == 0)
      continue;
    count++;
    if (gst_h264_parser_get_cabac (cabac, &state[210 + off]) != 0) {
      last = 4;
      break;
    }
  }

  if (last == 3)
    count++;

  if (count == 0)
    return TRUE;

  if (!gst_h264_parser_skip_cabac_coeff_levels (cabac, state, 257, count))
    return FALSE;

  *cbp_io |= 0x40u << plane;
  return TRUE;
}

static gboolean
gst_h264_parser_skip_cabac_luma_dc (GstH264CabacContext * cabac,
    guint8 * state, guint16 left_cbp, guint16 top_cbp, guint16 * cbp_io)
{
  gint ctx_idx;
  guint8 coeff_count;

  ctx_idx = gst_h264_parser_decode_cabac_residual_cbf_ctx (left_cbp, top_cbp,
      NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0);
  if (ctx_idx < 0)
    return FALSE;
  if (gst_h264_parser_get_cabac (cabac, &state[ctx_idx]) == 0)
    return TRUE;

  if (!gst_h264_parser_skip_cabac_residual_4x4 (cabac, state, 105, 166, 227,
          16, &coeff_count))
    return FALSE;

  *cbp_io |= 0x100u;
  return TRUE;
}

static gboolean
gst_h264_parser_decode_cabac_mb_qp_delta (GstH264CabacContext * cabac,
    guint8 * state, gboolean last_qscale_nonzero, gboolean * delta_nonzero)
{
  *delta_nonzero = FALSE;
  if (gst_h264_parser_get_cabac (cabac,
          &state[60 + (last_qscale_nonzero ? 1 : 0)]) == 0)
    return TRUE;
  *delta_nonzero = TRUE;

  for (;;) {
    static const guint ctx_map[2] = { 62, 63 };
    guint ctx = 62;
    guint val = 1;

    while (gst_h264_parser_get_cabac (cabac, &state[ctx]) != 0) {
      val++;
      ctx = ctx_map[val > 1];
      if (val > 128)
        return FALSE;
    }
    return TRUE;
  }
}

static gboolean
gst_h264_parser_skip_cabac_inter_residual (GstH264CabacContext * cabac,
    guint8 * state, guint16 left_cbp, guint16 top_cbp, guint16 * cbp_io,
    gboolean dct8x8_allowed,
    guint8 top_transform8x8, guint8 left_transform8x8,
    guint8 luma_nnz[4][4], const guint8 * top_luma_nnz,
    const guint8 left_luma_nnz[4], guint8 chroma_nnz[2][2][2],
    const guint8 * top_chroma_u_nnz, const guint8 left_chroma_u_nnz[2],
    const guint8 * top_chroma_v_nnz, const guint8 left_chroma_v_nnz[2],
    gboolean last_qscale_nonzero, gboolean * delta_nonzero_out,
    gboolean * transform8x8_out)
{
  guint16 cbp = *cbp_io;
  gboolean transform8x8 = FALSE;
  guint i8x8;

  if (cbp != 0) {
    if (!gst_h264_parser_decode_cabac_mb_qp_delta (cabac, state,
            last_qscale_nonzero, delta_nonzero_out))
      return FALSE;
  } else {
    *delta_nonzero_out = FALSE;
  }

  if (dct8x8_allowed && (cbp & 0x0F) != 0)
    transform8x8 = gst_h264_parser_get_cabac (cabac,
        &state[399 + top_transform8x8 + left_transform8x8]) != 0;

  for (i8x8 = 0; i8x8 < 4; i8x8++) {
    gint x8 = (i8x8 & 1) ? 2 : 0;
    gint y8 = (i8x8 & 2) ? 2 : 0;

    if ((cbp & (1u << i8x8)) == 0) {
      gst_h264_parser_fill_ref_rect ((gint8 (*)[4]) luma_nnz, x8, y8, 2, 2, 0);
      continue;
    }

    if (transform8x8) {
      gint ctx_idx;
      guint8 coeff_count;

      ctx_idx = gst_h264_parser_decode_cabac_residual_cbf_ctx (*cbp_io, *cbp_io,
          luma_nnz, top_luma_nnz, left_luma_nnz, NULL, NULL, NULL, 5, i8x8, 0);
      if (ctx_idx < 0)
        return FALSE;
      if (gst_h264_parser_get_cabac (cabac, &state[ctx_idx]) == 0) {
        gst_h264_parser_fill_ref_rect ((gint8 (*)[4]) luma_nnz, x8, y8, 2, 2,
            0);
        continue;
      }
      if (!gst_h264_parser_skip_cabac_residual_8x8 (cabac, state, 402, 417,
              426, &coeff_count))
        return FALSE;
      gst_h264_parser_fill_ref_rect ((gint8 (*)[4]) luma_nnz, x8, y8, 2, 2,
          coeff_count);
    } else {
      guint i4x4;

      for (i4x4 = 0; i4x4 < 4; i4x4++) {
        gint x4 = x8 + (i4x4 & 1);
        gint y4 = y8 + (i4x4 >> 1);
        gint ctx_idx;
        guint8 coeff_count;

        ctx_idx = gst_h264_parser_decode_cabac_residual_cbf_ctx (*cbp_io,
            *cbp_io, luma_nnz, top_luma_nnz, left_luma_nnz, NULL, NULL, NULL,
            2, y4 * 4 + x4, 0);
        if (ctx_idx < 0)
          return FALSE;
        if (gst_h264_parser_get_cabac (cabac, &state[ctx_idx]) == 0) {
          luma_nnz[y4][x4] = 0;
          continue;
        }
        if (!gst_h264_parser_skip_cabac_residual_4x4 (cabac, state, 134, 195,
                247, 16, &coeff_count))
          return FALSE;
        luma_nnz[y4][x4] = coeff_count;
      }
    }
  }

  if (cbp & 0x30) {
    if (!gst_h264_parser_skip_cabac_chroma_dc (cabac, state, left_cbp, top_cbp,
            0, cbp_io))
      return FALSE;
    if (!gst_h264_parser_skip_cabac_chroma_dc (cabac, state, left_cbp, top_cbp,
            1, cbp_io))
      return FALSE;
  }

  if (cbp & 0x20) {
    guint plane;

    for (plane = 0; plane < 2; plane++) {
      guint i;
      const guint8 *top_chroma_nnz = plane == 0 ? top_chroma_u_nnz :
          top_chroma_v_nnz;
      const guint8 *left_chroma_nnz = plane == 0 ? left_chroma_u_nnz :
          left_chroma_v_nnz;

      for (i = 0; i < 4; i++) {
        gint x = i & 1;
        gint y = i >> 1;
        gint ctx_idx;
        guint8 coeff_count;

        ctx_idx = gst_h264_parser_decode_cabac_residual_cbf_ctx (*cbp_io,
            *cbp_io, NULL, NULL, NULL, chroma_nnz[plane], top_chroma_nnz,
            left_chroma_nnz, 4, i, plane);
        if (ctx_idx < 0)
          return FALSE;
        if (gst_h264_parser_get_cabac (cabac, &state[ctx_idx]) == 0) {
          chroma_nnz[plane][y][x] = 0;
          continue;
        }
        if (!gst_h264_parser_skip_cabac_residual_4x4 (cabac, state, 152, 213,
                266, 15, &coeff_count))
          return FALSE;
        chroma_nnz[plane][y][x] = coeff_count;
      }
    }
  }

  *transform8x8_out = transform8x8;
  return TRUE;
}

static gboolean
gst_h264_parser_skip_cabac_intra16x16 (GstH264CabacContext * cabac,
    guint8 * state, guint16 left_cbp, guint16 top_cbp,
    guint8 left_chroma_pred_mode, guint8 top_chroma_pred_mode,
    guint8 luma_nnz[4][4], const guint8 * top_luma_nnz,
    const guint8 left_luma_nnz[4], guint8 chroma_nnz[2][2][2],
    const guint8 * top_chroma_u_nnz, const guint8 left_chroma_u_nnz[2],
    const guint8 * top_chroma_v_nnz, const guint8 left_chroma_v_nnz[2],
    gboolean last_qscale_nonzero, gint intra_mb_type, guint16 * cbp_io,
    guint8 * chroma_pred_mode_out, gboolean * delta_nonzero_out)
{
  guint16 cbp;
  guint intra_idx;
  guint i;

  if (intra_mb_type <= 0 || intra_mb_type >= 25)
    return FALSE;

  intra_idx = (guint) (intra_mb_type - 1);
  cbp = intra_idx >= 12 ? 0x0Fu : 0u;
  cbp |= ((intra_idx % 12u) >> 2) << 4;
  *cbp_io = cbp;

  *chroma_pred_mode_out = gst_h264_parser_decode_cabac_mb_chroma_pre_mode (
      cabac, state, left_chroma_pred_mode, top_chroma_pred_mode);

  if (!gst_h264_parser_decode_cabac_mb_qp_delta (cabac, state,
          last_qscale_nonzero, delta_nonzero_out))
    return FALSE;

  if (!gst_h264_parser_skip_cabac_luma_dc (cabac, state, left_cbp, top_cbp,
          cbp_io))
    return FALSE;

  if ((cbp & 0x0F) != 0) {
    for (i = 0; i < 16; i++) {
      gint ctx_idx;
      guint8 coeff_count;
      gint x = i & 3;
      gint y = i >> 2;

      ctx_idx = gst_h264_parser_decode_cabac_residual_cbf_ctx (left_cbp,
          top_cbp, luma_nnz, top_luma_nnz, left_luma_nnz, NULL, NULL, NULL, 1,
          i, 0);
      if (ctx_idx < 0)
        return FALSE;
      if (gst_h264_parser_get_cabac (cabac, &state[ctx_idx]) == 0) {
        luma_nnz[y][x] = 0;
        continue;
      }
      if (!gst_h264_parser_skip_cabac_residual_4x4 (cabac, state, 120, 181,
              237, 15, &coeff_count))
        return FALSE;
      luma_nnz[y][x] = coeff_count;
    }
  } else {
    memset (luma_nnz, 0, sizeof (guint8[4][4]));
  }

  if (cbp & 0x30) {
    if (!gst_h264_parser_skip_cabac_chroma_dc (cabac, state, left_cbp, top_cbp,
            0, cbp_io))
      return FALSE;
    if (!gst_h264_parser_skip_cabac_chroma_dc (cabac, state, left_cbp, top_cbp,
            1, cbp_io))
      return FALSE;
  }

  if (cbp & 0x20) {
    guint plane;

    for (plane = 0; plane < 2; plane++) {
      guint i4x4;
      const guint8 *top_chroma_nnz = plane == 0 ? top_chroma_u_nnz :
          top_chroma_v_nnz;
      const guint8 *left_chroma_nnz = plane == 0 ? left_chroma_u_nnz :
          left_chroma_v_nnz;

      for (i4x4 = 0; i4x4 < 4; i4x4++) {
        gint ctx_idx;
        guint8 coeff_count;
        gint x = i4x4 & 1;
        gint y = i4x4 >> 1;

        ctx_idx = gst_h264_parser_decode_cabac_residual_cbf_ctx (left_cbp,
            top_cbp, NULL, NULL, NULL, chroma_nnz[plane], top_chroma_nnz,
            left_chroma_nnz, 4, i4x4, plane);
        if (ctx_idx < 0)
          return FALSE;
        if (gst_h264_parser_get_cabac (cabac, &state[ctx_idx]) == 0) {
          chroma_nnz[plane][y][x] = 0;
          continue;
        }
        if (!gst_h264_parser_skip_cabac_residual_4x4 (cabac, state, 152, 213,
                266, 15, &coeff_count))
          return FALSE;
        chroma_nnz[plane][y][x] = coeff_count;
      }
    }
  } else {
    memset (chroma_nnz, 0, sizeof (guint8[2][2][2]));
  }

  return TRUE;
}

static gboolean
gst_h264_parser_identify_cabac_p_ref_usage (const GstH264PPS * pps,
    const GstH264SPS * sps, GstH264NalUnit * nalu, GstH264SliceHdr * slice,
    guint32 total_mbs, guint32 * used_ref_mask_l0)
{
  GstH264CabacContext cabac;
  guint8 state[1024];
  guint8 *rbsp = NULL;
  guint rbsp_size = 0;
  gint slice_qp;
  guint8 *top_non_skip = NULL;
  guint16 *top_cbp = NULL;
  guint8 *top_transform8x8 = NULL;
  guint8 *top_chroma_pred_mode = NULL;
  guint8 *top_luma_nnz = NULL;
  guint8 *top_chroma_u_nnz = NULL;
  guint8 *top_chroma_v_nnz = NULL;
  gint8 *top_ref_all = NULL;
  gint16(*top_mvd_all)[2] = NULL;
  gint8 left_ref[4] = { -1, -1, -1, -1 };
  gint16 left_mvd[4][2] = { {0, 0}, {0, 0}, {0, 0}, {0, 0} };
  guint8 left_luma_nnz[4] = { 0, 0, 0, 0 };
  guint8 left_chroma_u_nnz[2] = { 0, 0 };
  guint8 left_chroma_v_nnz[2] = { 0, 0 };
  guint8 left_chroma_pred_mode = 0;
  gboolean left_non_skip = FALSE;
  gboolean left_transform8x8 = FALSE;
  guint16 left_cbp = 0;
  gboolean last_qscale_nonzero = FALSE;
  guint mb_width;
  guint mb_addr;
  guint mb_x;
  guint mb_y;

  if (pps->num_slice_groups_minus1 > 0 || slice->cabac_init_idc > 2)
    return FALSE;

  if (!gst_h264_parser_extract_rbsp_suffix (nalu, slice->header_size, &rbsp,
          &rbsp_size))
    return FALSE;
  if (!gst_h264_parser_init_cabac_decoder (&cabac, rbsp, rbsp_size))
    goto done;

  slice_qp = CLAMP ((gint) pps->pic_init_qp_minus26 + 26 + slice->slice_qp_delta -
      6 * (gint) sps->bit_depth_luma_minus8, 0, 51);
  gst_h264_parser_init_p_slice_cabac_states (state, slice_qp,
      slice->cabac_init_idc);

  mb_width = sps->pic_width_in_mbs_minus1 + 1;
  top_non_skip = g_new0 (guint8, mb_width);
  top_cbp = g_new0 (guint16, mb_width);
  top_transform8x8 = g_new0 (guint8, mb_width);
  top_chroma_pred_mode = g_new0 (guint8, mb_width);
  top_luma_nnz = g_new0 (guint8, mb_width * 4);
  top_chroma_u_nnz = g_new0 (guint8, mb_width * 2);
  top_chroma_v_nnz = g_new0 (guint8, mb_width * 2);
  top_ref_all = g_new (gint8, mb_width * 4);
  top_mvd_all = (gint16 (*)[2]) g_new0 (gint16, mb_width * 4 * 2);
  memset (top_ref_all, 0xFF, mb_width * 4);

  mb_addr = slice->first_mb_in_slice;
  mb_x = mb_addr % mb_width;
  mb_y = mb_addr / mb_width;

  for (;;) {
    gint8 ref_grid[4][4];
    gint16 mvd_grid[4][4][2] = { {{0, 0}} };
    guint8 luma_nnz[4][4] = { {0, 0, 0, 0}, {0, 0, 0, 0},
      {0, 0, 0, 0}, {0, 0, 0, 0}
    };
    guint8 chroma_nnz[2][2][2] = {
      { {0, 0}, {0, 0} },
      { {0, 0}, {0, 0} }
    };
    guint8 *top_non_skip_mb;
    guint16 *top_cbp_mb;
    guint8 *top_transform8x8_mb;
    guint8 *top_chroma_pred_mode_mb;
    guint8 *top_luma_nnz_mb;
    guint8 *top_chroma_u_nnz_mb;
    guint8 *top_chroma_v_nnz_mb;
    gint8 *top_ref;
    gint16(*top_mvd)[2];
    gboolean skip;
    gboolean non_skip;
    gboolean transform8x8 = FALSE;
    guint8 chroma_pred_mode = 0;
    gboolean dct8x8_allowed = pps->transform_8x8_mode_flag;
    gboolean qp_delta_nonzero = FALSE;
    guint16 cbp = 0;
    guint16 left_cbp_ctx = left_cbp;
    guint16 top_cbp_ctx = 0;
    guint x4;
    guint y4;

    if (mb_addr >= total_mbs)
      goto done;

    memset (ref_grid, 0xFF, sizeof (ref_grid));
    top_non_skip_mb = top_non_skip + mb_x;
    top_cbp_mb = top_cbp + mb_x;
    top_transform8x8_mb = top_transform8x8 + mb_x;
    top_chroma_pred_mode_mb = top_chroma_pred_mode + mb_x;
    top_luma_nnz_mb = top_luma_nnz + mb_x * 4;
    top_chroma_u_nnz_mb = top_chroma_u_nnz + mb_x * 2;
    top_chroma_v_nnz_mb = top_chroma_v_nnz + mb_x * 2;
    top_ref = top_ref_all + mb_x * 4;
    top_mvd = top_mvd_all + mb_x * 4;
    top_cbp_ctx = *top_cbp_mb;

    if (mb_x == 0)
      left_cbp_ctx = 0x00Fu;
    if (mb_y == 0)
      top_cbp_ctx = 0x00Fu;

    skip = gst_h264_parser_decode_cabac_mb_skip (&cabac, state, left_non_skip,
        *top_non_skip_mb != 0);
    if (skip) {
      *used_ref_mask_l0 |= 1u;
      gst_h264_parser_fill_ref_rect (ref_grid, 0, 0, 4, 4, 0);
      gst_h264_parser_fill_mvd_rect (mvd_grid, 0, 0, 4, 4, 0, 0);
      non_skip = FALSE;
    } else {
      GstH264CabacPMbType mb_type;

      non_skip = TRUE;
      mb_type = gst_h264_parser_decode_cabac_p_mb_type (&cabac, state);
      if (mb_type == GST_H264_CABAC_P_MB_UNSUPPORTED) {
        gint intra_mb_type = gst_h264_parser_decode_cabac_intra_mb_type (&cabac,
            state);

        left_cbp_ctx = mb_x == 0 ? 0x7CFu : left_cbp;
        top_cbp_ctx = mb_y == 0 ? 0x7CFu : *top_cbp_mb;

        if (intra_mb_type > 0 && intra_mb_type < 25) {
          if (!gst_h264_parser_skip_cabac_intra16x16 (&cabac, state,
                  left_cbp_ctx, top_cbp_ctx, left_chroma_pred_mode,
                  *top_chroma_pred_mode_mb, luma_nnz, top_luma_nnz_mb,
                  left_luma_nnz, chroma_nnz, top_chroma_u_nnz_mb,
                  left_chroma_u_nnz, top_chroma_v_nnz_mb, left_chroma_v_nnz,
                  last_qscale_nonzero, intra_mb_type, &cbp, &chroma_pred_mode,
                  &qp_delta_nonzero))
            goto done;
        } else {
          goto done;
        }

        goto finish_mb;
      }

      if (mb_type == GST_H264_CABAC_P_MB_8X8) {
        GstH264CabacPSubMbType sub_mb_type[4];
        guint8 sub_ref[4];
        guint i;

        for (i = 0; i < 4; i++) {
          gint sx = (i & 1) ? 2 : 0;
          gint sy = (i & 2) ? 2 : 0;

          sub_mb_type[i] = gst_h264_parser_decode_cabac_p_sub_mb_type (&cabac,
              state);
          if (!gst_h264_parser_decode_cabac_mb_ref (&cabac, state,
                  gst_h264_parser_left_ref (ref_grid, left_ref, sx, sy),
                  gst_h264_parser_top_ref (ref_grid, top_ref, sx, sy),
                  slice->num_ref_idx_l0_active_minus1 + 1, &sub_ref[i]))
            goto done;
          *used_ref_mask_l0 |= 1u << sub_ref[i];
          gst_h264_parser_fill_ref_rect (ref_grid, sx, sy, 2, 2, sub_ref[i]);
        }

        for (i = 0; i < 4; i++) {
          if (sub_mb_type[i] != GST_H264_CABAC_P_SUB_MB_8X8)
            dct8x8_allowed = FALSE;
        }

        for (i = 0; i < 4; i++) {
          static const guint8 part_coords[4][2] = { {0, 0}, {1, 0}, {0, 1}, {1,
              1} };
          static const guint8 part_indices_8x8[1] = { 0 };
          static const guint8 part_indices_8x4[2] = { 0, 2 };
          static const guint8 part_indices_4x8[2] = { 0, 1 };
          static const guint8 part_indices_4x4[4] = { 0, 1, 2, 3 };
          const guint8 *part_indices = NULL;
          guint part_count = 0;
          guint part;
          gint sx = (i & 1) ? 2 : 0;
          gint sy = (i & 2) ? 2 : 0;

          switch (sub_mb_type[i]) {
            case GST_H264_CABAC_P_SUB_MB_8X8:
              part_indices = part_indices_8x8;
              part_count = 1;
              break;
            case GST_H264_CABAC_P_SUB_MB_8X4:
              part_indices = part_indices_8x4;
              part_count = 2;
              break;
            case GST_H264_CABAC_P_SUB_MB_4X8:
              part_indices = part_indices_4x8;
              part_count = 2;
              break;
            case GST_H264_CABAC_P_SUB_MB_4X4:
              part_indices = part_indices_4x4;
              part_count = 4;
              break;
          }

          for (part = 0; part < part_count; part++) {
            gint local = part_indices[part];
            gint px = sx + part_coords[local][0];
            gint py = sy + part_coords[local][1];
            gint16 mvd_x;
            gint16 mvd_y;
            gint w = 1;
            gint h = 1;

            if (!gst_h264_parser_decode_cabac_mb_mvd (&cabac, state, mvd_grid,
                    left_mvd, top_mvd, px, py, &mvd_x, &mvd_y))
              goto done;

            switch (sub_mb_type[i]) {
              case GST_H264_CABAC_P_SUB_MB_8X8:
                w = 2;
                h = 2;
                break;
              case GST_H264_CABAC_P_SUB_MB_8X4:
                w = 2;
                h = 1;
                break;
              case GST_H264_CABAC_P_SUB_MB_4X8:
                w = 1;
                h = 2;
                break;
              case GST_H264_CABAC_P_SUB_MB_4X4:
                w = 1;
                h = 1;
                break;
            }

            gst_h264_parser_fill_mvd_rect (mvd_grid, px, py, w, h, mvd_x,
                mvd_y);
          }
        }
      } else {
        guint part;
        guint part_count = mb_type == GST_H264_CABAC_P_MB_16X16 ? 1 : 2;
        guint8 part_ref[2] = { 0, 0 };

        for (part = 0; part < part_count; part++) {
          gint px = 0;
          gint py = 0;
          gint w = 4;
          gint h = 4;

          if (mb_type == GST_H264_CABAC_P_MB_16X8) {
            py = part == 0 ? 0 : 2;
            h = 2;
          } else if (mb_type == GST_H264_CABAC_P_MB_8X16) {
            px = part == 0 ? 0 : 2;
            w = 2;
          }

          if (!gst_h264_parser_decode_cabac_mb_ref (&cabac, state,
                  gst_h264_parser_left_ref (ref_grid, left_ref, px, py),
                  gst_h264_parser_top_ref (ref_grid, top_ref, px, py),
                  slice->num_ref_idx_l0_active_minus1 + 1, &part_ref[part]))
            goto done;
          *used_ref_mask_l0 |= 1u << part_ref[part];
          gst_h264_parser_fill_ref_rect (ref_grid, px, py, w, h, part_ref[part]);
        }

        for (part = 0; part < part_count; part++) {
          gint px = 0;
          gint py = 0;
          gint w = 4;
          gint h = 4;
          gint16 mvd_x;
          gint16 mvd_y;

          if (mb_type == GST_H264_CABAC_P_MB_16X8) {
            py = part == 0 ? 0 : 2;
            h = 2;
          } else if (mb_type == GST_H264_CABAC_P_MB_8X16) {
            px = part == 0 ? 0 : 2;
            w = 2;
          }

          if (!gst_h264_parser_decode_cabac_mb_mvd (&cabac, state, mvd_grid,
                  left_mvd, top_mvd, px, py, &mvd_x, &mvd_y))
            goto done;
          gst_h264_parser_fill_mvd_rect (mvd_grid, px, py, w, h, mvd_x, mvd_y);
        }
      }

      cbp = gst_h264_parser_decode_cabac_mb_cbp_luma (&cabac, state, left_cbp,
          *top_cbp_mb);
      cbp |= gst_h264_parser_decode_cabac_mb_cbp_chroma (&cabac, state,
          left_cbp, *top_cbp_mb) << 4;
      if (cbp != 0 && !gst_h264_parser_skip_cabac_inter_residual (&cabac, state,
              left_cbp_ctx, top_cbp_ctx, &cbp, dct8x8_allowed,
              *top_transform8x8_mb ? 1 : 0,
              left_transform8x8 ? 1 : 0, luma_nnz, top_luma_nnz_mb,
              left_luma_nnz, chroma_nnz, top_chroma_u_nnz_mb,
              left_chroma_u_nnz, top_chroma_v_nnz_mb, left_chroma_v_nnz,
              last_qscale_nonzero, &qp_delta_nonzero, &transform8x8))
        goto done;
finish_mb:
      last_qscale_nonzero = qp_delta_nonzero;
    }

    for (x4 = 0; x4 < 4; x4++) {
      top_ref[x4] = ref_grid[3][x4];
      top_mvd[x4][0] = mvd_grid[3][x4][0];
      top_mvd[x4][1] = mvd_grid[3][x4][1];
      top_luma_nnz_mb[x4] = luma_nnz[3][x4];
    }
    for (y4 = 0; y4 < 4; y4++) {
      left_ref[y4] = ref_grid[y4][3];
      left_mvd[y4][0] = mvd_grid[y4][3][0];
      left_mvd[y4][1] = mvd_grid[y4][3][1];
      left_luma_nnz[y4] = luma_nnz[y4][3];
    }
    top_chroma_u_nnz_mb[0] = chroma_nnz[0][1][0];
    top_chroma_u_nnz_mb[1] = chroma_nnz[0][1][1];
    top_chroma_v_nnz_mb[0] = chroma_nnz[1][1][0];
    top_chroma_v_nnz_mb[1] = chroma_nnz[1][1][1];
    left_chroma_u_nnz[0] = chroma_nnz[0][0][1];
    left_chroma_u_nnz[1] = chroma_nnz[0][1][1];
    left_chroma_v_nnz[0] = chroma_nnz[1][0][1];
    left_chroma_v_nnz[1] = chroma_nnz[1][1][1];
    *top_non_skip_mb = non_skip ? 1 : 0;
    *top_transform8x8_mb = transform8x8 ? 1 : 0;
    *top_chroma_pred_mode_mb = chroma_pred_mode;
    *top_cbp_mb = cbp;
    left_chroma_pred_mode = chroma_pred_mode;
    left_non_skip = non_skip;
    left_transform8x8 = transform8x8;
    left_cbp = cbp;

    mb_addr++;
    mb_x++;
    if (mb_x >= mb_width) {
      mb_x = 0;
      mb_y++;
      left_non_skip = FALSE;
      left_transform8x8 = FALSE;
      left_cbp = 0;
      memset (left_ref, 0xFF, sizeof (left_ref));
      memset (left_mvd, 0, sizeof (left_mvd));
      memset (left_luma_nnz, 0, sizeof (left_luma_nnz));
      memset (left_chroma_u_nnz, 0, sizeof (left_chroma_u_nnz));
      memset (left_chroma_v_nnz, 0, sizeof (left_chroma_v_nnz));
      left_chroma_pred_mode = 0;
    }

    if (gst_h264_parser_get_cabac_terminate (&cabac))
      break;
  }

done:
  g_free (top_mvd_all);
  g_free (top_ref_all);
  g_free (top_chroma_v_nnz);
  g_free (top_chroma_u_nnz);
  g_free (top_luma_nnz);
  g_free (top_chroma_pred_mode);
  g_free (top_transform8x8);
  g_free (top_cbp);
  g_free (top_non_skip);
  g_free (rbsp);
  return *used_ref_mask_l0 != 0;
}

gboolean
gst_h264_parser_identify_slice_ref_usage (GstH264NalParser * nalparser,
    GstH264NalUnit * nalu, GstH264SliceHdr * slice, guint32 * used_ref_mask_l0)
{
  const GstH264PPS *pps;
  const GstH264SPS *sps;
  NalReader nr;
  guint32 mb_skip_run = 0;
  guint32 total_mbs;
  guint32 remaining_mbs;

  (void) nalparser;

  if (used_ref_mask_l0 == NULL || nalu == NULL || slice == NULL || slice->pps == NULL)
    return FALSE;

  *used_ref_mask_l0 = 0;
  pps = slice->pps;
  sps = pps->sequence;
  if (sps == NULL)
    return FALSE;

  if (!GST_H264_IS_P_SLICE (slice) && !GST_H264_IS_SP_SLICE (slice))
    return FALSE;
  if (slice->field_pic_flag || !sps->frame_mbs_only_flag ||
      sps->mb_adaptive_frame_field_flag)
    return FALSE;
  if (slice->ref_pic_list_modification_flag_l0)
    return FALSE;

  total_mbs = (sps->pic_width_in_mbs_minus1 + 1) *
      (sps->pic_height_in_map_units_minus1 + 1);
  if (slice->first_mb_in_slice >= total_mbs)
    return FALSE;
  remaining_mbs = total_mbs - slice->first_mb_in_slice;

  /* Exact by construction: only one list0 reference exists. */
  if (slice->num_ref_idx_l0_active_minus1 == 0) {
    *used_ref_mask_l0 = 1u;
    return TRUE;
  }

  if (pps->entropy_coding_mode_flag) {
    /* Minimal FFmpeg-guided CABAC port:
     * walk frame-coded P/SP slices through skip flags, mb_type/sub_mb_type,
     * ref_idx_l0, mvd and inter residual syntax so used_ref_mask_l0 survives
     * coded macroblocks instead of dropping to generic active-list fallback. */
    return gst_h264_parser_identify_cabac_p_ref_usage (pps, sps, nalu, slice,
        total_mbs, used_ref_mask_l0);
  }

  /* Minimal first port of the FFmpeg inter-MB path for CAVLC:
   * if mb_skip_run consumes the whole slice, every macroblock is an implicit
   * P-skip using ref_idx_l0 = 0. */
  nal_reader_init (&nr, nalu->data + nalu->offset + nalu->header_bytes,
      nalu->size - nalu->header_bytes);
  if (!nal_reader_skip_long (&nr, slice->header_size))
    return FALSE;
  if (!nal_reader_get_ue (&nr, &mb_skip_run))
    return FALSE;

  if (mb_skip_run == remaining_mbs) {
    *used_ref_mask_l0 = 1u;
    return TRUE;
  }

  return FALSE;
}
// Alex: too much details //

/* Free MVC-specific data from subset SPS header */
static void
gst_h264_sps_mvc_clear (GstH264SPS * sps)
{
  GstH264SPSExtMVC *const mvc = &sps->extension.mvc;
  guint i, j;

  g_assert (sps->extension_type == GST_H264_NAL_EXTENSION_MVC);

  g_free (mvc->view);
  mvc->view = NULL;

  for (i = 0; i <= mvc->num_level_values_signalled_minus1; i++) {
    GstH264SPSExtMVCLevelValue *const level_value = &mvc->level_value[i];

    for (j = 0; j <= level_value->num_applicable_ops_minus1; j++) {
      g_free (level_value->applicable_op[j].target_view_id);
      level_value->applicable_op[j].target_view_id = NULL;
    }
    g_free (level_value->applicable_op);
    level_value->applicable_op = NULL;
  }
  g_free (mvc->level_value);
  mvc->level_value = NULL;

  /* All meaningful MVC info are now gone, just pretend to be a
   * standard AVC struct now */
  sps->extension_type = GST_H264_NAL_EXTENSION_NONE;
}

/**
 * gst_h264_sps_clear:
 * @sps: The #GstH264SPS to free
 *
 * Clears all @sps internal resources.
 *
 * Since: 1.6
 */
void
gst_h264_sps_clear (GstH264SPS * sps)
{
  g_return_if_fail (sps != NULL);

  switch (sps->extension_type) {
    case GST_H264_NAL_EXTENSION_MVC:
      gst_h264_sps_mvc_clear (sps);
      break;
  }
}

/**
 * gst_h264_sei_clear:
 * sei: The #GstH264SEIMessage to clear
 *
 * Frees allocated data in @sei if any.
 *
 * Since: 1.18
 */
void
gst_h264_sei_clear (GstH264SEIMessage * sei)
{
  switch (sei->payloadType) {
    case GST_H264_SEI_REGISTERED_USER_DATA:{
      GstH264RegisteredUserData *rud = &sei->payload.registered_user_data;

      g_free ((guint8 *) rud->data);
      rud->data = NULL;
      break;
    }
    case GST_H264_SEI_USER_DATA_UNREGISTERED:{
      GstH264UserDataUnregistered *udu = &sei->payload.user_data_unregistered;

      g_free ((guint8 *) udu->data);
      udu->data = NULL;
      break;
    }
    case GST_H264_SEI_UNHANDLED_PAYLOAD:{
      GstH264SEIUnhandledPayload *payload = &sei->payload.unhandled_payload;

      g_free (payload->data);
      payload->data = NULL;
      payload->size = 0;
      break;
    }
    default:
      break;
  }
}

/**
 * gst_h264_parser_parse_sei:
 * @nalparser: a #GstH264NalParser
 * @nalu: The %GST_H264_NAL_SEI #GstH264NalUnit to parse
 * @messages: The GArray of #GstH264SEIMessage to fill. The caller must free it when done.
 *
 * Parses @nalu containing one or more Supplementary Enhancement Information messages,
 * and allocates and fills the @messages array.
 *
 * Returns: a #GstH264ParserResult
 */
GstH264ParserResult
gst_h264_parser_parse_sei (GstH264NalParser * nalparser, GstH264NalUnit * nalu,
    GArray ** messages)
{
  NalReader nr;
  GstH264SEIMessage sei;
  GstH264ParserResult res;

  GST_DEBUG ("parsing SEI nal");
  nal_reader_init (&nr, nalu->data + nalu->offset + nalu->header_bytes,
      nalu->size - nalu->header_bytes);
  *messages = g_array_new (FALSE, FALSE, sizeof (GstH264SEIMessage));
  g_array_set_clear_func (*messages, (GDestroyNotify) gst_h264_sei_clear);

  do {
    res = gst_h264_parser_parse_sei_message (nalparser, &nr, &sei);
    if (res == GST_H264_PARSER_OK)
      g_array_append_val (*messages, sei);
    else
      break;
  } while (nal_reader_has_more_data (&nr));

  return res;
}

/**
 * gst_h264_parser_update_sps:
 * @nalparser: a #GstH264NalParser
 * @sps: (transfer none): a #GstH264SPS.
 *
 * Replace internal Sequence Parameter Set struct corresponding to id of @sps
 * with @sps. @nalparser will mark @sps as last parsed sps.
 *
 * Returns: a #GstH264ParserResult
 *
 * Since: 1.18
 */
GstH264ParserResult
gst_h264_parser_update_sps (GstH264NalParser * nalparser, GstH264SPS * sps)
{
  g_return_val_if_fail (nalparser != NULL, GST_H264_PARSER_ERROR);
  g_return_val_if_fail (sps != NULL, GST_H264_PARSER_ERROR);
  g_return_val_if_fail (sps->id >= 0 && sps->id < GST_H264_MAX_SPS_COUNT,
      GST_H264_PARSER_ERROR);

  if (!sps->valid) {
    GST_WARNING ("Cannot update with invalid SPS");
    return GST_H264_PARSER_ERROR;
  }

  GST_DEBUG ("Updating sequence parameter set with id: %d", sps->id);

  if (!gst_h264_sps_copy (&nalparser->sps[sps->id], sps))
    return GST_H264_PARSER_ERROR;

  nalparser->last_sps = &nalparser->sps[sps->id];

  return GST_H264_PARSER_OK;
}

/**
 * gst_h264_parser_update_pps:
 * @nalparser: a #GstH264NalParser
 * @pps: (transfer none): a #GstH264PPS.
 *
 * Replace internal Picture Parameter Set struct corresponding to id of @pps
 * with @pps. @nalparser will mark @pps as last parsed pps.
 *
 * Returns: a #GstH264ParserResult
 *
 * Since: 1.18
 */
GstH264ParserResult
gst_h264_parser_update_pps (GstH264NalParser * nalparser, GstH264PPS * pps)
{
  GstH264SPS *sps;

  g_return_val_if_fail (nalparser != NULL, GST_H264_PARSER_ERROR);
  g_return_val_if_fail (pps != NULL, GST_H264_PARSER_ERROR);
  g_return_val_if_fail (pps->id >= 0 && pps->id < GST_H264_MAX_PPS_COUNT,
      GST_H264_PARSER_ERROR);

  if (!pps->valid) {
    GST_WARNING ("Cannot update with invalid PPS");
    return GST_H264_PARSER_ERROR;
  }

  if (!pps->sequence) {
    GST_WARNING ("No linked SPS struct");
    return GST_H264_PARSER_BROKEN_LINK;
  }

  sps = gst_h264_parser_get_sps (nalparser, pps->sequence->id);
  if (!sps || sps != pps->sequence) {
    GST_WARNING ("Linked SPS is not identical to internal SPS");
    return GST_H264_PARSER_BROKEN_LINK;
  }

  GST_DEBUG ("Updating picture parameter set with id: %d", pps->id);

  if (!gst_h264_pps_copy (&nalparser->pps[pps->id], pps))
    return GST_H264_PARSER_ERROR;

  nalparser->last_pps = &nalparser->pps[pps->id];

  return GST_H264_PARSER_OK;
}

/**
 * gst_h264_quant_matrix_8x8_get_zigzag_from_raster:
 * @out_quant: (out): The resulting quantization matrix
 * @quant: The source quantization matrix
 *
 * Converts quantization matrix @quant from raster scan order to
 * zigzag scan order and store the resulting factors into @out_quant.
 *
 * Note: it is an error to pass the same table in both @quant and
 * @out_quant arguments.
 *
 * Since: 1.4
 */
void
gst_h264_quant_matrix_8x8_get_zigzag_from_raster (guint8 out_quant[64],
    const guint8 quant[64])
{
  guint i;

  g_return_if_fail (out_quant != quant);

  for (i = 0; i < 64; i++)
    out_quant[i] = quant[zigzag_8x8[i]];
}

/**
 * gst_h264_quant_matrix_8x8_get_raster_from_zigzag:
 * @out_quant: (out): The resulting quantization matrix
 * @quant: The source quantization matrix
 *
 * Converts quantization matrix @quant from zigzag scan order to
 * raster scan order and store the resulting factors into @out_quant.
 *
 * Note: it is an error to pass the same table in both @quant and
 * @out_quant arguments.
 *
 * Since: 1.4
 */
void
gst_h264_quant_matrix_8x8_get_raster_from_zigzag (guint8 out_quant[64],
    const guint8 quant[64])
{
  guint i;

  g_return_if_fail (out_quant != quant);

  for (i = 0; i < 64; i++)
    out_quant[zigzag_8x8[i]] = quant[i];
}

/**
 * gst_h264_quant_matrix_4x4_get_zigzag_from_raster:
 * @out_quant: (out): The resulting quantization matrix
 * @quant: The source quantization matrix
 *
 * Converts quantization matrix @quant from raster scan order to
 * zigzag scan order and store the resulting factors into @out_quant.
 *
 * Note: it is an error to pass the same table in both @quant and
 * @out_quant arguments.
 *
 * Since: 1.4
 */
void
gst_h264_quant_matrix_4x4_get_zigzag_from_raster (guint8 out_quant[16],
    const guint8 quant[16])
{
  guint i;

  g_return_if_fail (out_quant != quant);

  for (i = 0; i < 16; i++)
    out_quant[i] = quant[zigzag_4x4[i]];
}

/**
 * gst_h264_quant_matrix_4x4_get_raster_from_zigzag:
 * @out_quant: (out): The resulting quantization matrix
 * @quant: The source quantization matrix
 *
 * Converts quantization matrix @quant from zigzag scan order to
 * raster scan order and store the resulting factors into @out_quant.
 *
 * Note: it is an error to pass the same table in both @quant and
 * @out_quant arguments.
 *
 * Since: 1.4
 */
void
gst_h264_quant_matrix_4x4_get_raster_from_zigzag (guint8 out_quant[16],
    const guint8 quant[16])
{
  guint i;

  g_return_if_fail (out_quant != quant);

  for (i = 0; i < 16; i++)
    out_quant[zigzag_4x4[i]] = quant[i];
}

/**
 * gst_h264_video_calculate_framerate:
 * @sps: Current Sequence Parameter Set
 * @field_pic_flag: Current @field_pic_flag, obtained from latest slice header
 * @pic_struct: @pic_struct value if available, 0 otherwise
 * @fps_num: (out): The resulting fps numerator
 * @fps_den: (out): The resulting fps denominator
 *
 * Calculate framerate of a video sequence using @sps VUI information,
 * @field_pic_flag from a slice header and @pic_struct from #GstH264PicTiming SEI
 * message.
 *
 * If framerate is variable or can't be determined, @fps_num will be set to 0
 * and @fps_den to 1.
 */
void
gst_h264_video_calculate_framerate (const GstH264SPS * sps,
    guint field_pic_flag, guint pic_struct, gint * fps_num, gint * fps_den)
{
  gint64 num = 0;
  gint64 den = 1;

  /* To calculate framerate, we use this formula:
   *          time_scale                1                         1
   * fps = -----------------  x  ---------------  x  ------------------------
   *       num_units_in_tick     DeltaTfiDivisor     (field_pic_flag ? 2 : 1)
   *
   * See H264 specification E2.1 for more details.
   */

  if (sps) {
    if (sps->vui_parameters_present_flag) {
      const GstH264VUIParams *vui = &sps->vui_parameters;
      if (vui->timing_info_present_flag) {
        int delta_tfi_divisor = 1;
        num = vui->time_scale;
        den = vui->num_units_in_tick;

        if (vui->pic_struct_present_flag) {
          switch (pic_struct) {
            case 1:
            case 2:
              delta_tfi_divisor = 1;
              break;
            case 0:
            case 3:
            case 4:
              delta_tfi_divisor = 2;
              break;
            case 5:
            case 6:
              delta_tfi_divisor = 3;
              break;
            case 7:
              delta_tfi_divisor = 4;
              break;
            case 8:
              delta_tfi_divisor = 6;
              break;
          }
        } else {
          delta_tfi_divisor = field_pic_flag ? 1 : 2;
        }
        den *= delta_tfi_divisor;

        /* Picture is two fields ? */
        den *= (field_pic_flag ? 2 : 1);
      }
    }
  }

  while (num > G_MAXINT32 || den > G_MAXINT32) {
    num >>= 1;
    den >>= 1;
  }

  *fps_num = num;
  *fps_den = den;
}

static gboolean
gst_h264_write_sei_registered_user_data (NalWriter * nw,
    GstH264RegisteredUserData * rud)
{
  WRITE_UINT8 (nw, rud->country_code, 8);
  if (rud->country_code == 0xff)
    WRITE_UINT8 (nw, rud->country_code_extension, 8);

  WRITE_BYTES (nw, rud->data, rud->size);

  return TRUE;

error:
  return FALSE;
}

static gboolean
gst_h264_write_sei_user_data_unregistered (NalWriter * nw,
    GstH264UserDataUnregistered * udu)
{
  WRITE_BYTES (nw, udu->uuid, 16);
  WRITE_BYTES (nw, udu->data, udu->size);

  return TRUE;

error:
  return FALSE;
}

static gboolean
gst_h264_write_sei_frame_packing (NalWriter * nw,
    GstH264FramePacking * frame_packing)
{
  WRITE_UE (nw, frame_packing->frame_packing_id);
  WRITE_UINT8 (nw, frame_packing->frame_packing_cancel_flag, 1);

  if (!frame_packing->frame_packing_cancel_flag) {
    WRITE_UINT8 (nw, frame_packing->frame_packing_type, 7);
    WRITE_UINT8 (nw, frame_packing->quincunx_sampling_flag, 1);
    WRITE_UINT8 (nw, frame_packing->content_interpretation_type, 6);
    WRITE_UINT8 (nw, frame_packing->spatial_flipping_flag, 1);
    WRITE_UINT8 (nw, frame_packing->frame0_flipped_flag, 1);
    WRITE_UINT8 (nw, frame_packing->field_views_flag, 1);
    WRITE_UINT8 (nw, frame_packing->current_frame_is_frame0_flag, 1);
    WRITE_UINT8 (nw, frame_packing->frame0_self_contained_flag, 1);
    WRITE_UINT8 (nw, frame_packing->frame1_self_contained_flag, 1);

    if (!frame_packing->quincunx_sampling_flag &&
        frame_packing->frame_packing_type !=
        GST_H264_FRAME_PACKING_TEMPORAL_INTERLEAVING) {
      WRITE_UINT8 (nw, frame_packing->frame0_grid_position_x, 4);
      WRITE_UINT8 (nw, frame_packing->frame0_grid_position_y, 4);
      WRITE_UINT8 (nw, frame_packing->frame1_grid_position_x, 4);
      WRITE_UINT8 (nw, frame_packing->frame1_grid_position_y, 4);
    }

    /* frame_packing_arrangement_reserved_byte */
    WRITE_UINT8 (nw, 0, 8);
    WRITE_UE (nw, frame_packing->frame_packing_repetition_period);
  }

  /* frame_packing_arrangement_extension_flag */
  WRITE_UINT8 (nw, 0, 1);

  return TRUE;

error:
  return FALSE;
}

static gboolean
gst_h264_write_sei_mastering_display_colour_volume (NalWriter * nw,
    GstH264MasteringDisplayColourVolume * mdcv)
{
  gint i;

  for (i = 0; i < 3; i++) {
    WRITE_UINT16 (nw, mdcv->display_primaries_x[i], 16);
    WRITE_UINT16 (nw, mdcv->display_primaries_y[i], 16);
  }

  WRITE_UINT16 (nw, mdcv->white_point_x, 16);
  WRITE_UINT16 (nw, mdcv->white_point_y, 16);
  WRITE_UINT32 (nw, mdcv->max_display_mastering_luminance, 32);
  WRITE_UINT32 (nw, mdcv->min_display_mastering_luminance, 32);

  return TRUE;

error:
  return FALSE;
}

static gboolean
gst_h264_write_sei_content_light_level_info (NalWriter * nw,
    GstH264ContentLightLevel * cll)
{
  WRITE_UINT16 (nw, cll->max_content_light_level, 16);
  WRITE_UINT16 (nw, cll->max_pic_average_light_level, 16);

  return TRUE;

error:
  return FALSE;
}

static gboolean
gst_h264_write_sei_pic_timing (NalWriter * nw, GstH264PicTiming * tim)
{
  if (tim->CpbDpbDelaysPresentFlag) {
    WRITE_UINT32 (nw, tim->cpb_removal_delay,
        tim->cpb_removal_delay_length_minus1 + 1);
    WRITE_UINT32 (nw, tim->dpb_output_delay,
        tim->dpb_output_delay_length_minus1 + 1);
  }

  if (tim->pic_struct_present_flag) {
    const guint8 num_clock_ts_table[9] = {
      1, 1, 1, 2, 2, 3, 3, 2, 3
    };
    guint8 num_clock_num_ts;
    guint i;

    WRITE_UINT8 (nw, tim->pic_struct, 4);

    num_clock_num_ts = num_clock_ts_table[tim->pic_struct];
    for (i = 0; i < num_clock_num_ts; i++) {
      WRITE_UINT8 (nw, tim->clock_timestamp_flag[i], 1);
      if (tim->clock_timestamp_flag[i]) {
        GstH264ClockTimestamp *timestamp = &tim->clock_timestamp[i];

        WRITE_UINT8 (nw, timestamp->ct_type, 2);
        WRITE_UINT8 (nw, timestamp->nuit_field_based_flag, 1);
        WRITE_UINT8 (nw, timestamp->counting_type, 5);
        WRITE_UINT8 (nw, timestamp->full_timestamp_flag, 1);
        WRITE_UINT8 (nw, timestamp->discontinuity_flag, 1);
        WRITE_UINT8 (nw, timestamp->cnt_dropped_flag, 1);
        WRITE_UINT8 (nw, timestamp->n_frames, 8);

        if (timestamp->full_timestamp_flag) {
          WRITE_UINT8 (nw, timestamp->seconds_value, 6);
          WRITE_UINT8 (nw, timestamp->minutes_value, 6);
          WRITE_UINT8 (nw, timestamp->hours_value, 5);
        } else {
          WRITE_UINT8 (nw, timestamp->seconds_flag, 1);
          if (timestamp->seconds_flag) {
            WRITE_UINT8 (nw, timestamp->seconds_value, 6);
            WRITE_UINT8 (nw, timestamp->minutes_flag, 1);
            if (timestamp->minutes_flag) {
              WRITE_UINT8 (nw, timestamp->minutes_value, 6);
              WRITE_UINT8 (nw, timestamp->hours_flag, 1);
              if (timestamp->hours_flag)
                WRITE_UINT8 (nw, timestamp->hours_value, 5);
            }
          }
        }

        if (tim->time_offset_length > 0) {
          WRITE_UINT32 (nw, timestamp->time_offset, tim->time_offset_length);
        }
      }
    }
  }

  return TRUE;

error:
  return FALSE;
}

static GstMemory *
gst_h264_create_sei_memory_internal (guint8 nal_prefix_size,
    gboolean packetized, GArray * messages)
{
  NalWriter nw;
  gint i;
  gboolean have_written_data = FALSE;

  nal_writer_init (&nw, nal_prefix_size, packetized);

  if (messages->len == 0)
    goto error;

  GST_DEBUG ("Create SEI nal from array, len: %d", messages->len);

  /* nal header */
  /* forbidden_zero_bit */
  WRITE_UINT8 (&nw, 0, 1);
  /* nal_ref_idc, zero for sei nalu */
  WRITE_UINT8 (&nw, 0, 2);
  /* nal_unit_type */
  WRITE_UINT8 (&nw, GST_H264_NAL_SEI, 5);

  for (i = 0; i < messages->len; i++) {
    GstH264SEIMessage *msg = &g_array_index (messages, GstH264SEIMessage, i);
    guint32 payload_size_data = 0;
    guint32 payload_size_in_bits = 0;
    guint32 payload_type_data = msg->payloadType;
    gboolean need_align = FALSE;

    switch (payload_type_data) {
      case GST_H264_SEI_REGISTERED_USER_DATA:{
        GstH264RegisteredUserData *rud = &msg->payload.registered_user_data;

        /* itu_t_t35_country_code: 8 bits */
        payload_size_data = 1;
        if (rud->country_code == 0xff) {
          /* itu_t_t35_country_code_extension_byte */
          payload_size_data++;
        }

        payload_size_data += rud->size;
        break;
      }
      case GST_H264_SEI_USER_DATA_UNREGISTERED:{
        GstH264UserDataUnregistered *udu = &msg->payload.user_data_unregistered;

        payload_size_data = 16 + udu->size;
        break;
      }
      case GST_H264_SEI_FRAME_PACKING:{
        GstH264FramePacking *frame_packing = &msg->payload.frame_packing;
        guint leading_zeros, rest;

        /* frame_packing_arrangement_id: exp-golomb bits */
        count_exp_golomb_bits (frame_packing->frame_packing_id,
            &leading_zeros, &rest);
        payload_size_in_bits = leading_zeros + rest;

        /* frame_packing_arrangement_cancel_flag: 1 bit */
        payload_size_in_bits++;
        if (!frame_packing->frame_packing_cancel_flag) {
          /* frame_packing_arrangement_type: 7 bits
           * quincunx_sampling_flag: 1 bit
           * content_interpretation_type: 6 bit
           * spatial_flipping_flag: 1 bit
           * frame0_flipped_flag: 1 bit
           * field_views_flag: 1 bit
           * current_frame_is_frame0_flag: 1 bit
           * frame0_self_contained_flag: 1 bit
           * frame1_self_contained_flag: 1 bit
           */
          payload_size_in_bits += 20;

          if (!frame_packing->quincunx_sampling_flag &&
              frame_packing->frame_packing_type !=
              GST_H264_FRAME_PACKING_TEMPORAL_INTERLEAVING) {
            /* frame0_grid_position_x: 4bits
             * frame0_grid_position_y: 4bits
             * frame1_grid_position_x: 4bits
             * frame1_grid_position_y: 4bits
             */
            payload_size_in_bits += 16;
          }

          /* frame_packing_arrangement_reserved_byte: 8 bits */
          payload_size_in_bits += 8;

          /* frame_packing_arrangement_repetition_period: exp-golomb bits */
          count_exp_golomb_bits (frame_packing->frame_packing_repetition_period,
              &leading_zeros, &rest);
          payload_size_in_bits += (leading_zeros + rest);
        }
        /* frame_packing_arrangement_extension_flag: 1 bit */
        payload_size_in_bits++;

        payload_size_data = payload_size_in_bits >> 3;

        if ((payload_size_in_bits & 0x7) != 0) {
          GST_INFO ("Bits for Frame Packing SEI is not byte aligned");
          payload_size_data++;
          need_align = TRUE;
        }
        break;
      }
      case GST_H264_SEI_MASTERING_DISPLAY_COLOUR_VOLUME:
        /* x, y 16 bits per RGB channel
         * x, y 16 bits white point
         * max, min luminance 32 bits
         *
         * (2 * 2 * 3) + (2 * 2) + (4 * 2) = 24 bytes
         */
        payload_size_data = 24;
        break;
      case GST_H264_SEI_CONTENT_LIGHT_LEVEL:
        /* maxCLL and maxFALL per 16 bits
         *
         * 2 * 2 = 4 bytes
         */
        payload_size_data = 4;
        break;
      case GST_H264_SEI_PIC_TIMING:{
        GstH264PicTiming *tim = &msg->payload.pic_timing;
        const guint8 num_clock_ts_table[9] = {
          1, 1, 1, 2, 2, 3, 3, 2, 3
        };
        guint8 num_clock_num_ts;
        guint i;

        if (!tim->CpbDpbDelaysPresentFlag && !tim->pic_struct_present_flag) {
          GST_WARNING
              ("Both CpbDpbDelaysPresentFlag and pic_struct_present_flag are zero");
          break;
        }

        if (tim->CpbDpbDelaysPresentFlag) {
          payload_size_in_bits = tim->cpb_removal_delay_length_minus1 + 1;
          payload_size_in_bits += tim->dpb_output_delay_length_minus1 + 1;
        }

        if (tim->pic_struct_present_flag) {
          /* pic_struct: 4bits */
          payload_size_in_bits += 4;

          num_clock_num_ts = num_clock_ts_table[tim->pic_struct];
          for (i = 0; i < num_clock_num_ts; i++) {
            /* clock_timestamp_flag: 1bit */
            payload_size_in_bits++;

            if (tim->clock_timestamp_flag[i]) {
              GstH264ClockTimestamp *timestamp = &tim->clock_timestamp[i];

              /* ct_type: 2bits
               * nuit_field_based_flag: 1bit
               * counting_type: 5bits
               * full_timestamp_flag: 1bit
               * discontinuity_flag: 1bit
               * cnt_dropped_flag: 1bit
               * n_frames: 8bits
               */
              payload_size_in_bits += 19;
              if (timestamp->full_timestamp_flag) {
                /* seconds_value: 6bits
                 * minutes_value: 6bits
                 * hours_value: 5bits
                 */
                payload_size_in_bits += 17;
              } else {
                /* seconds_flag: 1bit */
                payload_size_in_bits++;

                if (timestamp->seconds_flag) {
                  /* seconds_value: 6bits
                   * minutes_flag: 1bit
                   */
                  payload_size_in_bits += 7;
                  if (timestamp->minutes_flag) {
                    /* minutes_value: 6bits
                     * hours_flag: 1bits
                     */
                    payload_size_in_bits += 7;
                    if (timestamp->hours_flag) {
                      /* hours_value: 5bits */
                      payload_size_in_bits += 5;
                    }
                  }
                }
              }

              /* time_offset_length bits */
              payload_size_in_bits += tim->time_offset_length;
            }
          }
        }

        payload_size_data = payload_size_in_bits >> 3;

        if ((payload_size_in_bits & 0x7) != 0) {
          GST_INFO ("Bits for Picture Timing SEI is not byte aligned");
          payload_size_data++;
          need_align = TRUE;
        }
        break;
      }
      default:
        break;
    }

    if (payload_size_data == 0) {
      GST_FIXME ("Unsupported SEI type %d", msg->payloadType);
      continue;
    }

    /* write payload type bytes */
    while (payload_type_data >= 0xff) {
      WRITE_UINT8 (&nw, 0xff, 8);
      payload_type_data -= 0xff;
    }
    WRITE_UINT8 (&nw, payload_type_data, 8);

    /* write payload size bytes */
    while (payload_size_data >= 0xff) {
      WRITE_UINT8 (&nw, 0xff, 8);
      payload_size_data -= 0xff;
    }
    WRITE_UINT8 (&nw, payload_size_data, 8);

    switch (msg->payloadType) {
      case GST_H264_SEI_REGISTERED_USER_DATA:
        GST_DEBUG ("Writing \"Registered user data\"");
        if (!gst_h264_write_sei_registered_user_data (&nw,
                &msg->payload.registered_user_data)) {
          GST_WARNING ("Failed to write \"Registered user data\"");
          goto error;
        }
        have_written_data = TRUE;
        break;
      case GST_H264_SEI_USER_DATA_UNREGISTERED:
        GST_DEBUG ("Writing \"Unregistered user data\"");
        if (!gst_h264_write_sei_user_data_unregistered (&nw,
                &msg->payload.user_data_unregistered)) {
          GST_WARNING ("Failed to write \"Unregistered user data\"");
          goto error;
        }
        have_written_data = TRUE;
        break;
      case GST_H264_SEI_FRAME_PACKING:
        GST_DEBUG ("Writing \"Frame packing\"");
        if (!gst_h264_write_sei_frame_packing (&nw,
                &msg->payload.frame_packing)) {
          GST_WARNING ("Failed to write \"Frame packing\"");
          goto error;
        }
        have_written_data = TRUE;
        break;
      case GST_H264_SEI_MASTERING_DISPLAY_COLOUR_VOLUME:
        GST_DEBUG ("Writing \"Mastering display colour volume\"");
        if (!gst_h264_write_sei_mastering_display_colour_volume (&nw,
                &msg->payload.mastering_display_colour_volume)) {
          GST_WARNING ("Failed to write \"Mastering display colour volume\"");
          goto error;
        }
        have_written_data = TRUE;
        break;
      case GST_H264_SEI_CONTENT_LIGHT_LEVEL:
        GST_DEBUG ("Writing \"Content light level\"");
        if (!gst_h264_write_sei_content_light_level_info (&nw,
                &msg->payload.content_light_level)) {
          GST_WARNING ("Failed to write \"Content light level\"");
          goto error;
        }
        have_written_data = TRUE;
        break;
      case GST_H264_SEI_PIC_TIMING:
        GST_DEBUG ("Writing \"Picture timing\"");
        if (!gst_h264_write_sei_pic_timing (&nw, &msg->payload.pic_timing)) {
          GST_WARNING ("Failed to write \"Picture timing\"");
          goto error;
        }
        have_written_data = TRUE;
        break;
      default:
        break;
    }

    if (need_align && !nal_writer_do_rbsp_trailing_bits (&nw)) {
      GST_WARNING ("Cannot insert traling bits");
      goto error;
    }
  }

  if (!have_written_data) {
    GST_WARNING ("No written sei data");
    goto error;
  }

  if (!nal_writer_do_rbsp_trailing_bits (&nw)) {
    GST_WARNING ("Failed to insert rbsp trailing bits");
    goto error;
  }

  return nal_writer_reset_and_get_memory (&nw);

error:
  nal_writer_reset (&nw);

  return NULL;
}

/**
 * gst_h264_create_sei_memory:
 * @start_code_prefix_length: a length of start code prefix, must be 3 or 4
 * @messages: (transfer none): a GArray of #GstH264SEIMessage
 *
 * Creates raw byte-stream format (a.k.a Annex B type) SEI nal unit data
 * from @messages
 *
 * Returns: a #GstMemory containing a SEI nal unit
 *
 * Since: 1.18
 */
GstMemory *
gst_h264_create_sei_memory (guint8 start_code_prefix_length, GArray * messages)
{
  g_return_val_if_fail (start_code_prefix_length == 3
      || start_code_prefix_length == 4, NULL);
  g_return_val_if_fail (messages != NULL, NULL);
  g_return_val_if_fail (messages->len > 0, NULL);

  return gst_h264_create_sei_memory_internal (start_code_prefix_length,
      FALSE, messages);
}

/**
 * gst_h264_create_sei_memory_avc:
 * @nal_length_size: a size of nal length field, allowed range is [1, 4]
 * @messages: (transfer none): a GArray of #GstH264SEIMessage
 *
 * Creates raw packetized format SEI nal unit data from @messages
 *
 * Returns: a #GstMemory containing a SEI nal unit
 *
 * Since: 1.18
 */
GstMemory *
gst_h264_create_sei_memory_avc (guint8 nal_length_size, GArray * messages)
{
  g_return_val_if_fail (nal_length_size > 0 && nal_length_size < 5, NULL);
  g_return_val_if_fail (messages != NULL, NULL);
  g_return_val_if_fail (messages->len > 0, NULL);

  return gst_h264_create_sei_memory_internal (nal_length_size, TRUE, messages);
}

static GstBuffer *
gst_h264_parser_insert_sei_internal (GstH264NalParser * nalparser,
    guint8 nal_prefix_size, gboolean packetized, GstBuffer * au,
    GstMemory * sei)
{
  GstH264NalUnit nalu;
  GstMapInfo info;
  GstH264ParserResult pres;
  guint offset = 0;
  GstBuffer *new_buffer = NULL;

  if (!gst_buffer_map (au, &info, GST_MAP_READ)) {
    GST_ERROR ("Cannot map au buffer");
    return NULL;
  }

  /* Find the offset of the first slice */
  do {
    if (packetized) {
      pres = gst_h264_parser_identify_nalu_avc (nalparser,
          info.data, offset, info.size, nal_prefix_size, &nalu);
    } else {
      pres = gst_h264_parser_identify_nalu (nalparser,
          info.data, offset, info.size, &nalu);
    }

    if (pres != GST_H264_PARSER_OK && pres != GST_H264_PARSER_NO_NAL_END) {
      GST_DEBUG ("Failed to identify nal unit, ret: %d", pres);
      gst_buffer_unmap (au, &info);

      return NULL;
    }

    if ((nalu.type >= GST_H264_NAL_SLICE && nalu.type <= GST_H264_NAL_SLICE_IDR)
        || (nalu.type >= GST_H264_NAL_SLICE_AUX
            && nalu.type <= GST_H264_NAL_SLICE_DEPTH)) {
      GST_DEBUG ("Found slice nal type %d at offset %d",
          nalu.type, nalu.sc_offset);
      break;
    }

    offset = nalu.offset + nalu.size;
  } while (pres == GST_H264_PARSER_OK);
  gst_buffer_unmap (au, &info);

  /* found the best position now, create new buffer */
  new_buffer = gst_buffer_new ();

  /* copy all metadata */
  if (!gst_buffer_copy_into (new_buffer, au, GST_BUFFER_COPY_METADATA, 0, -1)) {
    GST_ERROR ("Failed to copy metadata into new buffer");
    gst_clear_buffer (&new_buffer);
    goto out;
  }

  /* copy non-slice nal */
  if (nalu.sc_offset > 0) {
    if (!gst_buffer_copy_into (new_buffer, au,
            GST_BUFFER_COPY_MEMORY, 0, nalu.sc_offset)) {
      GST_ERROR ("Failed to copy buffer");
      gst_clear_buffer (&new_buffer);
      goto out;
    }
  }

  /* insert sei */
  gst_buffer_append_memory (new_buffer, gst_memory_ref (sei));

  /* copy the rest */
  if (!gst_buffer_copy_into (new_buffer, au,
          GST_BUFFER_COPY_MEMORY, nalu.sc_offset, -1)) {
    GST_ERROR ("Failed to copy buffer");
    gst_clear_buffer (&new_buffer);
    goto out;
  }

out:
  return new_buffer;
}

/**
 * gst_h264_parser_insert_sei:
 * @nalparser: a #GstH264NalParser
 * @au: (transfer none): a #GstBuffer containing AU data
 * @sei: (transfer none): a #GstMemory containing a SEI nal
 *
 * Copy @au into new #GstBuffer and insert @sei into the #GstBuffer.
 * The validation for completeness of @au and @sei is caller's responsibility.
 * Both @au and @sei must be byte-stream formatted
 *
 * Returns: (transfer full) (nullable): a SEI inserted #GstBuffer or %NULL
 *   if cannot figure out proper position to insert a @sei
 *
 * Since: 1.18
 */
GstBuffer *
gst_h264_parser_insert_sei (GstH264NalParser * nalparser, GstBuffer * au,
    GstMemory * sei)
{
  g_return_val_if_fail (nalparser != NULL, NULL);
  g_return_val_if_fail (GST_IS_BUFFER (au), NULL);
  g_return_val_if_fail (sei != NULL, NULL);

  /* the size of start code prefix (3 or 4) is not matter since it will be
   * scanned */
  return gst_h264_parser_insert_sei_internal (nalparser, 4, FALSE, au, sei);
}

/**
 * gst_h264_parser_insert_sei_avc:
 * @nalparser: a #GstH264NalParser
 * @nal_length_size: a size of nal length field, allowed range is [1, 4]
 * @au: (transfer none): a #GstBuffer containing AU data
 * @sei: (transfer none): a #GstMemory containing a SEI nal
 *
 * Copy @au into new #GstBuffer and insert @sei into the #GstBuffer.
 * The validation for completeness of @au and @sei is caller's responsibility.
 * Nal prefix type of both @au and @sei must be packetized, and
 * also the size of nal length field must be identical to @nal_length_size
 *
 * Returns: (transfer full) (nullable): a SEI inserted #GstBuffer or %NULL
 *   if cannot figure out proper position to insert a @sei
 *
 * Since: 1.18
 */
GstBuffer *
gst_h264_parser_insert_sei_avc (GstH264NalParser * nalparser,
    guint8 nal_length_size, GstBuffer * au, GstMemory * sei)
{
  g_return_val_if_fail (nalparser != NULL, NULL);
  g_return_val_if_fail (nal_length_size > 0 && nal_length_size < 5, NULL);
  g_return_val_if_fail (GST_IS_BUFFER (au), NULL);
  g_return_val_if_fail (sei != NULL, NULL);

  /* the size of start code prefix (3 or 4) is not matter since it will be
   * scanned */
  return gst_h264_parser_insert_sei_internal (nalparser, nal_length_size, TRUE,
      au, sei);
}

static GstH264DecoderConfigRecord *
gst_h264_decoder_config_record_new (void)
{
  GstH264DecoderConfigRecord *config;

  config = g_new0 (GstH264DecoderConfigRecord, 1);
  config->sps = g_array_new (FALSE, FALSE, sizeof (GstH264NalUnit));
  config->pps = g_array_new (FALSE, FALSE, sizeof (GstH264NalUnit));
  config->sps_ext = g_array_new (FALSE, FALSE, sizeof (GstH264NalUnit));

  return config;
}

/**
 * gst_h264_decoder_config_record_free:
 * @config: (nullable): a #GstH264DecoderConfigRecord data
 *
 * Free @config data
 *
 * Since: 1.22
 */
void
gst_h264_decoder_config_record_free (GstH264DecoderConfigRecord * config)
{
  if (!config)
    return;

  if (config->sps)
    g_array_unref (config->sps);

  if (config->pps)
    g_array_unref (config->pps);

  if (config->sps_ext)
    g_array_unref (config->sps_ext);

  g_free (config);
}

/**
 * gst_h264_parser_parse_decoder_config_record:
 * @nalparser: a #GstH264NalParser
 * @data: the data to parse
 * @size: the size of @data
 * @config: (out): parsed #GstH264DecoderConfigRecord data
 *
 * Parses AVCDecoderConfigurationRecord data and fill into @config.
 * The caller must free @config via gst_h264_decoder_config_record_free()
 *
 * This method does not parse SPS and PPS and therefore the caller needs to
 * parse each NAL unit via appropriate parsing method.
 *
 * Returns: a #GstH264ParserResult
 *
 * Since: 1.22
 */
GstH264ParserResult
gst_h264_parser_parse_decoder_config_record (GstH264NalParser * nalparser,
    const guint8 * data, gsize size, GstH264DecoderConfigRecord ** config)
{
  GstH264DecoderConfigRecord *ret;
  GstBitReader br;
  GstH264ParserResult result = GST_H264_PARSER_OK;
  guint8 num_sps, num_pps, i;
  guint offset;

  g_return_val_if_fail (nalparser != NULL, GST_H264_PARSER_ERROR);
  g_return_val_if_fail (data != NULL, GST_H264_PARSER_ERROR);
  g_return_val_if_fail (config != NULL, GST_H264_PARSER_ERROR);

#define READ_CONFIG_UINT8(val, nbits) G_STMT_START { \
  if (!gst_bit_reader_get_bits_uint8 (&br, &val, nbits)) { \
    GST_WARNING ("Failed to read " G_STRINGIFY (val)); \
    result = GST_H264_PARSER_ERROR; \
    goto error; \
  } \
} G_STMT_END;

#define SKIP_CONFIG_BITS(nbits) G_STMT_START { \
  if (!gst_bit_reader_skip (&br, nbits)) { \
    GST_WARNING ("Failed to skip %d bits", nbits); \
    result = GST_H264_PARSER_ERROR; \
    goto error; \
  } \
} G_STMT_END;

  *config = NULL;

  if (size < 7) {
    GST_WARNING ("Too small size avcC");
    return GST_H264_PARSER_ERROR;
  }

  gst_bit_reader_init (&br, data, size);

  ret = gst_h264_decoder_config_record_new ();

  READ_CONFIG_UINT8 (ret->configuration_version, 8);
  /* Keep parsing, caller can decide whether this data needs to be discarded
   * or not */
  if (ret->configuration_version != 1) {
    GST_WARNING ("Wrong configurationVersion %d", ret->configuration_version);
    result = GST_H264_PARSER_ERROR;
    goto error;
  }

  READ_CONFIG_UINT8 (ret->profile_indication, 8);
  READ_CONFIG_UINT8 (ret->profile_compatibility, 8);
  READ_CONFIG_UINT8 (ret->level_indication, 8);
  /* reserved 6bits */
  SKIP_CONFIG_BITS (6);
  READ_CONFIG_UINT8 (ret->length_size_minus_one, 2);
  if (ret->length_size_minus_one == 2) {
    /* "length_size_minus_one + 1" should be 1, 2, or 4 */
    GST_WARNING ("Wrong nal-length-size");
  }

  /* reserved 3bits */
  SKIP_CONFIG_BITS (3);

  READ_CONFIG_UINT8 (num_sps, 5);
  offset = gst_bit_reader_get_pos (&br);

  g_assert (offset % 8 == 0);
  offset /= 8;
  for (i = 0; i < num_sps; i++) {
    GstH264NalUnit nalu;

    result = gst_h264_parser_identify_nalu_avc (nalparser,
        data, offset, size, 2, &nalu);
    if (result != GST_H264_PARSER_OK)
      goto error;

    g_array_append_val (ret->sps, nalu);
    offset = nalu.offset + nalu.size;
  }

  if (!gst_bit_reader_set_pos (&br, offset * 8)) {
    result = GST_H264_PARSER_ERROR;
    goto error;
  }

  READ_CONFIG_UINT8 (num_pps, 8);
  offset = gst_bit_reader_get_pos (&br);

  g_assert (offset % 8 == 0);
  offset /= 8;
  for (i = 0; i < num_pps; i++) {
    GstH264NalUnit nalu;

    result = gst_h264_parser_identify_nalu_avc (nalparser,
        data, offset, size, 2, &nalu);
    if (result != GST_H264_PARSER_OK)
      goto error;

    g_array_append_val (ret->pps, nalu);
    offset = nalu.offset + nalu.size;
  }

  /* Parse chroma format and SPS ext data. We will silently ignore any
   * error while parsing below data since it's not essential data for
   * decoding */
  if (ret->profile_indication == 100 || ret->profile_indication == 110 ||
      ret->profile_indication == 122 || ret->profile_indication == 144) {
    guint8 num_sps_ext;

    if (!gst_bit_reader_set_pos (&br, offset * 8))
      goto out;

    if (!gst_bit_reader_skip (&br, 6))
      goto out;

    if (!gst_bit_reader_get_bits_uint8 (&br, &ret->chroma_format, 2))
      goto out;

    if (!gst_bit_reader_skip (&br, 5))
      goto out;

    if (!gst_bit_reader_get_bits_uint8 (&br, &ret->bit_depth_luma_minus8, 3))
      goto out;

    if (!gst_bit_reader_skip (&br, 5))
      goto out;

    if (!gst_bit_reader_get_bits_uint8 (&br, &ret->bit_depth_chroma_minus8, 3))
      goto out;

    if (!gst_bit_reader_get_bits_uint8 (&br, &num_sps_ext, 8))
      goto out;

    offset = gst_bit_reader_get_pos (&br);

    g_assert (offset % 8 == 0);
    offset /= 8;
    for (i = 0; i < num_sps_ext; i++) {
      GstH264NalUnit nalu;

      result = gst_h264_parser_identify_nalu_avc (nalparser,
          data, offset, size, 2, &nalu);
      if (result != GST_H264_PARSER_OK)
        goto out;

      g_array_append_val (ret->sps_ext, nalu);
      offset = nalu.offset + nalu.size;
    }

    ret->chroma_format_present = TRUE;
  }

out:
  {
    *config = ret;
    return GST_H264_PARSER_OK;
  }
error:
  {
    gst_h264_decoder_config_record_free (ret);
    return result;
  }

#undef READ_CONFIG_UINT8
#undef SKIP_CONFIG_BITS
}

typedef struct
{
  const gchar *name;
  GstH264Profile profile;
} H264ProfileMapping;


static const H264ProfileMapping h264_profiles[] = {
  {"baseline", GST_H264_PROFILE_BASELINE},
  {"main", GST_H264_PROFILE_MAIN},
  {"high", GST_H264_PROFILE_HIGH},
  {"high-10", GST_H264_PROFILE_HIGH10},
  {"high-4:2:2", GST_H264_PROFILE_HIGH_422},
  {"high-4:4:4", GST_H264_PROFILE_HIGH_444},
  {"multiview-high", GST_H264_PROFILE_MULTIVIEW_HIGH},
  {"stereo-high", GST_H264_PROFILE_STEREO_HIGH},
  {"scalable-baseline", GST_H264_PROFILE_SCALABLE_BASELINE},
  {"scalable-high", GST_H264_PROFILE_SCALABLE_HIGH},
};

/**
 * gst_h264_profile_from_string:
 * @string: the descriptive name for #GstH264Profile
 *
 * Returns a #GstH264Profile for the @string.
 *
 * Returns: the #GstH264Profile of @string or %GST_H265_PROFILE_INVALID on error
 *
 * Since: 1.24
 */
GstH264Profile
gst_h264_profile_from_string (const gchar * string)
{
  guint i;

  if (string == NULL)
    return GST_H264_PROFILE_INVALID;

  for (i = 0; i < G_N_ELEMENTS (h264_profiles); i++) {
    if (g_strcmp0 (string, h264_profiles[i].name) == 0) {
      return h264_profiles[i].profile;
    }
  }

  return GST_H264_PROFILE_INVALID;
}

/**
 * gst_h264_slice_type_to_string:
 * @slice_type: a #GstH264SliceType
 *
 * Returns the descriptive name for the #GstH264SliceType.
 *
 * Returns: (nullable): the name for @slice_type or %NULL on error
 *
 * Since: 1.24
 */
const gchar *
gst_h264_slice_type_to_string (GstH264SliceType slice_type)
{
  switch (slice_type) {
    case GST_H264_P_SLICE:
      return "P";
    case GST_H264_B_SLICE:
      return "B";
    case GST_H264_I_SLICE:
      return "I";
    case GST_H264_SP_SLICE:
      return "SP";
    case GST_H264_SI_SLICE:
      return "SI";
    default:
      GST_ERROR ("unknown %d slice type", slice_type);
  }

  return NULL;
}

/* GStreamer plugin for forward error correction
 * Copyright (C) 2017 Pexip
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Author: Mikhail Fludkov <misha@pexip.com>
 */

#ifndef __GST_RTP_RED_ENC_H__
#define __GST_RTP_RED_ENC_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_RTP_RED_ENC \
  (gst_rtp_red_enc_get_type())
#define GST_RTP_RED_ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RTP_RED_ENC,GstRtpRedEnc))
#define GST_RTP_RED_ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RTP_RED_ENC,GstRtpRedEncClass))
#define GST_IS_RTP_RED_ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RTP_RED_ENC))
#define GST_IS_RTP_RED_ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RTP_RED_ENC))

typedef struct _GstRtpRedEnc GstRtpRedEnc;
typedef struct _GstRtpRedEncClass GstRtpRedEncClass;

struct _GstRtpRedEncClass {
  GstElementClass parent_class;
};

struct _GstRtpRedEnc {
  GstElement parent;
  GstPad *srcpad;
  GstPad *sinkpad;

  gint pt;
  gint num_sent;
  gint distance; /* atomic; history is owned by the streaming thread */
  gboolean allow_no_red_blocks;

  GQueue *rtp_history;
  gint last_output_pt;
  gboolean is_current_caps_red;
  guint8 twcc_ext_id;

  gboolean ignoring_extension_warned;

  gint exclude_pt; /* -1 = wrap all PTs; >= 0 = pass through this PT unchanged */
  gint media_pt; /* -1 = all; otherwise wrap only media_pt and fec_pt */
  gint fec_pt;
  gint exact_distance;
  gint mtu; /* 0 = unlimited, includes the RTP header and extensions */
  guint32 history_ssrc;
  gboolean have_history_ssrc;
  gint redundant_sent;
  gint skipped_history;
  gint skipped_size;
  gint skipped_format;
  gint history_resets;
  /* Summary state is owned by the streaming thread, like payload history. */
  gint64 last_report_us;
  guint report_counters[6];
};

GType gst_rtp_red_enc_get_type (void);

G_END_DECLS

#endif /* __GST_RTP_RED_ENC_H__ */

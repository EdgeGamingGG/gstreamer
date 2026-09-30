/* GStreamer
 * Copyright (C) 2017 Matthew Waters <matthew@centricular.com>
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "gstwebrtcbin.h"
#include "utils.h"
#include "webrtctransceiver.h"

#define GST_CAT_DEFAULT webrtc_transceiver_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define webrtc_transceiver_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (WebRTCTransceiver, webrtc_transceiver,
    GST_TYPE_WEBRTC_RTP_TRANSCEIVER,
    GST_DEBUG_CATEGORY_INIT (webrtc_transceiver_debug,
        "webrtctransceiver", 0, "webrtctransceiver");
    );

#define DEFAULT_FEC_TYPE GST_WEBRTC_FEC_TYPE_NONE
#define DEFAULT_DO_NACK FALSE
#define DEFAULT_FEC_PERCENTAGE 100

enum
{
  PROP_0,
  PROP_WEBRTC,
  PROP_FEC_TYPE,
  PROP_FEC_PERCENTAGE,
  PROP_DO_NACK,
  PROP_AUDIO_RED,
  PROP_AUDIO_RED_DISTANCE,
  PROP_AUDIO_RED_ACTIVE,
};

void
webrtc_transceiver_set_transport (WebRTCTransceiver * trans,
    TransportStream * stream)
{
  GstWebRTCRTPTransceiver *rtp_trans;

  g_return_if_fail (WEBRTC_IS_TRANSCEIVER (trans));

  rtp_trans = GST_WEBRTC_RTP_TRANSCEIVER (trans);

  gst_object_replace ((GstObject **) & trans->stream, (GstObject *) stream);

  if (rtp_trans->sender) {
    gst_object_replace ((GstObject **) & rtp_trans->sender->transport,
        (GstObject *) stream->transport);
    g_object_notify (G_OBJECT (rtp_trans->sender), "transport");
  }

  if (rtp_trans->receiver) {
    gst_object_replace ((GstObject **) & rtp_trans->receiver->transport,
        (GstObject *) stream->transport);
    g_object_notify (G_OBJECT (rtp_trans->receiver), "transport");
  }
}

GstWebRTCDTLSTransport *
webrtc_transceiver_get_dtls_transport (GstWebRTCRTPTransceiver * trans)
{
  g_return_val_if_fail (WEBRTC_IS_TRANSCEIVER (trans), NULL);

  if (trans->sender) {
    return trans->sender->transport;
  } else if (trans->receiver) {
    return trans->receiver->transport;
  }

  return NULL;
}

static void
webrtc_transceiver_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  WebRTCTransceiver *trans = WEBRTC_TRANSCEIVER (object);

  switch (prop_id) {
    case PROP_WEBRTC:
      gst_object_set_parent (GST_OBJECT (trans), g_value_get_object (value));
      break;
  }

  GST_OBJECT_LOCK (trans);
  switch (prop_id) {
    case PROP_WEBRTC:
      break;
    case PROP_AUDIO_RED:
      trans->audio_red = g_value_get_boolean (value);
      break;
    case PROP_AUDIO_RED_DISTANCE:
      trans->audio_red_distance = g_value_get_uint (value);
      if (trans->audio_redenc)
        g_object_set (trans->audio_redenc, "distance",
            g_atomic_int_get (&trans->audio_red_active) ?
            trans->audio_red_distance : 0, NULL);
      break;
    case PROP_FEC_TYPE:
      trans->fec_type = g_value_get_enum (value);
      break;
    case PROP_DO_NACK:
      trans->do_nack = g_value_get_boolean (value);
      break;
    case PROP_FEC_PERCENTAGE:
      trans->fec_percentage = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (trans);
}

static void
webrtc_transceiver_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  WebRTCTransceiver *trans = WEBRTC_TRANSCEIVER (object);

  GST_OBJECT_LOCK (trans);
  switch (prop_id) {
    case PROP_AUDIO_RED:
      g_value_set_boolean (value, trans->audio_red);
      break;
    case PROP_AUDIO_RED_DISTANCE:
      g_value_set_uint (value, trans->audio_red_distance);
      break;
    case PROP_AUDIO_RED_ACTIVE:
      g_value_set_boolean (value, g_atomic_int_get (&trans->audio_red_active));
      break;
    case PROP_FEC_TYPE:
      g_value_set_enum (value, trans->fec_type);
      break;
    case PROP_DO_NACK:
      g_value_set_boolean (value, trans->do_nack);
      break;
    case PROP_FEC_PERCENTAGE:
      g_value_set_uint (value, trans->fec_percentage);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (trans);
}

static void
webrtc_transceiver_finalize (GObject * object)
{
  WebRTCTransceiver *trans = WEBRTC_TRANSCEIVER (object);

  gst_clear_object (&trans->stream);
  gst_clear_object (&trans->ulpfecdec);
  gst_clear_object (&trans->ulpfecenc);
  gst_clear_object (&trans->redenc);
  gst_clear_object (&trans->audio_redenc);

  if (trans->local_rtx_ssrc_map)
    gst_structure_free (trans->local_rtx_ssrc_map);
  trans->local_rtx_ssrc_map = NULL;

  gst_caps_replace (&trans->last_retrieved_caps, NULL);
  gst_caps_replace (&trans->last_send_configured_caps, NULL);

  g_free (trans->pending_mid);

  gst_event_replace (&trans->tos_event, NULL);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
webrtc_transceiver_class_init (WebRTCTransceiverClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->get_property = webrtc_transceiver_get_property;
  gobject_class->set_property = webrtc_transceiver_set_property;
  gobject_class->finalize = webrtc_transceiver_finalize;

  /* some acrobatics are required to set the parent before _constructed()
   * has been called */
  g_object_class_install_property (gobject_class,
      PROP_WEBRTC,
      g_param_spec_object ("webrtc", "Parent webrtcbin",
          "Parent webrtcbin",
          GST_TYPE_WEBRTC_BIN,
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_AUDIO_RED,
      g_param_spec_boolean ("audio-red", "Audio RED",
          "Offer RED-only protection for Opus (set before negotiation)",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_AUDIO_RED_DISTANCE,
      g_param_spec_uint ("audio-red-distance", "Audio RED distance",
          "Earlier Opus packet to copy; zero retains primary-only RED",
          0, 10, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_AUDIO_RED_ACTIVE,
      g_param_spec_boolean ("audio-red-active", "Audio RED active",
          "Whether the negotiated audio sender uses RED",
          FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_FEC_TYPE,
      g_param_spec_enum ("fec-type", "FEC type",
          "The type of Forward Error Correction to use",
          GST_TYPE_WEBRTC_FEC_TYPE,
          DEFAULT_FEC_TYPE,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_DO_NACK,
      g_param_spec_boolean ("do-nack", "Do nack",
          "Whether to send negative acknowledgements for feedback",
          DEFAULT_DO_NACK,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_FEC_PERCENTAGE,
      g_param_spec_uint ("fec-percentage", "FEC percentage",
          "The amount of Forward Error Correction to apply",
          0, 100, DEFAULT_FEC_PERCENTAGE,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
webrtc_transceiver_init (WebRTCTransceiver * trans)
{
  trans->audio_red_distance = 1;
}

WebRTCTransceiver *
webrtc_transceiver_new (GstWebRTCBin * webrtc, GstWebRTCRTPSender * sender,
    GstWebRTCRTPReceiver * receiver)
{
  WebRTCTransceiver *trans;

  trans = g_object_new (webrtc_transceiver_get_type (), "sender", sender,
      "receiver", receiver, "webrtc", webrtc, NULL);

  return trans;
}

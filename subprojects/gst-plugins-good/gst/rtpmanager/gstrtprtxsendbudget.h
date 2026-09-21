/* Private implementation of the optional rtprtxsend budget.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 * All state is protected by the rtprtxsend object lock. No downstream call or
 * rate wait holds that lock. The existing RTX pad task remains the sole worker.
 */

#define RTX_BUDGET_MAX_BYTES (G_MAXUINT64 / (8 * GST_SECOND))

enum
{
  BUDGET_REQUESTS, BUDGET_CACHE_MISSES, BUDGET_ADMITTED, BUDGET_COALESCED,
  BUDGET_QUEUE_FULL, BUDGET_EXPIRED, BUDGET_OVERSIZE_BURST,
  BUDGET_OVERSIZE_QUEUE, BUDGET_CANCELLED, BUDGET_ATTEMPTS,
  BUDGET_ATTEMPT_BYTES, BUDGET_ACCEPTED, BUDGET_ACCEPTED_BYTES,
  BUDGET_PUSH_FAILURES, BUDGET_N_COUNTERS
};

static const gchar *const budget_counter_names[] = {
  "requests", "cache-misses", "admitted", "coalesced", "queue-full",
  "expired", "packet-exceeds-burst", "packet-exceeds-queue-cap", "cancelled",
  "handoff-attempts", "handoff-attempt-bytes", "handoff-accepted",
  "handoff-accepted-bytes", "push-failures"
};

typedef struct
{
  guint64 history_id;
  GstBuffer *buffer;
  guint64 bytes;
  GstClockTime admitted, deadline;
} RtxBudgetItem;

struct _GstRtpRtxBudget
{
  gboolean enabled, flushing, started;
  guint64 rate, burst, cap;
  guint max_time_ms;
  GQueue waiting;
  GHashTable *pending;
  GCond changed;
  GstClock *clock;
  GstClockID wake;
  /* Credit uses bit-nanoseconds, retaining fractional bytes between wakes. */
  guint64 credit, last_refill, bytes, peak_bytes, next_history_id, epoch;
  guint64 counters[BUDGET_N_COUNTERS];
  RtxBudgetItem *in_flight;
};

static GstRtpRtxBudget *
rtx_budget_new (void)
{
  GstRtpRtxBudget *b = g_new0 (GstRtpRtxBudget, 1);
  b->rate = 3800000;
  b->burst = 4750;
  b->cap = 9500;
  b->max_time_ms = 20;
  b->flushing = TRUE;
  b->pending = g_hash_table_new (g_int64_hash, g_int64_equal);
  g_queue_init (&b->waiting);
  g_cond_init (&b->changed);
  return b;
}

static void
rtx_budget_wake (GstRtpRtxBudget * b)
{
  if (b->wake)
    gst_clock_id_unschedule (b->wake);
  g_cond_broadcast (&b->changed);
}

static void
rtx_budget_release (GstRtpRtxBudget * b, RtxBudgetItem * item)
{
  b->bytes -= item->bytes;
  g_hash_table_remove (b->pending, &item->history_id);
  gst_clear_buffer (&item->buffer);
  g_free (item);
}

static void
rtx_budget_flush (GstRtpRtxBudget * b, gboolean flushing)
{
  RtxBudgetItem *item;
  b->flushing = flushing;
  while ((item = g_queue_pop_head (&b->waiting))) {
    b->counters[BUDGET_CANCELLED]++;
    rtx_budget_release (b, item);
  }
  if (!flushing) {
    b->epoch++;
    b->credit = b->burst * 8 * GST_SECOND;
    b->last_refill = b->clock ? gst_clock_get_time (b->clock) : 0;
  }
  rtx_budget_wake (b);
}

static void
rtx_budget_free (GstRtpRtxBudget * b)
{
  rtx_budget_flush (b, TRUE);
  g_assert (b->in_flight == NULL);
  g_hash_table_unref (b->pending);
  gst_clear_object (&b->clock);
  g_cond_clear (&b->changed);
  g_free (b);
}

static void
rtx_budget_refill (GstRtpRtxBudget * b, GstClockTime now)
{
  guint64 capacity = b->burst * 8 * GST_SECOND;
  guint64 missing = capacity - b->credit;
  guint64 elapsed = now >= b->last_refill ? now - b->last_refill : 0;
  /* Compare before multiplying, so an arbitrarily long idle cannot overflow. */
  guint64 until_full = missing / b->rate + (missing % b->rate != 0);
  b->credit = elapsed >= until_full ? capacity : b->credit + elapsed * b->rate;
  b->last_refill = now;
}

static void
rtx_budget_expire (GstRtpRtxBudget * b, GstClockTime now)
{
  RtxBudgetItem *item;
  /* FIFO admissions have monotonic deadlines; equal deadlines get one service
   * opportunity in the worker. Never expire an already started handoff. */
  while ((item = g_queue_peek_head (&b->waiting)) && item->deadline < now) {
    g_queue_pop_head (&b->waiting);
    b->counters[BUDGET_EXPIRED]++;
    rtx_budget_release (b, item);
  }
}

static void
rtx_budget_admit (GstRtpRtxSend * rtx, BufferQueueItem * original)
{
  GstRtpRtxBudget *b = rtx->budget;
  GstClockTime now = gst_clock_get_time (b->clock);
  GstBuffer *buffer;
  guint64 size;
  RtxBudgetItem *item;

  rtx_budget_expire (b, now);
  if (b->flushing) {
    b->counters[BUDGET_CANCELLED]++;
    return;
  }
  if (g_hash_table_contains (b->pending, &original->history_id)) {
    b->counters[BUDGET_COALESCED]++;
    return;
  }
  /* Construction rewrites RTP extensions; use its actual full RTP size, not
   * payload length or an assumption about fixed header length. */
  buffer = gst_rtp_rtx_buffer_new (rtx, original->buffer);
  size = gst_buffer_get_size (buffer);
  if (size > b->burst) {
    b->counters[BUDGET_OVERSIZE_BURST]++;
  } else if (size > b->cap) {
    b->counters[BUDGET_OVERSIZE_QUEUE]++;
  } else if (size > b->cap - b->bytes) {
    b->counters[BUDGET_QUEUE_FULL]++;
  } else {
    item = g_new0 (RtxBudgetItem, 1);
    item->history_id = original->history_id;
    item->buffer = buffer;
    item->bytes = size;
    item->admitted = now;
    item->deadline = now + b->max_time_ms * GST_MSECOND;
    b->bytes += size;
    b->peak_bytes = MAX (b->peak_bytes, b->bytes);
    b->counters[BUDGET_ADMITTED]++;
    g_hash_table_add (b->pending, &item->history_id);
    g_queue_push_tail (&b->waiting, item);
    rtx_budget_wake (b);
    return;
  }
  gst_buffer_unref (buffer);
}

static void
rtx_budget_loop (GstRtpRtxSend * rtx)
{
  GstRtpRtxBudget *b = rtx->budget;
  RtxBudgetItem *item;
  GstClockTime now;
  GstFlowReturn flow;

  GST_OBJECT_LOCK (rtx);
  while (!b->flushing) {
    item = g_queue_peek_head (&b->waiting);
    if (!item) {
      g_cond_wait (&b->changed, GST_OBJECT_GET_LOCK (rtx));
      continue;
    }
    now = gst_clock_get_time (b->clock);
    rtx_budget_refill (b, now);
    rtx_budget_expire (b, now);
    item = g_queue_peek_head (&b->waiting);
    if (!item)
      continue;
    if (b->credit >= item->bytes * 8 * GST_SECOND) {
      b->credit -= item->bytes * 8 * GST_SECOND;
      g_queue_pop_head (&b->waiting);
      b->in_flight = item;
      b->counters[BUDGET_ATTEMPTS]++;
      b->counters[BUDGET_ATTEMPT_BYTES] += item->bytes;
      rtx->num_rtx_packets++;
      GST_OBJECT_UNLOCK (rtx);
      flow = gst_pad_push (rtx->srcpad, item->buffer);
      GST_OBJECT_LOCK (rtx);
      item->buffer = NULL;
      if (flow == GST_FLOW_OK) {
        b->counters[BUDGET_ACCEPTED]++;
        b->counters[BUDGET_ACCEPTED_BYTES] += item->bytes;
      } else {
        b->counters[BUDGET_PUSH_FAILURES]++;
      }
      b->in_flight = NULL;
      rtx_budget_release (b, item);
      if (flow != GST_FLOW_OK)
        rtx_budget_flush (b, TRUE);
      GST_OBJECT_UNLOCK (rtx);
      return;
    }
    if (item->deadline == now) {
      g_queue_pop_head (&b->waiting);
      b->counters[BUDGET_EXPIRED]++;
      rtx_budget_release (b, item);
      continue;
    }
    {
      guint64 missing = item->bytes * 8 * GST_SECOND - b->credit;
      guint64 delay = missing / b->rate + (missing % b->rate != 0);
      GstClockID wake = gst_clock_new_single_shot_id (b->clock,
          now + MIN (item->deadline - now, delay));
      b->wake = wake;
      GST_OBJECT_UNLOCK (rtx);
      gst_clock_id_wait (wake, NULL);
      GST_OBJECT_LOCK (rtx);
      b->wake = NULL;
      gst_clock_id_unref (wake);
    }
  }
  GST_OBJECT_UNLOCK (rtx);
  gst_pad_pause_task (rtx->srcpad);
}

static GstStructure *
rtx_budget_stats (GstRtpRtxBudget * b)
{
  guint i;
  RtxBudgetItem *head;
  GstClockTime age = 0;
  GstStructure *s;
  if (b->clock)
    rtx_budget_expire (b, gst_clock_get_time (b->clock));
  head = g_queue_peek_head (&b->waiting);
  if (head)
    age = gst_clock_get_time (b->clock) - head->admitted;
  s = gst_structure_new ("rtx-budget-stats",
      "enabled", G_TYPE_BOOLEAN, b->enabled,
      "epoch", G_TYPE_UINT64, b->epoch,
      "queue-bytes", G_TYPE_UINT64, b->bytes,
      "peak-queue-bytes", G_TYPE_UINT64, b->peak_bytes,
      "waiting-packets", G_TYPE_UINT, b->waiting.length,
      "worker-bytes", G_TYPE_UINT64, b->in_flight ? b->in_flight->bytes : 0,
      "oldest-wait-ns", G_TYPE_UINT64, age, NULL);
  for (i = 0; i < BUDGET_N_COUNTERS; i++)
    gst_structure_set (s, budget_counter_names[i], G_TYPE_UINT64,
        b->counters[i], NULL);
  return s;
}

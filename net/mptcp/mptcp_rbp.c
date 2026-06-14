/* MPTCP Receive Buffer Pre-division (RBP) Scheduler
 *
 * Implements the RBP flow control mechanism from:
 *   Han et al., "Receive Buffer Pre-division Based Flow Control for MPTCP"
 *   (CCIS 747, pp. 19-31, 2018)
 *
 * Divides the shared receive buffer among subflows proportionally to estimated
 * buffer occupancy, then applies per-subflow flow control via send limits.
 *
 * Registers two scheduler variants:
 *   "rbp"     - RBP flow control + min-RTT subflow selection
 *   "rbp_fwd" - RBP flow control + weighted forward-delay subflow selection
 */

#include <linux/module.h>
#include <net/mptcp.h>

/* Per-subflow RBP state, stored in mptcp_tcp_sock->mptcp_sched[] */
struct rbp_priv {
  u32 acwnd;          /* EWMA of congestion window (packets) */
  u32 alloc;          /* Allocated buffer share B_i (bytes) */
  u32 rwnd;           /* Per-subflow receive window (bytes) */
  u32 unordered;      /* Estimated unordered bytes from this subflow */
  u32 last_rbuf_opti; /* Timestamp for receive buffer optimization */
};

static struct rbp_priv *rbp_get_priv(const struct tcp_sock *tp)
{
  return (struct rbp_priv *)&tp->mptcp->mptcp_sched[0];
}

/*
 * ========================================================================
 * Helper functions (duplicated from mptcp_sched.c — they are static there)
 * ========================================================================
 */

static bool mptcp_is_def_unavailable(struct sock *sk)
{
  const struct tcp_sock *tp = tcp_sk(sk);

  if (!mptcp_sk_can_send(sk))
    return true;

  if (tp->mptcp->pre_established)
    return true;

  if (tp->pf)
    return true;

  return false;
}

static bool mptcp_is_temp_unavailable(struct sock *sk,
                                      const struct sk_buff *skb,
                                      bool zero_wnd_test)
{
  const struct tcp_sock *tp = tcp_sk(sk);
  unsigned int mss_now, space, in_flight;

  if (inet_csk(sk)->icsk_ca_state == TCP_CA_Loss) {
    if (!tcp_is_reno(tp))
      return true;
    else if (tp->snd_una != tp->high_seq)
      return true;
  }

  if (!tp->mptcp->fully_established) {
    if (skb && tp->mptcp->second_packet &&
        tp->mptcp->last_end_data_seq != TCP_SKB_CB(skb)->seq)
      return true;
  }

  if (test_bit(TSQ_THROTTLED, &tp->tsq_flags))
    return true;

  in_flight = tcp_packets_in_flight(tp);
  if (in_flight >= tp->snd_cwnd)
    return true;

  space = (tp->snd_cwnd - in_flight) * tp->mss_cache;
  if (tp->write_seq - tp->snd_nxt > space)
    return true;

  if (zero_wnd_test && !before(tp->write_seq, tcp_wnd_end(tp)))
    return true;

  mss_now = tcp_current_mss(sk);

  if (skb && !zero_wnd_test &&
      after(tp->write_seq + min(skb->len, mss_now), tcp_wnd_end(tp)))
    return true;

  return false;
}

static bool mptcp_is_available(struct sock *sk, const struct sk_buff *skb,
                               bool zero_wnd_test)
{
  return !mptcp_is_def_unavailable(sk) &&
         !mptcp_is_temp_unavailable(sk, skb, zero_wnd_test);
}

static int mptcp_dont_reinject_skb(const struct tcp_sock *tp,
                                   const struct sk_buff *skb)
{
  return skb &&
         mptcp_pi_to_flag(tp->mptcp->path_index) & TCP_SKB_CB(skb)->path_mask;
}

static bool subflow_is_backup(const struct tcp_sock *tp)
{
  return tp->mptcp->rcv_low_prio || tp->mptcp->low_prio;
}

static bool subflow_is_active(const struct tcp_sock *tp)
{
  return !tp->mptcp->rcv_low_prio && !tp->mptcp->low_prio;
}

/*
 * ========================================================================
 * RBP Core: Buffer Division Algorithm (Han et al. 2018, Algorithm 1)
 * ========================================================================
 */

/* Eq. 1: acwnd_i = (1 - beta) * acwnd_i + beta * cwnd_i, beta = 1/16 */
static void rbp_update_acwnd(struct tcp_sock *tp)
{
  struct rbp_priv *priv = rbp_get_priv(tp);

  priv->acwnd = priv->acwnd - (priv->acwnd >> 4) + (tp->snd_cwnd >> 4);
  if (priv->acwnd == 0)
    priv->acwnd = 1;
}

/* Eq. 2: Buf_i = acwnd_i * (ceil(max_{j!=i}(RTT_j) / (2*RTT_i)) + 1) */
static u32 rbp_compute_buf_occupancy(struct mptcp_cb *mpcb, struct tcp_sock *tp)
{
  struct rbp_priv *priv = rbp_get_priv(tp);
  struct tcp_sock *tp_it;
  u32 max_rtt_other = 0;
  u32 my_rtt = tp->srtt_us >> 3;
  u32 ratio;

  if (my_rtt == 0)
    my_rtt = 1;

  mptcp_for_each_tp(mpcb, tp_it) {
    u32 rtt_it;
    if (tp_it == tp)
      continue;
    rtt_it = tp_it->srtt_us >> 3;
    if (rtt_it > max_rtt_other)
      max_rtt_other = rtt_it;
  }

  /* Single subflow: no OOO possible */
  if (max_rtt_other == 0)
    max_rtt_other = my_rtt;

  /* ceil(max_rtt_other / (2 * my_rtt)) + 1
   * Paper has +1/2 +1/2 which simplifies to +1
   */
  ratio = (max_rtt_other + 2 * my_rtt - 1) / (2 * my_rtt) + 1;

  return priv->acwnd * ratio;
}

/* Full RBP buffer division: Eqs 1-4 from the paper */
static void rbp_divide_buffer(struct sock *meta_sk)
{
  struct tcp_sock *meta_tp = tcp_sk(meta_sk);
  struct mptcp_cb *mpcb = meta_tp->mpcb;
  struct tcp_sock *tp;
  u32 total_buf_est = 0;
  u32 recv_buffer;
  u32 meta_rwnd;
  u32 sum_positive = 0;

  recv_buffer = meta_sk->sk_rcvbuf;
  meta_rwnd = meta_tp->rcv_wnd;

  /* Phase 1: Update acwnd and compute buffer occupancy estimates (Eqs 1-2) */
  mptcp_for_each_tp(mpcb, tp) {
    struct rbp_priv *priv = rbp_get_priv(tp);
    rbp_update_acwnd(tp);
    priv->alloc = rbp_compute_buf_occupancy(mpcb, tp);
    total_buf_est += priv->alloc;
  }

  if (total_buf_est == 0)
    total_buf_est = 1;

  /* Phase 2: Allocate buffer proportionally: B_i = recv_buffer * Buf_i / total (Eq 3) */
  mptcp_for_each_tp(mpcb, tp) {
    struct rbp_priv *priv = rbp_get_priv(tp);
    priv->alloc = (u32)((u64)recv_buffer * priv->alloc / total_buf_est);
  }

  /* Phase 3: Estimate unordered_i (sender-side: all in-flight on subflow) */
  mptcp_for_each_tp(mpcb, tp) {
    struct rbp_priv *priv = rbp_get_priv(tp);
    if (before(tp->snd_una, tp->write_seq))
      priv->unordered = tp->write_seq - tp->snd_una;
    else
      priv->unordered = 0;
  }

  /* Phase 4: Divide receive window (Eq 4) */
  mptcp_for_each_tp(mpcb, tp) {
    struct rbp_priv *priv = rbp_get_priv(tp);
    if (priv->alloc > priv->unordered)
      sum_positive += (priv->alloc - priv->unordered);
  }

  if (sum_positive == 0)
    sum_positive = 1;

  mptcp_for_each_tp(mpcb, tp) {
    struct rbp_priv *priv = rbp_get_priv(tp);
    if (priv->alloc <= priv->unordered) {
      priv->rwnd = 0;
    } else {
      priv->rwnd = (u32)((u64)meta_rwnd *
                   (priv->alloc - priv->unordered) / sum_positive);
    }
  }
}

/*
 * ========================================================================
 * Subflow Selection: min-RTT variant (for "rbp" scheduler)
 * ========================================================================
 */

static struct sock *
rbp_get_subflow_rtt(struct mptcp_cb *mpcb, struct sk_buff *skb,
                    bool (*selector)(const struct tcp_sock *),
                    bool zero_wnd_test, bool *force)
{
  struct sock *bestsk = NULL;
  u32 min_srtt = 0xffffffff;
  bool found_unused = false;
  bool found_unused_una = false;
  struct sock *sk;

  mptcp_for_each_sk(mpcb, sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct rbp_priv *priv = rbp_get_priv(tp);
    bool unused = false;

    if (!(*selector)(tp))
      continue;

    if (!mptcp_dont_reinject_skb(tp, skb))
      unused = true;
    else if (found_unused)
      continue;

    if (mptcp_is_def_unavailable(sk))
      continue;

    if (mptcp_is_temp_unavailable(sk, skb, zero_wnd_test)) {
      if (unused)
        found_unused_una = true;
      continue;
    }

    /* RBP: skip subflows with zero allocated window */
    if (priv->rwnd == 0) {
      if (unused)
        found_unused_una = true;
      continue;
    }

    if (unused) {
      if (!found_unused) {
        min_srtt = 0xffffffff;
        bestsk = NULL;
      }
      found_unused = true;
    }

    if (tp->srtt_us < min_srtt) {
      min_srtt = tp->srtt_us;
      bestsk = sk;
    }
  }

  if (bestsk) {
    *force = found_unused;
  } else {
    *force = found_unused_una;
  }
  return bestsk;
}

/*
 * ========================================================================
 * Subflow Selection: forward-delay variant (for "rbp_fwd" scheduler)
 * ========================================================================
 */

static struct sock *
rbp_get_subflow_fwd(struct mptcp_cb *mpcb, struct sk_buff *skb,
                    bool (*selector)(const struct tcp_sock *),
                    bool zero_wnd_test, bool *force)
{
  struct sock *bestsk = NULL;
  u32 min_fwd_dly = 0xffffffff;
  bool found_unused = false;
  bool found_unused_una = false;
  struct sock *sk;

  mptcp_for_each_sk(mpcb, sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct rbp_priv *priv = rbp_get_priv(tp);
    bool unused = false;

    if (!(*selector)(tp))
      continue;

    if (!mptcp_dont_reinject_skb(tp, skb))
      unused = true;
    else if (found_unused)
      continue;

    if (mptcp_is_def_unavailable(sk))
      continue;

    if (mptcp_is_temp_unavailable(sk, skb, zero_wnd_test)) {
      if (unused)
        found_unused_una = true;
      continue;
    }

    /* RBP: skip subflows with zero allocated window */
    if (priv->rwnd == 0) {
      if (unused)
        found_unused_una = true;
      continue;
    }

    if (unused) {
      if (!found_unused) {
        min_fwd_dly = 0xffffffff;
        bestsk = NULL;
      }
      found_unused = true;
    }

    if (tp->sfw_dly_us < min_fwd_dly) {
      min_fwd_dly = tp->sfw_dly_us;
      bestsk = sk;
    }
  }

  if (bestsk) {
    *force = found_unused;
  } else {
    *force = found_unused_una;
  }
  return bestsk;
}

/*
 * ========================================================================
 * get_subflow callbacks (with buffer division)
 * ========================================================================
 */

static struct sock *rbp_get_available_subflow_rtt(struct sock *meta_sk,
                                                  struct sk_buff *skb,
                                                  bool zero_wnd_test)
{
  struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
  struct sock *sk;
  bool force;

  if (mpcb->cnt_subflows == 1) {
    sk = (struct sock *)mpcb->connection_list;
    if (!mptcp_is_available(sk, skb, zero_wnd_test))
      sk = NULL;
    return sk;
  }

  /* Recompute buffer division */
  rbp_divide_buffer(meta_sk);

  if (meta_sk->sk_shutdown & RCV_SHUTDOWN && skb && mptcp_is_data_fin(skb)) {
    mptcp_for_each_sk(mpcb, sk) {
      if (tcp_sk(sk)->mptcp->path_index == mpcb->dfin_path_index &&
          mptcp_is_available(sk, skb, zero_wnd_test))
        return sk;
    }
  }

  sk = rbp_get_subflow_rtt(mpcb, skb, &subflow_is_active,
                           zero_wnd_test, &force);
  if (force)
    return sk;

  sk = rbp_get_subflow_rtt(mpcb, skb, &subflow_is_backup,
                           zero_wnd_test, &force);
  if (!force && skb)
    TCP_SKB_CB(skb)->path_mask = 0;
  return sk;
}

static struct sock *rbp_get_available_subflow_fwd(struct sock *meta_sk,
                                                  struct sk_buff *skb,
                                                  bool zero_wnd_test)
{
  struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
  struct sock *sk;
  bool force;

  if (mpcb->cnt_subflows == 1) {
    sk = (struct sock *)mpcb->connection_list;
    if (!mptcp_is_available(sk, skb, zero_wnd_test))
      sk = NULL;
    return sk;
  }

  /* Recompute buffer division */
  rbp_divide_buffer(meta_sk);

  if (meta_sk->sk_shutdown & RCV_SHUTDOWN && skb && mptcp_is_data_fin(skb)) {
    mptcp_for_each_sk(mpcb, sk) {
      if (tcp_sk(sk)->mptcp->path_index == mpcb->dfin_path_index &&
          mptcp_is_available(sk, skb, zero_wnd_test))
        return sk;
    }
  }

  sk = rbp_get_subflow_fwd(mpcb, skb, &subflow_is_active,
                           zero_wnd_test, &force);
  if (force)
    return sk;

  sk = rbp_get_subflow_fwd(mpcb, skb, &subflow_is_backup,
                           zero_wnd_test, &force);
  if (!force && skb)
    TCP_SKB_CB(skb)->path_mask = 0;
  return sk;
}

/*
 * ========================================================================
 * Receive buffer optimization (same as default scheduler)
 * ========================================================================
 */

static struct sk_buff *rbp_rcv_buf_optimization(struct sock *sk, int penal)
{
  struct sock *meta_sk;
  const struct tcp_sock *tp = tcp_sk(sk);
  struct tcp_sock *tp_it;
  struct sk_buff *skb_head;
  struct rbp_priv *priv = rbp_get_priv(tp);

  if (tp->mpcb->cnt_subflows == 1)
    return NULL;

  meta_sk = mptcp_meta_sk(sk);
  skb_head = tcp_write_queue_head(meta_sk);

  if (!skb_head || skb_head == tcp_send_head(meta_sk))
    return NULL;

  if (!penal && sk_stream_memory_free(meta_sk))
    goto retrans;

  if (tcp_time_stamp - priv->last_rbuf_opti < usecs_to_jiffies(tp->srtt_us >> 3))
    goto retrans;

  mptcp_for_each_tp(tp->mpcb, tp_it) {
    if (tp_it != tp && TCP_SKB_CB(skb_head)->path_mask &
                           mptcp_pi_to_flag(tp_it->mptcp->path_index)) {
      if (tp->srtt_us < tp_it->srtt_us &&
          inet_csk((struct sock *)tp_it)->icsk_ca_state == TCP_CA_Open) {
        u32 prior_cwnd = tp_it->snd_cwnd;

        tp_it->snd_cwnd = max(tp_it->snd_cwnd >> 1U, 1U);

        if (prior_cwnd >= tp_it->snd_ssthresh)
          tp_it->snd_ssthresh = max(tp_it->snd_ssthresh >> 1U, 2U);

        priv->last_rbuf_opti = tcp_time_stamp;
      }
      break;
    }
  }

retrans:
  if (!(TCP_SKB_CB(skb_head)->path_mask &
        mptcp_pi_to_flag(tp->mptcp->path_index))) {
    bool do_retrans = false;
    mptcp_for_each_tp(tp->mpcb, tp_it) {
      if (tp_it != tp && TCP_SKB_CB(skb_head)->path_mask &
                             mptcp_pi_to_flag(tp_it->mptcp->path_index)) {
        if (tp_it->snd_cwnd <= 4) {
          do_retrans = true;
          break;
        }

        if (4 * tp->srtt_us >= tp_it->srtt_us) {
          do_retrans = false;
          break;
        } else {
          do_retrans = true;
        }
      }
    }

    if (do_retrans && mptcp_is_available(sk, skb_head, false))
      return skb_head;
  }
  return NULL;
}

/*
 * ========================================================================
 * next_segment callbacks
 * ========================================================================
 */

/* Common: get next segment from reinject or send queue */
static struct sk_buff *__rbp_next_segment(struct sock *meta_sk, int *reinject,
                                          struct sock *(*get_subflow_fn)(
                                              struct sock *, struct sk_buff *,
                                              bool))
{
  const struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
  struct sk_buff *skb = NULL;

  *reinject = 0;

  if (mpcb->infinite_mapping_snd || mpcb->send_infinite_mapping)
    return tcp_send_head(meta_sk);

  skb = skb_peek(&mpcb->reinject_queue);

  if (skb) {
    *reinject = 1;
  } else {
    skb = tcp_send_head(meta_sk);

    if (!skb && meta_sk->sk_socket &&
        test_bit(SOCK_NOSPACE, &meta_sk->sk_socket->flags) &&
        sk_stream_wspace(meta_sk) < sk_stream_min_wspace(meta_sk)) {
      struct sock *subsk = get_subflow_fn(meta_sk, NULL, false);
      if (!subsk)
        return NULL;

      skb = rbp_rcv_buf_optimization(subsk, 0);
      if (skb)
        *reinject = -1;
    }
  }
  return skb;
}

/* Common next_segment logic with RBP limit enforcement */
static struct sk_buff *rbp_next_segment_common(struct sock *meta_sk,
                                               int *reinject,
                                               struct sock **subsk,
                                               unsigned int *limit,
                                               struct sock *(*get_subflow_fn)(
                                                   struct sock *,
                                                   struct sk_buff *, bool))
{
  struct sk_buff *skb = __rbp_next_segment(meta_sk, reinject, get_subflow_fn);
  unsigned int mss_now;
  struct tcp_sock *subtp;
  struct rbp_priv *priv;
  u16 gso_max_segs;
  u32 max_len, max_segs, window, needed;

  *limit = 0;

  if (!skb)
    return NULL;

  *subsk = get_subflow_fn(meta_sk, skb, false);
  if (!*subsk)
    return NULL;

  subtp = tcp_sk(*subsk);
  priv = rbp_get_priv(subtp);
  mss_now = tcp_current_mss(*subsk);

  if (!*reinject &&
      unlikely(!tcp_snd_wnd_test(tcp_sk(meta_sk), skb, mss_now))) {
    skb = rbp_rcv_buf_optimization(*subsk, 1);
    if (skb)
      *reinject = -1;
    else
      return NULL;
  }

  if (skb->len <= mss_now) {
    /* RBP: even for single segments, enforce rwnd limit */
    if (priv->rwnd > 0 && priv->rwnd < (u32)skb->len)
      *limit = priv->rwnd;
    return skb;
  }

  gso_max_segs = (*subsk)->sk_gso_max_segs;
  if (!gso_max_segs)
    gso_max_segs = 1;
  max_segs = min_t(unsigned int, tcp_cwnd_test(subtp, skb), gso_max_segs);
  if (!max_segs)
    return NULL;

  max_len = mss_now * max_segs;
  window = tcp_wnd_end(subtp) - subtp->write_seq;

  needed = min(skb->len, window);
  if (max_len <= skb->len)
    *limit = max_len;
  else
    *limit = needed;

  /* RBP: further constrain by per-subflow rwnd */
  if (priv->rwnd > 0 && *limit > priv->rwnd)
    *limit = priv->rwnd;

  return skb;
}

static struct sk_buff *rbp_next_segment(struct sock *meta_sk, int *reinject,
                                        struct sock **subsk,
                                        unsigned int *limit)
{
  return rbp_next_segment_common(meta_sk, reinject, subsk, limit,
                                 rbp_get_available_subflow_rtt);
}

static struct sk_buff *rbp_fwd_next_segment(struct sock *meta_sk, int *reinject,
                                            struct sock **subsk,
                                            unsigned int *limit)
{
  return rbp_next_segment_common(meta_sk, reinject, subsk, limit,
                                 rbp_get_available_subflow_fwd);
}

/*
 * ========================================================================
 * Initialization and registration
 * ========================================================================
 */

static void rbp_init(struct sock *sk)
{
  struct tcp_sock *tp = tcp_sk(sk);
  struct rbp_priv *priv = rbp_get_priv(tp);

  priv->acwnd = tp->snd_cwnd;
  priv->alloc = 0;
  priv->rwnd = 0;
  priv->unordered = 0;
  priv->last_rbuf_opti = tcp_time_stamp;
}

static struct mptcp_sched_ops mptcp_sched_rbp = {
    .get_subflow = rbp_get_available_subflow_rtt,
    .next_segment = rbp_next_segment,
    .init = rbp_init,
    .name = "rbp",
    .owner = THIS_MODULE,
};

static struct mptcp_sched_ops mptcp_sched_rbp_fwd = {
    .get_subflow = rbp_get_available_subflow_fwd,
    .next_segment = rbp_fwd_next_segment,
    .init = rbp_init,
    .name = "rbp_fwd",
    .owner = THIS_MODULE,
};

static int __init mptcp_rbp_register(void)
{
  BUILD_BUG_ON(sizeof(struct rbp_priv) > MPTCP_SCHED_SIZE);

  if (mptcp_register_scheduler(&mptcp_sched_rbp))
    return -1;
  if (mptcp_register_scheduler(&mptcp_sched_rbp_fwd)) {
    mptcp_unregister_scheduler(&mptcp_sched_rbp);
    return -1;
  }
  return 0;
}

static void __exit mptcp_rbp_unregister(void)
{
  mptcp_unregister_scheduler(&mptcp_sched_rbp_fwd);
  mptcp_unregister_scheduler(&mptcp_sched_rbp);
}

module_init(mptcp_rbp_register);
module_exit(mptcp_rbp_unregister);

MODULE_AUTHOR("Parth");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MPTCP Receive Buffer Pre-division (RBP) Scheduler");

/*
 *	MPTCP implementation - JAIPUR congestion control Algorithm
 *
 *	Initial Design & Implementation:
 *	Parth Somani <psomani16k@gmail.com>
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */
#include "linux/printk.h"
#include "net/inet_connection_sock.h"
#include <linux/timekeeping.h>
#include <net/mptcp.h>
#include <net/tcp.h>

#include <linux/module.h>

/* Scaling is done in the numerator with alpha_scale_num and in the denominator
 * with alpha_scale_den.
 *
 * To downscale, we just need to use alpha_scale.
 *
 * We have: alpha_scale = alpha_scale_num / (alpha_scale_den ^ 2)
 */
static int alpha_scale_den = 10;
static int alpha_scale_num = 32;
static int alpha_scale = 12;

struct mptcp_jaipur {
  u64 alpha;
  bool forced_update;
  long min_fw_dly;
};

static inline int mptcp_jaipur_sk_can_send(const struct sock *sk) {
  return mptcp_sk_can_send(sk) && tcp_sk(sk)->srtt_us;
}

/* Per-PATH state: min_fw_dly lives in each subflow's own CC-private area.
 * Pass the subflow sk, NOT mptcp_meta_sk(sk).
 */
static inline long mptcp_get_min_fw_dly(const struct sock *sk) {
  return ((struct mptcp_jaipur *)inet_csk_ca(sk))->min_fw_dly;
}

/* Update this path's running minimum forward delay; returns the current min. */
static inline long mptcp_update_min_fw_dly(const struct sock *sk, long fw_dly) {
  struct mptcp_jaipur *ca = inet_csk_ca(sk);
  if (ca->min_fw_dly < 0 || fw_dly < ca->min_fw_dly)
    ca->min_fw_dly = fw_dly;
  return ca->min_fw_dly;
}

static inline u64 mptcp_get_alpha(const struct sock *meta_sk) {
  return ((struct mptcp_jaipur *)inet_csk_ca(meta_sk))->alpha;
}

static inline void mptcp_set_alpha(const struct sock *meta_sk, u64 alpha) {
  ((struct mptcp_jaipur *)inet_csk_ca(meta_sk))->alpha = alpha;
}

/* Per-PATH setter: writes THIS subflow's min_fw_dly slot. */
static inline void mptcp_set_min_fw_dly(const struct sock *sk, long min_dly) {
  ((struct mptcp_jaipur *)inet_csk_ca(sk))->min_fw_dly = min_dly;
}

static inline u64 mptcp_jaipur_scale(u32 val, int scale) {
  return (u64)val << scale;
}

static inline bool mptcp_get_forced(const struct sock *meta_sk) {
  return ((struct mptcp_jaipur *)inet_csk_ca(meta_sk))->forced_update;
}

static inline void mptcp_set_forced(const struct sock *meta_sk, bool force) {
  ((struct mptcp_jaipur *)inet_csk_ca(meta_sk))->forced_update = force;
}

static void mptcp_ccc_recalc_alpha(const struct sock *sk) {
  const struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;
  const struct sock *sub_sk;
  int best_cwnd = 0, best_rtt = 0, can_send = 0;
  u64 max_numerator = 0, sum_denominator = 0, alpha = 1;

  if (!mpcb)
    return;

  /* Only one subflow left - fall back to normal reno-behavior
   * (set alpha to 1)
   */
  if (mpcb->cnt_established <= 1)
    goto exit;

  /* Do regular alpha-calculation for multiple subflows */

  /* Find the max numerator of the alpha-calculation */
  mptcp_for_each_sk(mpcb, sub_sk) {
    struct tcp_sock *sub_tp = tcp_sk(sub_sk);
    u64 tmp;

    if (!mptcp_jaipur_sk_can_send(sub_sk))
      continue;

    can_send++;

    /* We need to look for the path, that provides the max-value.
     * Integer-overflow is not possible here, because
     * tmp will be in u64.
     */
    tmp = div64_u64(mptcp_jaipur_scale(sub_tp->snd_cwnd, alpha_scale_num),
                    (u64)sub_tp->srtt_us * sub_tp->srtt_us);

    if (tmp >= max_numerator) {
      max_numerator = tmp;
      best_cwnd = sub_tp->snd_cwnd;
      best_rtt = sub_tp->srtt_us;
    }
  }

  /* No subflow is able to send - we don't care anymore */
  if (unlikely(!can_send))
    goto exit;

  /* Calculate the denominator */
  mptcp_for_each_sk(mpcb, sub_sk) {
    struct tcp_sock *sub_tp = tcp_sk(sub_sk);

    if (!mptcp_jaipur_sk_can_send(sub_sk))
      continue;

    sum_denominator += div_u64(
        mptcp_jaipur_scale(sub_tp->snd_cwnd, alpha_scale_den) * best_rtt,
        sub_tp->srtt_us);
  }
  sum_denominator *= sum_denominator;
  if (unlikely(!sum_denominator)) {
    pr_err("%s: sum_denominator == 0, cnt_established:%d\n", __func__,
           mpcb->cnt_established);
    mptcp_for_each_sk(mpcb, sub_sk) {
      struct tcp_sock *sub_tp = tcp_sk(sub_sk);
      pr_err("%s: pi:%d, state:%d\n, rtt:%u, cwnd: %u", __func__,
             sub_tp->mptcp->path_index, sub_sk->sk_state, sub_tp->srtt_us,
             sub_tp->snd_cwnd);
    }
  }

  alpha = div64_u64(mptcp_jaipur_scale(best_cwnd, alpha_scale_num),
                    sum_denominator);

  if (unlikely(!alpha))
    alpha = 1;

exit:
  mptcp_set_alpha(mptcp_meta_sk(sk), alpha);
}

static void mptcp_jaipur_init(struct sock *sk) {
  /* Per-path field: init on THIS subflow's own storage (harmless for plain
   * TCP, where the field is simply unused). */
  mptcp_set_min_fw_dly(sk, -1);

  if (mptcp(tcp_sk(sk))) {
    /* Shared fields: init on the meta socket. */
    mptcp_set_forced(mptcp_meta_sk(sk), 0);
    mptcp_set_alpha(mptcp_meta_sk(sk), 1);
  }
  /* If we do not mptcp, behave like reno: return */
}

static void mptcp_jaipur_cwnd_event(struct sock *sk, enum tcp_ca_event event) {
  if (event == CA_EVENT_LOSS)
    mptcp_ccc_recalc_alpha(sk);
}

static void mptcp_jaipur_set_state(struct sock *sk, u8 ca_state) {
  if (!mptcp(tcp_sk(sk)))
    return;

  mptcp_set_forced(mptcp_meta_sk(sk), 1);
}

static void mptcp_jaipur_cong_avoid(struct sock *sk, u32 ack, u32 acked) {
  struct tcp_sock *tp = tcp_sk(sk);
  const struct mptcp_cb *mpcb = tp->mpcb;
  int snd_cwnd;

  if (!mptcp(tp)) {
    tcp_reno_cong_avoid(sk, ack, acked);
    return;
  }

  if (tp->sfw_dly_us != 0) {
    u32 dly = tp->sfw_dly_us;
    /* Per-path minimum: update this subflow's own slot. */
    long min = mptcp_update_min_fw_dly(sk, dly);
    u8 path = tp->mptcp->path_index;
    /* Monotonic clock in ns. Under DCE/NUSE ktime_get() is backed by
     * lib_current_ns(), so this is ns-3 simulation time, not host time.
     */
    u64 now_ns = ktime_get_ns();
    u64 mew = ((u64)dly * 10000) - ((u64)min * 10000);
    mew = mew / dly;
    printk("Time: %llu, Path: %u, mew: %llu\n", now_ns, path, mew);
  }

  if (!tcp_is_cwnd_limited(sk))
    return;

  if (tp->snd_cwnd <= tp->snd_ssthresh) {
    /* In "safe" area, increase. */
    tcp_slow_start(tp, acked);
    mptcp_ccc_recalc_alpha(sk);
    return;
  }

  if (mptcp_get_forced(mptcp_meta_sk(sk))) {
    mptcp_ccc_recalc_alpha(sk);
    mptcp_set_forced(mptcp_meta_sk(sk), 0);
  }

  if (mpcb->cnt_established > 1) {
    u64 alpha = mptcp_get_alpha(mptcp_meta_sk(sk));

    /* This may happen, if at the initialization, the mpcb
     * was not yet attached to the sock, and thus
     * initializing alpha failed.
     */
    if (unlikely(!alpha))
      alpha = 1;

    snd_cwnd = (int)div_u64((u64)mptcp_jaipur_scale(1, alpha_scale), alpha);

    /* snd_cwnd_cnt >= max (scale * tot_cwnd / alpha, cwnd)
     * Thus, we select here the max value.
     */
    if (snd_cwnd < tp->snd_cwnd)
      snd_cwnd = tp->snd_cwnd;
  } else {
    snd_cwnd = tp->snd_cwnd;
  }

  if (tp->snd_cwnd_cnt >= snd_cwnd) {
    if (tp->snd_cwnd < tp->snd_cwnd_clamp) {
      tp->snd_cwnd++;
      mptcp_ccc_recalc_alpha(sk);
    }

    tp->snd_cwnd_cnt = 0;
  } else {
    tp->snd_cwnd_cnt++;
  }
}

static struct tcp_congestion_ops mptcp_jaipur = {
    /* Sets up per-connection state (alpha=1, forced=0); called once when CC is
     * initialized for the socket, i.e. at connection/subflow start. */
    .init = mptcp_jaipur_init,
    /* Computes the new slow-start threshold (Reno: halve cwnd); called on a
     * congestion event that requires shrinking the window. */
    .ssthresh = tcp_reno_ssthresh,
    /* Grows snd_cwnd using the coupled alpha factor; called on every incoming
     * ACK to drive window growth (slow start / congestion avoidance). */
    .cong_avoid = mptcp_jaipur_cong_avoid,
    /* Recalculates alpha on CA_EVENT_LOSS; called whenever a cwnd-related
     * event fires (e.g. a loss is detected). */
    .cwnd_event = mptcp_jaipur_cwnd_event,
    /* Marks alpha stale via the forced_update flag; called on every TCP CA
     * state transition (open/disorder/recovery/loss). */
    .set_state = mptcp_jaipur_set_state,
    /* Owning module for refcounting; keeps the module pinned while any socket
     * uses this algorithm. */
    .owner = THIS_MODULE,
    /* User-visible name used to select this CC (sysctl / TCP_CONGESTION). */
    .name = "jaipur",
};

static int __init mpatc_jaipur_register(void) {
  BUILD_BUG_ON(sizeof(struct mptcp_jaipur) > ICSK_CA_PRIV_SIZE);
  return tcp_register_congestion_control(&mptcp_jaipur);
}

static void __exit mptcp_jaipur_unregister(void) {
  tcp_unregister_congestion_control(&mptcp_jaipur);
}

module_init(mpatc_jaipur_register);
module_exit(mptcp_jaipur_unregister);

MODULE_AUTHOR("Parth Somani");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MPTCP JAIPUR CONGESTION CONTROL ALGORITHM");
MODULE_VERSION("0.1");

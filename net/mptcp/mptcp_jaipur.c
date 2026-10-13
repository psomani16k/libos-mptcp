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
#include <net/mptcp.h>
#include <net/tcp.h>

#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>

/* Set to 0 to compile the trace out entirely. */
#define JAIPUR_DEBUG 1
/* Minimum spacing between two "sample" lines of the same subflow. Event lines
 * (init, ss_exit, cut, loss, state) are never rate limited.
 */
#define JAIPUR_TRACE_MS 10

/* mu, gamma, P and the filter gain are fractions in (-1, 1). The kernel has no
 * floating point, so they are carried as integers scaled by 2^20 (Q20).
 */
#define JAIPUR_SHIFT 20
#define JAIPUR_SCALE (1 << JAIPUR_SHIFT)

/* gamma is stored with 16 extra fractional bits (Q36). It is updated on every
 * ACK, so dt is often tens of microseconds and a single step is far smaller
 * than one Q20 unit; without the guard bits those steps would round away.
 */
#define JAIPUR_GAMMA_GUARD 16

/* Threshold gains per millisecond, in Q20. Values taken from GCC:
 * k_u = 0.01 when mu >= gamma, k_d = 0.00018 when mu < gamma.
 */
#define JAIPUR_K_U 10486
#define JAIPUR_K_D 189

/* Per-subflow state, living in that subflow's icsk_ca_priv area.
 *
 * NOTE: every MPTCP socket -- the meta socket and each subflow -- gets its own
 * copy of this struct. There is currently no shared/meta-level state; if the
 * JAIPUR rule ends up needing a coupling factor across subflows (the way LIA's
 * alpha did), that field must be read and written on mptcp_meta_sk(sk), while
 * everything below stays on the subflow sk.
 */
struct mptcp_jaipur {
  /* Adaptive threshold, Q36: the level of mu that is normal on THIS path. */
  s64 gamma;
  /* ktime_get_ns() at the last gamma update. tcp_time_stamp is not used
   * because it only advances once per jiffy.
   */
  u64 last_update_ns;
  /* Running minimum of tp->sfw_dly_us observed on THIS path.
   * U32_MAX means "no sample yet" -- it compares greater than any real
   * value, so the update below needs no separate unset check.
   */
  u32 min_fw_dly;
  /* Congestion pressure P = mu - gamma, Q20. Negative: path underloaded. */
  s32 pressure;
  /* Latest mu, Q20. Kept only so event lines can print it. */
  u32 mu;
  /* Simulation time (ms) of the last "sample" line, see JAIPUR_TRACE_MS. */
  u32 last_trace_ms;
  /* snd_nxt at the moment of the last delay-driven decrease. Another decrease
   * is allowed only once everything that was in flight then has been acked,
   * i.e. one RTT later -- the Vegas beg_snd_nxt pattern. Until then the ACKs
   * still arriving were sent under the old window and carry the same delay
   * rise the cut already answered. Init sets it to snd_una so the first
   * decrease is never held.
   */
  u32 cut_end_seq;
  /* ACKs with P > 0 that were held by cut_end_seq since the last decrease.
   * Trace only; shows how many per-ACK cuts the gate absorbed.
   */
  u32 held;
  /* 0 until gamma has been seeded with the first mu. */
  u8 gamma_valid;
  /* Set by init. The SYN-ACK's ACK reaches pkts_acked before init has run,
   * on zeroed memory; this flag lets that call be ignored instead of
   * computing mu from min_fw_dly == 0.
   */
  u8 inited;
};

/* Per-PATH state: pass the subflow sk, NOT mptcp_meta_sk(sk). */
static inline u32 mptcp_jaipur_get_min_fw_dly(const struct sock *sk) {
  return ((struct mptcp_jaipur *)inet_csk_ca(sk))->min_fw_dly;
}

/* Fold this path's latest forward-delay sample into its running minimum.
 *
 * tp->sfw_dly_us is derived from (rcv_tsval - rcv_tsecr), so it carries the
 * unknown constant offset between the two hosts' timestamp clocks. That offset
 * only cancels when samples are differenced against another sample from the
 * SAME path, which is why this minimum is per-subflow and must never be
 * compared as an absolute value against another subflow's.
 */
static inline u32 mptcp_jaipur_update_min_fw_dly(const struct sock *sk) {
  struct mptcp_jaipur *ca = inet_csk_ca(sk);
  u32 dly = tcp_sk(sk)->sfw_dly_us;

  if (dly != 0 && dly < ca->min_fw_dly)
    ca->min_fw_dly = dly;

  return ca->min_fw_dly;
}

static inline u32 mptcp_jaipur_now_ms(void) {
  return div_u64(ktime_get_ns(), NSEC_PER_MSEC);
}

#if JAIPUR_DEBUG
/* One line per event, key=value so it can be parsed with awk.
 *
 *   t        simulation time, ms
 *   path     MPTCP path index (0 for the meta socket / plain TCP)
 *   ev       init | sample | ss_exit | cut | loss | state | preinit
 *   dly/min  smoothed forward delay and its running minimum, us
 *   mu/gamma/P   the congestion signal, Q20 (divide by 1048576)
 *   cwnd/ssthresh  after the event; prev = cwnd before a decrease
 *   inflight packets_out, srtt in us, state = icsk_ca_state
 *            (0 Open, 1 Disorder, 2 CWR, 3 Recovery, 4 Loss)
 *   held     P > 0 ACKs absorbed by the once-per-RTT gate since the last cut
 */
static void mptcp_jaipur_log(struct sock *sk, const char *ev, u32 prev) {
  const struct mptcp_jaipur *ca = inet_csk_ca(sk);
  const struct tcp_sock *tp = tcp_sk(sk);

  /* sfw_dly_us and min are stored <<3 like srtt_us; print plain us. */
  pr_info("jaipur: t=%u path=%u ev=%s dly=%u min=%u mu=%u gamma=%d P=%d "
          "cwnd=%u prev=%u ssthresh=%u inflight=%u srtt=%u state=%u held=%u\n",
          mptcp_jaipur_now_ms(), tp->mptcp ? tp->mptcp->path_index : 0, ev,
          tp->sfw_dly_us >> 3,
          ca->min_fw_dly == U32_MAX ? 0 : ca->min_fw_dly >> 3, ca->mu,
          (s32)(ca->gamma >> JAIPUR_GAMMA_GUARD), ca->pressure, tp->snd_cwnd,
          prev, tp->snd_ssthresh, tp->packets_out, tp->srtt_us >> 3,
          inet_csk(sk)->icsk_ca_state, ca->held);
}

/* Periodic "sample" line, at most one per JAIPUR_TRACE_MS per subflow. */
static void mptcp_jaipur_trace_sample(struct sock *sk) {
  struct mptcp_jaipur *ca = inet_csk_ca(sk);
  u32 now = mptcp_jaipur_now_ms();

  if (now - ca->last_trace_ms < JAIPUR_TRACE_MS)
    return;
  ca->last_trace_ms = now;
  mptcp_jaipur_log(sk, "sample", 0);
}

static void mptcp_jaipur_state(struct sock *sk, u8 new_state) {
  if (new_state != inet_csk(sk)->icsk_ca_state)
    mptcp_jaipur_log(sk, "state", new_state);
}
#else
static inline void mptcp_jaipur_log(struct sock *sk, const char *ev,
                                    u32 prev) {}
static inline void mptcp_jaipur_trace_sample(struct sock *sk) {}
#endif

static void mptcp_jaipur_init(struct sock *sk) {
  struct mptcp_jaipur *ca = inet_csk_ca(sk);

  ca->gamma = 0;
  ca->last_update_ns = 0;
  ca->min_fw_dly = U32_MAX;
  ca->pressure = 0;
  ca->mu = 0;
  ca->last_trace_ms = 0;
  ca->cut_end_seq = tcp_sk(sk)->snd_una;
  ca->held = 0;
  ca->gamma_valid = 0;
  ca->inited = 1;

  mptcp_jaipur_log(sk, "init", 0);
}

/* Update the congestion signal: mu, gamma and P.
 *
 * This lives in pkts_acked rather than cong_avoid because cong_avoid is not
 * called while the subflow is in CWR/Recovery, which would freeze gamma exactly
 * when the path is congested. tcp_rtt_estimator() has already run for this ACK,
 * so tp->sfw_dly_us is fresh.
 */
static void mptcp_jaipur_pkts_acked(struct sock *sk, u32 num_acked,
                                    s32 rtt_us) {
  struct mptcp_jaipur *ca = inet_csk_ca(sk);
  struct tcp_sock *tp = tcp_sk(sk);
  u32 dly = tp->sfw_dly_us;
  u32 min_dly, mu;
  u64 now;

  if (!ca->inited) {
    mptcp_jaipur_log(sk, "preinit", 0);
    return;
  }

  /* dly == 0: no forward-delay sample yet (also true for the meta socket). */
  if (!mptcp(tp) || dly == 0)
    return;

  min_dly = mptcp_jaipur_update_min_fw_dly(sk);

  /* mu = (OWD - OWD_min) / OWD, in [0, 1). The <<3 scaling of sfw_dly_us
   * cancels in the ratio.
   */
  mu = div_u64((u64)(dly - min_dly) << JAIPUR_SHIFT, dly);
  ca->mu = mu;

  now = ktime_get_ns();

  if (!ca->gamma_valid) {
    /* gamma(0) = mu(0) */
    ca->gamma = (s64)mu << JAIPUR_GAMMA_GUARD;
    ca->gamma_valid = 1;
  } else {
    /* gamma += dt * k * (mu - gamma), with k chosen by the sign of the error.
     * dt is capped so that dt * k cannot overflow; the gain is capped at 1 so
     * that gamma never overshoots mu after an idle period.
     */
    u64 dt_us = min_t(u64, div_u64(now - ca->last_update_ns, NSEC_PER_USEC),
                      U32_MAX);
    s32 gamma = ca->gamma >> JAIPUR_GAMMA_GUARD;
    u32 k = (s32)mu >= gamma ? JAIPUR_K_U : JAIPUR_K_D;
    u64 gain = min_t(u64, div_u64(dt_us * k, USEC_PER_MSEC), JAIPUR_SCALE);

    /* Q20 * Q20 = Q40, brought down to gamma's Q36. */
    ca->gamma += ((s64)gain * ((s32)mu - gamma)) >>
                 (JAIPUR_SHIFT - JAIPUR_GAMMA_GUARD);
  }
  ca->last_update_ns = now;

  ca->pressure = (s32)mu - (s32)(ca->gamma >> JAIPUR_GAMMA_GUARD);

  mptcp_jaipur_trace_sample(sk);
}

static void mptcp_jaipur_cong_avoid(struct sock *sk, u32 ack, u32 acked) {
  struct mptcp_jaipur *ca = inet_csk_ca(sk);
  struct tcp_sock *tp = tcp_sk(sk);
  s32 pressure;
  u32 w;

  /* Plain TCP: behave like Reno, same fallback LIA used. */
  if (!mptcp(tp)) {
    tcp_reno_cong_avoid(sk, ack, acked);
    return;
  }

  pressure = ca->pressure;

  /* P > 0: queue larger than expected, back off without waiting for a loss.
   * Not guarded by tcp_is_cwnd_limited() -- congestion is a reason to shrink
   * whether or not the window is currently the bottleneck.
   */
  if (pressure > 0) {
    u32 prev = tp->snd_cwnd;
    bool slow_start;

    /* At most one decrease per RTT. Data sent before the last cut is still
     * being acked: those ACKs reflect the old window, and cutting again on
     * each of them is what turned a P of 0.05 into a 53 -> 12 cascade.
     * Hold the window (no growth either) until the cut has been seen by the
     * network, i.e. until snd_nxt at the time of the cut has been acked.
     */
    if (before(ack, ca->cut_end_seq)) {
      ca->held++;
      return;
    }

    /* Strict '<' as in cc.md: once slow start has been left with
     * cwnd == ssthresh, further reductions are the proportional ones.
     */
    slow_start = tp->snd_cwnd < tp->snd_ssthresh;
    if (slow_start)
      tp->snd_ssthresh = max(tp->snd_cwnd >> 1U, 2U);
    else
      /* cwnd * (1 - P), rounded to nearest: truncating would cost a full
       * packet on every ACK for any P > 0, however small.
       */
      tp->snd_ssthresh = max_t(
          u32,
          ((u64)tp->snd_cwnd * (JAIPUR_SCALE - pressure) + JAIPUR_SCALE / 2) >>
              JAIPUR_SHIFT,
          2U);

    tp->snd_cwnd = tp->snd_ssthresh;
    tp->snd_cwnd_cnt = 0;

    /* Only log cuts that changed something: with rounding, a small P leaves
     * cwnd untouched, and those ACKs are not interesting. The gate is armed
     * only by an effective cut, so a P too small to remove a packet cannot
     * block a larger one later in the same RTT.
     */
    if (tp->snd_cwnd != prev) {
      mptcp_jaipur_log(sk, slow_start ? "ss_exit" : "cut", prev);
      ca->cut_end_seq = tp->snd_nxt;
      ca->held = 0;
    }
    return;
  }

  if (!tcp_is_cwnd_limited(sk))
    return;

  /* P <= 0 in slow start: standard exponential growth. */
  if (tp->snd_cwnd <= tp->snd_ssthresh) {
    acked = tcp_slow_start(tp, acked);
    if (!acked)
      return;
  }

  /* P <= 0 in congestion avoidance: cwnd += (1 - P) / cwnd per ACK.
   * snd_cwnd counts whole packets, so this is expressed as "one more packet
   * per w ACKs" with w = cwnd / (1 - P). P == 0 gives w == cwnd, i.e. Reno;
   * P -> -1 gives w == cwnd / 2, i.e. twice Reno's rate.
   */
  w = max_t(u32,
            div_u64((u64)tp->snd_cwnd << JAIPUR_SHIFT, JAIPUR_SCALE - pressure),
            1U);
  tcp_cong_avoid_ai(tp, w, acked);
}

/* Loss (or ECN) response: Reno's halving, logged so loss-driven cuts can be
 * told apart from delay-driven ones in the trace.
 */
static u32 mptcp_jaipur_ssthresh(struct sock *sk) {
  u32 ssthresh = tcp_reno_ssthresh(sk);

  mptcp_jaipur_log(sk, "loss", ssthresh);
  return ssthresh;
}

static struct tcp_congestion_ops mptcp_jaipur = {
    /* Sets up per-subflow state; called once when the CC is initialized for
     * the socket, i.e. at connection/subflow start. */
    .init = mptcp_jaipur_init,
    /* Computes the new slow-start threshold (Reno: halve cwnd); called on a
     * congestion event that requires shrinking the window. */
    .ssthresh = mptcp_jaipur_ssthresh,
    /* Grows or shrinks snd_cwnd from the congestion pressure; called on every
     * incoming ACK that may raise the window. */
    .cong_avoid = mptcp_jaipur_cong_avoid,
    /* Updates mu, gamma and the congestion pressure; called on every ACK that
     * acknowledges new data, in all congestion states. */
    .pkts_acked = mptcp_jaipur_pkts_acked,
#if JAIPUR_DEBUG
    /* Trace only: logs transitions into/out of Recovery, Loss, CWR. */
    .set_state = mptcp_jaipur_state,
#endif
    /* Owning module for refcounting; keeps the module pinned while any socket
     * uses this algorithm. */
    .owner = THIS_MODULE,
    /* User-visible name used to select this CC (sysctl / TCP_CONGESTION). */
    .name = "jaipur",
};

static int __init mptcp_jaipur_register(void) {
  BUILD_BUG_ON(sizeof(struct mptcp_jaipur) > ICSK_CA_PRIV_SIZE);
  return tcp_register_congestion_control(&mptcp_jaipur);
}

static void __exit mptcp_jaipur_unregister(void) {
  tcp_unregister_congestion_control(&mptcp_jaipur);
}

module_init(mptcp_jaipur_register);
module_exit(mptcp_jaipur_unregister);

MODULE_AUTHOR("Parth Somani");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MPTCP JAIPUR CONGESTION CONTROL ALGORITHM");
MODULE_VERSION("0.1");

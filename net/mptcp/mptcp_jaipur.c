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

#include <linux/module.h>

/* Per-subflow state, living in that subflow's icsk_ca_priv area.
 *
 * NOTE: every MPTCP socket -- the meta socket and each subflow -- gets its own
 * copy of this struct. There is currently no shared/meta-level state; if the
 * JAIPUR rule ends up needing a coupling factor across subflows (the way LIA's
 * alpha did), that field must be read and written on mptcp_meta_sk(sk), while
 * everything below stays on the subflow sk.
 */
struct mptcp_jaipur {
  /* Running minimum of tp->sfw_dly_us observed on THIS path.
   * U32_MAX means "no sample yet" -- it compares greater than any real
   * value, so the update below needs no separate unset check.
   */
  u32 min_fw_dly;
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

static void mptcp_jaipur_init(struct sock *sk) {
  ((struct mptcp_jaipur *)inet_csk_ca(sk))->min_fw_dly = U32_MAX;
}

static void mptcp_jaipur_cong_avoid(struct sock *sk, u32 ack, u32 acked) {
  struct tcp_sock *tp = tcp_sk(sk);

  /* Plain TCP: behave like Reno, same fallback LIA used. */
  if (!mptcp(tp)) {
    tcp_reno_cong_avoid(sk, ack, acked);
    return;
  }

  /* Sample before any cwnd-limited check -- we want every delay measurement,
   * not just the ones taken while the window is the bottleneck.
   */
  mptcp_jaipur_update_min_fw_dly(sk);

  /* TODO: the JAIPUR window rule goes here. Until it exists, grow like Reno
   * so subflows still make progress. tcp_reno_cong_avoid() applies its own
   * tcp_is_cwnd_limited() guard, so do not add one around it.
   */
  tcp_reno_cong_avoid(sk, ack, acked);
}

static struct tcp_congestion_ops mptcp_jaipur = {
    /* Sets up per-subflow state; called once when the CC is initialized for
     * the socket, i.e. at connection/subflow start. */
    .init = mptcp_jaipur_init,
    /* Computes the new slow-start threshold (Reno: halve cwnd); called on a
     * congestion event that requires shrinking the window. */
    .ssthresh = tcp_reno_ssthresh,
    /* Grows snd_cwnd; called on every incoming ACK to drive window growth
     * (slow start / congestion avoidance). */
    .cong_avoid = mptcp_jaipur_cong_avoid,
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

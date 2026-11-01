# Forward Delay Aware Congestion Control — PoC evaluation

Date of runs: 28 September and 4 October 2026.
Simulation: ns-3.35 + DCE, Linux 4.1 MPTCP v0.89 kernel, script `dce-iperf-mptcp.cc`.
Comparison baseline: LIA (coupled, loss-based) with the same scheduler and topology.

## 1. Summary

| Scenario | Cross traffic on path 1 | FDACC | LIA | FDACC / LIA |
|---|---|---|---|---|
| A | starts 20 s after the MPTCP flow | 23.8 MB, **3.98 Mbit/s** | 42.2 MB, **7.00 Mbit/s** | 57 % |
| B | starts together with the MPTCP flow | 17.8 MB, **2.97 Mbit/s** | 30.1 MB, **5.03 Mbit/s** | 59 % |

Each figure is one 50 s iperf run. The simulation is deterministic: repeated runs
with the same inputs gave identical totals.

In both scenarios FDACC completes the transfer and behaves as specified, but
loses about 40 % of LIA's throughput. The trace shows why (Section 6): the
window rule as written removes slow start within the first RTT, cuts the window
one packet per ACK while the threshold catches up, and stops reacting to delay
on a path that stays congested.

## 2. Topology

Two multihomed hosts, N0 (sender) and N1 (receiver), connected by two disjoint
paths. Each path is made of a forward and a reverse point-to-point link with
independent bandwidth and delay, so the paths are asymmetric by design.

```
                 path 1 (subflow index 1)
        10.0.0.1 ---- R0 ==== 10 Mbit/s, 50 ms  ====> R1 ---- 10.1.0.1
                        <==== 5 Mbit/s, 250 ms  ====
   N0                                                              N1
                 path 2 (subflow index 4)
        10.2.0.1 ---- R2 ==== 5 Mbit/s, 150 ms  ====> R3 ---- 10.3.0.1
                        <==== 5 Mbit/s, 50 ms   ====

   T0 ---- R0   (UDP cross traffic source, 9 Mbit/s, into path 1 forward)
   T1 ---- R1   (UDP cross traffic sink)
```

| Element | Setting |
|---|---|
| Host and router access links | 100 Mbit/s, 1 ms |
| Path 1 forward (N0 → N1) | 10 Mbit/s, 50 ms |
| Path 1 reverse (N1 → N0) | 5 Mbit/s, 250 ms |
| Path 2 forward (N0 → N1) | 5 Mbit/s, 150 ms |
| Path 2 reverse (N1 → N0) | 5 Mbit/s, 50 ms |
| Queues | DropTail, 100 packets, on every link |
| Routing | Policy routing on N0 and N1 so that each subflow is pinned to its own path |
| MPTCP scheduler | `default` (minimum RTT) |
| Path manager | full mesh; the two cross combinations (indices 2 and 3) never establish because there is no route between them |
| SACK | on |
| Cross traffic | iperf UDP, 9 Mbit/s constant rate, T0 → T1, shares path 1's forward link |

Measured from the trace, with empty queues:

| Path | Forward one-way delay | Round-trip time |
|---|---|---|
| 1 | 52 ms | 308 ms |
| 2 | 152.5 ms | 209 ms |

Path 1 has the shorter forward delay and the longer RTT. An RTT-based
scheduler prefers path 2; a forward-delay-based one prefers path 1. The 9 Mbit/s
cross traffic saturates path 1's 10 Mbit/s forward link, so path 1 is the
congested path in both scenarios and path 2 is never congested by anything but
the MPTCP flow itself.

The bottleneck queue on path 1 (100 packets at 10 Mbit/s) adds up to about
120 ms of queueing delay when full. The trace shows path 1's forward delay
rising from 52 ms to 185–220 ms under cross traffic, with peaks above 500 ms
during loss recovery.

## 3. Timeline

Times are simulation time. The MPTCP flow starts at 30 s; "+t" below is time
since that start.

| Sim time | +t | Event |
|---|---|---|
| 0.2 s | | Addresses, policy routes and sysctls applied on all nodes |
| 30.0 s | +0 | iperf server starts on N1; iperf client starts on N0 (TCP, 50 s) |
| 30.31 s | +0.3 | Master subflow (path 1) established; CC state initialised |
| 30.62 s | +0.3 | Path 1 leaves slow start at cwnd 11–12 (both scenarios) |
| 30.82 s | +0.5 | Second subflow (path 2) established |
| 31.25 s | +0.9 | Path 2 leaves slow start at cwnd 11 (both scenarios) |
| 30.0 s | +0 | **Scenario B:** UDP cross traffic starts on path 1, runs 50 s |
| 50.0 s | +20 | **Scenario A:** UDP cross traffic starts on path 1, runs 30 s |
| 80.0 s | +50 | iperf client finishes; UDP traffic stops |
| 81.0 s | +51 | Simulation ends |

## 4. Congestion control configuration

The algorithm runs per subflow. There is no coupling between subflows and no
state on the meta socket. It is selected on all nodes with
`net.ipv4.tcp_congestion_control`, so the receiver's subflows use it too, but
they carry only ACKs.

### Signal

| Quantity | Definition | Source in the kernel |
|---|---|---|
| OWD | smoothed forward one-way delay of the subflow | timestamp option, `rcv_tsval − rcv_tsecr`, smoothed 7/8 like SRTT |
| OWD_min | running minimum of OWD on this subflow, never expires | per-subflow CC state |
| μ | (OWD − OWD_min) / OWD, in [0, 1) | computed on every ACK that acknowledges data |
| γ | adaptive threshold, seeded with μ(0) | `γ += Δt · k · (μ − γ)` |
| P | μ − γ, the congestion pressure | signed |

The timestamp option carries jiffies, so each raw delay sample is a multiple of
4 ms (`HZ = 250`); the smoothed value is finer. Δt is taken from the simulation
clock in microseconds, so γ is updated on every ACK with its exact spacing.

### Constants

| Constant | Value | Meaning |
|---|---|---|
| k_u | 0.01 per ms | gain when μ ≥ γ (time constant 100 ms) |
| k_d | 0.00018 per ms | gain when μ < γ (time constant 5.5 s) |
| gain cap | Δt · k ≤ 1 | prevents overshoot after idle periods |
| fixed point | Q20 for μ, P and gain; Q36 for γ | no floating point in the kernel |

The k values are Google Congestion Control's defaults and have not been tuned.

### Window rule

| Phase | P ≤ 0 (P < 0 in avoidance) | P > 0 |
|---|---|---|
| Slow start (cwnd < ssthresh) | cwnd += 1 per acked packet | ssthresh = max(cwnd/2, 2); cwnd = ssthresh |
| Congestion avoidance | cwnd += (1 − P) / cwnd per acked packet, i.e. 1× to 2× Reno | ssthresh = cwnd · (1 − P), rounded to nearest, floor 2; cwnd = ssthresh |

Decreases are applied on every ACK for which P > 0 and are not rate limited.
A decrease that rounds to no change leaves cwnd untouched. Growth only happens
while the window is the limiting factor (`tcp_is_cwnd_limited`).

Loss response is Reno's: ssthresh = cwnd / 2 on a loss or ECN event. The two
cross-path subflow indices (2, 3) see a SYN loss and nothing else.

### Simulation-side settings

| Setting | Value | Reason |
|---|---|---|
| `net.ipv4.tcp_low_latency` | 1 | Disables the TCP prequeue. With it enabled, scenario B crashed on the receiver in the MPTCP receive path (`mptcp_v4_do_rcv` → `inet_iif` on a packet without a route). Applied to both FDACC and LIA runs; LIA's result was unchanged by it. |
| Debug trace | on | One line per subflow every 10 ms, plus one line per slow-start exit, delay cut, loss and state change |

## 5. Results

### Scenario A — cross traffic from +20 s

| Period | FDACC (Mbit/s) | LIA (Mbit/s) |
|---|---|---|
| 0–15 s | 1–5, ramping linearly | 7–15 |
| 15–20 s | 4–7 | 7–10 |
| 20–50 s (cross traffic) | 2–6 | 3–7 |
| Total | **3.98** | **7.00** |

The 18 MB gap is almost entirely the first 18 seconds. Path 2 needed 18 s to
reach its bandwidth-delay product of about 90 packets by linear growth; LIA
reached the same point by doubling in about one second. Once the cross traffic
started, the two were roughly even.

### Scenario B — cross traffic from +0 s

| Period | FDACC (Mbit/s) | LIA (Mbit/s) |
|---|---|---|
| 0–5 s | 1.26 | 4.61 |
| 5–10 s | 2.73 | 4.82 |
| 10–15 s | 3.78 | 5.03 |
| 15–20 s | 1.89 | 5.03 |
| 20–25 s | 2.52 | 5.03 |
| 25–30 s | 3.57 | 5.66 |
| 30–35 s | 3.98 | 5.03 |
| 35–40 s | 2.10 | 5.03 |
| 40–45 s | 3.15 | 5.03 |
| 45–50 s | 4.61 | 5.03 |
| Total | **2.97** | **5.03** |

### Per-path behaviour, scenario B

Path 1, congested from the start:

| 5 s window | cwnd (avg) | OWD (avg) | μ | γ | delay cuts | losses |
|---|---|---|---|---|---|---|
| 0–5 | 13.8 | 72 ms | 0.26 | 0.25 | 4 | 0 |
| 5–10 | 20.8 | 156 ms | 0.64 | 0.66 | 0 | 1 |
| 10–50 | 14–16 | 185–218 ms | 0.71–0.75 | 0.84–0.86 | 0 | 1 every ~4.5 s (10 total) |

Path 2, never congested by cross traffic:

| Time | Event |
|---|---|
| 0–16 s | linear climb from cwnd 5 to about 81 |
| 16.1 s | 29 cuts in 67 ms: cwnd 81 → 2 (P peaked at 0.18) |
| 16–33 s | linear climb back to 98 |
| 33.4 s | 68 cuts in 200 ms: cwnd 98 → 12 (P peaked at 0.044) |
| 33–50 s | climb to 92 |
| 49.9 s | 41 cuts in 221 ms: cwnd 92 → 51 |

No losses on path 2 in either scenario.

### Signal quality

In steady state on path 2 the smoothed forward delay varied by about ±1.6 ms
and P by about 0.013 over a window, with step-to-step changes of about 0.0035.
The 4 ms timestamp resolution was not a limiting factor.

## 6. Observations

1. **Slow start ends within the first RTT.** γ is seeded with μ(0), and the
   first sample is its own minimum, so γ(0) = 0. The first ACK that shows any
   delay increase has P > 0 and halves the window. In every run both subflows
   left slow start at cwnd 11–12 after a delay rise of 0.5–1 ms (one timestamp
   tick folded into the smoothing). Growth from there is linear at about 5
   packets per second, which is where scenario A's gap comes from.

2. **Decreases cascade.** Once P > 0, every ACK removes about one packet until
   γ catches up with μ, which takes 100–200 ms at k_u = 0.01/ms. The size of a
   reduction is therefore set by the ACK rate and γ's lag, not by P: a P of
   0.044 took path 2 from 98 to 12. Recovery is linear from a low base with no
   slow start, so each collapse costs 10–15 s on the uncongested path.

3. **γ learns sustained congestion as normal.** On the congested path γ rose
   to match μ within the first 5 s and then sat slightly above it. From then on
   P was zero or negative, no delay-driven cuts happened, and the subflow was
   governed by Reno loss halving alone, like LIA. The delay signal was inert on
   the one path where it was meant to help.

4. **Net effect.** The algorithm pays the cost of delay sensitivity on the
   path that does not need it and receives no benefit on the path that does.
   Both scenarios land at about 60 % of LIA.

5. **Rounding to nearest acts as an implicit dead band.** A cut only changes
   cwnd when P · cwnd ≥ 0.5. Without it the cascades would be worse; with it
   small P is ignored entirely at small cwnd.

## 7. How this topology differs from the wireless target

The algorithm is intended for wireless access paths such as Wi-Fi and
cellular, where the forward delay carries information that RTT hides. The
simulation reproduces the asymmetry but little else of a wireless link.

| Property | Wireless path | This simulation |
|---|---|---|
| Link capacity | varies with signal quality, rate adaptation, contention, scheduling | constant point-to-point rate |
| Propagation and MAC delay | varies per packet (retries, contention, scheduling grants) | fixed per link |
| Delay variation when idle | present; OWD_min drifts | none; OWD_min is exact and static |
| Queue location | at the AP or base station, often large (bufferbloat) | DropTail, 100 packets, at the link head |
| Losses | congestive and non-congestive (interference, handover) | congestive only, from queue overflow |
| Asymmetry | uplink and downlink differ in rate, scheduling and delay | reproduced: reverse path has different rate and delay |
| Reverse-path congestion | common, e.g. uplink contention | reproduced by the slow reverse link; no reverse cross traffic in these runs |
| Competing traffic | bursty, many flows, variable | one constant-rate UDP flow |
| Mobility and handover | path characteristics change over time | none |

Consequences for reading the results:

- The delay signal here is clean and reflects queueing only. On a wireless link
  the same signal would include MAC-layer jitter and capacity changes, so the
  threshold γ would have more to absorb and the noise floor would be higher.
  The results are an easier case for the signal than the target.
- All losses here are congestive, which is exactly the case loss-based LIA is
  built for. A wireless link with non-congestive loss would penalise LIA and not
  FDACC; this topology cannot show that advantage.
- OWD_min never expires in the current code. That is harmless here because the
  minimum is static. On a wireless path it would need a windowed minimum.
- The 100-packet DropTail queue means the maximum queueing delay is bounded at
  about 120 ms on path 1. Wireless buffers are often larger, so μ would reach
  higher values and the γ-learning effect in observation 3 would be stronger.

## 8. Next steps

| Issue | Proposed change |
|---|---|
| Slow start ends within one RTT | do not allow P > 0 to end slow start until γ has settled for at least one RTT, or until P exceeds a minimum |
| Decreases cascade per ACK | apply at most one reduction per RTT, marked by `snd_nxt` as Vegas does |
| γ absorbs sustained congestion | k_u is 3–5× too fast for these RTTs; a time constant of several RTTs would let P stay positive long enough to act; alternatively bound γ |
| k values untuned | k_u and k_d are GCC's constants; μ is a ratio, not a delay in ms, so they are not directly transferable |
| No coupling | each subflow is at least as aggressive as Reno on its own; fairness at a shared bottleneck is not addressed |
| OWD_min never expires | windowed minimum, needed for the wireless target |

## 9. Data

Results of every run are in `ns-3-dce/results_backup/`:

| Directory | Scenario | CC |
|---|---|---|
| `default_fdacc_poc_28-09-2026_12:30:29` | A | FDACC, first build, no event trace |
| `default_lia_baseline_28-09-2026_12:31:52` | A | LIA |
| `default_fdacc_logging_04-10-2026_09:55:39` | A | FDACC, with event trace |
| `default_fdacc_traffic0_lowlat_04-10-2026_10:53:55` | B | FDACC |
| `default_lia_traffic0_lowlat_04-10-2026_10:55:38` | B | LIA |
| `default_lia_traffic0_04-10-2026_10:06:01` | B | LIA, before the prequeue workaround (identical result) |

Each contains `files-0/var/log/messages` (sender kernel log with the trace),
`files-0/var/log/*/stdout` (iperf client), `files-1/...` (receiver) and pcaps
of every link.

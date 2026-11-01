# Forward Delay Aware Congestion Control (FDACC)

A self-contained description of the algorithm as it currently runs, written
in terms of delays, packets and time rather than code.

Everywhere this document describes a choice that the original design leaves
open, or departs from it, the choice is marked like this:

> **Implementation note:** text.

These notes are the places where the design is not yet settled.

---

## 1. Purpose and scope

FDACC is a congestion control for MPTCP subflows. It decides, for each subflow
separately, how many packets may be in flight. It differs from Reno, CUBIC and
the coupled MPTCP controls (LIA, OLIA, BALIA) in the signal it reacts to:
instead of waiting for packet loss, it watches the **forward one-way delay** of
the subflow and backs off when the queue in the forward direction grows beyond
what is normal for that path.

The reason for using forward delay rather than round-trip time is specific to
wireless and other asymmetric access paths. RTT is the sum of the forward and
the reverse delay. When the reverse direction is congested, for example an
uplink carrying other users' traffic, RTT rises even though the forward path,
which is the one carrying this subflow's data, is empty. A controller or a
scheduler that reads RTT concludes the path is congested and moves away from
it. The forward delay does not contain the reverse path and does not make that
mistake.

This document covers:

- where the algorithm sits in an MPTCP connection and what it measures,
- what happens from connection establishment onward,
- what happens on every acknowledgement, with every case enumerated,
- the window rule in every phase and for every sign of the pressure signal,
- how loss, timeouts and recovery interact with it,
- a worked example, the observed behaviour in simulation, and the open
  questions.

It gives the formulas and their meaning, but not full derivations, and it
reports measurements only where they bear on how a mechanism behaves.

---

## 2. Terms and quantities

| Symbol | Name | Unit | Range and typical values |
|---|---|---|---|
| $OWD$ | Smoothed forward one-way delay of this subflow: time from a data packet leaving the sender to its acknowledgement leaving the receiver | ms | tens to hundreds of ms; 52 ms and 152 ms on the two simulated paths |
| $OWD_{\min}$ | Lowest $OWD$ seen on this subflow since it was established | ms | the propagation delay of the forward path when queues are empty |
| $q$ | Queueing delay, $OWD - OWD_{\min}$ | ms | 0 when the path is empty; up to the bottleneck buffer's drain time |
| $\mu$ | Queue fraction, $q / OWD$: the share of the current forward delay that is queueing | none | 0 to 1; 0 on an empty path, 0.5 when queueing equals propagation |
| $\gamma$ | Learned normal level of $\mu$ on this subflow | none | 0 to 1; follows $\mu$ with two different speeds |
| $P$ | Congestion pressure, $\mu - \gamma$ | none | −1 to 1; a few hundredths either side of 0 in practice |
| $\Delta t$ | Time since the previous acknowledgement on this subflow | ms | tens of microseconds to a few ms while data flows |
| $k_u$ | Rate at which $\gamma$ rises toward a higher $\mu$ | per ms | 0.01 per ms, i.e. a time constant of 100 ms |
| $k_d$ | Rate at which $\gamma$ falls toward a lower $\mu$ | per ms | 0.00018 per ms, i.e. a time constant of about 5.6 s |
| $cwnd$ | Congestion window: packets this subflow may have in flight | packets | starts at 10; 2 is the floor |
| $ssthresh$ | Slow-start threshold: the window below which slow start applies | packets | effectively infinite at start |
| $RTT$ | Smoothed round-trip time of the subflow, as TCP measures it | ms | includes the reverse path |
| acked | Number of packets a single acknowledgement covers | packets | 1 or 2 normally; more after lost ACKs |

Throughout, "packet" means one maximum-size segment (1448 bytes of payload in
the simulation).

---

## 3. Where the algorithm lives in the connection

### 3.1 One instance per subflow

An MPTCP connection consists of a connection-level socket and one TCP subflow
per path. FDACC runs as an independent instance inside each subflow, with its
own $OWD_{\min}$, $\gamma$ and $P$. Nothing is shared between subflows and
nothing is kept at the connection level.

This has a consequence that the original design does not address: there is no
coupling. Each subflow grows at least as fast as an independent Reno flow, so
an MPTCP connection with $N$ subflows through one shared bottleneck takes $N$
times the share a single TCP flow would. LIA, OLIA and BALIA exist to prevent
exactly that. FDACC currently does not. This is a design gap, not an
implementation choice; see §12.

### 3.2 Which end does the work

Both ends of the connection run FDACC, because the congestion control is
selected system-wide. Only the data sender's instances do anything of
consequence: the receiver's subflows carry acknowledgements, never fill their
window, and their delay samples are never used for a decision.

### 3.3 How the forward delay is measured

The forward delay comes from the TCP timestamp option, which both ends already
exchange:

1. Every data packet carries the sender's clock reading at the moment it was
   sent.
2. The receiver's acknowledgement carries the receiver's own clock reading at
   the moment the ACK was sent, plus an echo of the sender's reading from the
   data being acknowledged.
3. The sender subtracts the echoed reading from the receiver's reading. The
   difference is the time the data spent getting to the receiver, plus any time
   the receiver held the acknowledgement (delayed ACK), plus the fixed offset
   between the two hosts' clocks.

The raw difference is smoothed the same way TCP smooths RTT: the smoothed
value moves one eighth of the way toward each new sample, so a step change in
the path takes about eight acknowledgements to show fully. The first sample is
taken as is.

Three properties of this measurement shape everything that follows:

- **The clock offset is constant.** It cancels in $q = OWD - OWD_{\min}$,
  because both terms come from the same pair of clocks on the same path. It
  does not cancel in the denominator of $\mu$. In the simulation all hosts
  share one clock, so the offset is zero and $\mu$ is exact. On real hosts
  with unsynchronised clocks $\mu$ would be distorted by whatever the offset
  happens to be. The queueing delay $q$ is sound either way.
- **The clocks tick every 4 ms.** The timestamp values are coarse, so each raw
  sample is a multiple of 4 ms. Because packets leave at different points
  within a tick, the smoothed value averages across ticks and is finer than
  4 ms; the cost is noise of roughly half a millisecond rather than blindness.
- **Receiver-side ACK delay is included.** A delayed acknowledgement adds the
  receiver's hold time to the sample. This is the same for every delay-based
  scheme that uses the timestamp option.

---

## 4. Lifecycle

### 4.1 Connection establishment

The first subflow is set up with the ordinary three-way handshake. The
sequence, seen from the data sender (the client):

1. **SYN sent.** No FDACC state exists yet.
2. **SYN-ACK received.** TCP processes the acknowledgement part of the SYN-ACK
   first, and only then, once the connection is established, creates the
   congestion control state. The acknowledgement-processing step therefore
   calls into FDACC *before* FDACC has been initialised. The instance notices
   that it has not been set up and ignores that call. This is the
   "pre-initialisation" case; it happens exactly once per subflow and has no
   effect.
3. **State created.** The instance starts with:

   | Quantity | Initial value |
   |---|---|
   | $cwnd$ | 10 packets (TCP's initial window) |
   | $ssthresh$ | effectively infinite |
   | $OWD$ | one sample, taken from the SYN-ACK's timestamp; not yet used by FDACC |
   | $OWD_{\min}$ | unset until the first data acknowledgement |
   | $\gamma$ | unseeded; it takes the value of the first $\mu$ |
   | $P$ | 0 |
   | decrease gate (§6.6) | open, so the first decrease is never held |

4. **Final ACK sent, data flows.** The first data packets go out under the
   initial window of 10. Their acknowledgements, one round trip later, carry
   the first delay samples.

On the server side the state is created when the final ACK of the handshake
arrives. Since the server only sends acknowledgements, its instances stay at
their initial values.

### 4.2 Additional subflows

Each extra path is joined with its own handshake and gets its own fresh FDACC
instance, created the same way. Nothing is inherited from the first subflow:
the new path starts with no delay sample, an unset minimum and an unseeded
threshold, so it has to learn its own baseline from scratch.

A subflow that cannot be established (in the simulation, the two cross-path
combinations that have no route) never gets past its SYN. The only event its
FDACC instance ever sees is the SYN retransmission timeout, which it answers
with Reno's halving of a window that has never carried data. It has no other
effect.

### 4.3 Steady state and teardown

The state lives as long as the subflow. Nothing is written to any cache or
shared with later connections. When the subflow closes, its $OWD_{\min}$ and
$\gamma$ are discarded, and a new connection over the same path starts cold.

---

## 5. What happens on every acknowledgement

An acknowledgement passes through the following stages in order. Each stage
lists every case it can be in.

### 5.1 Which acknowledgements reach the algorithm

TCP itself decides, per ACK, which parts of FDACC to consult.

| Kind of acknowledgement | Signal update (§5.2–5.5) | Window rule (§6) |
|---|---|---|
| Acknowledges new data, subflow in normal operation | yes | yes |
| Acknowledges new data, subflow in loss recovery or window reduction | yes | **no** |
| Acknowledges new data, but the sender has not been using its whole window | yes | decrease only (§6.6); no growth |
| Duplicate ACK (no new data acknowledged) | no | no |
| Pure window update or SACK-only ACK | no | no |
| Subflow has no delay sample at all (only the connection-level socket) | no | runs with $P = 0$, i.e. plain TCP growth |
| Arrives before the instance is initialised (§4.1 step 2) | no | no |

Two points matter in practice:

- **The signal keeps learning while the window rule is suspended.** During
  loss recovery the window is under TCP's control, but every acknowledged
  packet still updates $OWD_{\min}$, $\gamma$ and $P$. When recovery ends,
  the window rule resumes with a current view of the path, not a stale one.
  This is why the signal update is separated from the window rule rather than
  bundled with it.
- **Decreases are not gated on the window being in use.** If the application
  is sending less than the window allows, the window does not grow (there is
  no evidence it is large enough), but a rise in forward delay still shrinks
  it.

### 5.2 Updating the minimum

$$
OWD_{\min} \leftarrow \min\left(OWD_{\min},\ OWD\right)
$$

| Case | Effect |
|---|---|
| First sample on this subflow | Becomes the minimum. Consequently $q = 0$ and $\mu = 0$ for this ACK. |
| Sample below the current minimum | Replaces it. $q$ and $\mu$ of this ACK are 0. |
| Sample at or above the minimum | Minimum unchanged. |

The minimum never expires and never rises. If the path's base delay later
increases (a route change, a handover to a cell with longer propagation), the
old lower minimum stays and every later sample looks like queueing. If the base
delay later *decreases*, the new lower value is adopted immediately. This
asymmetry is a design gap (§12), harmless in the simulation where paths are
static, and relevant on a mobile device.

### 5.3 Computing the queue fraction $\mu$

$$
\mu = \frac{OWD - OWD_{\min}}{OWD}
$$

$\mu$ is the fraction of the current forward delay that is queueing. It is
0 on an empty path and approaches 1 as queueing dwarfs propagation.

Two examples, a Wi-Fi path and a 5G path:

| Path | $OWD_{\min}$ | $OWD$ | $q$ | $\mu$ |
|---|---|---|---|---|
| Wi-Fi | 15 ms | 18 ms | 3 ms | 0.167 |
| 5G | 25 ms | 40 ms | 15 ms | 0.375 |

The 5G path is carrying proportionally more queue.

A fraction is used rather than the queueing delay in milliseconds so that the
same thresholds mean the same thing on paths with very different base delays:
3 ms of queueing is a lot on a 15 ms path and nothing on a 150 ms path.

$\mu$ is recomputed on every qualifying acknowledgement, after the minimum has
been updated with the same sample.

### 5.4 Updating the learned level $\gamma$

$\gamma$ is the controller's memory of what $\mu$ normally looks like on this
path. The congestion decision is never made on $\mu$ alone, always on how far
$\mu$ is from $\gamma$.

**Seeding.** On the first qualifying acknowledgement,

$$
\gamma(0) = \mu(0)
$$

Because the first sample is also the first minimum (§5.2), $\mu(0) = 0$, so in
practice **$\gamma$ always starts at 0**. Any rise in delay after that first
sample is pressure, however small.

**Update.** On every later acknowledgement,

$$
\gamma \leftarrow \gamma + \Delta t \cdot k \cdot (\mu - \gamma)
$$

where $\Delta t$ is the real time elapsed since the previous acknowledgement on
this subflow, and

$$
k =
\begin{cases}
k_u = 0.01 \text{ per ms} & \text{if } \mu \geq \gamma \\
k_d = 0.00018 \text{ per ms} & \text{if } \mu < \gamma
\end{cases}
$$

This is an exponential moving average with a time constant of $1/k$:

| Direction | Gain | Time constant | Gap closed after 1 time constant | 95% closed after |
|---|---|---|---|---|
| $\gamma$ rising toward a higher $\mu$ | $k_u$ = 0.01 per ms | 100 ms | 63% | about 300 ms |
| $\gamma$ falling toward a lower $\mu$ | $k_d$ = 0.00018 per ms | about 5.6 s | 63% | about 17 s |

The ratio between the two is about 55 to 1. The asymmetry is deliberate and
comes from Google Congestion Control, where this threshold originates:

- **Rise fast.** When queueing goes up and stays up, treat it as the new
  normal within a few hundred milliseconds. If the queue is being built by
  another flow sharing the bottleneck, backing off will not remove it, and a
  controller that kept treating it as congestion would starve itself while
  the other flow takes the capacity.
- **Fall slowly.** When queueing goes down, lower the bar reluctantly, so the
  controller does not flip back to "congested" on the next small fluctuation.

**Cases on each acknowledgement:**

| Case | What $\gamma$ does |
|---|---|
| $\mu > \gamma$ | Moves up by $\Delta t \cdot 0.01 \cdot (\mu - \gamma)$. At 100 µs between ACKs that is 0.1% of the gap per ACK. |
| $\mu < \gamma$ | Moves down by $\Delta t \cdot 0.00018 \cdot (\gamma - \mu)$. At 100 µs between ACKs that is 0.002% of the gap per ACK. |
| $\mu = \gamma$ | Unchanged. |
| Long gap since the last ACK (idle period) | The product $\Delta t \cdot k$ is capped at 1, so $\gamma$ moves at most all the way to $\mu$, never past it. |

**Worked example.** Acknowledgements arrive every 20 ms. $\gamma$ is 0.10.

| ACK | $\mu$ | Direction | $\Delta t \cdot k$ | $\gamma$ afterwards |
|---|---|---|---|---|
| 1 | 0.20 | up, $k_u$ | 20 × 0.01 = 0.20 | 0.10 + 0.20 × (0.20 − 0.10) = 0.120 |
| 2 | 0.20 | up, $k_u$ | 0.20 | 0.120 + 0.20 × 0.080 = 0.136 |
| 3 | 0.05 | down, $k_d$ | 20 × 0.00018 = 0.0036 | 0.136 + 0.0036 × (0.05 − 0.136) = 0.1357 |

Two ACKs of higher queueing moved $\gamma$ up by 0.036; one ACK of much lower
queueing moved it down by 0.0003.

> **Implementation note:** $k_u$ and $k_d$ are Google Congestion Control's
> published values and have not been tuned for this algorithm. GCC applies
> them to a signal measured in milliseconds of delay gradient; here the signal
> is a dimensionless fraction, so the numbers are not directly transferable.
> There is also a time-scale argument: for $\gamma$ to mean "the normal
> level", it should move more slowly than one round trip, otherwise it
> absorbs a delay rise before a full round trip of evidence has arrived. With
> round trips of 200–300 ms on the simulated paths, a 100 ms time constant is
> three to five times too fast. See §10 and §12.

### 5.5 Computing the pressure $P$

$$
P = \mu - \gamma
$$

computed after $\gamma$ has been updated with the same sample.

| Case | Meaning | Example |
|---|---|---|
| $P < 0$ | The queue is smaller than this path normally carries. The path is underloaded. | $\mu$ = 0.05, $\gamma$ = 0.20: $P$ = −0.15 |
| $P \approx 0$ | The path is at its normal level. | $\mu$ = 0.20, $\gamma$ = 0.19: $P$ = 0.01 |
| $P > 0$ | The queue is larger than normal. Congestion is emerging. | $\mu$ = 0.30, $\gamma$ = 0.20: $P$ = 0.10 |

Because $\gamma$ follows $\mu$ within a few hundred milliseconds, $P$ is only
clearly positive during the *onset* of a queue, and only clearly negative for
seconds after a queue has *drained*. In the simulation runs, $P$ was usually
within ±0.05 of zero, with brief excursions to 0.1–0.2 at the start of a
congestion episode and sustained values down to −0.7 after one ended.

---

## 6. The window rule

The window rule runs on acknowledgements that TCP allows to change the window
(§5.1). It reads the current $P$ and acts according to the phase the subflow
is in and the sign of $P$.

### 6.1 Which phase the subflow is in

TCP has two growth phases, separated by $ssthresh$:

- **Slow start:** the window grows by one packet for every packet
  acknowledged, doubling each round trip.
- **Congestion avoidance:** the window grows by about one packet per round
  trip.

The boundary is tested in two places, and not the same way:

| Decision | Test | Why |
|---|---|---|
| How to **grow** | slow start while $cwnd \leq ssthresh$ | TCP's own convention; its slow-start routine assumes it |
| How to **decrease** | slow start only while $cwnd < ssthresh$ | After an early exit (§6.3) the window sits exactly at $ssthresh$; with $\leq$, every later $P > 0$ would halve it again instead of applying the proportional rule |

> **Implementation note:** The original design uses the strict test $cwnd < ssthresh$
> throughout. The growth path uses $\leq$ because that is what TCP's slow
> start code requires; the decrease path keeps the strict test for the reason
> in the table. In practice the only moment they disagree is when
> $cwnd = ssthresh$, which is exactly the state an early exit leaves behind.

### 6.2 Slow start, $P \leq 0$

$$
cwnd \leftarrow cwnd + \text{acked}
$$

One packet of window per packet acknowledged, the standard exponential growth.
If this step would carry the window past $ssthresh$, it stops at $ssthresh$
and the remaining acknowledged packets are credited to congestion avoidance
(§6.4, §6.5) in the same step. The window is also capped at the receiver's
advertised limit.

Growth only happens if the sender has actually been using the window (§5.1).

### 6.3 Slow start, $P > 0$ — early exit

The path's queue is growing beyond normal while the window is still doubling.
Rather than wait for the queue to overflow and a packet to be lost, slow start
is ended now:

$$
ssthresh \leftarrow \max\left(\frac{cwnd}{2},\ 2\right), \qquad cwnd \leftarrow ssthresh
$$

Example: $cwnd$ = 11, $P$ = 0.009 → $ssthresh$ = 5, $cwnd$ = 5.

After this, $cwnd = ssthresh$, so the subflow is in congestion avoidance for
growth purposes and any further $P > 0$ uses the proportional rule (§6.6).

**Consequence.** Since $\gamma$ starts at 0 (§5.4) and slow start by its nature
fills the pipe and starts a queue, the first acknowledgement that shows *any*
rise in the smoothed forward delay ends slow start. In the simulation this
happened within the first round trip, at a window of 11 or 12 packets, on a
delay rise of about half a millisecond. Slow start effectively does not exist
in the current form of the algorithm. See §10 and §12.

### 6.4 Congestion avoidance, $P < 0$ — accelerated increase

$$
cwnd \leftarrow cwnd + \frac{1 - P}{cwnd} \quad \text{per acknowledged packet}
$$

Over one round trip, during which about $cwnd$ packets are acknowledged, the
window grows by $1 - P$ packets: one packet (Reno's rate) when $P$ is 0, up to
two packets when $P$ approaches −1. The path is carrying less queue than
normal, so the window is allowed to grow faster to use the headroom.

Since the window is a whole number of packets, this is carried out as "one
extra packet after every $cwnd / (1 - P)$ acknowledged packets", with the
remainder carried over between acknowledgements.

Example: $cwnd$ = 40, $P$ = −0.3. One extra packet after every
40 / 1.3 ≈ 31 acknowledged packets, i.e. 1.3 packets per round trip.

Growth only happens if the sender has been using the window (§5.1).

### 6.5 Congestion avoidance, $P = 0$

$$
cwnd \leftarrow cwnd + \frac{1}{cwnd} \quad \text{per acknowledged packet}
$$

One packet per round trip. This is exactly Reno, and it is the case the
algorithm spends most of its time in once $\gamma$ has caught up with $\mu$.

### 6.6 Congestion avoidance, $P > 0$ — proportional decrease

The queue is larger than this path's normal level. The window is reduced in
proportion to the excess:

$$
ssthresh \leftarrow \max\left(\operatorname{round}\left(cwnd \cdot (1 - P)\right),\ 2\right), \qquad cwnd \leftarrow ssthresh
$$

The product is rounded to the nearest whole packet and never goes below 2.

| $cwnd$ | $P$ | $cwnd \cdot (1 - P)$ | New $cwnd$ |
|---|---|---|---|
| 50 | 0.05 | 47.5 | 48 |
| 50 | 0.20 | 40.0 | 40 |
| 10 | 0.03 | 9.7 | 10 — no change |
| 10 | 0.06 | 9.4 | 9 |
| 3 | 0.50 | 1.5 | 2 — floor |

Because of the rounding, a decrease only takes effect when $P \cdot cwnd$ is
at least half a packet. For a window of 10 that means $P$ of at least 0.05;
for a window of 100, 0.005. Below that the acknowledgement changes nothing.

**One decrease per round trip.** After a decrease that changed the window,
the algorithm remembers how much data was in flight at that moment. Until all
of it has been acknowledged, which takes one round trip, further
acknowledgements with $P > 0$ are **held**: they neither decrease the window
again nor let it grow. The reasoning: those acknowledgements are for packets
sent under the *old*, larger window, and the delay rise they report is the
one the decrease has already answered. Only once the network has seen the
reduced window is a second reduction allowed.

| Situation | Action on a $P > 0$ acknowledgement |
|---|---|
| No decrease in the last round trip | Apply the proportional decrease |
| A decrease happened less than one round trip ago | Hold: no decrease, no growth |
| A decrease was computed but rounding left the window unchanged | Does not count as a decrease; the next $P > 0$ ACK may still decrease |

The decrease is applied whether or not the sender has been using its whole
window (§5.1).

> **Implementation note:** The original design specifies the formula but not how often it
> applies. The first proof-of-concept applied it on every acknowledgement with
> $P > 0$. Because $\gamma$ takes 100–200 ms to catch up with a risen $\mu$,
> that produced runs of one-packet reductions on consecutive ACKs, with the
> total cut determined by the acknowledgement rate rather than by $P$ (a $P$
> of 0.04 removed 88% of a 98-packet window in 200 ms). The once-per-round-trip
> hold was added on 13 October 2026 to make a single reduction mean what the
> formula says. Rounding to the nearest packet, and the floor of 2, are also
> choices not in the original design.

### 6.7 Summary

| Phase | $P$ | Action | Section |
|---|---|---|---|
| Slow start | $\leq 0$ | $cwnd$ += 1 per acknowledged packet | 6.2 |
| Slow start | $> 0$ | $ssthresh$ = max($cwnd$/2, 2); $cwnd$ = $ssthresh$; leave slow start | 6.3 |
| Avoidance | $< 0$ | $cwnd$ += $(1 - P)/cwnd$ per acknowledged packet | 6.4 |
| Avoidance | $= 0$ | $cwnd$ += $1/cwnd$ per acknowledged packet | 6.5 |
| Avoidance | $> 0$, no decrease in the last RTT | $cwnd$ = round($cwnd \cdot (1 - P)$), floor 2 | 6.6 |
| Avoidance | $> 0$, decrease within the last RTT | Hold: nothing | 6.6 |
| Any | any, window not in use | Decrease if $P > 0$; otherwise nothing | 5.1 |
| Any | any, subflow in recovery | Window rule not consulted; signal still updated | 5.1 |

---

## 7. Loss and recovery

FDACC adds the delay-based decrease; it does not replace TCP's loss handling.
Everything below is TCP's standard machinery, with FDACC contributing only
the size of the loss response.

| Event | What TCP does | FDACC's part | FDACC's state meanwhile |
|---|---|---|---|
| Loss detected by duplicate ACKs or SACK | Enters fast recovery, retransmits, reduces the window gradually toward the new $ssthresh$ | Supplies $ssthresh = \max(cwnd/2,\ 2)$, Reno's halving | $OWD_{\min}$, $\gamma$, $P$ keep updating on every ACK; window rule suspended until recovery ends |
| Retransmission timeout | Window collapses to 1 packet, slow start restarts toward $ssthresh$ | Same halving for $ssthresh$ | As above; the first ACK after the timeout re-enters the window rule in slow start, and the usual early-exit rule (§6.3) applies immediately if $P > 0$ |
| Loss turns out to be spurious (undo) | Restores the window to the larger of its current value and $2 \cdot ssthresh$ | Nothing (TCP's default undo) | Unchanged |
| Explicit congestion notification | Treated as a loss event | Same halving | Unchanged |
| Idle for longer than one retransmission timeout | Halves the window once per idle timeout | Nothing | **Not reset.** $\gamma$ keeps whatever level it had, and the decrease hold is unaffected. After a long idle period $\gamma$ may describe conditions that no longer exist. Design gap (§12). |

Loss-driven and delay-driven reductions are independent. A subflow can be
halved by a loss and then, after recovery, reduced again by $P > 0$, or the
other way round. The decrease hold in §6.6 applies only to delay-driven
reductions.

---

## 8. Interaction with the rest of MPTCP

### 8.1 Scheduler and congestion control

Two separate mechanisms share the sending decision:

- The **scheduler** picks which subflow carries the next segment. The minRTT
  scheduler prefers the subflow with the lowest round-trip time; the
  forward-delay scheduler prefers the lowest forward delay.
- **FDACC** sets, per subflow, how many packets may be outstanding.

They meet only through window space: the scheduler can only choose among
subflows that have room under their window. With a bulk sender, every
subflow's window is usually full, so when an acknowledgement frees space on
one subflow, that subflow is the only candidate and the scheduler's
preference never comes into play. In the simulation the two schedulers
produced byte-identical runs for this reason. The scheduler matters when
subflows have spare window, for example with application-limited traffic or
a receive-window limit.

### 8.2 No coupling

As noted in §3.1, each subflow runs FDACC on its own. When two subflows share
a bottleneck, each sees the same queueing delay in its own forward direction,
each learns it as normal, and each grows at Reno's rate or faster. The
connection as a whole takes two Reno shares of the bottleneck. The coupled
MPTCP controls scale the increase of every subflow so that the connection
takes one share in total; FDACC has no equivalent yet.

### 8.3 Growth is still per round trip

FDACC reads the forward delay to decide *when* to back off, but the window
still grows by a fixed amount *per round trip* (§6.4, §6.5), like Reno. On a
path whose RTT is inflated by a congested reverse direction, growth is slow
even though the forward path is empty and the delay signal says so.

In the reverse-congestion run: path 1 had a 52 ms forward delay, a free
10 Mbit/s forward link, and a 600 ms RTT because of a saturated reverse link.
$P$ stayed near zero, correctly. The window grew by about one packet per
0.6 s and reached 30 packets after 15 seconds, which at that RTT is about
0.5 Mbit/s on a 10 Mbit/s path. LIA, throttled by the same RTT but with a
window several times larger, carried seven times as much.

This is the central tension in the current design: the thing the forward
delay signal is best at (knowing the forward path is free despite a bad RTT)
is not connected to the thing that would exploit it (growing faster than one
packet per RTT). See §12.

---

## 9. Worked end-to-end example

One subflow on a 10 Mbit/s path with a 15 ms forward propagation delay and a
40 ms round trip. Packets are 1448 bytes, so the path holds about 34 packets
without queueing. Delay measurement is taken as exact (no 4 ms clock
granularity), $\gamma$ is tracked with its continuous-time response, and the
numbers are computed from the formulas in §5 and §6.

| Time | Event | $OWD$ | $\mu$ | $\gamma$ | $P$ | $cwnd$ | Action |
|---|---|---|---|---|---|---|---|
| 0 ms | SYN sent | — | — | — | — | — | no state |
| 40 ms | SYN-ACK; ACK part processed, then state created | 15.0 (from the SYN-ACK) | — | unseeded | 0 | 10 | pre-init call ignored; init |
| 80 ms | ACKs for the first 10 packets; first sample folded into the minimum | 15.0 | 0 | seeded 0 | 0 | 10 → 20 | slow start |
| 120 ms | ACKs for 20 packets; still under the pipe's capacity | 15.0 | 0 | 0 | 0 | 20 → 40 | slow start |
| 160 ms | 40 in flight exceeds the 34-packet pipe; queue of ~6 packets, ~7 ms; smoothed delay starts rising | 15.9 | 0.056 | 0.000 → 0.002 | +0.054 | 40 → 20 | **early exit** on the first ACK with a rise: $ssthresh$ = 20; hold armed for one RTT |
| 200 ms | Queue drained; samples back at 15 ms; smoothed delay decaying | 15.4 | 0.026 | 0.004 | +0.022 | 20 | held (within one RTT of the exit) |
| 240 ms | Hold expired; delay back to base | 15.0 | 0 | 0.004 | −0.004 | 20 → 21 | avoidance, slight acceleration |
| 1.0 s | Steady growth at just over 1 packet/RTT | 15.0 | 0 | 0.003 | −0.003 | 40 | avoidance |
| 5.0 s | Cross traffic adds 3 ms of standing queue | 18.0 | 0.167 | 0.003 → 0.01 | +0.157 | 40 → 34 | **decrease**: 40 × 0.843 = 33.7 → 34; hold |
| 5.04 s | Next RTT; $\gamma$ has risen 33% of the way | 18.0 | 0.167 | 0.062 | +0.105 | 34 → 30 | decrease: 34 × 0.895 = 30.4 → 30; hold |
| 5.08 s | | 18.0 | 0.167 | 0.097 | +0.070 | 30 → 28 | decrease: 30 × 0.930 = 27.9 → 28 |
| 5.12 s | | 18.0 | 0.167 | 0.120 | +0.047 | 28 → 27 | decrease: 28 × 0.953 = 26.7 → 27 |
| 5.16 s | | 18.0 | 0.167 | 0.136 | +0.031 | 27 → 26 | decrease: 27 × 0.969 = 26.2 → 26 |
| 5.20 s | $P \cdot cwnd$ now below half a packet | 18.0 | 0.167 | 0.146 | +0.021 | 26 | rounding leaves 26; no decrease |
| 5.4 s | $\gamma$ has caught up | 18.0 | 0.167 | 0.165 | +0.002 | 26 | effectively $P = 0$: Reno growth resumes |
| 20.0 s | Cross traffic stops; queue gone | 15.0 | 0 | 0.167 | −0.167 | 60 | accelerated growth, 1.17 packets/RTT |
| 25.6 s | One $k_d$ time constant later | 15.0 | 0 | 0.061 | −0.061 | ~75 | growth 1.06 packets/RTT |
| 30.0 s | A packet is lost (buffer overflow at 10 Mbit/s with a 75-packet window) | 16.5 | 0.09 | 0.03 | +0.06 | 75 → 37 | **loss**: TCP halves via $ssthresh$ = 37; recovery; window rule suspended, signal still updating |
| 30.1 s | Recovery ends | 15.2 | 0.013 | 0.04 | −0.027 | 37 | avoidance resumes |

Reading the table:

- The early exit at 160 ms is triggered by a 0.9 ms rise on a 15 ms path,
  because $\gamma$ is still 0.
- The congestion episode at 5 s costs five reductions over 160 ms, each
  smaller than the last as $\gamma$ catches up, taking the window from 40 to
  26 (35%) for a $\mu$ of 0.167. Without the once-per-RTT hold, the same
  episode would have produced a reduction on nearly every acknowledgement.
- Once $\gamma$ has absorbed the 3 ms standing queue, the subflow behaves as
  Reno on a path with 18 ms of delay; the queue is now "normal".
- After the queue disappears, the subflow is treated as underloaded for
  roughly 15 seconds, growing up to 17% faster than Reno, until $k_d$ brings
  $\gamma$ back down.

---

## 10. Observed behaviour in simulation

The observations below come from simulation runs of a two-host, two-path
wired topology (a 10 Mbit/s path with 50 ms forward delay and a 5 Mbit/s
path with 150 ms forward delay, each with a 5 Mbit/s reverse link and
100-packet queues) carrying a 60-second bulk transfer, with constant-rate
UDP cross traffic saturating one link. The runs were made with the
proof-of-concept before the once-per-round-trip hold (§6.6) was added, so
the cascade behaviour described below is of the earlier form. Everything
else applies to the current algorithm.

**Slow start ends within the first round trip (§6.3).** In every run, both
subflows left slow start at a window of 11–12 packets, 0.3–0.9 s after the
connection opened, on a smoothed delay rise of 0.4–1 ms — a single 4 ms
clock tick folded into the smoothing. All later growth was linear from a
base of 5 packets. On an uncontested 5 Mbit/s path with a 90-packet
bandwidth-delay product, reaching capacity took 18 seconds, against about
one second for LIA. This is the largest single cost in every scenario.

**Reductions were sized by the acknowledgement rate, not by $P$ (§6.6).** With
the per-ACK decrease, each $P > 0$ episode produced one-packet cuts on
consecutive acknowledgements until $\gamma$ caught up 100–200 ms later: a $P$
of 0.044 took a window from 98 to 12. The once-per-RTT hold was added in
response and has not yet been measured.

**$\gamma$ absorbs sustained congestion (§5.4).** On a path saturated by
constant cross traffic, $\gamma$ rose to match $\mu$ within about five
seconds and then sat slightly above it. From then on $P$ was zero or
negative, no delay-driven reductions occurred, and the subflow was governed
by loss halving alone, like Reno. The delay signal was inert on the one path
where it was meant to help.

**Growth is round-trip limited (§8.3).** On the path with a free forward link
and an RTT inflated to 600 ms by reverse congestion, the window grew at one
packet per 0.6 s and the subflow carried 0.46 Mbit/s of 10 available.

**Rounding is an implicit dead band (§6.6).** A $P$ below $0.5 / cwnd$ changes
nothing. This limited the damage from per-ACK cuts at small windows and is
the reason the window never collapsed all the way to 2 in the forward-traffic
scenarios.

**Signal noise is low (§3.3).** In steady state, $P$ varied by about 0.0035
between consecutive samples and 0.013 across a window. The 4 ms clock was not
a limiting factor for the signal, only for the first-RTT slow-start exit.

---

## 11. Parameters and their meaning

| Parameter | Current value | Physical meaning | Where it matters |
|---|---|---|---|
| $k_u$ | 0.01 per ms | $\gamma$ closes 63% of an upward gap in 100 ms, 95% in 300 ms | How long $P > 0$ lasts after a queue forms (§5.4, §6.6) |
| $k_d$ | 0.00018 per ms | $\gamma$ closes 63% of a downward gap in 5.6 s, 95% in 17 s | How long $P < 0$ lasts after a queue drains (§5.4, §6.4) |
| $\gamma$ seed | $\mu$ of the first sample, which is always 0 | The path starts with "no queueing is normal" | Slow-start exit on the first delay rise (§6.3) |
| $OWD_{\min}$ policy | lowest sample ever, never expires | Base delay of the path | Validity of $\mu$ after a route change (§5.2) |
| Minimum window | 2 packets | The window is never reduced below this by any rule | Floors in §6.3, §6.6, §7 |
| Initial window | 10 packets | TCP's default | §4.1 |
| Decrease rounding | nearest whole packet | A decrease needs $P \cdot cwnd \geq 0.5$ to take effect | Dead band (§6.6) |
| Decrease spacing | at most one per round trip | A reduction is applied once per window of evidence | §6.6 |
| Growth gating | only while the window is in use | Application-limited senders do not grow their window | §5.1 |
| Delay smoothing | 1/8 new, 7/8 old per sample | A step change shows fully after ~8 ACKs | §3.3 |
| Delay clock | 4 ms ticks | Raw samples are multiples of 4 ms; smoothed value is finer | §3.3, §10 |
| $\Delta t$ | exact time since the previous ACK on the subflow | $\gamma$ updates on every ACK with the true spacing | §5.4 |
| Loss response | $ssthresh = \max(cwnd/2, 2)$ | Reno | §7 |

---

## 12. Open design questions

Each item names the section where it arises.

- **Slow start cannot survive its own queue (§6.3).** Options: do not allow
  $P > 0$ to end slow start until $\gamma$ has had at least one round trip
  to settle; or require $P$ above a minimum such as 0.05; or seed $\gamma$
  from the first full round trip rather than the first sample.
- **$k_u$ is faster than one round trip on these paths (§5.4).** A time
  constant of several round trips would let the controller see a congestion
  episode for long enough to act on it. Alternatively bound $\gamma$, as GCC
  bounds its threshold, so sustained congestion cannot be learned as fully
  normal.
- **The signal goes inert under sustained congestion (§5.4, §10).** Related
  to the previous item; this is the GCC design doing what it was built for
  (coexist with a competing flow) in a setting where the goal may be
  different.
- **Growth is per round trip (§8.3).** To use a path whose RTT is inflated by
  its reverse direction, the increase would need to scale with the ratio of
  RTT to forward delay, or with the ratio of this subflow's RTT to the
  connection's smallest.
- **No coupling (§3.1, §8.2).** Fairness at a shared bottleneck is not
  addressed.
- **$OWD_{\min}$ never expires (§5.2).** A windowed minimum is needed for
  paths whose base delay changes.
- **No reset after idle or timeout (§7).** $\gamma$ may describe stale
  conditions after a long gap.
- **No dead band other than rounding (§6.6).** Noise around $P = 0$ can
  trigger reductions at large windows, where the rounding threshold is small.
- **$\mu$'s denominator depends on clock alignment (§3.3).** On real hosts,
  $q$ is sound but $\mu$ is not; a normalisation by RTT or by $OWD_{\min}$
  alone would be offset-free.

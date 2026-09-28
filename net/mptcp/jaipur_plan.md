# JAIPUR congestion control — PoC implementation plan

Companion to [`cc.md`](cc.md). Target file: `net/mptcp/mptcp_jaipur.c`.

**Goal of the PoC:** implement the rule in `cc.md` literally, get it running in
the DCE simulation, and trace `μ`, `γ`, `P` and `cwnd` per subflow. Known design
issues are listed in [Deferred issues](#deferred-issues) and are deliberately
not solved here.

## Scope

| In scope | Out of scope (deferred) |
|---|---|
| Per-subflow `μ`, `γ`, `P` from `tp->sfw_dly_us` | Coupling across subflows |
| Slow start / congestion avoidance rule from `cc.md` | Once-per-RTT decrease gate, dead band around `P = 0` |
| Fixed-point arithmetic, overflow safety | Clock-offset-robust `μ` |
| Precise `Δt` (`ktime`) | Finer delay samples (`HZ`, microsecond timestamps), microsecond RTT |
| Debug trace via `printk` | Tuning `k_u` / `k_d`, min-OWD expiry |

Only `net/mptcp/mptcp_jaipur.c` changes. `HZ` stays at 250, so existing
baselines remain comparable. The build wiring already exists (`net/ipv4/Kconfig`, `net/mptcp/Makefile`,
`CONFIG_TCP_CONG_JAIPUR=y` in `arch/lib/defconfig`), and the simulation script
already takes `--cc=`.

## Timing

JAIPUR depends on time in two places, and they have different fixes.

### Clocks in this build

| Clock | Resolution | Why |
|---|---|---|
| `jiffies` / `tcp_time_stamp` | 4 ms | `HZ=250` |
| `skb_mstamp_get()` | 4 ms | Calls `local_clock()`, which `arch/lib/sched.c` derives from jiffies |
| `ktime_get()` / `ktime_get_ns()` | 1 ns | `arch/lib/time.c` reads simulation time via `lib_current_ns()` |

Because of the second row, `rtt_us` and `srtt_us` are also stepped at one jiffy
in this simulation, for every congestion control, despite their µs units.

### What other algorithms do

| Algorithm | Time source | Used for |
|---|---|---|
| LIA, OLIA, BALIA | None; read `tp->srtt_us` | Coupling factor |
| Vegas, Veno, YeAH, wVegas | `rtt_us` passed to `.pkts_acked` | Base and current RTT |
| CUBIC, BIC, Westwood, H-TCP | `tcp_time_stamp` | Epoch and window timing |
| CUBIC HyStart | `ktime_get_real()` when `HZ < 1000` | ACK-train spacing |
| TCP-LP | `rcv_tsval` / `rcv_tsecr` in jiffies | One-way delay, same method as `sfw_dly_us` |

CUBIC HyStart is the precedent for using `ktime` when jiffies is too coarse.
Vegas and wVegas do "once per RTT" by marking `snd_nxt`, not with a clock; the
deferred decrease gate should follow that pattern.

### Decisions

| Problem | Decision | Result |
|---|---|---|
| `Δt` between `γ` updates | `ktime_get_ns()` inside JAIPUR | `γ` updates on every ACK with exact `Δt` |
| Resolution of the delay sample | Keep `HZ=250` | Raw samples in 4 ms steps; the smoothed value is finer |

### Why `HZ=250` is enough for the PoC

`sfw_dly_us` comes from the TCP timestamp option, and both ends write jiffies
into it (`tcp_output.c`, `opts->tsval = tcp_skb_timestamp(skb)`), so each raw
sample is a multiple of 4 ms. JAIPUR never reads raw samples, only the 1/8
smoothed value, and that is not limited to 4 ms steps:

- Packets leave at different points within a tick, so a true delay of 18 ms
  shows up as a mix of 16 ms and 20 ms samples that averages to 18 ms.
- The cost of the coarse tick is noise, not blindness: roughly ±0.5 ms on the
  smoothed delay at `HZ=250`, against roughly ±0.13 ms at `HZ=1000`.
- On an 18 ms path that is about ±0.03 of noise in `μ` and `P`.
- `γ` adapts to the path's normal level, so a constant bias in `μ` (for example
  from `min_fw_dly` tracking the low edge of the noise) is absorbed.

These figures are estimates, not measurements. The trace from Step 4 will show
the real noise level; raising `HZ` is listed under deferred issues if it turns
out to be too high.

## Choices made for the PoC

`cc.md` leaves these open; the PoC picks a default for each so the code can be
written. All are `#define`s at the top of the file.

| Item | PoC choice | Reason |
|---|---|---|
| Fixed-point scale | Q20 (`1 << 20`) for `μ`, `P` and the gain | No floats in the kernel |
| `γ` storage | `s64` in Q36 (Q20 plus 16 guard bits) | With per-ACK updates `Δt` is tens of µs, so single steps are far below one Q20 unit and would round away |
| `Δt` unit | microseconds, from `ktime_get_ns()` | See [Timing](#timing) |
| `k_u` | 0.01 per ms → `10486` in Q20 | GCC value |
| `k_d` | 0.00018 per ms → `189` in Q20 | GCC value |
| Gain | `Δt_us · k / 1000`, clamped to 1.0 | `k` is per ms; clamp is arithmetic safety after idle periods |
| cwnd floor | 2 packets in both phases | `cc.md` only states it for slow start; needed in CA too |
| Rounding of `cwnd · (1 − P)` | round to nearest | Truncation would drop cwnd by ≥ 1 packet on every ACK with any `P > 0` |
| Slow start test | `cwnd <= ssthresh` | Kernel 4.1 convention, required by `tcp_slow_start()`; `cc.md` says `<` |

## Step 1 — per-subflow state

Extend `struct mptcp_jaipur`. About 32 bytes, under `ICSK_CA_PRIV_SIZE`
(64 bytes); the existing `BUILD_BUG_ON` guards it.

```c
struct mptcp_jaipur {
  s64 gamma;          /* adaptive threshold, Q36 */
  u64 last_update_ns; /* ktime_get_ns() at the last gamma update */
  u32 min_fw_dly;     /* running min of tp->sfw_dly_us, U32_MAX = no sample */
  s32 pressure;       /* P = mu - gamma, Q20, signed */
  u32 last_trace;     /* jiffies of the last debug line (JAIPUR_DEBUG) */
  u8 gamma_valid;     /* 0 until gamma has been seeded with mu(0) */
};
```

`mptcp_jaipur_init()` sets `min_fw_dly = U32_MAX` and zeroes the rest.

## Step 2 — congestion signal (`.pkts_acked` hook)

Add `mptcp_jaipur_pkts_acked()` and register it in `tcp_congestion_ops`. The
signal is updated here rather than in `cong_avoid` because `cong_avoid` is not
called in CWR/Recovery, which would freeze `γ` while the path is congested. The
hook runs after `tcp_ack_update_rtt()`, so `sfw_dly_us` is fresh.

Per call:

1. `dly = tp->sfw_dly_us`; return if `!mptcp(tp)` or `dly == 0` (no sample yet,
   also covers the meta socket).
2. Update `min_fw_dly` (existing helper, moved here from `cong_avoid`).
3. `mu = div_u64((u64)(dly - min) << 20, dly)`. `sfw_dly_us` is stored ×8,
   which cancels in the ratio.
4. `now = ktime_get_ns()`.
5. If `!gamma_valid`: `gamma = (s64)mu << 16`, `last_update_ns = now`,
   `gamma_valid = 1`.
6. Otherwise:
   - `dt_us = div_u64(now - last_update_ns, 1000)`
   - `g20 = gamma >> 16` (threshold in Q20)
   - `k = (mu >= g20) ? K_U : K_D`
   - `gain = min(div_u64((u64)dt_us * k, 1000), SCALE)`
   - `gamma += ((s64)gain * (mu - g20)) >> 4` (Q20 × Q20 = Q40 → Q36)
   - `last_update_ns = now`
7. `pressure = mu - (s32)(gamma >> 16)`.

## Step 3 — window rule (`.cong_avoid`)

Replace the `TODO` in `mptcp_jaipur_cong_avoid()`:

```c
if (!mptcp(tp)) {                /* plain TCP: Reno fallback, unchanged */
  tcp_reno_cong_avoid(sk, ack, acked);
  return;
}

P = ca->pressure;

if (P > 0) {                     /* early congestion: decrease */
  if (tp->snd_cwnd <= tp->snd_ssthresh)
    tp->snd_ssthresh = max(tp->snd_cwnd >> 1, 2U);
  else
    tp->snd_ssthresh = max(round(cwnd * (SCALE - P) / SCALE), 2U);
  tp->snd_cwnd = tp->snd_ssthresh;
  tp->snd_cwnd_cnt = 0;
  return;
}

if (!tcp_is_cwnd_limited(sk))    /* growth only when window-limited */
  return;

if (tp->snd_cwnd <= tp->snd_ssthresh) {   /* slow start: cwnd += 1 per ACK */
  acked = tcp_slow_start(tp, acked);
  if (!acked)
    return;
}

/* CA: cwnd += (1 - P) / cwnd, with P <= 0  ->  1x..2x Reno */
w = max(((u64)cwnd << 20) / (SCALE + (-P)), 1);
tcp_cong_avoid_ai(tp, w, acked);
```

`P == 0` gives `w == cwnd`, which is exactly Reno.

Loss handling stays Reno: `.ssthresh = tcp_reno_ssthresh` is unchanged.

## Step 4 — debug trace

Behind `#define JAIPUR_DEBUG 1`:

- One line per subflow at most every `JAIPUR_TRACE_MS` (default 10 ms), gated
  by `last_trace`:

```
jaipur: path=<path_index> dly=<us> min=<us> mu=<Q20> gamma=<Q20> P=<Q20> cwnd=<pkts> ssthresh=<pkts>
```

`dly` and `min` are printed as `sfw_dly_us >> 3` to undo the ×8 smoothing scale.

## Step 5 — build and run

Kernel change, so a full bake build is required:

```bash
./build.sh --full
./run.sh './waf --run "mptcp --sch=default --cc=jaipur"'
./logs.sh            # trace lines + iperf results
```

Baseline for comparison:

```bash
./run.sh './waf --run "mptcp --sch=default --cc=lia"'
```

`sim.sh` does not pass `--cc`, so use `run.sh` for JAIPUR runs.

## PoC acceptance checks

1. `tcp_available_congestion_control` (printed by the sim) lists `jaipur`.
2. Trace shows `μ` in `[0, 2^20)`, `γ` following `μ` faster upward than
   downward, and `P` taking both signs.
3. `cwnd` grows in slow start while `P <= 0` and drops when `P > 0`.
4. The transfer completes on all three paths; throughput is recorded next to
   the LIA baseline, whatever the number is.
5. The noise on `dly` and `P` during a steady period is noted from the trace,
   to judge whether `HZ=250` is good enough.

## Deferred issues

Not addressed in the PoC. Each lists where the fix would slot in.

| # | Issue | Expected PoC symptom | Where the fix goes |
|---|---|---|---|
| 1 | Decrease fires on every ACK with `P > 0` | cwnd pinned near 2, low throughput | `snd_nxt` marker in the state (Vegas pattern), gate in the `P > 0` branch |
| 2 | No dead band around `P = 0` | cwnd sawtooth driven by noise | Threshold constant in the `P > 0` test |
| 3 | `k_u`, `k_d` are GCC defaults, untuned | `γ` too fast or too slow for these paths | `#define`s |
| 4 | `μ` denominator is absolute OWD, which includes the clock offset | None in DCE if node clocks are aligned; wrong on real hosts | Step 2, normalisation of `q` |
| 5 | Raw delay samples are in 4 ms steps | Noise of roughly ±0.03 on `P`; spurious decreases | Raise `HZ` to 1000 in `arch/lib/Kconfig` (the symbol has no prompt, so `defconfig` alone is ignored) and `arch/lib/defconfig`; baselines must be re-run |
| 6 | `rtt_us` / `srtt_us` are jiffy-stepped in this build | Affects RTT-based schedulers and CCs, not JAIPUR's signal | `sched_clock()` in `arch/lib/sched.c` reading `lib_current_ns()` |
| 7 | Sub-millisecond delay samples | Only matters if `HZ=1000` is still too coarse | Microsecond clock in the TCP timestamp option; invasive, about eight consumers in `tcp_input.c` plus TCP-LP assume jiffies |
| 8 | `min_fw_dly` is the min of a smoothed value and never expires | `q` slightly underestimated; stale after a route change | Step 2, windowed minimum |
| 9 | No coupling between subflows; Steps 4–6 of `cc.md` are missing | More aggressive than one TCP at a shared bottleneck | Meta-level state on `mptcp_meta_sk(sk)` |
| 10 | `γ` and `P` are not reset after RTO or idle restart | Stale threshold after a long gap | `.cwnd_event` / `.set_state` hooks |

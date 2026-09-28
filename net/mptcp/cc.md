## Step 1: Define the Congestion Signal

For each subflow $i$:

**Current OWD:**

$$
OWD_i(t)
$$

**Minimum OWD:**

$$
OWD_{i,\min}
$$

**Queueing delay:**

$$
q_i(t) = OWD_i(t) - OWD_{i,\min}
$$

**Then:**

$$
\mu_i(t) = \frac{q_i(t)}{OWD_i(t)}
$$

**or**

$$
\mu_i(t) = \frac{OWD_i(t) - OWD_{i,\min}}{OWD_i(t)}
$$

# Suppose:

**WiFi**

$$
OWD_{\min} = 15\,ms
$$

Current:

$$
OWD = 18\,ms
$$

Then:

$$
\mu = \frac{18 - 15}{18} = 0.167
$$

**5G**

$$
OWD_{\min} = 25\,ms
$$

Current:

$$
OWD = 40\,ms
$$

Then:

$$
\mu = \frac{40 - 25}{40} = 0.375
$$

The 5G path is carrying more queue buildup.

## Step 2: Build Adaptive Threshold

Each subflow maintains:

$$
\gamma_i(t)
$$

Initialize:

$$
\gamma_i(0) = \mu_i(0)
$$

Update using GCC:

$$
\gamma_i(t) = \gamma_i(t-1) + \Delta t \, k_i(t) \left( \mu_i(t) - \gamma_i(t-1) \right)
$$

where

$$
k_i(t) =
\begin{cases}
k_d & \mu_i < \gamma_i \\
k_u & \mu_i \geq \gamma_i
\end{cases}
$$

### Physical Meaning

$\gamma_i$ learns:

> "What level of queue buildup is normal on this path?"

## Step 3: Compute Congestion Pressure

Now define:

$$
P_i(t) = \mu_i(t) - \gamma_i(t)
$$

This is the key variable.

### Case A

$$
P_i < 0
$$

Meaning:

```
Queue smaller than expected.
```

Path underloaded.

### Case B

$$
P_i \approx 0
$$

Meaning:

```
Path operating normally.
```

### Case C

$$
P_i > 0
$$

Meaning:

```
Queue larger than expected.
```

Congestion emerging.

In GCC:

$$
\Delta T_i = t_i - t_{i-1}
$$

which is simply the elapsed time between two threshold updates.

Mathematically:

$$
\Delta t = \text{Current update time} - \text{Previous update time}
$$

## Example Using an MPTCP Subflow

Suppose subflow-1 receives ACKs every 20 ms.

### ACK #1

Time:

$$
t_1 = 0.020\,s
$$

### ACK #2

Time:

$$
t_2 = 0.040\,s
$$

Then:

$$
\Delta t = 0.040 - 0.020 = 0.020\,s
$$

### ACK #3

Time:

$$
t_3 = 0.060\,s
$$

Then:

$$
\Delta t = 0.020\,s
$$

again.

CC phases

## Step 7: Slow Start Phase

When:

$$
cwnd_i < ssthresh_i
$$

the subflow operates in Slow Start.

### Case 1: Healthy Path

If

$$
P_i \leq 0
$$

then:

$$
cwnd_i = cwnd_i + 1
$$

for every ACK.

This is standard exponential growth.

### Case 2: Early Congestion Detection

If

$$
P_i > 0
$$

then we do NOT wait for packet loss.

Instead:

$$
ssthresh_i = \max\left(\frac{cwnd_i}{2},\, 2\right)
$$

and

$$
cwnd_i = ssthresh_i
$$

Immediately exit Slow Start.

## Congestion Avoidance

When:

$$
cwnd_i \geq ssthresh_i
$$

### If $P_i < 0$

Path underloaded.

Use accelerated additive increase:

$$
cwnd_i = cwnd_i + \frac{1 - P_i}{cwnd_i}
$$

### If $P_i = 0$

Exactly TCP:

$$
cwnd_i = cwnd_i + \frac{1}{cwnd_i}
$$

### If $P_i > 0$

Reduce proportionally:

$$
ssthresh_i = cwnd_i (1 - P_i)
$$

$$
cwnd_i = ssthresh_i
$$

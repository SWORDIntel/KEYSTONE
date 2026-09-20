<!--
  SPDX-License-Identifier: AGPL-3.0-or-later
  Copyright (C) 2026 SWORDIntel. All rights reserved.
-->

# Streaming Telemetry & Deterministic Anomaly Engine

This document specifies the streaming telemetry ingestion, rolling feature windows, online statistical algorithms, and multi-stage deterministic anomaly engine in KEYSTONE.

Governed by Phase 4 of [`CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md`](../../../CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md), this subsystem provides real-time feature extraction and sub-millisecond anomaly detection over infrastructure metrics.

---

## 1. Engine Design & High-Throughput Ingest

CITADEL physical hosts and virtual machines emit continuous streams of CPU, memory, thermal, and I/O metrics. Traditional batch analytics cannot detect sudden anomalies or trend spikes in time to prevent hypervisor failures.

KEYSTONE's telemetry engine operates via:
- **Circular Sample Buffers**: Fixed-capacity ring buffers per node and metric, eliminating allocations on the hot path.
- **Online Welford Statistics**: Computes rolling mean, variance, and standard deviation incrementally in $O(1)$ time.
- **Multi-Tier Temporal Windows**: Concurrently maintains 1-minute, 5-minute, 15-minute, 1-hour, and 24-hour windows.
- **Deterministic Multi-Stage Anomaly Detection**: Combines static threshold verification with statistical $z$-score outlier detection ($|z| \ge 3.0$).

---

## 2. Ingested Metric Types & Structure

Defined in [`include/keystone_telemetry.h`](file:///home/john/Documents/KEYSTONE/include/keystone_telemetry.h):

```c
typedef enum {
    KEYSTONE_METRIC_CPU_UTIL_PCT     = 1, /* CPU load 0..100% */
    KEYSTONE_METRIC_MEM_USED_BYTES    = 2, /* Physical RAM consumption */
    KEYSTONE_METRIC_IO_READ_BYTES     = 3, /* Storage read bandwidth */
    KEYSTONE_METRIC_IO_WRITE_BYTES    = 4, /* Storage write bandwidth */
    KEYSTONE_METRIC_NET_RX_BYTES      = 5, /* Network ingress */
    KEYSTONE_METRIC_NET_TX_BYTES      = 6, /* Network egress */
    KEYSTONE_METRIC_TEMPERATURE_C     = 7, /* Silicon temperature */
    KEYSTONE_METRIC_POWER_WATTS       = 8, /* Package power draw */
    KEYSTONE_METRIC_ERROR_COUNT       = 9  /* Hypervisor/kernel error tokens */
} keystone_metric_type_t;
```

Each sample is ingested as a compact 32-byte record:
```c
typedef struct {
    uint32_t metric_type;
    uint32_t node_id;
    uint64_t timestamp_ms;
    double value;
    uint32_t flags;
    uint32_t tenant_id;
} keystone_telemetry_sample_t;
```

---

## 3. Rolling Feature Windows & Online Algorithms

For each monitored node and metric, the engine tracks statistical summaries across five rolling horizons:
1. `KEYSTONE_WINDOW_1M` (60,000 ms)
2. `KEYSTONE_WINDOW_5M` (300,000 ms)
3. `KEYSTONE_WINDOW_15M` (900,000 ms)
4. `KEYSTONE_WINDOW_1H` (3,600,000 ms)
5. `KEYSTONE_WINDOW_24H` (86,400,000 ms)

```c
typedef struct {
    uint32_t metric_type;
    uint32_t window_type;
    uint64_t sample_count;
    double min_value;
    double max_value;
    double mean;             /* Welford incremental mean */
    double variance;         /* Welford incremental variance */
    double stddev;           /* Standard deviation sqrt(variance) */
    double ewma;             /* Exponentially weighted moving average */
    double slope_trend;      /* Linear regression trend (dv/dt) */
    uint32_t burst_count;    /* Short-term burst count */
    double error_rate;       /* Frequency of error events */
} keystone_feature_window_t;
```

### Statistical Equations

1. **Welford Incremental Mean & Variance**:
   $$M_k = M_{k-1} + \frac{x_k - M_{k-1}}{k}$$
   $$S_k = S_{k-1} + (x_k - M_{k-1})(x_k - M_k)$$
   $$\sigma^2 = \frac{S_k}{k - 1}, \quad \sigma = \sqrt{\sigma^2}$$

2. **Exponentially Weighted Moving Average (EWMA)**:
   $$\text{EWMA}_t = \alpha \cdot x_t + (1 - \alpha) \cdot \text{EWMA}_{t-1}, \quad (\alpha = 0.2)$$

3. **Linear Regression Trend / Slope ($dv/dt$)**:
   $$\text{slope} = \frac{\sum (t_i - \bar{t})(v_i - \bar{v})}{\sum (t_i - \bar{t})^2}$$
   Measures rate of change (e.g., rapid memory leak or thermal runaway before critical limits are breached).

---

## 4. Multi-Stage Anomaly Detection Engine

```mermaid
flowchart TD
    Sample["Inbound Metric Sample (e.g. Temp = 88°C)"] --> StaticCheck{"Stage 1: Static Thresholds"}

    StaticCheck -->|Exceeds Critical (85°C)| CritAlert["Emit CRITICAL Anomaly\n(Severity: EMERGENCY)"]
    StaticCheck -->|Exceeds Warning (75°C)| WarnAlert["Emit WARNING Anomaly\n(Severity: WARNING)"]
    StaticCheck -->|Normal Range| StatCheck{"Stage 2: Statistical z-score"}

    StatCheck -->|Compute z = (x - mean) / stddev| ScoreCheck{"|z| >= 3.0 ?"}
    ScoreCheck -->|Yes| OutlierAlert["Emit STATISTICAL OUTLIER\n(3-Sigma Deviation)"]
    ScoreCheck -->|No| TrendCheck{"Stage 3: Slope Trend"}

    TrendCheck -->|Rapid dv/dt Spike| TrendAlert["Emit TREND SPIKE Anomaly\n(Predictive Warning)"]
    TrendCheck -->|Normal Trend| Pass["Normal Baseline (No Alert)"]
```

### 1. Stage 1: Static Thresholds
Fast verification against fixed hardware operating boundaries (e.g. CPU $\ge 95\%$, Temp $\ge 85^\circ\text{C}$). Immediately catches hard breaches.

### 2. Stage 2: Statistical $z$-Score Outlier Detection
Evaluates how many standard deviations the current sample deviates from the historical rolling baseline:
$$z = \frac{x - \mu}{\sigma}$$

An anomaly is flagged when $|z| \ge 3.0$ ($99.7\%$ probability boundary under Gaussian assumption).

#### Zero-Variance Boundary Protection
If a metric has remained constant for an extended duration, variance $\sigma^2 \to 0$. In this state, a division by zero could cause floating-point exceptions (`NaN` / `Inf`) or suppress detection of genuine spikes. The engine enforces:
$$\sigma_{eff} = \max(\sigma, 10^{-3})$$
ensuring immediate, reliable detection of sudden deviations from constant baselines.

### 3. Stage 3: Slope & Trend Anomaly
Detects rapid rates of change ($|dv/dt| > \text{threshold}$) even when the absolute value has not yet crossed warning limits, providing predictive advance warning of impending failure.

---

## 5. Structured Anomaly Evidence Output

When an anomaly is detected, `keystone_telemetry_check_anomaly()` returns a populated [`keystone_explain_t`](file:///home/john/Documents/KEYSTONE/include/keystone_federation.h) bundle:

- Exact observed value vs baseline mean and standard deviation.
- Computed $z$-score and slope trend.
- Severity rating: `KEYSTONE_SEVERITY_WARNING`, `CRITICAL`, or `EMERGENCY`.
- Causal timestamp and index generation watermark.

This evidence directly feeds the Incident Similarity Engine for automated mitigation lookup.

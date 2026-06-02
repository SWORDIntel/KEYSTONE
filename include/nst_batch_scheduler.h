/* NST Batch Scheduler — Telemetry & Latency Feedback Loop
 * Emits scheduling telemetry samples for latency analysis
 * and distributed batch workload feedback.
 */

#ifndef NST_BATCH_SCHEDULER_H
#define NST_BATCH_SCHEDULER_H

#ifdef __cplusplus
extern "C" {
#endif

void _nst_sched_emit_latency_sample(const char* payload_json);

#ifdef __cplusplus
}
#endif

#endif

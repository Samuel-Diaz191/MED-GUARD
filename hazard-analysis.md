# Hazard Analysis & Standard Mapping — MedGuard ECG Pipeline

**Scope note:** This is an educational simulation built in Wokwi, not a certified or
clinically validated medical device. The hazard analysis below follows the *structure*
of a real medical-device risk analysis process, and maps design decisions to the
standards that would govern a real deployment — it is not a claim of regulatory
compliance.

## Hazard Analysis Table

| # | Hazard | Potential Effect | Mitigation (as implemented) | Related Standard(s) |
|---|---|---|---|---|
| H1 | Queue back-pressure discards a sample at the exact moment it would have been the arrhythmia-triggering reading | A transient arrhythmia event goes undetected for one cycle | Queue depth (8) sized to 2x the computed worst-case burst (200 ms consumer stall ÷ 50 ms period = 4 samples); `dropped_samples` counter surfaced in both monitors so any drop is observable, not silent | ISO 14971 (risk control measure); IEC 62304 §5.6 (software risk management) |
| H2 | A blocking/slow alarm-delivery path delays a genuine arrhythmia alert reaching the responder | Delayed clinical response to a detected arrhythmia | Direct task notification (fastest available FreeRTOS primitive, no queue traversal or separate kernel object) chosen specifically for the coordinator→alert and button→alert paths; latency measured and reported (README §5) rather than assumed | IEC 60601-1-8 (alarm systems — alarm signal delay); ISO 14971 |
| H3 | A task's execution time exceeds its allotted period under adversarial or unanticipated input, causing a scheduling overrun | Missed sampling deadline for the ECG producer, degrading the whole pipeline's timing guarantees | WCET measurement (folded in from App 2) tracks mean/max per task and reports max+30% margin; this evidence is meant to be checked against total utilization before deployment, not assumed safe | IEC 62304 §5.7 (verification of software requirements); general RMS/EDF schedulability analysis |
| H4 | The arrhythmia threshold (`ARRHYTHMIA_THRESHOLD_MV`) is a simplistic fixed-value simulation, not a clinically validated detection algorithm | False positive or false negative arrhythmia classification if this were used as an actual clinical detector | Explicitly disclosed here and in the README as a non-clinical demonstration; a real deployment would require a clinically validated detection algorithm and regulatory clearance before any diagnostic claim | IEC 62304 (software safety classification); FDA general controls for clinical decision-support software (referenced for context, not claimed) |
| H5 | Manual patient-alert button contact bounce was found to be under-filtered (`DEBOUNCE_US = 200` microseconds, not milliseconds), causing a single physical press to register as multiple rapid ISR firings | Multiple spurious alert notifications from a single patient action; masked/altered timing evidence (see README §5's debounce finding) | Documented as an observed, real finding rather than hidden; recommended fix (raise debounce to a few milliseconds) noted for any physical-hardware deployment | IEC 60601-1 (basic safety — input device reliability); general embedded debouncing best practice |
| H6 | Observability plane (serial/web monitor, Wi-Fi/HTTP stack on Core 0) experiences load or failure | If observability and the real-time pipeline shared a core, monitoring load could delay real-time sampling/alerting | Core pinning: all four real-time tasks run exclusively on Core 1; the monitor and Wi-Fi/HTTP stack run exclusively on Core 0, so monitoring-plane load cannot steal cycles from the pipeline (justified in README §6) | IEC 60601-1 (essential performance must not depend on non-essential functions); general RTOS separation-of-concerns practice |
| H7 | The red alarm LED's pulse is a blocking `vTaskDelay` inside `alert_task`, which could theoretically delay processing of a rapid subsequent notification | A second alarm arriving during the ~150 ms LED pulse window would wait behind it | Acceptable for this demo given arrhythmia events are infrequent relative to the pulse duration (~every 2.35 s vs. 150 ms pulse); documented as a known limitation — a production design would move the pulse to a separate low-priority indicator task signaled independently, rather than blocking the alert path itself | ISO 14971 (documented residual risk); IEC 62304 (known limitation disclosure) |
| H8 | Central alarm aggregation (e.g., logging across multiple simulated beds) is out of scope for this build | A real multi-bed deployment would need a race-free, prioritized central alarm log — not present here | Explicitly out of scope for this capstone iteration; the project's earlier development history included and then removed a mutex-protected central alarm log fold-in during scope refinement — noted here so the omission is a documented decision, not an oversight | ISO 14971 (scope boundary documentation) |

## Standards referenced (for context)

- **ISO 14971** — Application of risk management to medical devices. Governs the overall
  process of identifying hazards, estimating and evaluating risk, and implementing and
  verifying risk control measures.
- **IEC 62304** — Medical device software — software life cycle processes. Governs
  software safety classification, risk management integration, and verification
  activities across the software development lifecycle.
- **IEC 60601-1** — Medical electrical equipment — general requirements for basic
  safety and essential performance. Relevant to how a real device's essential
  functions (like alarm delivery) must be protected from being compromised by
  non-essential functions (like a status display).
- **IEC 60601-1-8** — Collateral standard for alarm systems in medical electrical
  equipment. Relevant to alarm signal timing, priority, and delivery guarantees.

These are cited to show the mapping *process* a real device would go through, not to
claim this project has undergone or would pass a formal audit against them.

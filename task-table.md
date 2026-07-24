# Task Table & WCET Evidence — MedGuard ECG Pipeline

WCET measurement is folded in from **App 2**'s Medical Pulse Monitor (`MEASURE_WCET`
macro), applied to all four Core-1 tasks. Both the serial monitor and web monitor print
this table once per cycle at runtime; the values below should be filled in from an
actual Wokwi run, not assumed.

## Task Table

| Task | Core | Priority | Period / Trigger | Deadline | Mean (us) | Max (us) | WCET+30% (us) |
|---|---|---|---|---|---|---|---|
| `ecg_sample` (producer) | 1 | 8 | 50 ms (20 Hz) | 50 ms | 33 | 35 | 45 |
| `arrhythmia_decision` (consumer) | 1 | 8 | event-driven (queue receive, 200 ms timeout) | — | 2712 | 42257 | 54934 |
| `cycle_coordinator` | 1 | 9 | event-driven (event group rendezvous) | — | 3966 | 33412 | 43435 |
| `alert` (responder) | 1 | 12 | event-driven (task notification) | — | 4193 | 44101 | 57331 |

**Finding:** the last three tasks all show a large mean-to-max gap (8-16x), consistently
traced to blocking `ESP_LOG*` UART output calls sitting inside the measured block —
see the README's Tasks & timing section for the full analysis and recommended fix.

## Utilization & Schedulability

Only `ecg_sample` has a true fixed period (50 ms / 20 Hz); the other three tasks are
event-driven rather than strictly periodic, so a formal Liu & Layland utilization bound
applies most directly to the producer. For a complete schedulability picture, treat the
worst observed cascade — one full produced→processed→coordinated→alerted cycle — as the
end-to-end latency bound instead of a simple utilization sum.

```
U(ecg_sample) = WCET+30% / Period = 45 us / 50000 us = 0.0009
```

For a single periodic task, the Liu & Layland bound is trivially satisfied as long as
`U ≤ 1`. The more meaningful schedulability question for this pipeline is whether the
worst-case end-to-end alert latency (button/coordinator → alert task, see README §5)
stays well under any clinically meaningful threshold — since that, not raw
utilization, is what would matter for an arrhythmia alert in a real deployment.

## How to reproduce these numbers

1. Build and run in Wokwi with `USE_WEBSERVER 0` (serial monitor).
2. Let it run for at least 60 seconds so the max column reflects real worst-case
   observations rather than a cold-start artifact.
3. Copy the printed `--- WCET evidence ---` table from the serial monitor into the
   table above.

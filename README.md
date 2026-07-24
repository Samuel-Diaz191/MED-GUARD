# MedGuard — Real-Time Systems Final Capstone

**Course:** EEL 4775 Real-Time Systems — Final Integration Capstone
**Wokwi project:** [Diaz-FINAL-RTS26Summer](https://wokwi.com/projects/470273216543239169)
**Portfolio site:** [https://samuel-diaz191.github.io/MED-GUARD/](https://samuel-diaz191.github.io/MED-GUARD/)

## One sentence

A real-time ECG monitoring system that detects arrhythmias locally and reports measured
worst-case execution time evidence for every pipeline stage, built to demonstrate
safety-critical embedded scheduling analysis and graceful degradation for a **medical
device embedded firmware engineering** role.

## Demo

- Video: [https://youtu.be/37ZBnvYzgKM](https://youtu.be/37ZBnvYzgKM)
- Live Wokwi: [Diaz-FINAL-RTS26Summer](https://wokwi.com/projects/470273216543239169)
- Portfolio site: `docs/index.html` (this repo, served via GitHub Pages)

## Architecture

MedGuard simulates a bedside ECG patient monitor on an ESP32-S3, running a dual-core
FreeRTOS pipeline. Full diagram: [`docs/architecture.svg`](docs/architecture.svg).

- **`ecg_sample_task`** (producer, 20 Hz, Core 1) — generates a simulated decimated ECG sample and pushes it into a queue
- **`arrhythmia_decision_task`** (consumer, Core 1) — pulls samples from the queue and flags any reading above a threshold as an arrhythmia event
- **`cycle_coordinator_task`** (Core 1) — waits, via an event group, for both "sample produced" and "sample processed" to be true for the current cycle, then notifies the alert task
- **`alert_task`** (responder, Core 1) — wakes via direct task notification (from the coordinator, or from a manual patient-alert button ISR) and logs either a routine cycle-complete tick or an arrhythmia alert
- **Core 0** runs an isolated observability plane — a serial monitor (default) or a web monitor (optional), printing queue depth, event bits, dropped-sample count, per-task heartbeats, and WCET evidence, all read via lock-free 32-bit reads that never signal back into the Core 1 pipeline

### Hardware indicators

Two physical LEDs give an at-a-glance status view, independent of reading the serial log:

- **Blue LED (GPIO 2)** — flashes once every time the WCET evidence table is printed. A simple "the monitor is alive and reporting" heartbeat.
- **Red LED (GPIO 4)** — flashes once every time `alert_task` raises a genuine arrhythmia alert (not routine cycle-complete ticks).

Both are implemented as a deliberate **on → brief hold → off pulse**, not a state toggle — see the build notes near the end of this README for why that distinction mattered.

### IPC primitive role justification

| Primitive | Role | Why this primitive |
|---|---|---|
| Queue (`data_q`) | ECG sample producer → arrhythmia consumer | Typed, fixed-size data (an `ecg_sample_t` struct) moving from one producer to one consumer at a known rate — the textbook queue use case, and it natively supports the back-pressure policy described below. |
| Event group (`evt_group`) | Rendezvous between producer, consumer, and coordinator | The coordinator needs to know that *both* "a new sample was produced" AND "that sample was evaluated" have happened before treating the cycle as complete — a multi-condition rendezvous, which is exactly what an event group is for. A queue or notification alone can't express "wait for both of these independent things." |
| Direct task notification (`xTaskNotifyGive` / `ulTaskNotifyTake`) | Coordinator → alert task, and button ISR → alert task | Exactly one receiving task (the alert task), from two different senders (the coordinator task and the button ISR). This is the single fastest primitive available — no separate kernel object, no queue traversal — appropriate for both a per-cycle tick and a manual alert button where responsiveness matters. |

### Queue sizing — the math

**Chosen size:** depth 8, item size `sizeof(ecg_sample_t)` (8 bytes: a `uint32_t` timestamp + an `int` reading).

**Worst-case burst calculation:**
- Producer runs at a fixed 20 Hz → one sample every **50 ms**.
- Consumer calls `xQueueReceive` with a 200 ms timeout — meaning in the worst case, the consumer could be delayed processing for up to 200 ms before giving up on that receive cycle.
- Worst-case backlog during that stall: 200 ms ÷ 50 ms = **4 samples**.
- Chosen depth of **8** gives a 2x safety margin above that computed worst case, absorbing normal scheduling jitter without growing the queue arbitrarily large (which would just hide backlog rather than surface it).

### Back-pressure policy — drop oldest, keep newest

When `xQueueSend` fails (queue full), the producer discards the **oldest** queued sample to make room for the sample it just captured, rather than:
- **Blocking the producer** — this would stall real-time sampling, which is unacceptable for a periodic 20 Hz sensor task.
- **Silently dropping the new sample** — this would hide the *most current* reading behind stale backlog, which is actively worse for a monitoring system where the newest reading is what matters clinically.

Every drop increments a `dropped_samples` counter, surfaced in both the serial and web monitors, so back-pressure activity is observable rather than silent.

### Event group vs. N semaphores

An event group is the better fit here because the coordinator's condition is inherently a **multi-bit AND** ("wait until BOTH the produced bit and the processed bit are set"), evaluated together and cleared together atomically on exit (`pdTRUE` clear-on-exit in `xEventGroupWaitBits`). Implementing the same rendezvous with two separate counting/binary semaphores would require either:
- Taking both semaphores in sequence (introducing an artificial ordering dependency that doesn't actually exist between "produced" and "processed"), or
- Some other manual bookkeeping to detect "both have now happened since I last checked" — which the event group's bit-clearing semantics give for free.

N semaphores would be the better choice instead of an event group only in a scenario where the conditions are truly independent resources being acquired one at a time (e.g., a counting semaphore for a pool of N interchangeable resources) rather than a set of distinct named conditions that must jointly hold — which is not this case.

### Direct notification vs. binary semaphore — measured latency

The latency comparison is implemented directly in `main.c`, ported from **App 3**'s
`button_isr` / `btn_task_sem` / `btn_task_notif` pattern: pressing the "PATIENT ALERT"
button fires a binary semaphore (`bench_sem`) and a direct task notification
(`bench_notif_handle`) from the **same ISR instant** (`bench_isr_entry_us`), so both
paths are measured under identical conditions — same trigger, same task priority (12),
same core.

**Captured data (one button press):**

```
I (29050) medguard_a5: [bench-sem]   wake latency=23 us   (max=3137)
I (29050) medguard_a5: [bench-sem]   wake latency=150 us  (max=3137)
I (29050) medguard_a5: [bench-sem]   wake latency=1455 us (max=3137)
I (29050) medguard_a5: [bench-notif] wake latency=158 us  (max=32071)
```

**An unexpected but genuinely useful finding:** a single button press produced **three**
`[bench-sem]` wake events but only **one** `[bench-notif]` wake event. This traces back
to `DEBOUNCE_US` being set to `200` — that's 200 **microseconds**, not milliseconds —
which is far shorter than typical mechanical (or simulated) contact bounce. A single
physical click was registering as multiple rapid ISR firings within the same
millisecond tick.

That bounce exposed a real behavioral difference between the two primitives rather than
just being noise to discard:

- **Binary semaphore:** each bounce gives the semaphore again. Since `bench_task_sem`
  was fast enough to wake, take, and re-arm between bounces, all three bounces were
  captured as three separate wake events (23 us, 150 us, 1455 us).
- **Direct task notification:** the rapid repeated `xTaskNotifyGive` calls **coalesced**
  into a single accumulated notification value before `bench_task_notif` was scheduled
  to take it — so all three bounces collapsed into one wake-up (158 us), computed
  against the timestamp of the *last* bounce rather than the first.

This means a raw latency-number comparison from this one press is not a clean
apples-to-apples result — the notification's 158 us is measured from a later starting
point than the semaphore's first 23 us wake, because of the coalescing behavior itself,
not because the underlying wake mechanism is slower. With only one press captured, this
is treated as a qualitative finding rather than a statistically averaged result; a
cleaner quantitative comparison would first fix `DEBOUNCE_US` to a few milliseconds
(e.g. `5000` us) to get one clean ISR entry per physical click, then collect multiple
presses to average.

**Practical takeaway:** direct task notifications naturally coalesce multiple rapid
signals into one, while a binary semaphore delivers (and a task can observe) each
individual signal — a real, non-obvious tradeoff between the two primitives beyond raw
speed, and one this benchmark surfaced by accident rather than by design.

### Why pin the web monitor to Core 0?

The web monitor (and serial monitor) are pinned to **Core 0** rather than Core 1 because:
- Core 1 is the dedicated real-time plane running all four pipeline tasks (producer, consumer, coordinator, alert) at priorities 8–12 — introducing a Wi-Fi stack and HTTP server onto that same core risks preempting or delaying time-sensitive pipeline work, especially since Wi-Fi/lwIP processing is not typically priority-managed the same way application tasks are.
- Core 0 is effectively the "networking plane" on the ESP32 — Wi-Fi and the HTTP server naturally belong there, physically isolating observability/reporting work from the real-time work being observed.

**What would go wrong on Core 1:** the monitor task, the HTTP server's internal processing, and lwIP's networking tasks would all compete directly with the ECG pipeline for the same core's CPU time. Under load (e.g., many simultaneous HTTP requests, or Wi-Fi retransmissions), this could delay the producer's 20 Hz timing, the consumer's queue draining, or the coordinator's rendezvous — turning an observability feature into a source of the very timing problems this pipeline is supposed to avoid. Keeping the monitor on Core 0 means a burst of monitor/network activity cannot directly steal cycles from the real-time pipeline.

## Tasks & timing (WCET evidence)

WCET measurement is folded in from **App 2**'s Medical Pulse Monitor (`MEASURE_WCET`
macro), applied to all four Core-1 tasks. Full table and reproduction steps in
[`docs/task-table.md`](docs/task-table.md). Fill in from an actual Wokwi run — let it run
for at least a minute so the max column reflects real worst-case observations, not just
a cold start.

| Task | Period T | WCET C | U=C/T | Priority | Deadline |
|---|---:|---:|---:|---:|---:|
| `ecg_sample` (producer) | 50 ms | 45 us | 0.0009 | 8 | 50 ms |
| `arrhythmia_decision` (consumer) | event-driven | 54934 us | — | 8 | — |
| `cycle_coordinator` | event-driven | 43435 us | — | 9 | — |
| `alert` (responder) | event-driven | 57331 us | — | 12 | — |

Total utilization U = 0.0009 for `ecg_sample` (the only task with a true fixed period —
see `docs/task-table.md` for the fuller schedulability discussion for the event-driven
tasks). `ecg_sample`'s own timing is extremely lightweight and well-bounded (33 us mean,
35 us max) — it's essentially just generating a value and enqueueing it.

**A real finding worth noting:** the other three tasks all show a large gap between
their mean and max execution time — `arrhythmia_decision` is ~15.6x (2712 us mean vs.
42257 us max), `cycle_coordinator` is ~8.4x (3966 us vs. 33412 us), and `alert` is
~10.5x (4193 us vs. 44101 us). This consistent pattern across three otherwise-simple
tasks points to a shared cause rather than three unrelated coincidences: all three call
`ESP_LOGI`/`ESP_LOGW`/`ESP_LOGE` inside their measured `MEASURE_WCET` block, and UART
log output can block for a variable, sometimes-long duration if the serial monitor
isn't draining the TX buffer fast enough. In a real deployment, this would be a genuine
concern — a task's *worst-case* timing being 8-15x its typical case, driven by a logging
call, is exactly the kind of thing WCET analysis is supposed to catch before it becomes
a missed deadline. A production fix would move logging out of the timed critical path
(e.g., queue log messages to a separate low-priority logger task) rather than call
blocking UART output from inside a time-sensitive task.

## Hazard analysis & standard mapping

Full table in [`docs/hazard-analysis.md`](docs/hazard-analysis.md) — an educational risk
analysis (not a regulatory claim) mapping design decisions to the standards that would
govern a real deployment (ISO 14971, IEC 62304, IEC 60601-1/-8). Highlights:

- **Queue back-pressure** could discard a sample at the wrong instant → mitigated by depth sized to 2x worst-case burst, with drops surfaced via a monitor counter rather than silent.
- **Slow alarm-delivery path** could delay a genuine arrhythmia alert → mitigated by choosing direct task notification (fastest available primitive) and measuring, not assuming, its latency.
- **Button contact bounce** was found to be under-filtered (200 us, not ms) → discovered via real benchmark data, documented rather than hidden, with a concrete fix recommended for hardware deployment.

## Graceful degradation

The producer's queue back-pressure policy is the graceful-degradation path for this
system: if `data_q` fills (the consumer stalls or falls behind), the producer does not
block waiting for space, and it does not silently drop the newest reading either. It
discards the *oldest* queued sample to make room for the current one, incrementing
`dropped_samples`, so local ECG sampling continues at its full 20 Hz rate no matter what
the consumer is doing. Demonstrated live by artificially stalling
`arrhythmia_decision_task` to force the queue to fill and watching `dropped_samples`
climb while `ecg_sample`'s heartbeat keeps incrementing normally.

## Build & run

1. Open the Wokwi project (`Diaz-FINAL-RTS26Summer`) or clone `firmware/` locally with the ESP-IDF toolchain targeting ESP32-S3.
2. Default build (`USE_WEBSERVER 0`) runs entirely in Wokwi with no Wi-Fi — serial monitor prints queue depth, event bits, heartbeats, and the WCET evidence table once a second.
3. To try the web monitor, set `USE_WEBSERVER 1`, fill in real `WIFI_SSID` / `WIFI_PASS` values, and rebuild — requires actual Wi-Fi hardware/credentials, not available in the Wokwi simulator.
4. Press the "PATIENT ALERT" button to trigger the manual alert path and the App 3 latency benchmark; watch for the blue (WCET heartbeat) and red (arrhythmia alert) LEDs.

## Tailored for

**Medical device embedded firmware engineer.** Every major decision in this project
prioritizes measured evidence over assumption — queue sizing from worst-case burst math,
notification-vs-semaphore latency from an actual benchmark (not a spec sheet claim), and
WCET evidence gathered rather than guessed. Real bugs found during development (a macro
argument-count compile error, an LED toggle-vs-pulse mismatch, an under-filtered button
debounce) are documented in place rather than silently fixed and hidden, which is the
same instinct a safety-relevant embedded role requires: a system's real behavior matters
more than its intended behavior, and both need to be checked.

---

## Build notes (bugs found and fixed during development)

### LED toggle vs. pulse (and protecting the WCET data)

The two status LEDs were initially implemented as a **state toggle** (flip on, flip
off, flip on...) rather than a pulse. That's a subtle but real bug: a toggle only
turns the LED *on* every other event — the alternating events turn it *off* — so
watching the blue LED against the WCET table printing every single cycle looked
mismatched/laggy, since only half the prints actually lit the LED. Fixed by switching
both LEDs to an explicit **on → brief hold → off pulse** instead, so every WCET print
and every real arrhythmia alert produces exactly one visible flash, with no
skipped/inverted events.

A second detail mattered for the red LED specifically: its pulse (`vTaskDelay`) is
placed **outside** the `MEASURE_WCET` block in `alert_task`, not inside it. Blocking
inside the timed block would have inflated `alert_task`'s reported WCET by the pulse
duration, corrupting exactly the evidence this fold-in from App 2 is supposed to
produce. A local `bool this_was_an_alarm` flag carries the decision out of the timed
block so the pulse can happen afterward without touching the measurement.

### A real macro bug worth documenting

While wiring up the App 2 `MEASURE_WCET` macro, `ecg_sample_task` initially failed to
compile with `macro "MEASURE_WCET" passed 6 arguments, but takes just 4`. Root cause:
the task built its data item with a designated struct initializer,
`{ .timestamp_ms = ..., .ecg_mv = ... }`, passed as the macro's code-block argument. The
C preprocessor only tracks `()` nesting when splitting macro arguments — not `{}` — so
the comma between the two struct fields was misread as separating `MEASURE_WCET`'s own
arguments. Fixed by assigning the two fields as separate statements
(`sample.timestamp_ms = ...; sample.ecg_mv = ...;`) instead of one comma-separated
initializer, which avoids any top-level (unparenthesized) comma inside the macro
argument. The other three `MEASURE_WCET` call sites were unaffected, since every comma
inside them already sat safely inside a function call's own parentheses (e.g.
`ESP_LOGW(TAG, "...", arg)`).

## Reused code (honor code citation)

- Web monitor Wi-Fi STA connection and HTTP server pattern (`wifi_init_sta`, `handle_root`, `start_webserver`) — adapted from App 1's HTTP server implementation, per the assignment's explicit allowance to reuse this infrastructure.
- Heartbeat counters (`hb_prod`, `hb_cons`, `hb_coord`, `hb_resp`) — provided in the scaffold, originally from App 2's pattern.
- Latency benchmark (`bench_sem`, `bench_notif_handle`, `bench_isr_entry_us`, `bench_task_sem`, `bench_task_notif`, and the dual-fire logic in `button_isr`) — ported directly from **App 3**'s `button_isr` / `btn_task_sem` / `btn_task_notif` pattern, which fires a semaphore and a notification from the same ISR instant to produce a fair head-to-head latency comparison.
- WCET measurement infrastructure (`MEASURE_WCET` macro, `wcet_*_max_us` / `wcet_*_total_us` / `wcet_*_count` variables, and the mean/max/WCET+30% reporting in both monitors) — folded in directly from **App 2**'s Medical Pulse Monitor, which used the identical macro and reporting pattern across its four periodic tasks.

## AI usage disclosure

I used Claude (Anthropic) throughout this project, on **App 5 itself** as well as the
fold-ins from prior apps. Specifically, Claude helped:

- Implement **App 5**'s core pipeline — the `ecg_sample_task` (producer),
  `arrhythmia_decision_task` (consumer), `cycle_coordinator_task`, and `alert_task`
  (responder) task bodies, the queue/event-group/notification wiring between them, the
  medical ECG theme, the queue sizing math, and the back-pressure policy defense.
- Add and debug the two status LEDs on **App 5** (including fixing a toggle-vs-pulse
  visual bug and keeping the red LED's pulse outside the WCET timing block to avoid
  corrupting that data).
- Fold in and adapt **App 1**'s web monitor Wi-Fi/HTTP server pattern into App 5's
  `webmonitor_task`, per the assignment's explicit allowance to reuse this
  infrastructure.
- Fold in **App 2**'s `MEASURE_WCET` macro and mean/max/WCET+30% reporting pattern
  across all four App 5 tasks, and diagnose/fix a real compile error this introduced
  (a struct initializer's comma being misread inside the macro argument).
- Fold in **App 3**'s `button_isr` / `btn_task_sem` / `btn_task_notif` dual-fire
  latency-benchmark pattern into App 5, to produce the measured notification-vs-semaphore
  comparison in this README.
- Build the capstone-level deliverables: the GitHub Pages portfolio site (`index.html`,
  the system architecture diagram, the hazard analysis and task-table documents), the
  concurrency sequence diagram, and this README's structure.

The latency measurements and debounce finding documented above are from my own Wokwi
run, as required — Claude did not generate or fabricate those numbers as well for the WCET
table.

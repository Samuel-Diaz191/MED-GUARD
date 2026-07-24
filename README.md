# MedGuard — Final Capstone: Real-Time ECG Monitoring System

**Course:** EEL 4775 Real-Time Systems — Final Integration Capstone
**Wokwi project:** [PASTE YOUR WOKWI URL HERE — should be named `<LASTNAME>-FINAL-RTS26Summer`]

## Capstone Step 1 — Integration

**Target role:** Medical device embedded firmware engineer

**Theme (one sentence):** A real-time ECG monitoring system that detects arrhythmias
locally and reports measured worst-case execution time evidence for every pipeline
stage, built to demonstrate safety-critical embedded scheduling analysis and graceful
degradation for a medical device embedded firmware engineering role.

**Integrated capability (folded in from a prior app):** App 5 (this system) serves as
the spine. Folded in from **App 2**: the `MEASURE_WCET` timing macro and mean/max/
WCET+30% reporting pattern from App 2's Medical Pulse Monitor, applied to all four
Core-1 tasks (`ecg_sample`, `arrhythmia_decision`, `cycle_coordinator`, `alert`) — this
directly produces the **Task table + WCET evidence** required for the capstone
portfolio site, which nothing in the base App 5 scaffold provided on its own.

**Graceful degradation (used in the demo):** the producer's queue back-pressure policy
already implements this — if `data_q` fills (the consumer stalls or falls behind), the
producer does not block waiting for space, and it does not silently drop the newest
reading either. It discards the *oldest* queued sample to make room for the current
one, incrementing `dropped_samples`, so local ECG sampling continues at its full 20 Hz
rate no matter what the consumer is doing. This is the degradation path to demonstrate
live in the capstone video (e.g., by artificially stalling `arrhythmia_decision_task`
to force the queue to fill and watching `dropped_samples` climb while `ecg_sample`'s
heartbeat keeps incrementing normally).

## Theme

MedGuard's App 5 simulates a simplified ECG monitoring pipeline running entirely on Core 1, observed from Core 0:

- **`ecg_sample_task`** (producer, 20 Hz) — generates a simulated decimated ECG sample and pushes it into a queue
- **`arrhythmia_decision_task`** (consumer) — pulls samples from the queue and flags any reading above a threshold as an arrhythmia event
- **`cycle_coordinator_task`** — waits (via an event group) for both "sample produced" and "sample processed" to be true for the current cycle, then notifies the alert task
- **`alert_task`** (responder) — wakes via direct task notification (from the coordinator, or from a manual patient-alert button ISR) and logs either a routine cycle-complete tick or an arrhythmia alert
- **Core 0** runs an observability plane — a serial monitor (default) or a web monitor (optional deliverable), printing queue depth, event bits, dropped-sample count, and per-task heartbeats

## Hardware indicators

Two physical LEDs give an at-a-glance status view, independent of reading the serial log — useful for the demo video:

- **Blue LED (GPIO 2)** — flashes once every time the WCET evidence table is printed (once a second in the serial monitor; once per page load in the web monitor). A simple "the monitor is alive and reporting" heartbeat.
- **Red LED (GPIO 4)** — flashes once every time `alert_task` raises a genuine arrhythmia alert (not routine cycle-complete ticks). Lets a viewer see an alarm event happen on hardware, in sync with the `*** ARRHYTHMIA ALERT ***` log line.

Both are implemented as a deliberate **on → brief hold → off pulse**, not a toggle — see the build note below for why that distinction actually mattered here.

## 1. IPC primitive role justification

| Primitive | Role | Why this primitive |
|---|---|---|
| Queue (`data_q`) | ECG sample producer → arrhythmia consumer | Typed, fixed-size data (an `ecg_sample_t` struct) moving from one producer to one consumer at a known rate — the textbook queue use case, and it natively supports the back-pressure policy described below. |
| Event group (`evt_group`) | Rendezvous between producer, consumer, and coordinator | The coordinator needs to know that *both* "a new sample was produced" AND "that sample was evaluated" have happened before treating the cycle as complete — a multi-condition rendezvous, which is exactly what an event group is for. A queue or notification alone can't express "wait for both of these independent things." |
| Direct task notification (`xTaskNotifyGive` / `ulTaskNotifyTake`) | Coordinator → alert task, and button ISR → alert task | Exactly one receiving task (the alert task), from two different senders (the coordinator task and the button ISR). This is the single fastest primitive available — no separate kernel object, no queue traversal — appropriate for both a per-cycle tick and a manual alert button where responsiveness matters. |

## 2. Queue sizing — the math

**Chosen size:** depth 8, item size `sizeof(ecg_sample_t)` (8 bytes: a `uint32_t` timestamp + an `int` reading).

**Worst-case burst calculation:**
- Producer runs at a fixed 20 Hz → one sample every **50 ms**.
- Consumer calls `xQueueReceive` with a 200 ms timeout — meaning in the worst case, the consumer could be delayed processing for up to 200 ms before giving up on that receive cycle.
- Worst-case backlog during that stall: 200 ms ÷ 50 ms = **4 samples**.
- Chosen depth of **8** gives a 2x safety margin above that computed worst case, absorbing normal scheduling jitter without growing the queue arbitrarily large (which would just hide backlog rather than surface it).

## 3. Back-pressure policy — drop oldest, keep newest

When `xQueueSend` fails (queue full), the producer discards the **oldest** queued sample to make room for the sample it just captured, rather than:
- **Blocking the producer** — this would stall real-time sampling, which is unacceptable for a periodic 20 Hz sensor task.
- **Silently dropping the new sample** — this would hide the *most current* reading behind stale backlog, which is actively worse for a monitoring system where the newest reading is what matters clinically.

Every drop increments a `dropped_samples` counter, surfaced in both the serial and web monitors, so back-pressure activity is observable rather than silent.

## 4. Event group vs. N semaphores

An event group is the better fit here because the coordinator's condition is inherently a **multi-bit AND** ("wait until BOTH the produced bit and the processed bit are set"), evaluated together and cleared together atomically on exit (`pdTRUE` clear-on-exit in `xEventGroupWaitBits`). Implementing the same rendezvous with two separate counting/binary semaphores would require either:
- Taking both semaphores in sequence (introducing an artificial ordering dependency that doesn't actually exist between "produced" and "processed"), or
- Some other manual bookkeeping to detect "both have now happened since I last checked" — which the event group's bit-clearing semantics give for free.

N semaphores would be the better choice instead of an event group only in a scenario where the conditions are truly independent resources being acquired one at a time (e.g., a counting semaphore for a pool of N interchangeable resources) rather than a set of distinct named conditions that must jointly hold — which is not this case.

## 5. Direct notification vs. binary semaphore — measured latency

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

## 6. Engineering analysis — why pin the web monitor to Core 0?

The web monitor (and serial monitor) are pinned to **Core 0** rather than Core 1 because:
- Core 1 is the dedicated real-time plane running all four pipeline tasks (producer, consumer, coordinator, alert) at priorities 8–12 — introducing a Wi-Fi stack and HTTP server onto that same core risks preempting or delaying time-sensitive pipeline work, especially since Wi-Fi/lwIP processing is not typically priority-managed the same way application tasks are.
- Core 0 is effectively the "networking plane" on the ESP32 — Wi-Fi and the HTTP server naturally belong there, physically isolating observability/reporting work from the real-time work being observed.

**What would go wrong on Core 1:** the monitor task, the HTTP server's internal processing, and lwIP's networking tasks would all compete directly with the ECG pipeline for the same core's CPU time. Under load (e.g., many simultaneous HTTP requests, or Wi-Fi retransmissions), this could delay the producer's 20 Hz timing, the consumer's queue draining, or the coordinator's rendezvous — turning an observability feature into a source of the very timing problems this pipeline is supposed to avoid. Keeping the monitor on Core 0 means a burst of monitor/network activity cannot directly steal cycles from the real-time pipeline.

## Task Table + WCET Evidence

*(Required capstone portfolio deliverable — fill in after running in Wokwi. Values come
from the WCET table both monitors print once a second; let it run for at least a minute
so the max column reflects real worst-case observations, not just a cold start.)*

| Task | Core | Priority | Period/Trigger | Mean (us) | Max (us) | WCET+30% (us) |
|---|---|---|---|---|---|---|
| `ecg_sample` (producer) | 1 | 8 | 50 ms (20 Hz) | *[fill in]* | *[fill in]* | *[fill in]* |
| `arrhythmia_decision` (consumer) | 1 | 8 | event-driven (queue receive) | *[fill in]* | *[fill in]* | *[fill in]* |
| `cycle_coordinator` | 1 | 9 | event-driven (event group) | *[fill in]* | *[fill in]* | *[fill in]* |
| `alert` (responder) | 1 | 12 | event-driven (task notification) | *[fill in]* | *[fill in]* | *[fill in]* |

## Build note — LED toggle vs. pulse (and protecting the WCET data)

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

## Build note — a real macro bug worth documenting

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
run, as required — Claude did not generate or fabricate those numbers, and the WCET
table above still needs my own run's real values filled in.

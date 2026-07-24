# Final Reflection — MedGuard Capstone

**EEL 4775 Real-Time Systems, Summer 2026**

## What I would do differently

If I were starting over, I would draw a harder line between "the required App 5
baseline" and "experimental fold-ins" before writing a single line of integration code.
Partway through this capstone, I built out a full mutex-protected central alarm log
folded in from App 4 — complete with a second simulated writer task, a ground-truth
counter, and an induced-failure race demo — only to later strip all of it back out in
favor of a simpler, more directly useful fold-in from App 2 (the WCET measurement
macro). The App 4 work wasn't wasted effort exactly, but it was scope I committed to
before stepping back and asking which integrated capability would actually earn its
place in the final system. Next time, I would sketch out two or three fold-in
candidates on paper first, weigh what each one actually *proves* about the system, and
commit to one before writing code, rather than building first and re-evaluating after.

I would also set `DEBOUNCE_US` correctly from the very first commit. It sat at 200
microseconds — instead of milliseconds — for most of the project, and while that
"bug" ended up producing one of the most interesting findings in this whole capstone
(the notification-coalescing discovery in the latency benchmark), that was luck, not
design. I'd rather find real hardware quirks on purpose than stumble into them and
have to reverse-engineer why my data looked strange after the fact.

## What was harder than expected

Two things surprised me. The first was a genuinely obscure C preprocessor bug: a
struct's designated initializer, `{ .timestamp_ms = ..., .ecg_mv = ... }`, passed
directly inside a macro call, caused a "passed 6 arguments, but takes just 4" compile
error. It took real digging to realize the C preprocessor only tracks parentheses when
splitting macro arguments, not curly braces — so a completely ordinary struct
initializer was silently being torn apart by the macro expansion. Nothing about that
bug was hard to *fix* once I understood it; the hard part was building the mental model
of *why* it was happening at all, since the error message pointed at the call site, not
the actual root cause.

The second was harder in a different way: publishing the GitHub Pages site turned out
to be its own small project. Files I uploaded landed flat at the repo root instead of
in the nested `docs/`/`firmware/` structure I'd planned around, which broke every
relative link in my portfolio page. Then, separately, GitHub's default Jekyll
processing quietly renamed my README on build, breaking a link I assumed would just
work. Neither issue was conceptually difficult, but both were the kind of "the
platform did something I didn't expect" problem that's easy to underestimate until
you're the one debugging a 404 on a page that looks otherwise correct.

## The most valuable thing I learned

The most valuable realization came from the WCET evidence itself, not from the IPC
primitives I expected to be the centerpiece of this project. Three of my four tasks
showed an 8–16x gap between mean and max execution time, and tracing that down led
somewhere genuinely useful: all three were calling blocking `ESP_LOG*` UART output
*inside* the timed section, and that logging call's variable latency — not the actual
work the task was doing — was dominating the worst case. That's a small, specific
finding, but it generalizes into something I'll carry into every embedded project
going forward: a task's *typical* behavior and its *worst-case* behavior can come from
completely different sources, and if you only ever look at the average, you will
never see the thing that actually threatens your deadline.

More broadly, this capstone reinforced that documenting a real bug, honestly and in
place, is more valuable — not less — than quietly fixing it and moving on. Every
"build note" in my README (the macro bug, the LED toggle-vs-pulse mismatch, the
debounce discovery, the logging-latency finding) started as something going wrong.
Writing each one up turned a mistake into evidence that I actually understand the
system I built, which is exactly the habit I want to bring into a medical device
embedded firmware role: the system's real, measured behavior matters more than its
intended behavior, and the only way to know the difference is to go looking for it.

---

## AI usage disclosure

Claude (Anthropic). Accessed 2026-07-23. Used to help draft the structure and language
of this reflection based on my own project experiences, decisions, and debugging
history throughout the capstone (the App 4 mutex work later removed, the debounce
finding, the macro/comma compile bug, the GitHub Pages folder-flattening and Jekyll
link issues, and the WCET logging-latency finding). Content reflects my own
recollection of these events; reviewed and edited by me before submission.

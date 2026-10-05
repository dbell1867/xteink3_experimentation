# Lesson 03 — Buttons on resistor ladders: debounce, long press, and key events

**Stages:** 1 (raw values), 3, 10 (event queue) · **Code:** `stages/03-buttons/`, `src/buttons.{h,cpp}`

## Objectives
- Read seven keys from three pins: two analog **resistor ladders** + one digital key.
- Debounce without missing presses, including presses made *during* a screen refresh.
- Turn raw readings into events (Pressed / LongPress / Released) a UI can use.

## Hardware: a resistor ladder
Several keys share one ADC pin; each pulls the voltage to a different level through a
different resistor. One reading tells you which key is down (but not two on the same ladder).

| Key | Pin | Raw ADC (X3, measured) | X4 average (CrossPoint) |
|---|---|---|---|
| none | GPIO1 / GPIO2 | **4095** | — |
| Back | GPIO1 | 3483 | 3512 |
| Confirm | GPIO1 | 2663 | 2694 |
| Left | GPIO1 | 1469 | 1493 |
| Right | GPIO1 | 0 | 5 |
| Up (side) | GPIO2 | 2207 | 2242 |
| Down (side) | GPIO2 | 4 | 5 |
| Power | GPIO3 | digital, LOW when pressed | — |

Within ~30 counts of the X4, so CrossPoint's midpoint thresholds work; we added "nothing
pressed" above 3900.

## Design
- **Sample every 5 ms; accept a reading only after it holds 20 ms.** The worry: pressing
  Left (≈1470) makes the voltage *fall* from 4095, passing through Back's and Confirm's ranges.
  A naive reader would fire Back first. We logged every rejected short-lived reading
  ("glitch") to find out. **Prediction: few or none. Result: 0** across all presses, holds and
  double-taps — but the rule costs nothing to keep.
- **Each pin debounced separately**, so keys on different ladders can combine later.
- **LongPress fires once at 700 ms** while still held; Released carries the hold time.
- **Buttons keep being sampled during a refresh** via the display driver's idle hook — presses
  made while the panel was busy were counted (the log proved it even when it didn't feel like it).

## Stage 10 — events must not be handled mid-refresh
Once menus existed, the callback could fire *inside* a refresh (idle hook). So the callback
only **queues** events (a 16-slot ring buffer); `loop()` handles them between refreshes.
And a new trap: if Back *closed* a menu on **press**, its **release** arrived in the next
screen and reopened the menu. Fix: **Back and Confirm act on release** everywhere; arrow keys
act on press.

## Parked: long press on page keys
Holding Right fires the 1-page turn on press, then the +10 jump at 700 ms → the wrong page
shows for ~200 ms. Options discussed: act on release (+~190 ms per turn — measured taps were
130–240 ms), press + auto-repeat, a separate jump gesture. Parked; see PLAN.md.

## Real bugs and fixes
| Symptom | Cause | Fix |
|---|---|---|
| Idle readings labelled "Back?/Up?" in the probe | no "nothing pressed" range | `ADC_NO_BUTTON = 3900` |
| Back's tap and hold both fired | tap acted on press | tap = release before 700 ms |
| Closing a menu reopened it | release delivered to the next screen | Back/Confirm on release everywhere |
| (Lesson 05) light sleep would miss keys | ladder voltages aren't digital LOW → can't GPIO-wake | poll on each 50 ms timer wake |

## C++ for a Python programmer
- `using Handler = void (*)(Button, ButtonEvent, uint32_t);` names a function-pointer type —
  like `Callable[[Button, ButtonEvent, int], None]`.
- `struct Channel { Button stable = Button::None; … }; Channel ch[3];` — an array of small
  records, like `[Channel() for _ in range(3)]`, but laid out in one block of memory.
- A ring buffer (`keyQueue[16]`, `qHead`, `qTail`) ≈ `collections.deque(maxlen=16)` without allocation.

## Cheat-sheet
```cpp
buttons.begin(onButton);          // onButton(Button, ButtonEvent, heldMs)
buttons.update();                 // call often; samples every 5 ms
buttons.busy();                   // a key is down or still debouncing -> don't light-sleep
```

## Glossary
- **Debounce** — ignore the brief chatter of a contact; here, "must hold 20 ms".
- **ADC attenuation 11 dB** — lets the ADC measure up to ~3.1 V.
- **Glitch** (our log) — a reading that changed but didn't last long enough to be accepted.

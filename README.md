# Cycle Tester

A four-servo cycle-testing robot for an Arduino Mega with a touchscreen. Pick how many cycles each servo should
do, press START, and watch the counters. Each servo sweeps 180°, returns to its start position, and counts one
cycle. Servos can be paused and resumed individually or all at once.

| Folder | What it is |
|---|---|
| `CycleTester/` | **The sketch.** Open `CycleTester/CycleTester.ino` in the Arduino IDE. |
| `tools/ServoRangeTest/` | A small helper sketch that moves a servo to an exact pulse width, to measure its real travel. Optional. |
| `tests/host/` | A PC simulation of the sketch (no hardware needed), used to test the logic and preview the screens. |
| `SATv3_oasistest/` | Your earlier Snap & Test sketch, untouched. |

## 1. Set up

**Board:** Arduino Mega 2560.

**Libraries** (Library Manager, unless noted):

| Library | Notes |
|---|---|
| ServoEasing (Armin Joachimsmeyer) | Tested against 3.6.0. Installs the Servo library it needs. |
| Adafruit FT6206 Library | Also needs **Adafruit BusIO** (the Library Manager offers to install it). |
| LCDWIKI_SPI and LCDWIKI_gui | The copies you used for the Snap & Test sketch, from the Hosyond / LCDWIKI download. They must be the versions that know the `ST7796S` display. The public GitHub copy of `LCDWIKI_SPI` does not. |

Compile and upload `CycleTester/CycleTester.ino`. Nothing moves at power-up: the servos get no signal until you press START.

## 2. Wiring

Your existing pins, unchanged (they are also written at the top of `Config.h`):

| Servo | Pin | | Display / touch | Pin |
|---|---|---|---|---|
| Servo 1 | 5 | | LCD_CS | 8 |
| Servo 2 | 6 | | LCD_RST | 7 |
| Servo 3 | 3 | | LCD_RS (DC) | 9 |
| Servo 4 | 4 | | SDI / SCK / SDO | 51 / 52 / 50 |
| | | | CTP_INT | 2 |
| | | | CTP_SDA / CTP_SCL | SDA / SCL |
| | | | CTP_RST, LED, VCC | 3.3 V, 5 V, 5 V |

Note: the old sketch also declared `CTP_RST` as pin 6, which is Servo 2's pin. Because CTP_RST is wired to 3.3 V
that did nothing useful, and it is not repeated here.

### Servo power (read this before connecting four servos)

* The ANNIMOS 35 KG servo is rated 5 – 8.4 V; 7.4 V is the nominal figure. Never exceed 8.4 V.
* Seller listings for this servo family show a **stall current of about 3 A each at 7.4 V**, so four servos can in
  theory ask for ~13 A. A lightly loaded fixture draws far less, but use a regulated supply sized for your measured
  peak (10 A or more is a sensible starting point), a fuse in the servo supply line, and thick wire.
* **Do not power the servos from the Mega's 5 V pin.** Give them their own supply and **connect its ground to the
  Mega's GND**, otherwise the signal has no reference ([Arduino Servo reference](https://reference.arduino.cc/reference/en/libraries/servo)).
* Power the Mega from USB (or its own adapter), not from the servo rail. A servo current spike can otherwise reset it.
* Put a large capacitor (a couple of thousand µF, 16 V or more) across the servo supply near the servos.
* Put a switch in the servo supply line. It is your emergency stop.
* Servos are started 250 ms apart (`START_STAGGER_MS`) so they do not all draw their start-up current at once.

## 3. First run, in this order

1. Open `CycleTester/Config.h` (the `Config.h` tab in the Arduino IDE) and check the section "SERVO POSITIONS". It is set for a **270° servo with the 180° sweep centred in its travel** (`SERVO_FULL_TRAVEL_DEG = 270`, `SWEEP_START_OFFSET_DEG = 45`, which is 833 → 2167 µs). If your servos turn out to be the 180° version, use `180` and `0` (500 → 2500 µs). The ANNIMOS 35 KG family is sold both ways.
2. Upload `CycleTester`, run a short test (100 cycles) with a light load and **no product on the fixture**, and watch the first few cycles.
3. Fine tune where each arm starts and how far it sweeps, and how fast it goes, as described in section 5. Every change is made in `Config.h` and needs a new upload.

(`tools/ServoRangeTest` is a separate sketch for finding a servo's real end stops: in its Serial Monitor at 115200 baud, line ending "Newline", type e.g. `1 1500` or `all 1500` to move to a pulse width, then `1 +5` / `1 -1` to jog.)

## 4. Using it

These are renders produced by the PC simulation (`tests/host`) using the display library's own drawing routines, so layout and colours are what the screen should show. They are not photos of the real display.

| Setup | Running, one servo paused | All paused | Finished |
|---|---|---|---|
| ![Setup screen](docs/screenshots/setup.png) | ![Running, S2 paused](docs/screenshots/run-one-paused.png) | ![All paused](docs/screenshots/run-all-paused.png) | ![Complete](docs/screenshots/run-complete.png) |

**Setup screen:** each servo has `-1000 -100 [count] +100 +1000` buttons. Hold a button to repeat. `OFF` (0) leaves that
servo out of the test. `ALL = S1` copies servo 1's count to every servo. The estimated run time is shown above START.

**Run screen:** each row shows the cycle count, the target, a progress bar, the state, and a **PAUSE / RESUME** button for that servo.
The bottom bar has **PAUSE ALL**, **RESUME ALL** and **SETUP**. SETUP asks "SURE?" (tap again within 3 s) because it ends
the test and the counts are lost. When everything has finished it becomes **NEW TEST**.

How it behaves:

* **One cycle** = start position → 180° → back to the start. The counter goes up each time a servo gets back to the start.
* **Pause** freezes the servo exactly where it is. It stays powered and keeps holding that position (and its current draw and heat), so do not leave a loaded servo paused for hours. **Resume** carries on from that spot.
* When a servo reaches its target it stops, settles, and is released (no signal, so it is silent and cool). Set `DETACH_WHEN_DONE = false` to make it hold the start position instead.
* **SETUP** while running sends the servos slowly back to their start position, then releases them.
* Counts are kept in RAM only. A power cut ends the test and loses the counts.
* Changing screens redraws everything, which takes about 2.5 s on this display. Tapping is ignored briefly afterwards.

## 5. Fine tuning the position and the speed (in `Config.h`)

Everything below is a number in `CycleTester/Config.h`. Edit it, press Upload, and the change applies the next time a test starts. To try a number first without uploading, see "Try a position" below.

### Where each servo starts and how far it sweeps

| Setting | What it does |
|---|---|
| `SWEEP_START_OFFSET_DEG` | **Coarse, all four servos.** Where the 180° sweep begins, in whole degrees from the servo's MIN-pulse end. `45` centres it on a 270° servo. |
| `SERVO_START_TRIM_DEG` | **Fine, one number per servo** (S1, S2, S3, S4). Moves that servo's whole sweep, start and end together, by this many degrees. Decimals are fine. The sweep stays 180°. |
| `SERVO_MEASURED_SWEEP_DEG` | **True 180°, one number per servo.** Type how far the arm really turned. The sketch widens or narrows the sweep so it comes out at 180°. Leave at `180.0` until you have measured. |

```cpp
//                                                       S1      S2      S3      S4
constexpr float SERVO_START_TRIM_DEG[NUM_SERVOS]     = {   0.0f,  -3.5f,  1.25f,   0.0f };   // S2 moved 3.5 degrees one way, S3 1.25 the other
constexpr float SERVO_MEASURED_SWEEP_DEG[NUM_SERVOS] = { 180.0f, 180.0f, 177.5f, 180.0f };   // S3 only turned 177.5 degrees
```

* **Which way is positive?** A positive trim turns the arm towards the MAX-pulse end of its travel. Which way that is on your fixture depends on how the servo is mounted, so try `+5.0` on one servo and see; if you want the other direction, flip the sign.
* **How small a step?** One pulse-width step (1 µs) is about **0.135°** on a 270° servo (0.09° on a 180° one), so the numbers are good to roughly 0.1°. Values are rounded to the nearest microsecond. The servo's own dead band (2 – 3 µs) means the arm cannot really be placed more finely than about 0.3°.
* **Limits.** The sketch refuses to compile (and tells you why) if a trim or measured sweep would push a pulse outside `SERVO_PULSE_MIN_US` … `SERVO_PULSE_MAX_US` (500 – 2500 µs), because the arm would then run into its end stop.

### Try a position before putting it in `Config.h` (Serial Monitor)

You do not have to upload after every try. Type the number in the Serial Monitor and the arm goes there. Open it with Tools → Serial Monitor in the Arduino IDE, set **115200 baud** and the line ending to **Newline**, and type a command followed by Enter. It works whenever no test is open on the touchscreen (on the setup screen).

| Type | What happens |
|---|---|
| `S1 -5.0` | S1 goes to its **start** position shifted by 5.0° (a trim of -5.0). The number is a **small shift from the normal start, not an angle**: 0 is the normal start, and on a 270° servo it can only go from about -45 to +45. It is exactly the number that goes in `SERVO_START_TRIM_DEG`. A trailing `f`, as in `-5.0f`, is fine, and so is lower case. |
| `S1 -5.0 end` | The same trim, but go to the **end** of the sweep. |
| `S1 start` / `S1 end` | Go to the start / end with the numbers typed so far. |
| `S1 sweep 177.5` | Say that the arm turned 177.5° when 180° was asked for, and go to the end of the corrected sweep. This is the number for `SERVO_MEASURED_SWEEP_DEG`. |
| `S1 off` | Stop sending a signal; the servo goes limp. |
| `S1 reset` | Back to the numbers in `Config.h`. |
| `show` | Print the numbers typed so far as two lines you can copy straight into `Config.h`. |
| `help` | The list of commands. |

`S1` can be `S1`, `S2`, `S3`, `S4` or `ALL` (for example `ALL start`, `ALL 0`, `ALL off`).

* The **first** command for a servo makes the arm jump straight to that position at the servo's own speed (it cannot glide from a limp arm). Later commands glide at `SERVO_SPEED_DEG_PER_SEC`.
* A number that would push a pulse outside 500 – 2500 µs is refused, with a message, and nothing moves.
* The typed numbers are kept in memory only. They **also apply to the next test you start**, so you can run a few cycles with them, until you reset or power-cycle the Mega. Copy them into `Config.h` and upload to make them permanent.
* Commands are refused, with a message, while a test is open on the touchscreen (tap SETUP, or NEW TEST, first). Set `SERIAL_COMMANDS = false` in `Config.h` to switch them off.

**Procedure, one servo at a time:**

1. Setup screen showing, no test open. Type `S1 0` and look at where the arm sits. This is the start position with no trim.
2. Type `S1 -3.5`, `S1 1.25`, … until the start is exactly where you want it (measure with the fixture, a square or a protractor). That number is this servo's `SERVO_START_TRIM_DEG`.
3. Type `S1 sweep 180` and measure how far the arm turns from the start to the end. Call that angle A (say 177.5°). Type `S1 sweep 177.5`: the sweep is now corrected and the arm should turn exactly 180°. Check it. A is this servo's `SERVO_MEASURED_SWEEP_DEG`.
4. Repeat for the other servos, then type `show` and copy its two lines over the two lines in `Config.h`. Upload once.

**Prefer to work in microseconds?** Use `tools/ServoRangeTest` to jog a servo to exactly where you want its start and end, then type the two numbers it reports straight into that servo's entry of `SERVO_START_US[]` and `SERVO_END_US[]` (just below the two settings above in `Config.h`). Those entries then replace the trim for that servo. Swapping a servo's start and end numbers makes it sweep the other way. (If you type a degree command for that servo afterwards, the typed numbers take over until you use `S1 reset`.)

### How fast

| Setting | What it does |
|---|---|
| `SERVO_SPEED_DEG_PER_SEC` | **The one speed for every servo**, in whole degrees per second (5 – 400). One 180° sweep takes `180 / speed` seconds: 45 → 4 s, 90 → 2 s, 180 → 1 s. |
| `DWELL_AT_END_MS`, `DWELL_AT_START_MS` | How long the arm rests at each end before reversing, in milliseconds. |
| `SERVO_EASING` | How a sweep speeds up and slows down: `EASING_SINE` (gentlest, default), `EASING_CUBIC`, `EASING_QUADRATIC`, or `EASING_LINEAR` (constant speed, abrupt starts and stops). With sine easing the peak speed is about 1.57 × the setting. |

One cycle is two sweeps plus the two rests, so to hit a wanted cycle time:

```
speed = 360 / (cycle time in seconds - the two rests in seconds)
```

For example a 3 s cycle with 250 ms rests: `360 / (3 - 0.5) = 144`. The setup screen shows the estimated time for the whole test using your current numbers. The servo itself tops out at roughly 430°/s unloaded at 7.4 V, so stay well below that with a real load.

## 6. All settings (`CycleTester/Config.h`)

| Setting | Default | Meaning |
|---|---|---|
| `SERVO_SPEED_DEG_PER_SEC` | 90 | **The universal speed.** One 180° sweep takes `180 / speed` seconds. Allowed 5 – 400. |
| `SERVO_EASING` | `EASING_SINE` | Ramp up and down. With sine easing the peak speed is ~1.57× the setting. `EASING_LINEAR` = constant speed. |
| `DWELL_AT_END_MS` / `DWELL_AT_START_MS` | 250 / 250 | Rest at each end before reversing. |
| `DONE_SETTLE_MS`, `ATTACH_SETTLE_MS` | 500, 600 | Settling time before a finished servo is released, and at the start position before the first sweep. |
| `START_STAGGER_MS` | 250 | Gap between servos switching on (0 = all together). |
| `DETACH_WHEN_DONE` | true | Release a servo when it has finished. |
| `SERVO_FULL_TRAVEL_DEG`, `SWEEP_START_OFFSET_DEG` | 270, 45 | Servo travel (180 or 270) and where the sweep begins, see section 5. |
| `SERVO_START_TRIM_DEG[]`, `SERVO_MEASURED_SWEEP_DEG[]` | 0, 180 | Per-servo fine tuning in degrees, see section 5. |
| `SERVO_START_US[]`, `SERVO_END_US[]` | derived | The resulting pulse widths. Advanced: replace an entry with a plain number to set it in µs. |
| `DEFAULT_TARGET_CYCLES`, `MAX_TARGET_CYCLES`, `STEP_SMALL/LARGE` | 1000, 999900, 100/1000 | Setup screen. |
| `TOUCH_POLL_MS`, `TOUCH_THRESHOLD`, `REPEAT_*` | 25, 40, … | Touch behaviour. |
| `SERIAL_LOG`, `SERIAL_COMMANDS`, `TOUCH_DEBUG` | true, true, false | Serial Monitor output at 115200 baud, typed position commands (section 5), and touch debugging. |

With the defaults one cycle takes 4.5 s (2 × 2 s sweeps + 2 × 0.25 s rests).

## 7. Customising the screens later

Everything visual is in `CycleTester/Ui.cpp`; the cycling logic never touches the screen, so you can restyle freely.

* The section **"LOOK AND LAYOUT"** at the top holds every colour, size and position.
* Add a button: add an entry to the `Action` enum, draw it (`drawButton`), return it from `hitTest()`, and handle it in `perform()`.
* Text is drawn by `drawText` / `drawField` using the font in `Font5x7.h` (any size by scale). `drawField` repaints only the characters that changed.
* Preview your changes without hardware: `cd tests/host && make pictures` writes PNGs of every screen to `tests/host/out/`.

## 8. Design notes (why it is built this way)

* **Servo motion runs from a timer interrupt** (ServoEasing). The display is slow (about 7 µs per pixel, ~4 ms per character), so a screen update can hold up `loop()` for tens of milliseconds. If motion depended on `loop()`, the arms would stutter every time a counter changed. With the interrupt they do not. The screen repaints one out-of-date item per pass to keep those pauses short (longest ≈ 90 ms in simulation).
* **Each servo is attached exactly once per test.** ServoEasing's `attach()` takes a new slot in its table on every call, even for an already-attached servo, which eventually stops servos responding. The code never attaches a servo twice.
* **Touch is polled every 25 ms, with one I2C read.** Constant I2C traffic delays the servo timer interrupt and shows up as jitter (your earlier sketch hit this). The Adafruit touch library does not check whether an I2C read succeeded, so a failed read can return random data; to be safe a press must be seen on two polls in a row, and a release needs two empty polls.
* **Text is drawn by the sketch's own small routine**, not `Print_String()`, which (as called in your earlier sketch) builds a heap-allocated `String` on every call. The sketch allocates no memory while running, which matters for multi-day runs.
* **A servo that fails to attach reports FAULT** instead of silently counting cycles it never made.
* **Pulses above 2476 µs used to be silently clipped.** ServoEasing attaches the Servo library with its default 400–3500 µs range, but the AVR Servo library stores that range in signed 8-bit numbers that overflow, so the effective maximum became 2476 µs. With 180° servo settings the 2500 µs end pulse was really sent as 2476 µs. `Config.h` now sets `MINIMUM_PULSE_WIDTH 400` / `MAXIMUM_PULSE_WIDTH 2600` before ServoEasing is included, which keeps the range within 8-bit limits. The PC simulation reproduces the real clip (`make test180` fails if the fix is removed). Your 270° settings (833 – 2167 µs) were never affected.

## 9. What has and has not been verified

Verified in this repository (no hardware available):

* Compiles for the Arduino Mega 2560 with the AVR compiler (`-Wall -Wextra`, no warnings from this code). Flash ≈ 42.1 KB (16 %), RAM ≈ 3.2 KB (39 %). The AVR compiler also produces exactly the pulse widths the simulation expects for a set of trim / measured-sweep values.
* `tests/host` runs the real sketch code against a simulated clock, servo library, display and touch panel. It checks cycle timing (4.5 s), counts reaching exactly the target, pause/resume with no position jump, PAUSE ALL / RESUME ALL, per-servo OFF, start stagger, abort and homing, no double-attach, `millis()` rollover during a run, hold-to-repeat, phantom touches, the 270° configuration, and the typed position commands (parsing, glide speed, refusals, switching off, a test taking over from typed positions). 171 checks pass.
  * `make test180` repeats the pulse-range checks for a 180° servo (21 checks). Its 2500 µs end pulse would be clipped to 2476 µs by the Servo library without the fix in `Config.h`, so it also guards that fix.
  * `make testtrim` sets non-zero `SERVO_START_TRIM_DEG` and `SERVO_MEASURED_SWEEP_DEG` values and checks the resulting pulse widths against numbers worked out separately, that the start moves by the trim, and that a servo whose measured sweep was corrected turns a true 180° (31 checks).
  * `make testguards` checks that out-of-range trims or measured sweeps are refused at compile time with a readable message (5 cases).

**Not** verified, because they need your hardware:

* Where your servos' real end stops are, and how accurately a trim in degrees lands on your arm: the sketch works from a nominal scale (about 7.4 µs per degree on a 270° servo) and `SERVO_MEASURED_SWEEP_DEG` corrects it once you have measured.
* The real display's drawing speed, colours, and touch alignment. The drawing speed in the simulation comes from reading the display library's code.
* Servo current draw and your power supply under load.
* The vendor `LCDWIKI_SPI` library itself. It was compiled against the public GitHub copy with `ST7796S` defined, because that copy does not support your display.

I could not open the Amazon listing for the servo from the development environment, so the servo facts above come from search results for the ANNIMOS 35 KG family
([listing](https://www.amazon.com/ANNIMOS-Steering-Digital-Stainless-Waterproof/dp/B0C69KF77Q)) and the generic
[DS3235 datasheet](https://hajim.rochester.edu/me/sites/kelley/me240/DS3235-270_datasheet.pdf) (500 – 2500 µs, 2 µs dead band, 50 – 330 Hz). Check them against your servo's box.

## 10. Troubleshooting

| Symptom | Try |
|---|---|
| "Touch controller not found" | Reseat the CTP_INT wire (pin 2), then check SDA/SCL and power. The sketch keeps retrying and carries on once it finds the controller. |
| Touches land in the wrong place | Set `TOUCH_DEBUG = true`, open the Serial Monitor, and compare the printed screen coordinates with where you touched. The mapping is in `readTouch()` in `Ui.cpp`. |
| Blank or white screen | Wrong `LCDWIKI_SPI` version (needs `ST7796S` support), or the LED pin is not on 5 V. |
| Arm travels 120° instead of 180° (or hits the end stop) | `SERVO_FULL_TRAVEL_DEG` does not match the servo (270 or 180). If the sweep is slightly off, use `SERVO_MEASURED_SWEEP_DEG` (section 5). |
| Servo hums at an end stop | The sweep reaches the end of the servo's mechanical travel. Move it away from that end with `SERVO_START_TRIM_DEG` (a few degrees), or check `SERVO_FULL_TRAVEL_DEG` and `SWEEP_START_OFFSET_DEG`. |
| Mega resets when servos start | Supply sagging: separate supply for the Mega, bigger capacitor, check wire gauge, raise `START_STAGGER_MS`. |
| Small jitter while holding | A digital servo with a 2 – 3 µs dead band reacts to tiny pulse-width changes; check supply decoupling. Each touch read briefly occupies the I2C interrupt, which can nudge the servo pulses by a few µs; raise `TOUCH_POLL_MS` to 40 – 50 to reduce it. Servos are released when finished. |
| Compile error about `ST7796S` | You have the public GitHub `LCDWIKI_SPI`. Use the copy from the Hosyond / LCDWIKI download. |

## 11. Running the PC simulation

```
cd tests/host
make            # runs all checks (needs g++)
make pictures   # also saves PNGs of the screens in out/ (needs Python 3 + Pillow)
make test180    # same, with the 180° servo settings
make testtrim   # same, with example fine-tuning numbers in Config.h
make testguards # out-of-range trims must be refused at compile time
```

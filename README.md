# sega-psg-core

Instance-owned native SN76489 synthesis extracted from Genesis and SMS/Game Gear
runners. Link `sega-psg::core` and include `sega_psg.h`. Each instance owns all
chip state, scratch queues, audition state and mutable derived lookup tables.
No process-global CPU/device state, SDL, guest execution or game data is needed.

## Profiles and clocks

Create `SEGA_PSG_GENESIS` with an input master clock such as 53693175 Hz. Advance
in master cycles; one mono frame is synthesized per 240 cycles. Create
`SEGA_PSG_SMS_GG` with a Z80 clock such as 3579545 Hz; one interleaved stereo frame
is synthesized per 16 T-states. `sega_psg_sample_rate` retains the donors' integer
rate convention. Supply a different input clock for another clock standard;
this does not claim full console PAL qualification.

The profiles deliberately preserve the existing donors' different noise clock
behavior. Genesis has an additional divide-by-two noise flip-flop; SMS/GG's
accepted implementation shifts its LFSR at every noise-counter edge. Both use
16-bit feedback with white-noise taps 0/3 and preserve current attenuation and
low-pass arithmetic. This extraction establishes donor parity, not a new
hardware-accuracy verdict on the difference.

Write latch/data bytes with `sega_psg_write`. SMS/GG also supports the GG routing
register through `sega_psg_write_stereo`; its default 0xff sends every channel to
both outputs. Genesis returns unsupported for that operation. Genesis exposes
its existing canonical-independent audition controls and filtered/raw audition
peek; SMS/GG returns unsupported for audition.

Call `sega_psg_advance`, then drain up to `sega_psg_available` frames with
`sega_psg_render`. Capacity remains 8192 frames: additional synthesis is dropped
when the undrained scratch is full, preserving donor behavior. Consumers must
advance/drain at appropriate existing boundaries. There are no new background
threads, per-sample dispatch callbacks or implicit device synchronization.
Serialize calls on an instance; different instances share no mutable state and
may be owned by different threads.

Snapshots use current-build donor-compatible FILE data. Take them after draining
PCM: load discards queued PCM as in the donor. Invalid loads are checked in a
private candidate context before publication. Native game saves are unrelated;
no cross-build snapshot compatibility or conversion is promised. Snapshot FILEs
remain owned by the caller.

## Build and validation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --timeout 0
```

The standalone test compiles byte-exact frozen donor sources independently and
compares generated samples and serialized state. It covers both profiles, all
noise controls and attenuation levels, latch/data bytes, zero/small tone periods,
cycle remainders, stereo routing, Genesis audition, scratch saturation, reset,
state roundtrips, malformed transactional loads and independent instances.
Optimized Genesis silent spans and SMS/GG event spans remain active.

The extracted core shares register handling/reload/attenuation code. Remaining
profile render paths are intentionally separate because their noise, stereo and
audition contracts differ. Production consumer pins, actual gameplay, speaker
quality and built-game performance remain integration-stage work.

## Provenance and licensing

`PROVENANCE.json` pins both source revisions and frozen source hashes. The source
is licensed under PolyForm Noncommercial 1.0.0; donor additional permission grants
are retained under `licenses/`. No retail images, audio captures, firmware or
generated guest source are included.

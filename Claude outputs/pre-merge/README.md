# CoreRay

A native **C++ / raylib** indoor-cycling app for the **Wahoo KICKR CORE** (and any Bluetooth **FTMS** smart trainer) plus a heart-rate strap or watch.

CoreRay is a sibling of CoreKick, rebuilt as one native binary. The core is a faithful C++ port of CoreKick's Rust code: physics, workouts, FTP tests, the simulator, the FIT encoder, FTMS control, routes (generator, builder, GPX, in-ride sectors), ride history and stats, automatic FTP, recurring events, weather and Strava. It runs the same unit tests, and its database uses CoreKick's schema, so your whole CoreKick history imports in one click. On top of that, CoreRay has its own HDR 3D renderer with streaming terrain, live weather, day/night with lanterns, and wildlife, plus a CoreKick-style 2D dashboard.

---

## Build & run (macOS)

Prerequisites: Xcode Command Line Tools (`xcode-select --install`) and CMake (`brew install cmake`). SQLite and `curl` ship with macOS.

```bash
cd ~/CoreRay
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/CoreRay
```

The first configure downloads raylib 5.5 and SimpleBLE 1.4.0 (about a minute). After that, rebuilds are incremental.

**Bluetooth permission.** macOS asks the first time CoreRay scans. CoreRay is a plain executable with an embedded `Info.plist`, so the request is made for the app you launched it from (Terminal, iTerm, VS Code). If scanning finds nothing, allow that app under *System Settings → Privacy & Security → Bluetooth*.

**No bike handy?** Go to *Devices* and turn on **Use the simulator**, or run `./build/CoreRay --sim`.

Tests:

```bash
ctest --test-dir build --output-on-failure    # 1,000+ checks ported from CoreKick's Rust tests, FTMS parsing, database + import
```

## What's in it

### Home

The sidebar has Home, Ride, Routes, Workouts, Events, Activities, Stats, Devices and Profile. **Home** is a dashboard:
- This week's hours against your weekly goal, rides, distance and TSS.
- **Form** (TSB) with fitness and fatigue, and a six-week form sparkline.
- FTP and W/kg, with your current **eFTP** and its confidence.
- Your week streak.
- A **suggested session** picked from your form (recover, endurance, sweet spot, threshold or VO2), one click to ride it.
- **Upcoming events** this week, with a Start button on the day.
- 12-week volume, and your recent rides.
- A banner when your recent rides say your FTP has moved (Set FTP / Dismiss).

### Ride modes (Ride)

- **Route.** Pick any route from your library. Loops can be ridden for 1–20 laps. The trainer gets the real gradient, scaled by your difficulty setting, with lookahead and smoothing. Ride against a **ghost** of your fastest completed ride of that route and lap count.
- **Workout.** Any of the **31 built-in sessions** ported from CoreKick, or your own. Ride them in ERG or in resistance mode.
- **Test.** Ramp (auto-detects failure, FTP = 75 % of your best minute), 20-minute (95 %), or 2 × 8-minute (90 %). The summary offers to apply the new FTP.
- **Free ride.** Choose gradient (SIM), fixed power (ERG), resistance level, or **heart-rate-controlled** ERG. Optionally set a goal: time, distance, kJ, kcal or climbing.
- Workouts, tests and free rides use an **endless road** as scenery, or any loop from your library.

### Routes

- **Library.** Six built-in routes plus your own, filtered by favourites, loops, point-to-point, GPX and your own. Each shows distance, climbing, max grade, difficulty, estimated time, the profile, every categorised climb, and **your rides on it with the PB highlighted**.
- **Generator.** CoreKick's seeded generator: terrain (flat → alpine), distance, climbing, max gradient, smoothness, start elevation, and finish anywhere / back at the start / on a summit. Shuffle until you like it, then save.
- **Builder.** Type segments, one per line (`3km 6%`, `800m -4%`), with a live profile.
- **GPX import.** Drop a `.gpx` file onto the window. Tracks and routes are both read, elevations smoothed, loops detected.
- **Show in 3D** puts the route in the background world.

### Events

Recurring rides: a route (with laps), a workout or an FTP test, once, daily, weekly on chosen days (every N weeks), or every N days, with an optional end date and a colour. Events appear on Home on the day with a Start button; rides started from an event are linked to it. Each event shows its **streak** and how many of the last 8 weeks' sessions you did, and a month **calendar** shows events (ticked when done) and your rides.

### Workouts

The library can be filtered by category and sorted by duration or TSS. Each workout shows TSS, IF, NP, work and time in zone at your FTP.

- **Editor.** Uses CoreKick's text format with a live preview:
  ```
  10m 50-75%                   ramp
  20m @90% 85rpm               steady with a cadence target
  5x 1m @120% / 1m @50%        intervals
  3x (30s @150%, 30s @50%)     repeat set (nestable)
  2m free · 30s effort @2%     rider-controlled
  1:30 @250w · 1h @65%         mm:ss, hours, absolute watts
  ```
- **Import.** Drag `.zwo`, `.erg`, `.mrc` or `.txt` files onto the window.
- **Export.** Save any workout as `.zwo`.
- **Your own workouts** live in `~/.coreray/workouts/*.txt` (plain text, easy to edit).

### Riding

- 3-second power with zone and W/kg, heart rate with live zone, cadence, speed, mode chip (SIM % / ERG W / level / HR→W / gear).
- Workout step panel: target, time left, next step, cadence target, a compliance bar and 3-2-1 beeps.
- Current or next climb (category, distance to the top, metres left to climb), goal progress, ghost gap, laps.
- Next 2 km profile with gradients, the full route profile, and a rolling 5-minute power / HR / cadence chart.
- **V** switches between the 3D world and a full **2D dashboard** in CoreKick's style.
- **E** opens the edit drawer:
  - **Intervals:** lengthen or shorten the step, nudge its target or the overall intensity, add one more rep, skip, skip to cool-down, switch ERG / resistance, end the workout.
  - **Ride:** free-ride mode, goal, difficulty, laps, gears.
  - **Road ahead** (or press **R**): splice a climb, descent, flat, rollers, a sprint or a stretch of terrain into the road in front of you: length, gradient, how far ahead, whether to return to the original height afterwards, and whether to time it. The 3D road is re-laid ahead of you (the road under your wheels never moves) and the trainer follows the new gradient. A banner counts down to it, times it, and it appears in your splits. **Backspace** undoes.
  - **Insert:** add interval sets to any ride in the text format.
- **Heart-rate alerts:** a tone when you leave your zone range, with a warm-up grace period.
- **Virtual shifting** for single-speed setups (KICKR CORE + Zwift Cog): 24 gears, using CoreKick's cadence × gear → speed → ERG-watts model.

| Key | Action |
| --- | --- |
| `Space` | Pause / resume |
| `L` | Lap |
| `E` | Edit drawer |
| `R` | Change the road ahead |
| `Backspace` | Undo the last road change |
| `↑` / `↓` | Nudge target (intensity, watts, gradient, level, HR or difficulty) |
| `→` | Skip to the next workout step |
| `[` `]` / PgUp PgDn | Virtual gears |
| `H` | Heart-rate alerts on / off |
| `V` | 2D dashboard / 3D world |
| `C` | Camera: chase, close, side, drone, rider, cinematic |
| `T` | Time of day: morning, midday, golden hour, dusk, night (where the sun really is today at your location), then real time, then a day/night cycle |
| `W` | Weather: changing, live, then each fixed condition |
| `M` | Sound on / off |
| `D` | Simulator test-data panel |
| `Shift+↑/↓`, `Alt+↑/↓` | Simulator power ±10 W, heart rate ±2 bpm |
| `Tab` | Hide the HUD |
| `?` | Help |
| `Esc` | Menu (resume, 2D/3D, end & save, discard) |

### After the ride, Activities and Stats

Every ride is saved to a SQLite database (`~/.coreray/coreray.db`) with its full 1-second data. The summary (and any ride in **Activities**) shows time, distance, climbing, average / NP / max power, IF, VI, TSS, kJ, kcal (from work, and from heart rate), heart rate, cadence and W/kg. It also shows time in power zones and heart-rate zones, power bests (1 s → 90 min), laps and route-lap splits, and power / HR / cadence / elevation / speed charts.

Rides are also written to `~/CoreRay Rides/` as:
- **`.fit`**: a Virtual Ride, encoder ported from CoreKick's (validated against Garmin's SDK there). Upload it to Strava, Garmin Connect, TrainingPeaks or intervals.icu.
- **`.tcx`** and **`.csv`**.

**Activities** lists every ride by month. Open one to rename it, add notes, re-export it, upload it to Strava, ride the route again, or delete it.

**Stats** shows this week, last week, this month, this year and all time; **fitness, fatigue and form** (CTL 42-day / ATL 7-day / TSB) over 6 weeks, 3 months or a year; 16 weeks of volume against your goal; your **power curve** (all-time vs 90 days, in W or W/kg); time in zones over 28 days; your records; and your FTP history.

**Automatic FTP.** After each real ride (and once a day) CoreRay estimates your FTP from the last 42 days: a critical-power fit to your best 3, 5, 8, 10 and 20-minute efforts when it fits well, otherwise 95 % of your best 20 minutes. If it has moved, Home and the ride summary offer to update it; or set Profile → *Raise automatically*. Simulator rides never count.

**Strava.** Profile → Connections walks you through creating your free Strava API app (callback domain `127.0.0.1`), then **Connect** opens Strava in your browser. Rides upload as Virtual Rides (trainer), with an optional auto-upload for every real ride. Duplicates are detected and linked rather than uploaded twice.

### Devices

Scan, then pick **Use as trainer** / **Use as HRM**. Both auto-reconnect on the next launch. The Bluetooth layer follows CoreKick's `ble.rs`:
- A supervisor thread per device.
- FTMS *Request Control → Start* handshake.
- **Serialised control-point writes**: one outstanding request, 2 s timeout.
- Coalescing, so only the newest SIM/ERG target is sent.
- Re-request on *control not permitted* or *control permission lost*.
- Reconnect with back-off, re-sending the current target.
- Heart rate falls back to the trainer's own HR field if no strap is connected.

The **simulator** is CoreKick's test mode:
- A simulated rider shaped by ERG / grade / level, or fixed watts.
- Patterns: steady, surges, wave, fade.
- Heart rate: modelled, fixed or off; cadence auto or fixed.
- A noise level, and a trainer-dropout switch.

### Profile

- FTP, weight, bike weight, age, sex.
- HR max / rest, and zone model (% max, reserve, or Friel's 7 LTHR zones).
- HR alerts.
- CdA from riding position, Crr from road surface, trainer difficulty and gradient clamps, gross efficiency.
- Virtual shifting, and graphics quality.
- A live "250 W = x km/h flat / y km/h up 6 %" readout.
- Weekly hours goal, and automatic FTP (suggest / raise automatically / off).
- **Connections:** Strava, and **Import from CoreKick**, which reads `~/Library/Application Support/com.corekick.app/corekick.db` (read-only) and brings across your profile, custom workouts, every ride with its samples, routes, events, FTP history and Strava keys. Running it again only adds what's new.
- **World:** your location (searchable, used for the real sun and moon and live weather), time of day (real time / fixed / day-night cycle), weather (changing / live / fixed), graphics quality, wildlife, lanterns, sound.

## The 3D engine

- **HDR pipeline.** Scene rendered to an RGBA16F target → soft-knee bright pass → two-scale Gaussian bloom → screen-space **sun shafts** → ACES tone mapping → FXAA.
- **Cascaded shadow maps.** Two cascades, up to 4096², texel-snapped so they don't shimmer, with 12-tap rotated Poisson PCF and normal-offset bias.
- **Streaming world.** Any route, of any length, is laid out as a road: loops become a closed, star-shaped circuit, point-to-point routes a meandering road that never crosses itself. The land around it is generated in 512 m chunks at four levels of detail on background threads as you ride, out to 7.5 km, with skirts hiding the seams.
- **Real sky.** The sun and moon are where they really are for your location and time, and the moon shows its real phase. A continuous palette runs from midday through golden hour, sunset and dusk to a moonlit night with stars and a faint Milky Way. Drifting self-shadowed clouds.
- **Weather.** CoreKick's fifteen conditions (clear → blizzard), blended smoothly: cloud cover, darkness, fog, rain streaks, snowflakes and hail, wind in the trees, lightning bolts that light the land, with thunder arriving later. Roads get wet with puddles that reflect the sky and dry out afterwards; snow settles on the fields, trees and verges (but not the wheel tracks) and melts. Sunshowers can leave a rainbow. Above the snow line, rain turns to snow.
- **Night.** Victorian swan-neck lanterns line the road and flicker on as you approach. They are real spotlights (the nearest 15) throwing pools of warm light onto the road and verge, with soft light shafts that thicken in rain, snow and fog, and glowing halos. Your bike's headlight is a spotlight down the road ahead.
- **Rain and snow.** Rain falls as velocity-aligned streaks, lit by the sky, lightning and nearby lanterns, splashing on the ground with ripples in the puddles; distant rain falls in sheets, and drops gather on the lens in the chase views. Snow falls as soft fluttering flakes, with big out-of-focus flakes drifting past the camera and a distant layer beyond.
- **Wildlife.** Sheep (curly wool, dark faces), cows (Holstein patches, red or cream herds, horns, udders), horses (with manes and tails) and deer (pale belly and rump) are smooth lofted models with articulated two-segment legs: they graze (head down, chewing, ears flicking, tails swishing), wander, trot, and deer bound away when you get close. Flocks of birds wheel overhead and a bird of prey circles high above. Birds sing by day, crickets at night.
- **Sound.** All synthesised: rain, wind that rises and falls, birdsong, crickets, thunder, plus the chimes and alerts.
- **Atmosphere.** Height-attenuated aerial-perspective fog tinted towards the sun.
- **Textures, generated at start-up** (no image files): a grass-blade texture, asphalt with aggregate, gravel pebbles, bedded rock (strata, joints, lichen, iron staining), river sand / silt with current ripples, bare eroded soil, a leaf atlas (broad leaves, small birch leaves, spruce sprays, pine needle tufts) and soft sprites for rain, snow, splashes and glows.
- **Terrain generation** (`src/Terrain`), built rivers-first and endless:
  - *Your route shapes the land.* The route's elevations are spread over the land, and every road section tilts its surroundings by its own gradient, so climbs traverse hillsides and descents run down valleys. Steeper routes get more rugged country, with mountain massifs rising away from the road.
  - *Rivers and valleys.* A jittered drainage lattice (180 m) drains the land: depressions fill to their spill point (a local priority flood) and drain over the pass, catchment is counted upstream, and each link becomes a meandering river whose channel, banks, floodplain and valley walls are sized by its catchment. The ground is built outwards from this valley network, so ridges form where neighbouring valley walls meet and there are no stray pits. Big enough catchments carry water; deep hollows hold lakes (one per hollow, with rivers flowing through them), and the outflow cuts a gorge through the rim. Rivers keep clear of the road and pass under it at right angles, under stone bridges.
  - *Particle erosion* after [SimpleHydrology](https://github.com/weigert/SimpleHydrology) (droplets with momentum and discharge maps, sediment transport and thermal cascading) on 512 m tiles with an apron. Droplets are seeded from world cells and the tiles are cross-faded with a partition of unity, so neighbouring tiles agree whichever order (or thread) they're built in: the world tiles itself forever and is the same every time you ride it. Erosion adds gullies, rills, fans and fresh sediment; the road formation and river banks are left as designed. It runs on the worker threads; near chunks appear straight away and gain the eroded surface a moment later.
- **Terrain texturing.** Height-blended layers driven by the erosion: grass (two scales, lush / dry / damp patches, blade relief), triplanar bedded rock on steep and scoured ground, bare soil, sand and gravel bars along streams and shores, silt and algae under water, the sheen of a film of water in small streams, snow on high ground; surface-gradient bump mapping up close.
- **Road.** Textured asphalt with aggregate, worn paint, cracks near the edges, repair patches, polished tyre tracks and oil drips. The edge crumbles into a gravel shoulder that blends into the grass verge (the verge strips sit on the terrain, so there's no seam). Wet roads darken, pool in the low spots and ripple with raindrops, and lanterns leave long shimmering reflections.
- **Grass.** Tens of thousands of instanced tufts of curved, tapered blades (with wildflowers in the meadows) around you, swaying in the wind with gust waves rolling across the field, brushed aside by the bike, mown short along the verge, flattened by snow.
- **Trees.** Six species built procedurally from real branching skeletons: oak, beech, birch (white bark, small drooping leaves), poplar, spruce (whorls of needle sprays) and Scots pine (orange upper bark, needle tufts), two variants each. Leaves are alpha-tested cards that cast dappled shadows, flutter in the wind and glow when backlit; species mix with altitude. Near trees are fully detailed; distant ones switch to a lighter version.
- **Water.** Rivers and lakes are real surfaces over a real bed. The opaque scene (colour and linear depth) is copied before the water is drawn, so the surface refracts the riverbed, absorbs light along the actual path through the water (Beer-Lambert, clearer in lakes, browner in fast or flooded rivers) and scatters light back. Caustics play on the bed, the banks are reflected by a screen-space ray march (falling back to the sky), ripples are dragged along by the current (flow-mapped), rain rings the surface, and foam gathers at the shallow edges and in rapids. The waterline fades in over a few centimetres, so shores don't show an edge.
- **The rider** is a to-scale (1.78 m) human: one mesh skinned to a 17-bone skeleton, posed every frame by IK: the balls of the feet on the pedals at the real crank angle (with ankling through the stroke), hands on the hoods or drops, torso from saddle to bars. Kit: jersey with side panels, bib shorts, socks, shoes, gloves, helmet and glasses.
- **Animated from your real data.**
  - The cranks turn at exactly your trainer's reported cadence (pedalling forward), and the wheels turn with your speed.
  - Out of the saddle on steep or hard efforts, with bike rock.
  - Tucks on fast descents and drops into the drops on hard flat efforts.
  - Pedals level when freewheeling, and leans into bends.
- **Ghost rider** for your personal best.
- **Cameras:** CoreKick's five, placed as CoreKick places them: **Chase**, **Cinematic** (six shots that cut every 16 s: chase, side, drone, a low shot ahead looking back, a high orbiting crane, and the rider close-up), **Side** (from the far verge), **Rider** (a close three-quarter view) and **Drone**, plus a **Close** chase and **Eyes**. The camera is smoothed relative to the rider so it never lags along the road, and widens a little at speed.
- **Courses:** every route in your library, including GPX imports and generated routes.

## Project layout

```
src/core/       pure C++ (no UI/BLE) – ported from CoreKick's corekick-core, unit-tested
  Physics       forces, air density, steady-state solver, momentum, virtual gears
  Metrics       NP / IF / TSS, bests, zones, calories (work + Keytel HR)
  Workout       blocks → steps, text format, ZWO/ERG/MRC, 31 built-ins, FTP tests
  Sim           virtual trainer + rider, test mode
  Engine        ride engine: SIM/ERG/level/HR control, workouts, tests, goals, laps, climbs, ghost, sectors
  Export        FIT / TCX / CSV
  Route         routes: generator, builder, GPX, climbs, stats, sectors (route.rs)
  Records       ride summaries, events, stats (CTL/ATL/TSB, curves, records), eFTP (model.rs, stats.rs, ...)
  Weather       15 conditions, timeline, live weather parsing, sun / moon astronomy
  Dates, Json   calendar maths, CoreKick-compatible JSON
src/Protocol    FTMS / HRS byte protocol
src/Ble         SimpleBLE supervisor (trainer + HRM)
src/Course      lays any route out as a road; road-anchored regional heights
src/Terrain     landform, drainage network, rivers, lakes, bridges, particle erosion
src/Renderer    frame, lighting, rider, shadows, post (Shaders.h holds the GLSL)
src/World       streaming terrain chunks, vegetation, road, lanterns
src/Fx          rain / snow / hail, lightning, wildlife, birds
src/Audio       chimes, alerts, synthesised ambience
src/Db          SQLite store with CoreKick's schema, CoreKick import
src/Net         HTTPS via curl, OAuth loopback, Strava, Open-Meteo
src/App + *Ui   app flow and pages (Home, Library, Events, History, Settings, Ride, Summary)
src/Store       ~/.coreray/profile.ini, custom workouts
tests/          core_tests + history_tests (ported from CoreKick), db_tests, protocol_tests,
                terrain_tests (determinism, order independence, seams, road integrity)
tools/          terrain_dump: shaded relief maps of the generated land (development)
```

## Notes

- **Strava uploads** need your own Strava API app (Strava's rule for personal tools). Newer Strava API apps may require a Strava subscription to upload; CoreRay tells you if Strava refuses.
- **Live weather and place search** use Open-Meteo (free, no key).
- Your old ghosts from CoreRay 0.1 aren't carried over: ghosts now come from your fastest completed ride of a route in the database (including imported CoreKick rides).

## Licences

- raylib: zlib.
- The droplet erosion in `src/Terrain.cpp` is adapted from SimpleHydrology by Nicholas McDonald (weigert): MIT.
- SimpleBLE 1.4: Business Source License 1.1. Free for personal / non-commercial use; check its terms before any commercial use.
- Barlow font: SIL OFL. DejaVu Sans (symbols): Bitstream Vera licence (see `assets/fonts/DejaVu-LICENSE.txt`).

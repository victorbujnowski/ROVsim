# ROVsim

Underwater ROV (Remotely Operated Vehicle) simulator built in **Unreal Engine 5.8 / C++**.
The vehicle is modelled after MBARI's work-class ROV **Doc Ricketts**, using the published specs
(mass, dimensions, thrust) to drive a 6-DOF hydrodynamic model. It can be flown locally (keyboard,
gamepad) or from outside over UDP/TCP, and it emits NMEA sensor data like a real vehicle.

## Controls

| Keyboard | Gamepad (BlueROV2 / ArduSub layout) | Action |
|---|---|---|
| `W` / `S` | right stick ↕ | Surge (forward / back) |
| `A` / `D` | right stick ↔ | Sway (left / right) |
| `E` / `Q` | left stick ↕ | Heave (up / down) |
| `←` / `→` | left stick ↔ | Yaw |
| Mouse / wheel | D-pad / RB, LB | Orbit or pan-tilt the camera / zoom |
| `C` | Y | Toggle camera: external (spring arm) / cockpit (pilot camera) |
| `M` | Menu | Toggle mode: **Simulator** / **Demo** |
| `L` | X | Lights on / off |
| `H` | L3 | Auto heading on / off |
| `J` | R3 | Auto depth on / off |
| `K` | | Auto altitude on / off |
| `P` | | Station keeping (position hold) on / off |
| hold `R` | hold B | Reset to the start position |

Gamepad sticks use a 15% dead zone and a quadratic response curve.

## Simulation

**Simulator mode**: full physics with real-world values.

| Model | Value |
|---|---|
| Mass | 4,763 kg (10,500 lb, MBARI) |
| Hull (collision box) | 3.66 x 1.83 x 2.13 m (12 x 6 x 7 ft, MBARI) |
| Buoyancy | Archimedes, ρ = 1025 kg/m³, V = 4.66 m³ → ~13 kg net positive buoyancy |
| Stability | Center of buoyancy 20 cm above CG → righting moment in pitch/roll |
| Drag | Per-axis linear + quadratic (diagonal Fossen model) on the **water-relative** velocity, projected areas 3.9 / 7.8 / 6.7 m² |
| Added mass | 0.4 / 0.7 / 0.9 of the mass (surge / sway / heave), 0.5 for inertia |
| Thrusters | Surge 650 kgf, heave 550 kgf (MBARI), 0.25 s first-order response, lose thrust out of the water |
| Current | Speed (knots) and direction it flows **toward** (0° = North); lateral force acts 30 cm aft of the CG, so a cross current yaws the bow into it (weathervane) |
| Swell | Linear (Airy) deep-water waves: height, period and direction it comes **from** (0° = North — the opposite of the current convention). λ = gT²/2π; the orbital water velocity a·ω·e^(−kz) is added to the current for drag, plus the Morison inertia force (Froude-Krylov + added mass) from the water acceleration, so a near-neutral ROV rides the orbits; the surface elevation η changes the submerged fraction near the surface. Height capped at λ/7 (breaking limit). Amplitude ramps up over 2 periods when switched on, to avoid a start-up transient |

Resulting top speeds: surge ≈ 1.6 m/s (3.2 kn), heave ≈ 1.2 m/s, yaw ≈ 29°/s — in line with a real
work-class ROV. A 3 kn current (1.54 m/s) leaves the vehicle at its limit.

**Demo mode**: arcade controls with no physics (8 m/s forward, 5 m/s lateral/vertical, 90°/s yaw),
level attitude and swept collision against the seabed.

**Marine snow**: a Niagara system (`/Game/ROV/FX/NS_MarineSnow`, ~10,000 slowly sinking flakes in a 24 × 24 × 14 m box) follows the active camera in world space, so flakes stream past as the ROV moves — the main visual cue of motion underwater. The asset is generated from the engine's HangingParticulates template by an editor commandlet:

```
UnrealEditor-Cmd.exe ROVsim.uproject -run=ROVCreateMarineSnow
```

## Autopilots

- **Auto heading**: PID controller, shortest-path error across 359°/0°. The integral (limited to 30%) cancels the weathervane torque when the vehicle is held against a cross current.
- **Auto depth / auto altitude** (mutually exclusive, both on the heave axis): PID with the integral
  limited to 10% of thrust — enough to hold the 13 kg positive buoyancy without windup. A wave filter (notch at
  the swell frequency) holds the mean depth and lets the vehicle ride the waves instead of chasing them.
- Pilot input moves the target (as in commercial ROVs and ArduSub); after release the autopilot
  brakes and latches the target where the vehicle stops. The HUD shows the phase:
  *ajustando → freando → segurando* (adjusting → braking → holding).
- **Station keeping** (position hold): one PID per world axis (North, East) with an integral that
  learns the current force; the requested force is rotated into the hull frame and split between
  surge and sway thrust. A **wave filter** (2nd-order notch at the swell frequency) removes the wave
  motion from the measured position and velocity, so the thrusters fight the current but let the
  vehicle ride the wave orbits instead of chasing every wave. Position reference is the simulator's
  ground truth (a real ROV would use USBL + DVL).
- Tested to ±2° heading and ±0.2 m depth/altitude with a 1 kn cross current.

## Network interfaces

| Port | Protocol | Direction | Content |
|---|---|---|---|
| 5000 | UDP, JSON | in | Pilot commands `{"type":"cmd","seq":n,"surge","sway","heave","yaw"}` with a 0.5 s watchdog; while commands arrive, local keyboard/gamepad piloting is blocked (camera stays free) |
| 5001 | UDP, JSON | out | Telemetry at 10 Hz (position, depth, altitude, attitude, velocities) |
| 5002 | TCP, text lines | in/out | Commands with replies: `PING`, `STATUS`, `MODE SIM\|DEMO`, `LIGHTS ON\|OFF`, `CAMERA EXT\|COCKPIT`, `RESET`, `CURRENT <kn> <deg>`, `SWELL <height_m> <period_s> <from_deg>` (`SWELL 0` = off), `AUTO HEADING\|DEPTH\|ALT ON\|OFF\|<value>`, `AUTO POS ON\|OFF\|<north_m> <east_m>`, `AUTO OFF` |

### Emulated sensors (NMEA 0183)

| Sensor | Sentence | Rate | Transport | Why |
|---|---|---|---|---|
| Gyrocompass | `$HEHDT` | 10 Hz | UDP 10001 | Fast, many readers, a lost sample is replaced 100 ms later |
| Motion sensor | `$PRDID` (Teledyne RDI) | 20 Hz | UDP 10002 | Fastest stream; latency matters more than loss |
| DVL | `$VDVBW` (water and ground speed) | 5 Hz | TCP 10010 | Navigation integrates velocity: every lost sample becomes position error |
| Depth sensor | `$PRSMD` (ROVsim proprietary) | 2 Hz | COM (fallback UDP 10003) | Classic serial instruments; legacy software reads COM ports |
| Altimeter | `$SDDPT` | 5 Hz | COM (fallback UDP 10004) | Same; both fit easily in 9600 baud |

The serial port defaults to `COM20` (8N1, 9600 baud, written from its own thread). While that port does not
exist, the serial sensors fall back to UDP — depth on **10003**, altimeter on **10004** — and the HUD shows
it. The serial path is implemented but **not yet exercised**: the com0com virtual pair driver fails to load
on Windows 11 with signature enforcement (Code 52), and disabling enforcement was not worth it; a real
USB-serial or serial-server port with a signed driver should work as is. Everything is configurable on the
`Sensors` component.

### Web panel (surface console in the browser)

`Tools/ROVWebPanel` is a small ASP.NET Core bridge (no third-party packages) that talks to the simulator
**only through the network interfaces above**, like a real topside console, and serves a dashboard over
WebSocket at 10 Hz:

```
cd Tools/ROVWebPanel
dotnet run            # then open http://localhost:8080   (options: --sim <host> --port <http port>)
```

- Navigation (compass with autopilot target, artificial horizon), velocities, thruster bars, autopilot
  phase and deviation, current and swell (with on/off switches), 60 s depth/heading charts
- Every NMEA sensor received by the bridge, with measured rate, checksum failures and decoded fields
- Command buttons/forms and a console for the TCP command protocol (replies shown in green/red)
- Piloting from the browser with the keyboard or a gamepad (Gamepad API, BlueROV2 layout), sent as UDP
  commands at 20 Hz — the simulator's 0.5 s watchdog stops the thrusters if the page is closed

The bridge binds UDP 5001 and 10001–10004, so the PowerShell monitors cannot run at the same time.

### Test tools (PowerShell, in `Tools/`)

| Script | Purpose |
|---|---|
| `rov_udp_client.ps1 -Surge 1 -Seconds 8` | Sends pilot commands over UDP |
| `rov_udp_monitor.ps1` | Shows the UDP telemetry, counts lost packets |
| `rov_tcp_console.ps1` | Interactive TCP command console |
| `rov_nmea_monitor.ps1 [-ComPort COM21]` | Listens to all sensor outputs, validates checksums, measures rates |
| `rov_net_compare.ps1 -HostLog .. -ObserverLog ..` | Aligns host and observer ROV traces (`-ROVNetTrace`): measured delay and remaining error |

## Multiplayer (work in progress)

Server-authoritative model: the pilot's machine is a **listen server** that runs the physics, autopilots
and all network/sensor interfaces. Anyone who connects afterwards joins as an **observer** (supervisor,
client, instructor): no pawn, the camera follows the server's ROV and the HUD shows the replicated
telemetry, environment and autopilot phases. Movement is replicated by the engine (30 Hz, physics
smoothing on the client); the rest travels in one `FROVNetState` struct, sent only when it changes.

```
UnrealEditor.exe ROVsim.uproject "/Game/Maps/Underwater1?listen" -game -windowed    # pilot (host)
UnrealEditor.exe ROVsim.uproject 127.0.0.1 -game -windowed                          # observer
```

**Observer camera**: free orbit around the ROV (mouse or right stick, wheel to zoom, follows the heading,
collides with the seabed); `C` / `Y` switches to the ROV's own video camera. It is local: the pilot's
view does not change.

**Network test** (one machine, a bad link emulated with the engine's packet simulation):

```
# host: smoke test + ROV position trace at 10 Hz
UnrealEditor.exe ROVsim.uproject "/Game/Maps/Underwater1?listen" -game -ROVSmokeTest -ROVNetTrace -ABSLOG=<dir>\NetHost.log
# observer: 150-250 ms delay and 5% loss on everything it receives
UnrealEditor.exe ROVsim.uproject 127.0.0.1 -game -ROVObserverTest=90 -ROVNetTrace -PktIncomingLagMin=150 -PktIncomingLagMax=250 -PktIncomingLoss=5 -ABSLOG=<dir>\NetClient.log
.\Tools\rov_net_compare.ps1 -HostLog <dir>\NetHost.log -ObserverLog <dir>\NetClient.log
```

`-ROVObserverTest[=seconds]` logs what the observer receives, checks smoothness (largest per-frame jump
beyond what the velocity explains, after a 3 s join convergence), takes a free-camera and a ROV-camera
screenshot and exits. `rov_net_compare.ps1` finds the time shift that best aligns both traces (the
measured delay) and the error left after aligning. Measured with the bad link: delay 210 ms, error after
alignment 3.3 cm RMS.

With physics (Simulator mode) the engine smooths the observer's copy. Without physics (Demo mode) it
would teleport the copy on every packet (up to 0.9 m jumps with packet loss), so the observer copy stores
the last received state and each frame extrapolates it with the replicated velocity, correcting the
remaining error gradually (dead reckoning).

## Code

| File | Purpose |
|---|---|
| `ROVPawn` | Vehicle: components, both control modes, physics, current, swell, autopilots, cameras, input |
| `ROVPid.h` | PID controller (saturation, anti-windup, derivative on measurement) |
| `ROVNotch.h` | Notch (band-stop) filter used as the wave filter (station keeping, auto depth/altitude) |
| `ROVsimTools` (editor module) | `ROVCreateMarineSnow` commandlet that builds the marine snow Niagara asset |
| `ROVNmea.h` | NMEA sentence builder and checksum validation |
| `ROVUdpLinkComponent` | UDP command input (own receive thread) and telemetry output |
| `ROVTcpCommandComponent` | TCP command server with line framing |
| `ROVSensorEmulatorComponent` | NMEA sensors over UDP, TCP and serial (own writer thread) |
| `ROVHUD` | Telemetry HUD in labelled sections with units and sign conventions |
| `ROVGameMode` | Spawns the ROV, sets the HUD, automated smoke test |
| `ROVPlayerController` | Pilot vs observer role; observer camera on the server's ROV; observer network test |
| `Tools/ROVWebPanel` | Web panel: C# bridge (UDP/TCP ↔ WebSocket) and the dashboard page |

## Tests

**Automation tests** (no rendering, a few seconds):

```
UnrealEditor-Cmd.exe ROVsim.uproject -ExecCmds="Automation RunTests ROVsim" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
```

- `ROVsim.PID`: saturation, P-only steady-state error, PID removes it
- `ROVsim.NMEA`: checksum against a reference sentence, validation of corrupted sentences
- `ROVsim.Notch`: wave filter removes a 10 s wave, passes slow drift and constants

**Smoke test** (runs the game ~8 minutes and exits): flies both modes and both cameras, checks seabed
collision, pitch/roll stability, current drift, the three autopilots, the swell (heave period,
e^(−kz) decay between ~34 m and ~8 m deep, no drift) and station keeping (1 kn cross current + 2 m swell at
15 m: mean offset, peak offset, thruster activity, heave not chasing waves, zero mean heading error), logs telemetry (`LogROV`)
and saves screenshots to `Saved/Screenshots/WindowsEditor/SmokeTest/`:

```
UnrealEditor.exe ROVsim.uproject /Game/Maps/Underwater1 -game -windowed -ResX=1280 -ResY=720 -ROVSmokeTest
```

## Credits

- "Doc Ricketts ROV" 3D model by **MBARI**, licensed under
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/):
  https://sketchfab.com/3d-models/doc-ricketts-rov-def365ad47894a06b5f0fb2876795bf9
- Vehicle specifications: [MBARI – ROV Doc Ricketts](https://www.mbari.org/technology/rov-doc-ricketts)
- Environment assets (sand, rocks, industrial pipes) from Fab / Quixel Megascans, under their respective Fab licenses.

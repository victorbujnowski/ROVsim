# ROVsim

Underwater ROV (Remotely Operated Vehicle) simulator built in **Unreal Engine 5.8 / C++**.
The vehicle is modelled after MBARI's work-class ROV **Doc Ricketts**, using the published specs
(mass, dimensions, thrust) to drive a 6-DOF hydrodynamic model.

## Controls

| Input | Action |
|---|---|
| `W` / `S` | Surge (forward / back) |
| `A` / `D` | Sway (left / right) |
| `E` / `Q` | Heave (up / down) |
| `←` / `→` | Yaw |
| Mouse | Orbit the external camera / pan-tilt the cockpit camera |
| Mouse wheel | Zoom (external camera) |
| `C` | Toggle camera: external (spring arm) / cockpit (pilot camera) |
| `M` | Toggle mode: **Simulator** / **Demo** |
| `L` | Lights on / off |

A gamepad is also mapped (left stick: surge/sway, bumpers: heave, triggers: yaw).

## Modes

**Simulator**: full physics with real-world values.

| Model | Value |
|---|---|
| Mass | 4,763 kg (10,500 lb, MBARI) |
| Hull (collision box) | 3.66 x 1.83 x 2.13 m (12 x 6 x 7 ft, MBARI) |
| Buoyancy | Archimedes, ρ = 1025 kg/m³, V = 4.66 m³ → ~13 kg net positive buoyancy |
| Stability | Center of buoyancy 20 cm above CG → righting moment in pitch/roll |
| Drag | Per-axis linear + quadratic (diagonal Fossen model), projected areas 3.9 / 7.8 / 6.7 m² |
| Added mass | 0.4 / 0.7 / 0.9 of the mass (surge / sway / heave), 0.5 for inertia |
| Thrusters | Surge 650 kgf, heave 550 kgf (MBARI), 0.25 s first-order response, lose thrust out of the water |

Resulting top speeds: surge ≈ 1.6 m/s (3.2 kn), heave ≈ 1.2 m/s, yaw ≈ 29°/s. These are in line with a
real work-class ROV.

**Demo**: arcade controls with no physics. Commands map directly to velocity (8 m/s forward,
5 m/s lateral/vertical, 90°/s yaw), the vehicle stays level, and swept collision keeps it from
going through the seabed.

## Cameras

- **External**: spring arm with mouse orbit, zoom, collision probe and position/rotation lag.
  It follows the ROV heading.
- **Cockpit**: the ROV's front video camera, with clamped pan/tilt like a real camera head.
  The lights are mounted high and to the sides to reduce backscatter.

The HUD shows depth, altitude (altimeter ray-cast), heading, pitch/roll, body-frame velocities,
thruster output and mass.

## Code

| File | Purpose |
|---|---|
| `Source/ROVsim/Public/ROVPawn.h` / `Private/ROVPawn.cpp` | Vehicle: components, both control modes, physics, cameras, input (Enhanced Input built in C++) |
| `Source/ROVsim/Public/ROVHUD.h` / `Private/ROVHUD.cpp` | Telemetry HUD |
| `Source/ROVsim/Public/ROVGameMode.h` / `Private/ROVGameMode.cpp` | Spawns the ROV at the PlayerStart, sets the HUD, automated smoke test |

## Automated smoke test

The smoke test flies the ROV through both modes and both cameras, logs telemetry (`LogROV`)
every second, checks seabed collision and pitch/roll stability, saves screenshots to
`Saved/Screenshots/WindowsEditor/SmokeTest/`, then exits:

```
UnrealEditor.exe ROVsim.uproject /Game/Maps/Underwater1 -game -windowed -ResX=1280 -ResY=720 -ROVSmokeTest
```

## Credits

- "Doc Ricketts ROV" 3D model by **MBARI**, licensed under
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/):
  https://sketchfab.com/3d-models/doc-ricketts-rov-def365ad47894a06b5f0fb2876795bf9
- Vehicle specifications: [MBARI – ROV Doc Ricketts](https://www.mbari.org/technology/rov-doc-ricketts)
- Environment assets (sand, rocks, industrial pipes) from Fab / Quixel Megascans, under their respective Fab licenses.

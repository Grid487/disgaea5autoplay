# Disgaea 5 Autoplay

A **Disgaea 5 Complete** mod for Windows that automates battles and can repeat a supported stage. Written in **C++17**, it loads as an x64 XInput proxy DLL beside the game executable.

**Current version: v5.4.2.** Combat autoplay and stage repeat are separate toggles, and both start disabled.

## Preview
https://www.youtube.com/watch?v=3MRZhXz_9wY

## Features

- Automatic deployment, movement, basic attacks, learned offensive skills, healing, and turn advancement.
- Team coordination that accounts for queued damage, healing, SP costs, and movement destinations.
- Up to five deployed units, including one affordable healer when available.
- Emergency healing, SP conservation, elemental evaluation, and safe area targeting.
- Movement toward useful attack or healing positions, including obstacle detours.
- Automatic dismissal of recognized single-OK battle and Stage Clear notifications.
- Independent stage repeat through the game's results and stage-selection menus.

## Controls

| Control | Action |
|---|---|
| **F1** | Toggle combat autoplay |
| **Back + R3** on an XInput controller | Toggle combat autoplay |
| **F2** | Toggle stage repeat independently of F1 |

The game window title shows the toggle states. Press a toggle again to disable its feature.

**F2 does not enable F1 or fight battles.** With only F2 enabled, you play the battle yourself and the mod handles supported repeat transitions after a verified win.

## Requirements and compatibility

- A legitimate Windows x64 installation of **Disgaea 5 Complete**; developed and tested with the Steam PC version.
- The supported game executable listed below. Native offsets are specific to this build; other builds and platforms are unverified.
- No separate mod loader is required.

Tested `disgaea5.exe` SHA-256:

```text
49A7530BAA1C3E15E4BF9C63C56FEFD71D7E51A6E641BCE80280B37B2DB282CE
```

Check your executable from PowerShell in the game folder:

```powershell
Get-FileHash .\disgaea5.exe -Algorithm SHA256
```

Other mods that use the same `XINPUT9_1_0.dll` filename conflict with this installation method. Proxy chaining is not implemented; do not overwrite another mod's DLL without preserving it.

## Installation

1. Close the game and back up your saves.
2. [Build it from source](#building-from-source).
3. Put the mod's `XINPUT9_1_0.dll` in the same folder as `disgaea5.exe`. Preserve any existing DLL with that name first.
4. Launch the game normally and enter a stage.
5. Press **F1** for combat autoplay. Optionally enable **F2** before the stage finishes to repeat it.

```text
Disgaea 5 Complete/
├── disgaea5.exe
└── XINPUT9_1_0.dll
```

The DLL loads at game startup, forwards normal XInput calls to Windows, and hooks native battle and menu routines. It reads battle state and submits commands through the game's own systems. It does not replace the game executable on disk. Restart the game after installing a different DLL.

## How combat autoplay decides

The planner evaluates available actions for the team using the current battle state and actions already accepted into the engine queue. It replans after deployment, accepted movement, execution, or relevant state changes.

| Decision | Policy |
|---|---|
| Emergency healing | Prioritize living allies at or below **25% projected HP**, execute the heal, then refresh the battle state. |
| Normal healing | Consider allies below **50% projected HP** when the cast would restore at least **20% of their maximum HP**. Account for queued healing. |
| Offensive skills | Compare expected damage, kills, accuracy, elemental resistance, affected targets, reach, and SP cost with basic attacks. Prefer skills that add a kill, hit multiple enemies usefully, reach an otherwise inaccessible target, or improve effective damage by at least **25%**. |
| SP reserves | Normally retain **20% maximum SP**. A healer retains at least the greater of that amount and its cheapest usable healing cost. Emergency healing and a predicted stage-finishing action may spend the reserve. |
| Coordination | Subtract queued damage and add queued healing before assigning more actions. Equivalent choices prefer lower SP cost, then stable unit and skill order. |
| Friendly fire | Reject offensive placements that would hit an ally or the caster. Skip skills with unsupported targeting or side effects. |
| Deployment | Preserve units already deployed. Fill available slots with one usable healer when available, then damage dealers ranked against the stage's enemies. |
| Positioning | Move toward legal positions that enable useful actions. Allow detours and prefer less exposed tiles among similarly effective choices. |

Basic attacks remain useful for inexpensive finishes. Only learned, supported skills are considered, using the player's existing skill power and area settings.

Immediately before confirmation, the mod checks the actor, cost, targets, and affected area again. An action counts only after its engine queue entry is observed. Rejected or stale actions are canceled and replanned, with a two-attempt limit. A unit with no useful action or movement finishes planning instead of repeatedly opening menus.

## Stage repeat with F2

1. Enter the stage you want to repeat normally.
2. Enable **F2** before finishing it. Enable **F1** too if you want automated combat.
3. After a verified victory, the mod advances supported results screens, finds the same native stage ID and variant in the returned stage list, confirms the expected entry prompt, and enters again.

Initial support covers **already-cleared normal stages that return directly to their stage list**. Item World, first clears, unsupported modes, and special stages requiring custom record or capacity choices are excluded.

Unknown dialogs, changed stage identity, missing stage rows, unverified outcomes, or exhausted retries stop repeat. Other choice dialogs require manual input.

## Performance

v5.4.2 reduces repeated memory checks and duplicate movement-goal work while preserving the tactical search and combat policy.

Measured planning times on Second Trial:

| Planning phase | Previous median | v5.4.2 median |
|---|---:|---:|
| Deployment | 5,030 ms | 17 ms |
| Combat action | 867 ms | 48 ms |
| Useful movement goals | 946 ms | 134 ms |

These measurements cover one battle per build with profiling enabled on both; enemy and action states can differ between runs. They exclude menu transitions and animations. Native path validation uses the same algorithm and measured about 88 ms median in the optimized run. Timings vary by stage, roster, and computer.

Planning is spread across updates with a shared soft 1 ms budget and bounded native queries. Individual engine calls cannot be interrupted, so this is not a guarantee that every frame or decision completes within 1 ms.

## Building from source

Requirements:

- Windows and PowerShell; the scripts work with Windows PowerShell 5.1.
- Visual Studio 2022 Build Tools with the **C++ x64 toolchain** and **Windows SDK**.
- The supported game installation.

The supplied scripts expect `mod_src/` directly inside the game folder, with `disgaea5.exe` one directory above it. Even a staged build records the executable hash. They currently use the standard Visual Studio 2022 **BuildTools** installation path; adjust the `vcvars64.bat` path in the scripts if your toolchain is elsewhere.

Run from PowerShell in the game folder:

```powershell
# Compile to a staging directory without installing.
powershell -ExecutionPolicy Bypass -File .\mod_src\build.ps1 -StageOnly

# With the game closed: compile, back up the existing DLL, and install.
powershell -ExecutionPolicy Bypass -File .\mod_src\build.ps1
```

Build output goes to `mod_src/build/<timestamp>/`. The manifest records source, executable, and DLL hashes. Installation verifies a backup of the previous DLL, replaces the installed DLL, and verifies its hash. It refuses installation while a Disgaea 5 process is detected. Failed compilation leaves the installed DLL unchanged.

Previous DLL backups are stored in `mod_src/backups/builds/`. The normal build does not enable the optional profiling or synthetic-notification development switches. 

## Limitations

- This is an experimental, reactive planner, without deep multi-turn simulation.
- Damage and healing are base estimates. Some additive, healing, and rare passive modifiers are not modeled.
- Unsupported skills are skipped; basic combat remains the fallback.
- Inventory items, buffs/debuffs, status cures, revival, lift/throw, and Geo strategies are not automated.
- Saved skill power and area settings are not changed.
- Only recognized notifications and verified repeat prompts are handled automatically.

## Troubleshooting

- **Nothing happens:** confirm the mod DLL is beside `disgaea5.exe`, restart the game, and check the toggle indicators in the window title.
- **Battles are not automated with F2 on:** enable F1 too. F2 handles repeat transitions independently.
- **A skill or dialog is skipped:** its targeting, effects, or menu contract may be unsupported. Handle it manually and retain the log for investigation.
- **Build succeeds but installation fails:** keep the staged DLL, close the game, and rerun the build script. Check that another process is not holding the DLL.
- **Unexpected behavior after a game update:** compare the executable hash with the supported build above.

The mod writes **`autoplay_mod.log`** in the game folder. It records initialization/version information, accepted decisions, expected effects, SP costs, planning timings, and transition failures. Review logs for personal paths before sharing them publicly.

## Uninstall

Close the game and remove this mod's `XINPUT9_1_0.dll`. If you replaced another proxy DLL, restore its preserved copy. The optional `autoplay_mod.log` can also be removed.

## Source layout

| Path | Purpose |
|---|---|
| `mod_src/xinput_proxy.cpp`, `mod_src/xinput9_1_0.def` | DLL entry points, XInput forwarding, hooks, and hotkeys |
| `mod_src/combat_planner.h`, `mod_src/team_planning.h` | Combat policy, snapshots, candidate scoring, and team planning |
| `mod_src/native_*.h` | Adapters to verified native game contracts |
| `mod_src/planning_budget.h`, `mod_src/tactical_path.h` | Bounded planning and tactical path support |
| `mod_src/stage_repeat.h` | Stage-repeat state machine |
| `mod_src/build.ps1` | Staged build and backup-preserving installation |
  
## Credits

Developed with **OpenAI Codex, using GPT-6-family agents**, with human direction and in-game feedback. Reverse-engineering and modding work used **Reverse Engineer Anything (REA)**, **Universal Modder**, and local analysis tools including Ghidra. Builds use Microsoft Visual Studio C++ Build Tools.

Disgaea 5 Complete and its content belong to their respective rights holders. This is an unofficial project and is not affiliated with or endorsed by the game's developers or publishers.

## License

The original mod code and documentation are licensed under the **MIT License**; see [LICENSE](LICENSE). MIT permits reuse, modification, and commercial redistribution while retaining the copyright and license notice. The software is provided without warranty.

This license does not grant rights to Disgaea 5 Complete, its executable, assets, or other third-party material.


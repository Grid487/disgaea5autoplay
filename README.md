# Disgaea 5 Autoplay

A **Disgaea 5 Complete** mod for Windows that automates battles and can repeat a supported stage. Written in **C++17**, it loads as an x64 XInput proxy DLL beside the game executable.

**Current version: v5.5.2.** Combat autoplay and stage repeat are separate toggles, and both start disabled. Custom party is optional and starts OFF.

## Preview

[Watch the gameplay preview](https://www.youtube.com/watch?v=3MRZhXz_9wY).

## Features

- Automatic deployment, movement, basic attacks, learned offensive skills, healing, and turn advancement.
- Team coordination that accounts for queued damage, healing, SP costs, and movement destinations.
- Automatic five-unit selection, including one affordable healer when available.
- Optional custom parties of **1–10 heroes**, deployed in your chosen order, with automatic reserve replacements.
- Native in-game party editor with persistent settings separate from game saves.
- Emergency healing, SP conservation, elemental evaluation, and safe area targeting.
- Movement toward useful attack or healing positions, including obstacle detours.
- Automatic dismissal of recognized single-OK battle, Stage Clear, and post-clear surrender notifications.
- Independent stage repeat through the game's results and stage-selection menus.

## Controls

| Control | Action |
|---|---|
| **F1** | Toggle combat autoplay |
| **Back + R3** on an XInput controller | Toggle combat autoplay |
| **F2** | Toggle stage repeat independently of F1 |
| **F3** or **Back + L3** on an XInput controller | Open/close Autoplay Party |

The game window title shows the toggle states. Press a toggle again to disable its feature.

**F2 does not enable F1 or fight battles.** With only F2 enabled, you play the battle yourself and the mod handles supported repeat transitions after a verified win.

## Custom party

Load a saved game, then press **F3** in the idle hub, a settled stage list, the pre-battle screen, or an idle player-turn cursor. The panel uses the game's configured direction, Confirm, and Cancel controls. It cannot open during animations, targeting, confirmations, or another modal.

1. Choose **Choose heroes**, or use Left/Right to switch to the roster. Confirm selects or removes a hero; numbered selections show deployment priority.
2. In **Change deployment order**, Confirm a selected hero to choose **Move Up**, **Move Down**, or **Remove**.
3. Turn **Custom party ON**, then choose **Apply and save**. An enabled party requires 1–10 distinct selections.

Cancel from the Options page or press F3 to discard edits. Turning Custom party OFF preserves the saved list and restores the automatic five-unit policy. F1/F2 commands and their timeouts pause while the panel is open; their requested toggle states resume when the closing input is released.

Changes during combat display **Applies next battle**. They do not change the current battle's party target. Changes before combat apply to the upcoming battle.

Available selected heroes deploy in priority order. Missing, fainted, or ineligible selections leave vacancies that the existing healer/offense ranking fills from eligible reserves. The selected count remains the target: a one-hero party stays a one-hero party when replacing a casualty. Manually deployed units count toward the target and are never withdrawn automatically. Native deployment restrictions still apply, including blocked Base Panels and remaining dispatch capacity. With no controllable hero and no legal deployment, F1 pauses with an explanation.

Native Base Panel group tabs do not limit the custom party. Autoplay resolves the actual filtered list records, temporarily reveals a hidden eligible selection for confirmation, and restores your original tab afterward. An unexpected hero being deployed pauses F1 and records the mismatch.

Preferences are saved to **`autoplay_party.ini`** beside the DLL, using persistent character serials and the identity of the loaded save file. Names and roster sorting do not affect selections. Each loaded save slot has its own profile; another slot starts OFF until configured. A replaced or moved save may require reselection. Invalid settings or ambiguous character identities disable Custom party. Party settings do not edit game saves, character names, or native eligibility flags. Keep this preferences file private and back it up if you want to preserve your selections.

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
3. The normal build command installs `XINPUT9_1_0.dll` beside `disgaea5.exe` automatically and backs up the previous DLL. If you used `-StageOnly`, copy the staged DLL there manually after preserving the existing DLL.
4. Launch the game and load a save. Optionally configure a custom party with **F3**, then enter a stage.
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
| Deployment | Preserve units already deployed. With Custom party OFF, fill five slots with one usable healer when available, then ranked damage dealers. With it ON, prefer the selected order and use automatic ranking for replacement vacancies. |
| Positioning | Move toward legal positions that enable useful actions. Allow detours and prefer less exposed tiles among similarly effective choices. |

Basic attacks remain useful for inexpensive finishes. Only learned, supported skills are considered, using the player's existing skill power and area settings.

Immediately before confirmation, the mod checks the actor, cost, targets, and affected area again. An action counts only after its engine queue entry is observed. Rejected or stale actions are canceled and replanned, with a two-attempt limit. A unit with no useful action or movement finishes planning instead of repeatedly opening menus.

## Stage repeat with F2

1. Enter the stage you want to repeat normally.
2. Enable **F2** before finishing it. Enable **F1** too if you want automated combat.
3. After a verified victory, the mod advances supported results screens, finds the same native stage ID and variant in the returned stage list, confirms the expected entry prompt, and enters again.

Initial support covers **already-cleared normal stages that return directly to their stage list**. Item World, first clears, unsupported modes, and special stages requiring custom record or capacity choices are excluded.

Unknown dialogs, changed stage identity, missing stage rows, unverified outcomes, or exhausted retries stop repeat. Other choice dialogs require manual input.

v5.5.2 handles surrender notices that appear after results advance to the post-clear waiting phase. F2 retains ownership across that transition, closes verified single-OK notices even with F1 OFF, and continues repeating. The navigation timeout and choice-dialog restrictions remain in place.

## Performance

v5.5.2 retains the planning improvements introduced in v5.4.2. The table below records those earlier live measurements; it is not a new v5.5.2 benchmark.

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

Build output goes to `mod_dev/build/<timestamp>/`, outside the source folder. The manifest records source, executable, and DLL hashes. Installation verifies a backup of the previous DLL, replaces the installed DLL, and verifies its hash. It refuses installation while a Disgaea 5 process is detected. Failed compilation leaves the installed DLL unchanged.

Previous DLL backups are stored in `mod_dev/backups/builds/`. The build script creates these output directories automatically. The normal build does not enable the optional profiling or synthetic-notification development switches.

## Validation

v5.5.2 passed **20 local offline suites**, retaining the original **16 path and state regressions**. These cover combat, party selection and replacements, persistence, filtered deployment lists, notification handling, repeat transitions, and planning limits. Tests use fake memory and stub engine functions. They are development tools kept separately in `mod_dev/` and are not needed to build the mod. Passing them does not establish compatibility with another executable or every stage.

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
- **Custom party is not used:** turn Custom party ON and choose Apply and save. Changes during combat apply next battle. Load the same save slot you configured. Base Panel group filters do not require switching to ALL.
- **F3 does not open:** close other menus and wait for an idle hub, settled stage list, pre-battle screen, or free player-turn cursor.
- **A skill or dialog is skipped:** its targeting, effects, or menu contract may be unsupported. Handle it manually and retain the log for investigation.
- **Build succeeds but installation fails:** keep the staged DLL, close the game, and rerun the build script. Check that another process is not holding the DLL.
- **Unexpected behavior after a game update:** compare the executable hash with the supported build above.

The mod writes **`autoplay_mod.log`** in the game folder. It records initialization/version information, accepted decisions, expected effects, SP costs, planning timings, and transition failures. Review logs for personal paths before sharing them publicly.

## Uninstall

Close the game and remove this mod's `XINPUT9_1_0.dll`. If you replaced another proxy DLL, restore its preserved copy. The optional `autoplay_mod.log` can also be removed. Remove `autoplay_party.ini` and its backup only if you also want to discard saved party preferences.

## Source layout

| Path | Purpose |
|---|---|
| `mod_src/xinput_proxy.cpp`, `mod_src/xinput9_1_0.def` | DLL entry points, XInput forwarding, hooks, and hotkeys |
| `mod_src/combat_planner.h`, `mod_src/team_planning.h` | Combat policy, snapshots, candidate scoring, and team planning |
| `mod_src/native_*.h` | Adapters to verified native game contracts |
| `mod_src/planning_budget.h`, `mod_src/tactical_path.h` | Bounded planning and tactical path support |
| `mod_src/stage_repeat.h` | Stage-repeat state machine |
| `mod_src/party_roster.h`, `mod_src/party_settings.h`, `mod_src/party_io.h` | Party policy, settings format, and background persistence |
| `mod_src/build.ps1` | Staged build and backup-preserving installation |

## Development files

`mod_src/` contains the source and build script needed for a normal build. Tests, research, planning notes, generated build output, and backups live separately in **`mod_dev/`**. They are not required to compile the mod; `mod_dev/` is ignored by Git. The optional native-notification QA build requires its local helper in `mod_dev/support/`.

For the repository, upload `README.md`, `LICENSE`, and the contents of `mod_src/`. Keep game files, saves, personal party settings, logs, and `mod_dev/` out of the repository.

## Credits

Developed with **OpenAI Codex, using GPT-6-family agents**, with human direction and in-game feedback. Reverse-engineering and modding work used **Reverse Engineer Anything (REA)**, **Universal Modder**, and local analysis tools including Ghidra. Builds use Microsoft Visual Studio C++ Build Tools.

Disgaea 5 Complete and its content belong to their respective rights holders. This is an unofficial project and is not affiliated with or endorsed by the game's developers or publishers.

## License

The original mod code and documentation are licensed under the **MIT License**; see [LICENSE](LICENSE). MIT permits reuse, modification, and commercial redistribution while retaining the copyright and license notice. The software is provided without warranty.

This license does not grant rights to Disgaea 5 Complete, its executable, assets, or other third-party material.

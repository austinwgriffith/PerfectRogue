# Perfect Random on Carrington Evolved

This tree is [Chesluk-Industries/Perfect-Dark-CE](https://github.com/Chesluk-Industries/Perfect-Dark-CE)
(branch `port`) with Austin's Perfect Random work carried over from the Dabs
fork. Work happens on the `perfect-random` branch. The `chesluk` remote is CE
itself, so `git fetch chesluk && git merge chesluk/port` takes in CE's updates.

## What is here

| Feature | Where |
| --- | --- |
| Perfect Random, the Randomizer, the run, the Gauntlet | `src/game/modrandom.c`, `modrun.c`, `modcareer.c`, `modalarm.c`, `modoptions.c`, `modrules.c`, menus in `port/src/randommenu.c` (Perfect Menu: **Perfect Random** and **Randomizer**) |
| Female guard heads (the head bank) | `src/game/modheads.c`, data in `perfect-random-heads/` beside the exe; textures keyed apart from the stock head's by `struct tex`'s `srcmod` (`texdecompress.c`, `mod.c`) |
| Female guard voice (Cici) | `port/src/ragdoll/wavsound.c`, `port/include/guardvoice.h`, data in `data/sounds/guards/cici/`; `Audio.CiciGuardVoices` in pd.ini |
| Gore | `port/src/ragdoll/dismember.c`, `ragdoll.c`, `src/game/splat.c` (pools, trails, headshots), `wallhit.c`/`wallhitreset.c` (blood decals kept), `chr.c` `chrShotBlood()` |
| Super Shotgun | close-range limb cuts and spin (`dismember.c`, `ragdoll.c`), the reload loop fix (`bondgun.c`), enemy shotgun accuracy in Perfect Random (`chraction.c`) |

The settings use the same `Mod.*` keys in pd.ini as on the Dabs fork.

## What was left behind, and the seams it left

Perfect Random was written on the Dabs fork, which also has GoldenEye (GE
Plus, GoldenEye X, the XBLA release), netplay, Ghost Trials, a mod loader and
the Stage Loader. None of those is in this tree. The code that still asks
them questions goes through two files:

- `src/include/prcompat.h`: constants and macros Dabs has and CE does not,
  and `PR_GOLDENEYE` (0), which guards the GoldenEye guns in the deal.
- `port/src/prstubs.c` / `src/include/prstubs.h`: every Dabs function that is
  still called, answering as if its module were idle (no GoldenEye level, no
  online game, no trial, no mod stage, no Mission Respawn).

**Adding GoldenEye later:** port the Dabs modules (`port/src/ge*.c`,
`gex*.c`, `gebean*.c`, `modloader.c`, the Stage Loader), delete their stubs
from `prstubs.c`, and set `PR_GOLDENEYE` to 1. The linker then names every
place that still needs something. The generated "PR" maps (`generated-missions/`)
come back with the mod loader: until then the Gauntlet deals solo missions.

Other Dabs options in `g_ModOptions` (jumping, akimbo, third person, fog...) are
still in the struct so the menus and pd.ini round-trip, but CE has no code
behind them.

## Build (Windows, MSYS2 MINGW64 shell)

```sh
cd /c/Games/Recomps/pd-dabs-mod-x86_64-windows/source
cmake -G"Unix Makefiles" -B build-win -DCMAKE_BUILD_TYPE=RelWithDebInfo .
cmake --build build-win -j$(nproc)
cp build-win/pd.x86_64.exe ..
```

The first configure downloads Jolt Physics (needs git and a connection).


## Character names (use these when we talk about them)

| Name in the menu | What it is | pd.ini key |
|---|---|---|
| Stage guards | the level's own guards (was "Default") | Mod.RandomGuardModel |
| Hunters | the reinforcement wave that goes after the player (was "Reinforcements") | Mod.RandomReinforcementModel |
| Respawn | guards that come back after dying (was "Returning") | Mod.RandomReturningGuardModel |
| Punishment | the guards called in for killing a civilian | Mod.RandomPunishmentModel |
| Civilian | the civilians walking the level | Mod.RandomCivilianModel |
| Co-op Partners | the friends called in with the Friend Phone | Mod.RandomAllyModel |
| Kill target | a target the player must kill (Cassandra by default) | Mod.RandomKillTargetModel |
| KO target | a target the player must knock out (the lab woman by default) | Mod.RandomKOTargetModel |

Menu: Perfect Rogue > Perfect Rogue Options > Character Models.

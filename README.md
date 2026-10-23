# Chicago3DS

Open-city Nintendo 3DS homebrew game, written in C (devkitPro, libctru, citro3d, citro2d). Everything is in `source/main.c`
plus one vertex shader (`source/vshader.v.pica`).

> **Status, stated plainly.** The 3DS-side code has been compiled for ARM with the project's own compiler flags and the
> title music, packaging and game logic have been tested on a PC (see [Testing](#testing)). It has **not** been run on a
> real Nintendo 3DS by the author of these tests, so frame rate, rendering, sound and input feel on hardware are unverified.
> No frame rate is promised anywhere in this file; measure it on your console with the in-game FPS readout.

## Controls
| Button | Action |
| --- | --- |
| Circle Pad | move (strafes while aiming) |
| C-Stick / D-Pad / touch-drag | look (D-Pad Left/Right turn, Up/Down tilt) |
| B | run |
| A | interact: open a door, use a shop counter or the ticket machine, leave a building |
| L or ZL | aim (over-the-shoulder camera + crosshair) |
| R or ZR | shoot (firing also aims; no weapons inside buildings) |
| Hold X | weapon wheel: pick with the Circle Pad or by touch, release to equip |
| Y | quick-switch camera style |
| SELECT | pause menu: RESUME, WEATHER, GRAPHICS (fog, windows, FPS readout; the readout is on by default), CAMERA, MAIN MENU |
| START | quit (title screen only) |

Weapons: pistol, SMG, shotgun, rocket launcher, each with its own ammo. Touch HACK LIGHTS / BLACKOUT on the bottom screen
to use the phone (that strip is not available inside a building, where the bottom screen shows the floor plan).

## Menus and game modes
Title screen: press A / tap, then **Single Player** (Free Roam) or **Multiplayer** (Deathmatch, Battle Royale, Target Practice).
Every mode shows a loading screen with a description, then a 3-2-1-GO countdown. Pause (SELECT) > MAIN MENU leaves a mode.

- **Free Roam** - the whole city, traffic, pedestrians, wildlife, shops. Buildings can be entered (below).
- **Deathmatch** - random spawn, NPCs hunt you; levels at 10 / 25 / 30 / 45 / 50 kills make them tougher. Endless.
- **Battle Royale** - no weapons at the start; guns, ammo and medkits lie around the map, 11 NPC fighters, a storm shrinks in 5 stages.
- **Target Practice** - first person, in a shooting range far east of the city (x = 1000); 5 rounds x 3 shots against 3 NPC rivals.

Deathmatch, Battle Royale and Target Practice are played in the middle of the city / at the range; the interiors, shops,
wildlife and traffic are Free Roam only.

## The city: what is real, what is scenery, what is simulated
The city is 14 x 14 blocks, 40 m between road centre lines (560 m x 560 m). The player can walk anywhere inside +-300 m.
Blocks are chosen in `pickBlock()` / built in `layoutBlock()`; counts in the shipped layout: 17 glass-tower blocks, 4 low-rise,
10 mid-rise, 20 parks, 5 plazas, 31 house blocks, 16 mixed shop-and-flat, 9 strip-mall, 29 industrial, 6 dock, 11 vacant lots,
10 courts, plus a row of 14 rail blocks and a row of 14 freeway blocks. Lake on the east edge. About 300 buildings, 290 trees.

| Thing | What it really is |
| --- | --- |
| Buildings, fences, benches, trees, parked cars, station walls | **Solid.** Axis-aligned collision boxes (`bldg[]`); the player and the NPC nav grid avoid them. |
| 43 doors into 6 interiors | **Implemented.** A at a door fades inside; every door was driven through enter, shop and exit in the host tests, and the exit spot is always walkable. Interiors: Quick Stop x4, Lakeside Diner x4, Wash & Fold x5, Apartment x5, Warehouse x23, L Station x2. |
| Shops | **Implemented.** Cash (street pickups, $15 for crates delivered in the warehouse) buys: speed boosts (energy drink, chips, coffee, milkshake, deep-dish slice, cheeseburger), cap, sunglasses, jackets, ammo, and sleeping in the apartment (changes the time of day). |
| Elevated rail line | **Scenery you can walk under.** The embankment is solid; there are tunnels at every cross street. The player cannot climb onto it. |
| Moving train (outdoors) | **Simulated, not boardable.** 6 cars run along the elevated track and stop at the two stations for a few seconds (`updateTrain()`); the player cannot get on or collide with it. |
| Train station interiors | **Implemented rooms.** The train visible from the platform is a timer-driven loop, separate from the outdoor train. |
| Ticket machine ($5) | **A fade-out teleport** to the other station's front door (`ACT_RIDE`). There is no ride, no view from inside a train, and the outdoor train is not involved. |
| Elevated freeway | **Scenery you can walk under.** Piers and underpass car parks are real; the deck, the west-end ramp and the east end over the lake are visual only (the engine has no vertical collision, the player is always at ground level). The ramp is **not** climbable and the freeway cannot be driven or walked on. |
| Pigeons (14) and squirrels (10) | **Simple animation AI**, Free Roam only: pigeons scatter within ~5 m, squirrels run to a tree and climb it. They have no gameplay effect. |
| Traffic (36 cars) and pedestrians (30) | Simple lane and sidewalk movement, Free Roam only. |

Why the ramp and train are not interactive: the whole engine assumes a flat ground plane. Walking up a ramp or standing on a
moving train would need a height field for the player, vertical collision against the deck, camera changes and re-testing
every spawn and door. That is a bigger change than "stabilise"; it is not done, and it is not claimed.

## Pedestrians
Each pedestrian gets a random look in `styleNpc()`: 10 skin tones, 9 hair colours, hair styles, caps, beanies and head scarves, 8 outfits,
shoes, backpacks / bags, sunglasses, beards, heights and builds.

## Sound and music
* The title music is `assets/title.mp3`, converted to `romfs/title.pcm` (32 kHz stereo) and streamed from the app's RomFS.
  To change the song, replace `assets/title.mp3`, run `tools/convert_audio.sh` (needs ffmpeg) and rebuild.
* **Fallback order, tested on a PC with fake RomFS/SD folders:** `romfs:/title.pcm` -> `sdmc:/3ds/Chicago3DS/title.pcm` (also
  `sdmc:/3ds/chicago3ds/` and `sdmc:/Chicago3DS/`) -> a built-in synthesised theme. The built-in theme is also used if the file is
  shorter than 4 bytes, the reader thread cannot start, or the file cannot be read while streaming. The title screen's top line says which
  source is active, or "NO SOUND" without DSP firmware.
* Sound effects are generated in code. Audio needs the DSP firmware dump at `sdmc:/3ds/dspfirm.cdc`; without it the game runs silently.

## Build
With devkitPro (3ds-dev installed), in this folder:

```sh
make
```

Output: `chicago3ds.3dsx`. Copy it to the SD card and start it from the Homebrew Launcher.

### Packaging, and why the command is what it is
`3dsxtool` only embeds a RomFS **together with an SMDH**. Running it as `3dsxtool X.elf X.3dsx --romfs=romfs` alone stops with
`Cannot open SMDH file!` (checked against the 3dsxtool source and by running it). The working command, used by the Makefile, is

```
3dsxtool chicago3ds.elf chicago3ds.3dsx --smdh=chicago3ds.smdh --romfs=romfs
```

If embedding the RomFS fails for any reason, the Makefile prints a warning and packages **without** it
(`3dsxtool X.elf X.3dsx --smdh=X.smdh`); the game then finds the music on the SD card or uses the built-in theme. The GitHub
Actions build reports whether the RomFS ended up inside the `.3dsx` (it reads the RomFS offset from the file's header).
The icon is `assets/icon.png` (48x48); the devkitPro default icon is the fallback if it is missing.

### GitHub Actions
`.github/workflows/build.yml` has two jobs: **build** (devkitARM container: `make`, packaging report, uploads the `.3dsx`) and
**hosttest** (the PC tests below). It runs on pushes and pull requests to `main`, or manually from the Actions tab.

## Testing
`tools/hosttest/run.sh` compiles `source/main.c` on a PC against the **real** libctru / citro3d / citro2d headers and runs the game's
own logic under AddressSanitizer and UndefinedBehaviorSanitizer. The 3DS libraries are replaced by stand-ins
(`tools/hosttest/hoststubs.c`); a function the game calls that has no stand-in is a link error, and an undeclared one is a compile error,
so API drift is caught. What it runs:

* city generation: capacity limits, building overlaps, lot bounds, vertex-buffer fit;
* every one of the 43 doors: walk up, enter, use the counter, buy, leave; all interiors and crates reachable; ticket ride arrival;
* train range and both station stops, wildlife, NPC navigation grid (A*), fighters never leave the map or stand inside a solid;
* Deathmatch, Battle Royale and a full 5-round Target Practice match;
* the music source order and fallbacks above;
* the game's real `main()` for 3000 frames per mode with scripted input (menus, loading, play), and a forced vertex-buffer allocation failure;
* a draw-budget scan over 29,768 camera views.

Host results from the last run (these are PC measurements of data, not of speed):

| Measure | Value |
| --- | --- |
| Static city vertices | 169,422 of the 229,998-vertex buffer (36 bytes each, 8.27 MB linear memory) |
| Worst frame buffer use | 208,398 vertices on the title orbit (all districts visible); 174,660 in play |
| Static vertices submitted per frame in play | average 49,268; worst 104,358 with fog on (about 35k triangles); worst 135,864 with fog off |
| Linear memory (host accounting of every `linearAlloc`, including citro3d/citro2d buffers and the framebuffers) | 12.65 MiB peak |
| Static data (.bss) / code (.text), ARM compile | about 1.66 MB / 186 KB |

Reading libctru's `allocateHeaps()`, an application that gets the usual 64 MB allocation ends up with about 32 MB of linear heap, so
12.65 MiB should fit; this is from the source code, depends on how the app is launched, and was not measured on a console. If the 8.27 MB vertex buffer cannot be
allocated, the game now shows a "not enough linear memory" screen and exits instead of crashing.

**What no test here can tell you:** the real frame rate, whether 35k-100k vertices per frame run smoothly on a New 3DS (or at all on an
Old 3DS), how it looks, how it sounds, or whether the touch and button handling feels right. Look at the FPS readout (bottom-left of the bottom screen; pause menu > GRAPHICS > FPS turns it off or on); if it is low, turn off FOG or WINDOWS or lower the draw distances in `main.c`. The FPS readout shows `FULL` if the vertex buffer
ever overflows.

Not yet verified at all: a full link against the real devkitPro libraries and a run of the GitHub Actions workflow for these changes.

## Tuning
New 3DS: the game switches the CPU to 804 MHz at start-up (`osSetSpeedupEnable`). It has not been tried on an Old 3DS. Draw distance is
cut by the district visibility rule in `computeVis()` (near districts plus a forward cone, none beyond the fog) and by `gCullR` for
cars, people and signals.

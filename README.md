# Chicago3DS

Nintendo 3DS homebrew source project.

## Controls
| Button | Action |
| --- | --- |
| Circle Pad | move (strafes while aiming) |
| C-Stick / D-Pad / touch-drag | look (D-Pad Left/Right turn, Up/Down tilt) |
| A | use: enter a door, talk to a shopkeeper, leave a building (a panel shows what A will do) |
| B | run |
| L or ZL | aim (over-the-shoulder camera + crosshair) |
| R or ZR | shoot (firing also aims) |
| Hold X | weapon wheel: pick with the Circle Pad or by touch, release to equip |
| Y | quick-switch camera style |
| SELECT | pause menu: RESUME, WEATHER, GRAPHICS, CAMERA, TITLE |
| START | quit (title screen only) |

Weapons: pistol, SMG, shotgun, rocket launcher. Each has its own ammo count (shown in the wheel and on the map screen).
A pedestrian goes down after 4 hits (a shotgun blast counts as one hit); rocket explosions kill everyone nearby. Yellow
crates on the sidewalks refill ammo. Camera styles (CAMERA menu): Default, Watch Dogs (close over-the-shoulder), First Person.
Touch HACK LIGHTS / BLACKOUT on the bottom screen to use the phone.

## Menus and game modes
Title screen: press A / tap, then **Single Player** (Free Roam) or **Multiplayer** (Deathmatch, Battle Royale, Target Practice). The menus use the same neon style as the pause menu; B goes back. Every mode shows a black loading screen with a description, then a 3-2-1-GO countdown. Pause (SELECT) > **MAIN MENU** leaves a mode.

- **Deathmatch** - random spawn, NPCs hunt you. Every enemy hit costs 10 health (10 hits = dead). Stay out of sight for ~2 s and you heal back up to 50% only. Endless: you cannot win. Levels at 10 / 25 / 30 / 45 / 50 kills (Level 5 max; before 10 kills is a warm-up "Level 0"). Each level adds more enemies, more health, faster fire, longer bursts and better aim. Kills refill some ammo.
- **Battle Royale** - random spawn, no weapons. Guns, ammo drops and medkits lie around the map (blue = gun, yellow = ammo, green = medkit on the map). Medkits go on the weapon wheel (hold X, 6th tile). 11 NPCs loot, heal and shoot each other. A storm shrinks in 5 stages; last one alive wins.
- **Target Practice** - first person only, you cannot move. 5 rounds x 3 shots against 3 NPC rivals, ring score 1-10 per shot (bullseye 10). Circle Pad / C-stick / D-pad look, R fires, L steadies the aim. Highest total wins; equal top scores are a draw.

## City layout
The city is 14 x 14 blocks (560 m x 560 m), 40 m between road centre lines, bordered by Lake Michigan on the east. It is one connected map:

| District | Where | What is there |
| --- | --- | --- |
| Downtown (the Loop) | middle | glass towers on plazas, up to ~95 m tall, the densest streets |
| Residential | north and west | brick houses with gaps, yards, trees |
| Mixed use / commercial | around the centre | mid-rises with car parks, shop fronts with awnings, signs |
| Industrial / docks | south and south-east | warehouses with loading-dock doors, container stacks, cranes at the water |
| Parks, courts, plazas | east (Grant Park) and scattered | grass, paths, ponds, fountains, basketball courts, playgrounds, benches |
| Vacant lots | scattered | fenced lots, rubble, weeds |
| Rail line (row 4 from the north) | all the way across | elevated embankment with a track, a **moving L train** that stops at both stations, and road **tunnels** under it at every north-south street |
| Expressway (row 11 from the north) | all the way across | multi-lane elevated freeway on pillars with guardrails and overhead signs. It comes down on a ramp at the west end and runs out over the lake at the east end. Streets pass **underneath** it, and the shaded space under the deck has parked cars. |

Streets have sidewalks, crosswalks, traffic signals, street signs, street lights, hydrants, trash cans, benches, fences and parked cars. Neighbourhood names (7 x 7 districts of 2 x 2 blocks) show on the bottom screen and as a banner when you cross into one. Block types are chosen in `pickBlock()` and built in `layoutBlock()`, `layoutRail()` and `layoutFreeway()` in `main.c`.

**Important limitation:** the freeway deck, its ramp and the train platforms are scenery you walk *under or beside*. There is no vertical collision, so you cannot walk up the ramp or ride on the train. The train is something you watch (and a ticket machine takes you between stations).

## Free Roam: buildings, shops, money
Press **A** when the prompt "A  ENTER ..." appears at a door. The screen fades, you are inside and the bottom screen shows a floor plan (pink = the spot to use, green = the exit). Press A at the pink spot to open the shop menu: D-Pad or touch to choose, A to buy, B to leave. No weapons can be fired indoors. There are 43 doors in the city.

| Building | What you can do |
| --- | --- |
| Quick Stop (convenience store) | energy drink $4 (speed boost 90 s), chips $2 (boost 30 s), Chicago cap $12, sunglasses $15 (both show on your character) |
| Lakeside Diner | coffee $3, milkshake $5, deep dish slice $7, cheeseburger $8: each gives a speed boost |
| Wash & Fold (laundromat) | red / blue / green / black jacket $6 each (changes your character's jacket) |
| Apartment | sleep until morning, sunset or night (free, changes the time of day) |
| Warehouse (the loading-dock doors) | the dealer sells pistol / SMG / shotgun / rocket ammo; 5 loose crates inside pay $15 each when you walk into them |
| L Station (two of them) | ticket machine: $5 to ride to the other station |

You start with $40. Cash bills ($5 / $10 / $20, shown as green dots on the map) lie around the city and come back after a while. Not enough cash, an item you already own, or full ammo all show a message instead of doing nothing.

## Wildlife and ambience
Pigeons (plazas, parks, courts, commercial streets) and squirrels (parks, house yards, they run up trees) are spawned only near their habitats, 24 animals in total, and only in Free Roam. They scatter when you get close. The background sound changes by district: traffic downtown, birds and crickets in parks and houses, an industrial hum and birdsong at the docks, an expressway roar near and under the freeway, an echoing rumble inside tunnels, a train that gets louder as it passes, and a quiet room tone inside buildings.

## Pedestrians
Each pedestrian gets a random look in `styleNpc()`: 10 skin tones, 9 hair colours, hair styles (short, long, afro, bun, bald), caps, beanies and head
scarves, 8 outfits (tee, long sleeves, shorts, dress, hoodie, suit, coat, tank), shoes, backpacks / bags, sunglasses, beards, and different heights and builds.

## Sound and music
* The title music is `assets/title.mp3`, converted to `romfs/title.pcm` (32 kHz stereo) and streamed from the app's romfs. To use a different
  song, replace `assets/title.mp3` and run `tools/convert_audio.sh` (needs ffmpeg), then rebuild.
* If `title.pcm` can't be found (romfs or `sdmc:/3ds/Chicago3DS/title.pcm`), a built-in synthesized theme plays instead. The title screen's top line says which
  one is active, or "NO SOUND" if `dspfirm.cdc` is missing.
* Gunshots, explosions, footsteps, traffic, rain, crickets, birdsong, horns and sirens are generated in code.
* Audio needs the DSP firmware dump at `sdmc:/3ds/dspfirm.cdc` (create it once with the DSP1 homebrew). Without it the game runs silently.

## Boot sequence
Three developer logos (original placeholders, edit `logoName` / `logoTag` in `main.c`) fade in and slowly out to black, then the title
screen fades in and the music starts. A or START skips the logos.

## Performance notes
The city is built once into one vertex buffer (about 169,000 static vertices of a 230,000 budget, 8.3 MB of linear memory). Each frame only the 80 m districts in front of you and inside the fog distance are drawn, which on a PC measurement averages about 53,000 static vertices and peaks around 100,000 in clear weather (fewer in rain, night and sunset, where the fog is closer). Cars, people, wildlife and lights are rebuilt each frame within a 140 m radius. If the frame rate is low, turn off FOG or WINDOWS in the pause menu, or lower `gCullR` / the fog distances in `main.c`.

Tuned for a New 3DS: the game switches the CPU to 804 MHz at start-up (`osSetSpeedupEnable`). It still runs on an Old 3DS, just with a shorter safe draw distance.
The bottom-left of the map shows a live FPS counter. If it drops below 60 on an Old 3DS, open the pause menu > GRAPHICS and turn off FOG or WINDOWS,
or lower `gCullR` (draw distance for cars, people and signals) in `main.c`. The window textures are procedural (no image files needed).

## Build locally
Install devkitPro with the 3DS development packages (3ds-dev), open a terminal in this folder, then run:

```sh
make
```

The output should be `chicago3ds.3dsx`. Copy it to your SD card and launch it with the Homebrew Launcher.

## Build with GitHub Actions
Upload this folder's contents to a GitHub repository, preserving the `.github/workflows/build.yml` path. Push to the repository or open Actions and run **Build 3DSX** manually. Download the `chicago3ds` artifact from the completed workflow run.

## What has and has not been verified
Checked on a PC (the game logic is compiled on Linux with 3DS stubs and run with the address sanitizer; run `tools/hosttest/run.sh` to repeat it): every door can be reached on foot; enter, shop, buy and leave works for all 43 doors; every room's service spot, exit and crates are reachable; all shop items change cash and state correctly (and refuse when you cannot pay); the train stops at both stations and stays on its rails; wildlife never ends up inside walls; the per-frame vertex budget does not overflow; Deathmatch, Battle Royale and Target Practice still run. `main.c` also type-checks without warnings against the real libctru, citro3d and citro2d headers (on x86, not ARM). Interiors and street views were rendered on the PC to look at the geometry.

**Not done:** the code has not been built with devkitPro/devkitARM (that toolchain could not be installed in the environment it was written in) and has never run on a real 3DS, so frame rate, memory use (the vertex buffer grew from 1.4 MB to 8.3 MB), audio mixing and touch behaviour on the real hardware are untested. Build it with GitHub Actions or locally and use this checklist on the console:
1. Title screen: music plays, menus work. Start Free Roam.
2. Walk to a shop door (small coloured squares on the map). A prompt appears; press A. Buy something, leave, check the cash counter.
3. Open the pause menu inside a shop and back out; check the A prompt does not appear while paused.
4. Walk under the freeway and through a rail tunnel; watch the train arrive at a station; ride from one station to the other.
5. Look at the FPS counter (pause > GRAPHICS > FPS) downtown and in clear weather, then with fog off. Note the lowest value.
6. Start Deathmatch, Battle Royale and Target Practice and make sure they still work on the larger map.
7. Report anything that crashes or stutters, with where you were standing.

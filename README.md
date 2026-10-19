# Chicago3DS

Nintendo 3DS homebrew source project.

## Controls
| Button | Action |
| --- | --- |
| Circle Pad | move (strafes while aiming) |
| C-Stick / D-Pad / touch-drag | look (D-Pad Left/Right turn, Up/Down tilt) |
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

## City layout
The city is 8 x 8 blocks with 40 m between road centre lines (wide streets, wide sidewalks). Each block is a glass tower on a plaza, low-rise brick
houses with gaps between them, a mid-rise with a car park, a park (grass, paths, pond, trees) or a fountain plaza. Downtown (the Loop) is tallest, Grant
Park and the lake are on the east side. Block types are chosen in `pickBlock()` and built in `layoutBlock()` in `main.c`.

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

Note: the latest changes (open city, NPC variety, music fallback) were type-checked and run on a PC, but not compiled with devkitPro or tried on a 3DS. Build with GitHub Actions or locally and check the title screen status line.

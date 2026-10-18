[README.md](https://github.com/user-attachments/files/33630385/README.md)
# Chicago3DS

Nintendo 3DS homebrew source project.

## Controls
Circle Pad = move, B = run, L/R or C-Stick = turn, D-Pad Up/Down = look, touch-drag = look.
Touch HACK LIGHTS / BLACKOUT on the bottom screen to use the phone.
SELECT = pause menu (RESUME, WEATHER: sunny / rainy / sunset / night, GRAPHICS: fog / windows / FPS toggles, TITLE). START quits from the title screen.

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

Note: this archive has been reorganized to match the existing Makefile. A successful build still needs to be confirmed in a devkitPro environment.

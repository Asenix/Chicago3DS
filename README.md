# Chicago3DS

Nintendo 3DS homebrew source project.

## Build locally
Install devkitPro with the 3DS development packages (3ds-dev), open a terminal in this folder, then run:

```sh
make
```

The output should be `chicago3ds.3dsx`. Copy it to your SD card and launch it with the Homebrew Launcher.

## Build with GitHub Actions
Upload this folder's contents to a GitHub repository, preserving the `.github/workflows/build.yml` path. Push to the repository or open Actions and run **Build 3DSX** manually. Download the `chicago3ds` artifact from the completed workflow run.

Note: this archive has been reorganized to match the existing Makefile. A successful build still needs to be confirmed in a devkitPro environment.

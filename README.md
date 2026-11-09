# LibertyBikes

**Real Vice City motorbikes in GTA III.** Ride the PCJ-600 through Liberty City with Vice City's own bike physics, rider animations, crashes and drive-bys.

This is not a car with a bike model. GTA III never had working bikes, so LibertyBikes adds a full bike vehicle to the engine, ported from [reVC](https://github.com/mrxenginner/reVC):

- **Two-wheel physics:** real suspension, leaning into corners, wheelies and stoppies, brake wobble, sand slowing you down
- **VC's handling** for the PCJ-600, read from `LibertyBikes.ini`
- **Animated bike:** steering fork, swing arm, spinning wheels and a leaning chassis
- **Rider animations from Vice City:** jumping on from either side, leaning left/right and forward/back, getting off
- **Falling off:** hit a car, a wall or an object hard enough and you're thrown off with a knockdown matching the direction of the hit; flipping the bike or spinning out throws you off too
- **Drive-bys:** shoot left, right or straight ahead with the Uzi
- **Crash damage, fire and explosions** like any GTA III vehicle
- **Safe saves:** bikes are never written to your save file

## Requirements

- **GTA III for PC, version 1.0** (`gta3.exe` 1.0). The Steam/Rockstar Launcher versions need a downgrader first.
- **[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)** in your GTA III folder (as `d3d8.dll` or `dinput8.dll`). If you already use mods like SilentPatch or the Widescreen Fix you have it.
- **GTA Vice City for PC installed** (any version). The bike's model, textures, collision and animations are taken from **your own copy** of Vice City by the installer. No Rockstar files are included in this mod.

## Installation

1. Download `LibertyBikes-<version>.zip` from the [Releases](../../releases) page.
2. Extract it **into your GTA III folder** (the folder with `gta3.exe`). You should end up with:
   ```text
   GTA III\
     LibertyBikesInstaller.exe
     scripts\LibertyBikes.asi
     scripts\LibertyBikes.ini
   ```
   If you don't have a `scripts` folder yet, the zip creates it.
3. Run **`LibertyBikesInstaller.exe`** from the GTA III folder.
   - It finds Vice City automatically if it's next to GTA III or in a usual Steam/Rockstar location. Otherwise it asks you to paste your Vice City folder (the one with `gta-vc.exe`).
   - It backs up the three files it changes (`models\gta3.dir`, `models\coll\vehicles.col`, `data\default.ide`) to `_backups\LibertyBikes`, adds the PCJ-600 to GTA III, and converts Vice City's bike animations to GTA III's character skeleton.
4. Start GTA III, load a save, and press **F4** to spawn a PCJ-600 in front of you.

## Controls

| Action | Control |
|---|---|
| Spawn a PCJ-600 | **F4** (change or turn off with `SpawnKey` in `scripts\LibertyBikes.ini`) |
| Get on / off | Enter/exit vehicle key |
| Throttle, brake/reverse, steer | Your normal vehicle keys |
| Lean forward / back (wheelies, stoppies) | Vehicle **Steer Up / Steer Down** keys (gamepad: left stick up/down) |
| Drive-by | Select the Uzi, hold **Look Left** or **Look Right** and press the vehicle fire key. Fire without looking to shoot straight ahead. |

## Uninstall

Run `LibertyBikesInstaller.exe --uninstall` from a command prompt in the GTA III folder. It puts the three backed-up files back and removes the `scripts\LibertyBikes*` files.

Don't remove only `LibertyBikes.asi` while the bike is still in `data\default.ide`: the game can't handle a bike without the plugin and spawning one would crash.

## Configuration

`scripts\LibertyBikes.ini`:

- `SpawnKey` is the F4 test spawner key (`0` turns it off).
- `Handling` / `BikeHandling` hold the PCJ-600's Vice City handling lines, unchanged. Tweak them like `handling.cfg`.
- `AudioDonorModel` is the GTA III car whose engine sound the bike uses (default 111, the Kuruma).

## Known limitations

- Only the PCJ-600 so far. Other VC bikes (Sanchez, Angel, Freeway, Faggio, Pizzaboy) are planned.
- The player is the only rider: no traffic bikes, AI riders or parked bikes yet.
- The engine sound is borrowed from a GTA III car.
- Garages and the instant replay haven't been tested with bikes.

## Troubleshooting

`scripts\LibertyBikes.log` says what the plugin did at startup. Every line after `Hooks:` should end in `=1`, and you should see `Loaded 20/20 bike animations` and `ProcessCollision bike suspension patch: 1`. If a hook says `0`, your `gta3.exe` isn't version 1.0, and the plugin turns itself off instead of crashing.

## Building from source

- Visual Studio 2022 (C++ desktop workload, x86) and [plugin-sdk](https://github.com/DK22Pac/plugin-sdk) with `Plugin_III.lib` built. Set `PLUGIN_SDK_DIR`, or put plugin-sdk in `..\_deps\plugin-sdk`.
- `Build.ps1` builds `bin\LibertyBikes.asi`.
- The installer is plain Python 3 (`tools\installer.py`, which uses `tools\retarget_ifp.py`). `Package.ps1` turns it into `LibertyBikesInstaller.exe` with PyInstaller and zips a release.

How it works: [DESIGN.md](DESIGN.md).

## Credits

- **reVC** by mrxenginner and the reVC contributors: https://github.com/mrxenginner/reVC. The bike physics, rider and crash logic are ported from reVC's `Bike.cpp` and ped code. See [LICENSE-reVC.txt](LICENSE-reVC.txt).
- **plugin-sdk** by DK22Pac and contributors: https://github.com/DK22Pac/plugin-sdk ([Plugin-SDK-LICENSE.txt](Plugin-SDK-LICENSE.txt)).
- **Rockstar Games** for GTA III and Vice City. The PCJ-600 and its animations are theirs; they're read from your own game files and never distributed here.

LibertyBikes is a fan-made mod, not affiliated with or endorsed by Rockstar Games or Take-Two Interactive.

## License

MIT, see [LICENSE](LICENSE). Code derived from reVC stays under reVC's notice.

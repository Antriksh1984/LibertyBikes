# How LibertyBikes works

GTA III was built with bikes in mind and then shipped without them. `gta3.exe` still reads a `bike` vehicle type from `vehicles.ide` (type 5) and still has the bike frame-name table (`chassis_dummy`, `forks_front`, `forks_rear`, `wheel_front`, `wheel_rear`, `mudguard`). What it lacks is the bike itself. LibertyBikes supplies that as an ASI plugin on the original `gta3.exe` 1.0, using code ported from reVC's `CBike`.

## The bike class

- **A normal vehicle slot.** A bike lives in an ordinary vehicle-pool slot (the size of a car) and keeps the car's memory layout for everything the two share: wheel collision points, suspension, wheel states and ground contacts. GTA III's car sound code reads those fields, so the bike gets an engine sound by borrowing a donor car's sound set while the sound code runs.
- **Bike-only data on the heap.** State that only a bike has (lean, fork geometry, rider lean, burnouts and so on) lives in a separate block on the heap.
- **Own vtable.** The bike has its own vtable: ProcessControl, PreRender, collision, inputs, blow-up, tyres and the door queries are reVC's logic.
- **Spawning.** The plugin hooks the car constructor: when the model is a bike, a bike is built in the same slot. Script `create_car`, car generators and the F4 spawner all produce bikes this way.
- **Old bike code in the engine.** GTA III's collision loop still resets "bike" suspension values at the cut bikes' old memory offsets. The plugin patches those writes to point at the real suspension values, as Vice City does.

## Rider

- **Getting on and off.** Requests to get on or off a bike are intercepted at `CPed::SetObjective`. The player plays VC's jump-on or get-off animation and is then placed in or out of the vehicle, using the same calls as GTA III's own warp-into-car script command.
- **Seat position.** `CPed::SetPedPositionInCar` puts the rider on the bike's lean matrix.
- **Leaning.** VC's rider-lean logic (`CPed::DriveVehicle`, bike branch) poses the left/right and forward/back animations every frame.
- **Falls** are VC's `CBike::KnockOffRider`. They're triggered by VC's crash-damage formula, a violent spin, flipping over, or reversing too fast.
- **Drive-bys** are VC's `CBike::DoDriveByShootings`, with GTA III's own car-weapon firing.

## Animations

VC's ped animations are made for a skinned "Bip01" skeleton. GTA III peds have a segmented skeleton with different bones. `tools/retarget_ifp.py` converts them:

1. It calibrates a fixed rotation offset per bone from an animation both games share (`idle_stance`).
2. It rebuilds every VC frame as GTA III bone rotations, plus the pelvis position.

The converted animations become an extra animation group (group 25, ids 200 and up) that GTA III's own animation functions can play.

## Installer

`tools/installer.py` takes the PCJ-600 model and textures, `bikes.ifp` and both player skeletons from the user's own games. It:

- re-stamps the textures for GTA III's RenderWare version,
- adds the model to `gta3.img`, the collision to `vehicles.col` and the bike line (ID 151) to `default.ide`,
- writes the converted animations,
- backs up everything it changes, which is what `--uninstall` restores.

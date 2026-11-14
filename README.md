# vDS Mango

vDS Mango is a Linux-only fork of [vDS](https://github.com/hurryman2212/vds)
(virtual DualSense) by Jihong Min. It is developed and tested on CachyOS and
Arch Linux.

> [!WARNING]
>
> This is a beta. It has been tested on one machine: CachyOS with the
> `linux-cachyos` kernel, KDE Plasma, PipeWire and one DualSense. A DualSense
> Edge, several controllers at once and Secure Boot are untested. See
> [Reporting Issues](#reporting-issues) if something does not work.

It is a virtual USB-to-Bluetooth bridge for DualSense and DualSense Edge
Wireless Controllers. It supports all USB-based DualSense features over
Bluetooth (except firmware update), including quadraphonic haptic feedback
(vibration and speaker output), adaptive triggers, microphone input, and
headphone output.

`vdsd` communicates directly with the physical controller over Bluetooth HID
and translates between its Bluetooth protocol and virtual USB traffic. The
virtual DualSense-class device is exposed through `vds_hcd.ko`. Games and
applications see a USB DualSense-class controller, while `vdsd` owns the
physical Bluetooth HID Control and Interrupt channels.

> [!NOTE]
>
> Starting with 0.3.0, the internal database format used by `vdsd` has changed
> for future extensibility and is not compatible with older releases. Remove
> `/var/lib/vds`, then register your controllers again with `vdsctl attach ...`.

## Credits

- [vDS](https://github.com/hurryman2212/vds) by Jihong Min is the project this
  fork is based on. You can
  [sponsor him](https://github.com/sponsors/hurryman2212).
- Microphone and headphone support were contributed to vDS by
  [@TechAntohere](https://github.com/TechAntohere).
- Detailed DualSense output and haptics packet handling is based on
  [DS5Dongle](https://github.com/awalol/DS5Dongle) and protocol capture
  research.

## How it works

The project includes a custom virtual USB HCD primarily because Linux's
`dummy_hcd` module does not support isochronous transfers, which are required
for the USB audio path used by DualSense haptics. The custom HCD currently also
hosts the `/dev/vdsX` bridge and virtual port lifecycle, but that architecture
may be revisited if `dummy_hcd` gains suitable isochronous transfer support.

The current architecture uses both the kernel module and userspace daemon for
runtime communication. For example, haptic feedback currently follows this flow:

```text
Application
  -> Userspace audio server (e.g. PipeWire)
  -(ALSA PCM)-> Linux ALSA stack [snd-usb-audio.ko]
  -(USB isochronous OUT URBs)-> Linux USB stack [vds_hcd.ko]
  -(/dev/vdsX)-> vdsd
  -(AF_BLUETOOTH L2CAP socket)-> Linux Bluetooth stack
  -(Bluetooth HID Control/Interrupt)-> DualSense (Edge) controller
```

The virtual USB HID endpoint descriptors match each controller's USB HID
specification: DualSense HID IN/OUT use 4 ms intervals, while DualSense Edge HID
IN uses 1 ms and HID OUT uses 4 ms. The HID IN interval paces virtual USB input
URB completion.

In theory, other physical gamepad controllers could be implemented as backends
and attached, as long as they can provide the DualSense-specific features
required by games. The infrastructure could also be extended to support physical
transports other than Bluetooth.

## Quick start

Install kernel headers for the running kernel first, then the package from the
AUR, which builds it from source:

```sh
sudo pacman -S linux-cachyos-headers # linux-headers on Arch
paru -S vds-mango                    # or another AUR helper
```

Without matching headers the install stops and prints the header package to
install. The package builds `vds_hcd` through DKMS, installs the udev and audio
rules, and enables and starts `vdsd.service`. It conflicts with the upstream
`vds` package because both ship the same binaries and module.

With Secure Boot enabled the kernel only loads signed modules. DKMS signs
`vds_hcd` with its own key, which has to be enrolled once, followed by a
reboot:

```sh
sudo mokutil --import /var/lib/dkms/mok.pub
```

After installing, open **vDS Mango** from the application menu (or run
`vds-mango`). The setup wizard does the rest:

1. It starts the service and hands DualSense controllers from Bluetooth to
   vDS Mango. This asks for your password once and restarts Bluetooth.
2. Hold Create and PS until the light bar blinks rapidly, pick the controller,
   and the wizard pairs, trusts and registers it.
3. Press the PS button. Games now see a USB DualSense.

No group membership, logout or audio configuration is needed.

Removing the package (`sudo pacman -R vds-mango`) gives the Bluetooth HID
channels back to `bluetoothd` and restarts Bluetooth. Registered controllers
in `/var/lib/vds` and the log are kept.

## Graphical app

Besides the wizard, `vds-mango` lists the registered controllers with their
connection state, changes which controller model games see and which virtual
ports a controller may use, removes controllers, and toggles trace logging.
Setup > Restore Bluetooth input undoes the Bluetooth change.

Edit also sets, per controller, how much audio the controller queues before
it plays haptics and speaker sound. Smaller values react sooner; too small
makes the speaker crackle. The choice is saved with the controller and takes
effect without reconnecting it.

## Command-line setup

The same setup without the graphical app:

1. Hand the Bluetooth HID channels to `vdsd` (see
   [Bluetooth limitation](#bluetooth-limitation)):

```sh
sudo /usr/share/vds-mango/override-bluetoothd.sh disable-input --restart
```

The script prints the effective `bluetooth.service` ExecStart when it is done.
`enable-input --restart` reverts it.

2. Pair and trust the controller. Hold Create and PS until the light bar blinks
   rapidly, then in `bluetoothctl`:

```text
scan on
pair XX:XX:XX:XX:XX:XX
trust XX:XX:XX:XX:XX:XX
```

Both steps are required: an untrusted controller will not be allowed to
reconnect on its own. When re-pairing a controller that BlueZ already knows,
run `remove XX:XX:XX:XX:XX:XX` before `scan on`.

3. Register the controller, then connect it:

```sh
vdsctl list-targets
vdsctl attach XX:XX:XX:XX:XX:XX
```

`attach` registers the address, it does not connect. Press the PS button on the
controller; the physical pad initiates the Bluetooth connection.

4. Check the result:

```sh
vdsctl list
```

`connected` reports whether the pad is live, `path` is the virtual device
(`/dev/vds0`..`/dev/vds3`) once connected, and `error` carries the last connect
failure. The daemon log is `/var/log/vdsd.log`.

## Controller configuration

`vdsctl` is a control client. Every operation is sent through the daemon control
interface and executed by `vdsd`.

`vdsctl attach` registers paired physical Bluetooth controllers in `vdsd.db`.
Use `vdsctl list-targets` to list paired attachable Bluetooth controllers and
their addresses.

```sh
vdsctl list-targets
vdsctl attach aa:bb:cc:dd:ee:01 --profile ds5 --ports 0 # Register as DualSense (press PS to connect)
vdsctl attach aa:bb:cc:dd:ee:02 --profile dse --ports 1 # Register as DualSense Edge
vdsctl detach aa:bb:cc:dd:ee:02 # Detach aa:bb:cc:dd:ee:02
vdsctl list # Show registered controllers
```

Omit `--ports` or pass `--ports ""` to allow all configured ports. Omit
`--profile` or pass `--profile ""` to use the physical controller profile.

`vdsctl attach` on an address that is already registered updates its profile
and ports. `vdsctl list` shows each registered controller with its connection
state, the virtual device path while connected, and the last connection error.

> [!TIP]
>
> `--profile` can expose a controller as a different controller type than the
> physical device. This can be useful when an application supports DualSense but
> not DualSense Edge, or the other way around.

```jsonl
{"address":"aa:bb:cc:dd:ee:01","profile":"","ports":[]}
{"address":"aa:bb:cc:dd:ee:02","profile":"dse","ports":[1],"audio_buffer":"24"}
```

`audio_buffer` appears only once a controller's haptics and speaker delay was
changed from Standard in the graphical app.

With `--ports 0,2`, `vdsctl list` reports `/dev/vds0` or `/dev/vds2` as the
active device path.

## Build from source

### Dependencies

#### Runtime Dependencies

- `libopus` (Bluetooth audio encoding)
- `libudev` (virtual device discovery)
- `libsystemd` (who is logged in at the computer)
- `libdbus-1` (BlueZ device metadata)
- `libbluetooth` (Bluetooth address and L2CAP helpers)
- `qt6-base` (graphical app)
- `polkit` (password prompt of the setup wizard)

#### Build Dependencies

- `gnu99` compiler toolchain compatible with the target kernel build
- `git` (for versioning)
- `make`
- Target Linux kernel headers
- `dkms` (for installing and rebuilding the kernel module automatically)
- `c++20` compiler toolchain for userspace
- `cmake` (3.16 or newer)
- `pkg-config`

```sh
sudo pacman -S git base-devel dkms cmake pkgconf
sudo pacman -S opus systemd dbus bluez-libs qt6-base polkit
```

Install the header package matching your running kernel separately. If your
kernel was built with the LLVM toolchain, install the matching LLVM toolchain
instead of relying only on the default GNU toolchain.

### Kernel Module Install

Build the kernel module locally:

```sh
make -C module
```

Install or remove it through DKMS:

```sh
sudo make -C module install
sudo make -C module uninstall
```

Load the module:

```sh
sudo modprobe vds_hcd
```

Without a `max_port` parameter, the module creates 4 virtual controller ports.
To override the number of virtual controller ports:

```sh
sudo modprobe vds_hcd max_port=2 # must be in the range `1..4`
```

### Userspace Install

Build the daemon and CLI:

```sh
cmake . -B build
make -C build
```

Install or remove the userspace tools:

```sh
sudo make -C build install
sudo make -C build uninstall
```

The installed tools are:

```sh
/usr/local/bin/vdsd
/usr/local/bin/vdsctl
/usr/local/bin/vds-mango
```

To install and remove the `vdsd` systemd service with the userspace tools,
configure with `INSTALL_SERVICE=YES`:

```sh
cmake . -B build -DINSTALL_SERVICE=YES
make -C build
sudo make -C build install
sudo make -C build uninstall
```

When `INSTALL_SERVICE=YES` is used, CMake enables the `vdsd.service` systemd
service for future boots. The service is not started immediately after
installation. To start it without rebooting, run:

```sh
sudo systemctl restart vdsd.service
```

A source install does not create the `vds` group, so `vdsctl` needs `sudo`
unless you create the group yourself (`sudo groupadd --system vds`) and restart
`vdsd`.

### Package build

```sh
# The tagged release, as the AUR package builds it
(cd packaging/ArchLinux && makepkg -si)

# The working tree, without makepkg
packaging/ArchLinux/build-arch.sh --pkgrel 1 --output-dir dist/ArchLinux
```

## Bluetooth limitation

> [!IMPORTANT]
>
> `bluetoothd` has to run with its input plugin disabled (`--noplugin=input`).
> A DualSense connects on the two Bluetooth HID channels, and the input plugin
> otherwise receives it first and exposes it as a plain Bluetooth gamepad.
>
> While the plugin is off, classic Bluetooth keyboards, mice and gamepads do
> not work. Bluetooth audio is not affected. The setup wizard lists the paired
> devices this affects before it changes anything.

The wizard and `override-bluetoothd.sh` install a `bluetooth.service` drop-in
and restart Bluetooth. In a source checkout the script is
`scripts/override-bluetoothd.sh`.

## Audio

The virtual controller is a 48 kHz, 4-channel S16_LE USB audio device: channels
1 and 2 are the speaker, channels 3 and 4 drive the haptics.

With PipeWire and a current `alsa-ucm-conf` the controller's default profile
already exposes all four channels, so nothing has to be configured. The package
installs a WirePlumber rule to
`/usr/share/wireplumber/wireplumber.conf.d/` that gives the controller stable
display names, disables channel-mix normalization, and keeps the controller
from becoming the default output or microphone.

In a source checkout, install the rule yourself and restart the audio services:

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp data/99-vds-dualsense-wireplumber.conf ~/.config/wireplumber/wireplumber.conf.d/
systemctl --user restart pipewire pipewire-pulse wireplumber
```

If the sink shows only two channels, select the card's `pro-audio` profile with
`wpctl set-profile`.

## Input Setup

This step may not be required on every system, but `vds_hcd` is a platform
controller. Without an override, a DualSense touchpad connected through
`vds_hcd` can be classified as an `internal` touchpad instead of an `external`
touchpad like a USB-connected controller. Install the included udev rule so the
virtual controller touchpad is classified like the real USB controller:

```sh
sudo cp data/99-vds-dualsense-udev.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input
```

The package installs this rule and reloads udev already, so this step is only
for source checkouts.

## Testing

Check that the virtual USB controller enumerated:

```sh
lsusb -d 054c:
```

Check input devices and force-feedback support with standard tools:

```sh
evtest
fftest /dev/input/eventX
```

Check the virtual USB audio endpoint with standard ALSA tools:

```sh
aplay -l
speaker-test -D hw:<card-number>,<device-number> -c 4 -r 48000 -F S16_LE -t sine
```

## Runtime notes

- The control socket is `/run/vdsd.sock`. `vdsd` serves root, members of the
  `vds` group, and users logged in at the computer itself. Remote-only users
  (SSH) and service accounts are refused; add such a user to `vds` to allow it.
- `vdsctl list` and `vdsctl list-targets` print a table when stdout is a
  terminal. Pass `--json`, or pipe the output, to get the JSONL form that
  scripts can parse.
- `vdsctl` exits 0 on success and 1 on failure; daemon errors go to stderr.
- `vdsctl trace status` reports the currently enabled trace scopes without
  changing them.
- `SIGHUP` reopens `/var/log/vdsd.log` for logrotate; it does not reload
  `/var/lib/vds/vdsd.db`. Use `vdsctl attach` or `detach` to change it.
- `vdsctl attach` on an already registered address updates its profile and
  ports instead of failing. A profile change reconnects the controller.
- `vdsd --help` lists the runtime prerequisites, the signal behavior, and the
  database and log paths.

## Reporting Issues

Report bugs on the [CachyOS Discord](https://discord.gg/cachyos-862292009423470592).
The GitHub issue tracker is not open yet.

Include the distribution, kernel (`uname -r`), vDS Mango version
(`vdsctl --version`), controller model, your Bluetooth adapter, the affected
application or game, and the steps to reproduce it.

For runtime problems, enable tracing before reproducing the issue, then turn it
off afterwards:

```sh
vdsctl trace on --scope all
vdsctl trace off --scope all
```

You can check or attach the `vdsd` log from `/var/log/vdsd.log`.

<p align="center">
  <img src="data/icons/vds-mango.svg" width="128" alt="vDS Mango logo">
</p>

# vDS Mango

vDS Mango is a Linux-only fork of [vDS](https://github.com/hurryman2212/vds)
(virtual DualSense) by Jihong Min. It is developed and tested on CachyOS and
Arch Linux.

> [!WARNING]
>
> This is a beta. It has been tested on one machine: CachyOS with the
> `linux-cachyos` kernel, KDE Plasma, PipeWire and one DualSense. A DualSense
> Edge and several controllers at once are untested. It needs a kernel with a
> new DualSense fix that so far only CachyOS ships; see
> [Requirements](#requirements). See [Reporting Issues](#reporting-issues) if
> something does not work.

It is a virtual USB-to-Bluetooth bridge for DualSense and DualSense Edge
Wireless Controllers. It supports all USB-based DualSense features over
Bluetooth (except firmware update), including quadraphonic haptic feedback
(vibration and speaker output), adaptive triggers, microphone input, and
headphone output.

`vdsd` communicates directly with the physical controller over Bluetooth HID
and translates between its Bluetooth protocol and virtual USB traffic. The
virtual DualSense-class device is plugged into `vhci_hcd`, the USB/IP host
controller that ships with the kernel, so nothing is built for your kernel.
Games and applications see a USB DualSense-class controller.

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

A DualSense plays haptics through a USB sound card, so the virtual controller
needs a virtual USB port that carries audio. The kernel's `vhci_hcd` (USB/IP)
does, and `vdsd` plays the USB device behind it over a local socket. Nothing
goes over the network. Haptic feedback follows this flow:

```text
Application
  -> Userspace audio server (e.g. PipeWire)
  -(ALSA PCM)-> Linux ALSA stack [snd-usb-audio.ko]
  -(USB isochronous OUT URBs)-> Linux USB stack [vhci_hcd.ko]
  -(USB/IP over a local socket)-> vdsd
  -(hidraw)-> Linux Bluetooth stack
  -(Bluetooth HID Control/Interrupt)-> DualSense (Edge) controller
```

`vhci_hcd` refuses to let the system sleep while a device is plugged into it,
so `vdsd` unplugs the virtual controllers when the system is about to sleep.
Press PS after waking up to reconnect.

The virtual USB HID endpoint descriptors match each controller's USB HID
specification: DualSense HID IN/OUT use 4 ms intervals, while DualSense Edge HID
IN uses 1 ms and HID OUT uses 4 ms. The HID IN interval paces virtual USB input
URB completion.

In theory, other physical gamepad controllers could be implemented as backends
and attached, as long as they can provide the DualSense-specific features
required by games. The infrastructure could also be extended to support physical
transports other than Bluetooth.

## Requirements

- A kernel whose `hid-playstation` driver ignores the controller's Bluetooth
  audio reports. `linux-cachyos` has the fix from 7.3.6 and `linux-cachyos-rc`
  from 7.4-rc4. `linux-cachyos-lts` 6.18 and the mainline kernel, and with it
  stock Arch, do not have it yet.
- The kernel's USB/IP host controller `vhci_hcd`, which Arch and CachyOS
  kernels ship as a module.

`vdsd` tests the driver the first time a controller connects. On a kernel
without the fix it does not connect the controller, and the app and
`vdsctl list` show the requirement instead. The controller then stays an
ordinary Bluetooth gamepad.

## Quick start

Install the package from the AUR, which builds it from source:

```sh
paru -S vds-mango # or another AUR helper
```

The package installs the udev and audio rules, and enables and starts
`vdsd.service`, which loads `vhci_hcd`. There is no kernel module to build and
nothing to sign for Secure Boot. It conflicts with the upstream `vds` package
because both ship the same binaries.

After installing, open **vDS Mango** from the application menu (or run
`vds-mango`). The setup wizard does the rest:

1. Hold Create and PS until the light bar blinks rapidly, pick the controller,
   and the wizard pairs, trusts and registers it.
2. Press the PS button. Games now see a USB DualSense.

No group membership, logout or audio configuration is needed, and other
Bluetooth devices keep working. On a kernel without the DualSense fix the
wizard says so at step 2; see [Requirements](#requirements).

Removing the package (`sudo pacman -R vds-mango`) keeps the registered
controllers in `/var/lib/vds` and the log.

## Graphical app

Besides the wizard, `vds-mango` shows a card for each registered controller
with its connection state, battery level, and headset and microphone state.
From a card you change which controller model games see and which virtual
ports the controller may use, remove it, or press Test to make it vibrate
briefly. Test needs the controller's audio to be idle, so close a game that
plays through it first.

The line along the bottom checks the service, the kernel fix and Bluetooth;
a banner explains whichever one fails. Diagnostics opens the trace logging
switches and a summary of your setup to copy into a bug report.

Edit also sets, per controller, how much audio the controller queues before
it plays haptics and speaker sound. Smaller values react sooner; too small
makes the speaker crackle. The choice is saved with the controller and takes
effect without reconnecting it.

## Command-line setup

The same setup without the graphical app:

1. Pair and trust the controller. Hold Create and PS until the light bar blinks
   rapidly, then in `bluetoothctl`:

```text
scan on
pair XX:XX:XX:XX:XX:XX
trust XX:XX:XX:XX:XX:XX
```

Both steps are required: an untrusted controller will not be allowed to
reconnect on its own. When re-pairing a controller that BlueZ already knows,
run `remove XX:XX:XX:XX:XX:XX` before `scan on`.

2. Register the controller, then connect it:

```sh
vdsctl list-targets
vdsctl attach XX:XX:XX:XX:XX:XX
```

`attach` registers the address, it does not connect. Press the PS button on the
controller; the physical pad initiates the Bluetooth connection.

3. Check the result:

```sh
vdsctl list
```

`connected` reports whether the pad is live, `path` names the virtual port
(`vds0`..`vds3`) once connected, and `error` carries the last connect
failure. If it says that this kernel reads the controller's Bluetooth audio as
button presses, the kernel lacks the fix; see [Requirements](#requirements).
The daemon log is `/var/log/vdsd.log`.

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
state, the virtual port while connected, and the last connection error.

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

With `--ports 0,2`, `vdsctl list` reports `vds0` or `vds2` as the active
virtual port.

## Build from source

### Dependencies

#### Runtime Dependencies

- `libopus` (Bluetooth audio encoding)
- `libudev` (finding the controller's Bluetooth side)
- `libsystemd` (who is logged in at the computer, sleep announcements)
- `libdbus-1` (BlueZ device metadata)
- `qt6-base` and `qt6-svg` (graphical app)
- `polkit` (password prompt of the setup wizard)

#### Build Dependencies

- `git` (for versioning)
- `c++20` compiler toolchain
- `cmake` (3.16 or newer)
- `pkg-config`

```sh
sudo pacman -S git base-devel cmake pkgconf
sudo pacman -S opus systemd dbus qt6-base qt6-svg polkit
```

`vdsd` needs the kernel's USB/IP host controller, which Arch and CachyOS
kernels ship as a module:

```sh
sudo modprobe vhci-hcd
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

## Bluetooth

BlueZ connects the controller as usual and `vdsd` drives it through its hidraw
node. Nothing about Bluetooth is changed, so other Bluetooth devices keep
working. While a controller is registered, a udev rule keeps its Bluetooth
side away from other programs, so games and Steam only see the virtual USB
controller.

On a kernel without the fix the `hid-playstation` driver reads the
controller's microphone packets as button presses. `vdsd` tests the driver the
first time a controller connects and refuses to connect it there; see
[Requirements](#requirements).

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

`data/72-vds-dualsense-hide.rules` hides a registered controller's Bluetooth
side from other programs (see [Bluetooth](#bluetooth)):

```sh
sudo cp data/72-vds-dualsense-hide.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input
```

The package installs the rule and reloads udev already, so this step is only
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

Paste the summary from Diagnostics > Copy system info in the graphical app
(versions, kernel, the three checks and each controller's firmware; no
Bluetooth addresses), and add your Bluetooth adapter, the affected
application or game, and the steps to reproduce it.

For runtime problems, enable tracing before reproducing the issue, then turn it
off afterwards:

```sh
vdsctl trace on --scope all
vdsctl trace off --scope all
```

You can check or attach the `vdsd` log from `/var/log/vdsd.log`.

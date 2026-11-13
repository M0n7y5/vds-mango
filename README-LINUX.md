# vDS Linux Guide

[Back to README.md](README.md)

> [!NOTE]
>
> Starting with 0.3.0, the internal database format used by `vdsd` has changed
> for future extensibility and is not compatible with older releases. Remove
> `/var/lib/vds`, then register your controllers again with `vdsctl attach ...`.

Linux uses a custom virtual USB HCD and raw BlueZ/L2CAP input. Games and
applications see a USB DualSense-class controller, while `vdsd` owns the
physical Bluetooth HID Control and Interrupt channels.

This project includes a custom virtual USB HCD primarily because Linux's
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
IN uses 1 ms and HID OUT uses 4 ms. vDS uses the HID IN interval to pace virtual
USB input URB completion.

## Quick start (packages)

Install kernel headers for the running kernel first, then the package:

```sh
sudo pacman -S linux-headers
sudo pacman -U vds-<version>-<arch>.pkg.tar.zst
```

Without matching headers the install stops and prints the header package to
install. The package builds `vds_hcd` through DKMS, installs the udev rule,
creates the `vds` group, and enables and starts `vdsd.service`.

1. Give vDS the Bluetooth HID channels. Bluetooth keyboards and mice stop
   working while the BlueZ input plugin is disabled:

```sh
sudo /usr/share/vds/override-bluetoothd.sh disable-input --restart
```

The script prints the effective `bluetooth.service` ExecStart when it is done.
`enable-input --restart` reverts it.

2. Pair and trust the controller. Hold Create and PS until the light bar blinks
   rapidly, then in `bluetoothctl`:

```text
pair XX:XX:XX:XX:XX:XX
trust XX:XX:XX:XX:XX:XX
```

Both steps are required: an untrusted controller will not be allowed to
reconnect on its own.

3. Use `vdsctl` without `sudo`:

```sh
sudo usermod -aG vds $USER
```

Log out and back in so the new group membership applies.

4. Register the controller, then connect it:

```sh
vdsctl list-targets
vdsctl attach XX:XX:XX:XX:XX:XX
```

`attach` registers the address, it does not connect. Press the PS button on the
controller; the physical pad initiates the Bluetooth connection.

5. Check the result:

```sh
vdsctl list
```

`connected` reports whether the pad is live, `path` is the virtual device
(`/dev/vds0`..`/dev/vds3`) once connected, and `error` carries the last connect
failure.

6. Set the `pro-audio` card profile as described in
   [Audio Setup](#audio-setup), then install the shipped WirePlumber rule:

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp /usr/share/doc/vds/examples/99-vds-dualsense-wireplumber.conf \
  ~/.config/wireplumber/wireplumber.conf.d/
systemctl --user restart pipewire pipewire-pulse wireplumber
```

The daemon log is `/var/log/vdsd.log`.

## Build from source

### Dependencies

#### Runtime Dependencies

- `libopus` (Bluetooth audio encoding)
- `libudev` (virtual device discovery)
- `libdbus-1` (BlueZ device metadata)
- `libbluetooth` (Bluetooth address and L2CAP helpers)

#### Build Dependencies

- `gnu99` compiler toolchain compatible with the target kernel build
- `git` (for versioning)
- `make`
- Target Linux kernel headers
- `dkms` (for installing and rebuilding the kernel module automatically)
- `c++20` compiler toolchain for userspace
- `cmake` (3.12 or newer)
- `pkg-config`

On Arch/CachyOS-based systems:

```sh
sudo pacman -S git base-devel dkms cmake pkgconf
sudo pacman -S opus systemd dbus bluez-libs
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

Without a `max_port` parameter, vDS creates 4 virtual controller ports. To
override the number of virtual controller ports:

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

## Initial Bluetooth Pairing

> [!IMPORTANT]
>
> Current limitation: run `bluetoothd` with the input plugin disabled
> (`--noplugin=input`). vDS needs direct ownership of the Bluetooth HID Control
> and Interrupt L2CAP channels. If the normal BlueZ input plugin is active, it
> can claim the controller first and expose it as a regular Bluetooth gamepad
> instead of letting vDS bridge it as a virtual USB controller.
>
> In simpler terms, using vDS on Linux currently means giving up Bluetooth
> devices like mice or keyboards. :( A BlueZ userspace stack patch is being
> prepared to remove this limitation.
>
> In the meantime, the helper script `override-bluetoothd.sh` can install or
> remove the required `bluetooth.service` drop-in override:
>
> ```sh
> sudo ./scripts/override-bluetoothd.sh disable-input --restart
> sudo ./scripts/override-bluetoothd.sh enable-input --restart
> ```
>
> Packages install the same script as
> `/usr/share/vds/override-bluetoothd.sh`.

Pair each physical controller once before registering it with `vdsctl`. Start
`bluetoothctl`, put the controller in Bluetooth pairing mode by holding Create
and PS until the light bar blinks rapidly, then use the MAC address printed by
`scan on`:

```text
agent NoInputNoOutput
default-agent
pairable on
scan on
pair XX:XX:XX:XX:XX:XX
trust XX:XX:XX:XX:XX:XX
scan off
quit
```

When re-pairing a controller that BlueZ already knows, run
`remove XX:XX:XX:XX:XX:XX` before `scan on`.

## Audio Setup

Configure the virtual controller audio output as 48 kHz 4-channel S16_LE PCM.
The exact setup differs by audio stack, such as PipeWire, PulseAudio, ALSA, or
JACK.

For PipeWire/WirePlumber, set the controller card profile to `pro-audio`. Find
the WirePlumber device id:

```sh
wpctl status
```

List the available profile indexes for that device:

```sh
pw-cli e <device-id> EnumProfile | awk '/Profile:index/{getline; idx=$2} /Profile:name/{getline; gsub(/"/, "", $2); print idx, $2}'
```

Set the `pro-audio` profile:

```sh
wpctl set-profile <device-id> <pro-audio-profile-index>
```

After setting the `pro-audio` profile, install the included WirePlumber rule. It
gives the virtual controller stable display names, sets the output node to 4
channels, disables channelmix normalization, and gives the microphone source a
low priority:

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp data/99-vds-dualsense-wireplumber.conf ~/.config/wireplumber/wireplumber.conf.d/
```

Packages ship this file as
`/usr/share/doc/vds/examples/99-vds-dualsense-wireplumber.conf` and never
install it into a user configuration directory.

Restart the user audio services after changing this file:

```sh
systemctl --user restart pipewire pipewire-pulse wireplumber
```

## Input Setup

This step may not be required on every system, but `vds_hcd` is a platform
controller. Without an override, a DualSense touchpad connected through vDS can
be classified as an `internal` touchpad instead of an `external` touchpad like a
USB-connected controller. Install the included udev rule so the virtual
controller touchpad is classified like the real USB controller:

```sh
sudo cp data/99-vds-dualsense-udev.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input
```

Packages install this rule and reload udev already, so this step is only for
source checkouts.

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

- The control socket is `/run/vdsd.sock`, owned `root:vds` with mode `0660`
  when the `vds` group exists. Group members run `vdsctl` without `sudo`, and
  `sudo vdsctl` always works.
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

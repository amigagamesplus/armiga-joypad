# Armiga Joypad Linux Driver

Out-of-tree Linux input driver for the gamepad of Allwinner H700 handhelds: analog sticks read through a single SARADC channel and a CD4051/CD4052 analog multiplexer, GPIO buttons and PWM rumble. Written for the [ARMIGA](https://github.com/amigagamesplus/armiga) distribution.

## Status

- **Tested on:** Anbernic RG40XX H (Allwinner H700) with the ARMIGA kernel 7.3.0.
- **Verified on hardware:** all buttons (with debounce), D-pad as hat, both sticks, MODE, rumble, and unloading/reloading the module at runtime (`rmmod` / `insmod`).
- **Implemented but not tested on hardware:** system suspend/resume handling, and the multi-channel SARADC path (`armiga,joypad`), which the H700 does not use.
- Other devices and other kernel versions are untested.

## Features

- **Analog sticks:** single SARADC + analog multiplexer (`armiga,singleadc-joypad`); a path for SoCs with several native SARADC channels (`armiga,joypad`) is also present. Axis range is +/-(900 x `button-adc-scale`), i.e. +/-1800 with the scale of 2 used on the RG40XX H. Dead zone is radial and uses integer arithmetic (`int_sqrt`).
- **Optional EMA filter:** disabled by default. Enable it with `armiga,filter-alpha` (see below); a strong filter adds visible latency when releasing a stick.
- **Buttons:** hardware interrupts on both edges, with a 10 ms debounce wait in the threaded handler. Pins whose GPIO bank has no interrupt support fall back to polling.
- **D-pad as hat:** buttons marked with `armiga,dpad-hat` are polled and reported as `ABS_HAT0X` / `ABS_HAT0Y` (-1..1). `BTN_DPAD_*` capabilities are declared (so SDL button numbering is unchanged) but never reported.
- **MODE button:** injected by the kernel's `adc-keys` driver through `joypad_input_g` (see Kernel requirements).
- **Rumble:** PWM force feedback (`FF_RUMBLE`). Period and polarity come from the `pwms` property, and a `rumble_enable` sysfs attribute enables or disables it.
- **System sleep:** the rumble PWM is stopped on suspend and rescheduled on resume (not tested on hardware).
- **Input identity:** name, vendor, product and revision come from the device tree, so existing RetroArch autoconfig files and SDL mappings keep working.

## Kernel requirements

- Developed and tested on **7.3.0**. The quoted form `MODULE_IMPORT_NS("IIO_CONSUMER")` requires a recent kernel (6.13+); older kernels are untested.
- The PWM calls use `pwm_apply_might_sleep()` on 6.11+ and `pwm_apply_state()` before that.
- **Required kernel patch:** the module references the symbol `joypad_input_g`, exported by a patched `adc-keys` driver (patch `0004` in `board/armiga/linux/patches/` of the ARMIGA repository). Without that patch the symbol is unresolved and the module will not load.

## Build and install

```
# Compile against the running kernel:
make

# Or cross-compile (e.g. ARM64):
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/kernel/build

# Install (modules_install + depmod):
sudo make install

# Clean:
make clean
```

The module file is `armiga-joypad.ko` (module name `armiga_joypad`). There is no DKMS configuration.

## Device tree

### Compatibles

- `armiga,singleadc-joypad`: single SARADC + multiplexer (the one used on the RG40XX H).
- `armiga,joypad`: several native SARADC channels (untested on hardware).
- `rocknix-singleadc-joypad`: legacy value, still matched so that older device trees keep working.

### Properties

| Property | Meaning |
|---|---|
| `joypad-name` | Input device name |
| `joypad-bustype`, `joypad-vendor`, `joypad-product`, `joypad-revision` | Input ID (bus type defaults to `BUS_HOST`) |
| `io-channels`, `io-channel-names` | IIO channel of the SARADC (`amux_adc` in the multiplexer setup) |
| `amux-count` | Number of axes (0 = no analog sticks) |
| `amux-channel-mapping` | Multiplexer channel of each axis, in the order RY, RX, Y, X |
| `amux-a-gpios`, `amux-b-gpios` | Multiplexer select lines (`A = channel >> 1`, `B = channel & 1`) |
| `amux-en-gpios` | Optional multiplexer enable line (driven low to enable the multiplexer) |
| `button-adc-scale`, `button-adc-deadzone`, `button-adc-fuzz`, `button-adc-flat` | Axis scaling (default 1), dead zone, fuzz and flat (default 0 each) |
| `abs_*-p-tuning`, `abs_*-n-tuning` | Per-axis, per-direction gain in percent (`x`, `y`, `rx`, `ry`). Default 180: the deflection is multiplied by 1.8 and then clamped to the axis range |
| `invert-absx`, `invert-absy`, `invert-absrx`, `invert-absry` | Invert an axis |
| `poll-interval` | Polling interval in milliseconds |
| `armiga,filter-alpha` | EMA alpha, 1..256 (256 = filter disabled, the default; out-of-range values are clamped) |
| `pwms`, `pwm-names` | Rumble PWM (period and polarity are taken from here) |
| `rumble-boost-weak`, `rumble-boost-strong` | Level offset added to weak / strong effects (limited to 0xffff) |

In the direct SARADC path (`armiga,joypad`, no `amux-count`) the axes come from `io-channel-names` in order X, Y, RX, RY: at most 4 channels are used, in pairs (3 channels become 2).

Each child node is one button: `gpios`, `label`, `linux,code`, optional `linux,input-type` (defaults to `EV_KEY`) and optional `armiga,dpad-hat` (1 = up, 2 = down, 3 = left, 4 = right; the legacy `rocknix,dpad-hat` is also read).

### Example (Anbernic RG40XX H)

```
joypad: armiga-singleadc-joypad {
	compatible = "armiga,singleadc-joypad";
	joypad-name = "H700 Gamepad";
	joypad-vendor = <0x484B>;
	joypad-product = <0x14DF>;
	joypad-revision = <0x0100>;
	io-channels = <&gpadc 0>;
	io-channel-names = "amux_adc";
	amux-count = <4>;
	amux-channel-mapping = <0 2 1 3>;
	amux-a-gpios = <&pio 8 1 GPIO_ACTIVE_HIGH>;
	amux-b-gpios = <&pio 8 2 GPIO_ACTIVE_HIGH>;
	button-adc-scale = <2>;
	button-adc-deadzone = <128>;
	button-adc-fuzz = <32>;
	button-adc-flat = <32>;
	pwms = <&pwm 3 1000000 0>;
	pwm-names = "enable";

	sw1 {
		gpios = <&pio 0 6 GPIO_ACTIVE_LOW>;
		label = "GPIO DPAD-UP";
		linux,code = <BTN_DPAD_UP>;
		armiga,dpad-hat = <1>;
	};
	/* ... one node per button ... */
};
```

## Files

- `armiga_joypad.h`: constants, structures and macros.
- `armiga_joypad.c`: driver source.
- `Makefile`: out-of-tree Kbuild script (`all`, `clean`, `install`).
- `.gitattributes`: forces LF line endings.

## License

GPL (`MODULE_LICENSE("GPL")`).

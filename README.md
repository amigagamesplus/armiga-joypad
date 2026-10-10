# Armiga Joypad Linux Driver

Modern, unified, high-performance driver for embedded gamepads and handheld consoles, compatible with modern Linux kernels (**6.6+ LTS, 6.12+ LTS, and 7.x branches**).

## Key Features

1. **Unified Analog Architecture:**
   - Support for both SoCs with multiple native SARADC channels (`armiga,joypad`) and configurations with a single SARADC and CD4051/CD4052 analog multiplexer (`armiga,singleadc-joypad`).
2. **Zero Floating-Point Operations (`float`):**
   - All mathematical calculations (radial vector, scaling, filters, and dead zones) are processed strictly using fixed-point integer arithmetic to prevent architecture faults on ARM/ARM64.
3. **Scaled Radial Dead Zone:**
   - Calculation via Euclidean magnitude vector (`int_sqrt`), guaranteeing smooth circular movement and full 360° diagonal range.
4. **EMA Anti-Noise Filter (Exponential Moving Average):**
   - Attenuation of electrical noise and SARADC fluctuations without adding perceptible latency.
5. **Hardware Interrupt Buttons (IRQ):**
   - Button handling via `devm_request_threaded_irq()` for instantaneous response (zero input latency) and dramatic battery saving. Includes automatic fallback to polling if the SoC GPIO bank does not support interrupts.
6. **Modern Resource Management (RAII):**
   - Utilizes modern directives `guard(mutex)(&joypad->lock)` and `device_for_each_child_node_scoped()` introduced in Linux 6.6 and 6.11+.
7. **Vibration Support (PWM Force Feedback):**
   - Integrated with the modern `pwm_apply_might_sleep()` and `input_ff_create_memless()` APIs.

## Project Structure

- `armiga_joypad.h`: Constant definitions, structures, and macros.
- `armiga_joypad.c`: Driver source code.
- `armiga-joypad.dts`: Device Tree Overlay example.
- `Makefile`: Kbuild compilation script and out-of-tree / DKMS support.

## Compilation and Installation

```bash
# Compile against the currently running kernel:
make

# Or cross-compile for another architecture (e.g., ARM64):
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/kernel/build

# Install the module:
sudo make install
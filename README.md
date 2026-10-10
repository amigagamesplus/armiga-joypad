# Armiga Joypad Linux Driver

Driver moderno, unificado y de alto rendimiento para gamepads embebidos y consolas portátiles, compatible con kernels modernos de Linux (**6.6+ LTS, 6.12+ LTS y ramas 7.x**).

## Características principales

1. **Arquitectura Analógica Unificada:**
   - Soporte tanto para SoCs con múltiples canales SARADC nativos (`armiga,joypad`) como para configuraciones con un solo SARADC y multiplexor analógico CD4051/CD4052 (`armiga,singleadc-joypad`).
2. **Cero operaciones en punto flotante (`float`):**
   - Todos los cálculos matemáticos (vector radial, escalado, filtros y zonas muertas) se procesan estrictamente con aritmética entera en punto fijo para evitar violaciones de arquitectura en ARM/ARM64.
3. **Zona Muerta Radial Escalada:**
   - Cálculo por vector de magnitud euclidiana (`int_sqrt`), garantizando movimiento circular suave y alcance completo de las diagonales en 360°.
4. **Filtro Anti-ruido EMA (Exponential Moving Average):**
   - Atenuación de ruido eléctrico y fluctuaciones del SARADC sin añadir latencia perceptible.
5. **Botones por Interrupciones Hardware (IRQ):**
   - Manejo de botones mediante `devm_request_threaded_irq()` para respuesta instantánea (cero latencia de entrada) y ahorro drástico de batería. Incluye fallback automático a sondeo si el banco GPIO del SoC no soporta interrupciones.
6. **Manejo Seguro de Recursos (RAII):**
   - Utiliza las directivas modernas `guard(mutex)(&joypad->lock)` y `device_for_each_child_node_scoped()` introducidas en Linux 6.6 y 6.11+.
7. **Soporte de Vibración (PWM Force Feedback):**
   - Integrado con la API moderna `pwm_apply_might_sleep()` y `input_ff_create_memless()`.

## Estructura del proyecto

- `armiga_joypad.h`: Definiciones de constantes, estructuras y macros.
- `armiga_joypad.c`: Código fuente del controlador.
- `armiga-joypad.dts`: Ejemplo de Device Tree Overlay.
- `Makefile`: Script de compilación Kbuild y soporte out-of-tree / DKMS.

## Compilación e instalación

```bash
# Compilar contra el kernel actualmente en ejecución:
make

# O compilar de forma cruzada para otra arquitectura (ej. ARM64):
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/ruta/al/kernel/build

# Instalar el módulo:
sudo make install
```

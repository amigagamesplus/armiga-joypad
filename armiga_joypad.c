// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Armiga Unified Joypad Driver
 *
 * Driver moderno y de alto rendimiento para gamepads integrados en
 * plataformas Armiga y dispositivos de juego embebidos.
 *
 * Características:
 *  - Soporte para SARADC multicanal directo o multiplexado analógico (AMUX).
 *  - Filtrado digital anti-ruido (EMA - Media Móvil Exponencial) en aritmética entera.
 *  - Zona muerta radial escalada con cálculo por vector de magnitud sin pérdida diagonal.
 *  - Botones digitales gestionados por interrupciones (IRQs) con fallback a polling.
 *  - Soporte Force Feedback (Rumble) por PWM con API moderna sin bloqueos.
 *  - Limpieza automática RAII (guard / scoped) compatible con kernels modernos (6.6+, 6.12 LTS, 7.x).
 *
 * Copyright (C) 2026 Armiga Project <https://github.com/armiga>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/iio/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/of.h>
#include <linux/pwm.h>
#include <linux/cleanup.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/version.h>

#include "armiga_joypad.h"

#define ARMIGA_TUNING_FACTOR(x, p)	(((x) * (p)) / 100)
#define ARMIGA_CLAMP(x, low, high)	clamp_t(int, (x), (low), (high))

struct armiga_axis {
	const char *name;
	int code;			/* ABS_X, ABS_Y, ABS_RX, ABS_RY, etc. */
	int min;
	int max;
	int cal;			/* Centro calibrado al inicio */
	int scale;
	bool invert;
	int channel;			/* Índice de canal IIO o canal AMUX */
	int tuning_p;			/* Afinación cuadrante positivo */
	int tuning_n;			/* Afinación cuadrante negativo */
	int last_filtered;		/* Valor anterior para filtro EMA */
	int value;
};

struct armiga_button {
	const char *label;
	struct gpio_desc *desc;
	int irq;
	int linux_code;
	int report_type;
	bool old_value;
	bool use_irq;
};

struct armiga_amux {
	struct iio_channel *iio_ch;
	struct gpio_desc *sel_a;
	struct gpio_desc *sel_b;
	struct gpio_desc *en;
	u32 channel_map[ARMIGA_AMUX_CH_NUM];
};

struct armiga_joypad {
	struct device *dev;
	struct input_dev *input;
	struct mutex lock;
	bool enabled;

	/* Arquitectura analógica */
	bool use_amux;
	struct armiga_amux amux;
	struct iio_channel **iio_channels;

	int num_axes;
	struct armiga_axis *axes;

	/* Botones digitales */
	int num_buttons;
	struct armiga_button *buttons;
	bool any_button_polling;

	/* Parámetros de filtrado y zona muerta */
	int poll_interval_ms;
	int deadzone;
	int fuzz;
	int flat;
	int ema_alpha;			/* Factor alpha para EMA (0..256) */

	/* Rumble (PWM Force Feedback) */
	bool has_rumble;
	bool rumble_enabled;
	struct pwm_device *pwm;
	struct work_struct rumble_work;
	u16 rumble_level;
	u32 boost_weak;
	u32 boost_strong;
};

/*----------------------------------------------------------------------------*/
/* Filtro EMA (Exponential Moving Average) en aritmética de enteros            */
/*----------------------------------------------------------------------------*/
static int armiga_filter_ema(struct armiga_joypad *joypad, struct armiga_axis *axis, int raw)
{
	int alpha = joypad->ema_alpha;
	int filtered;

	if (axis->last_filtered == 0 && axis->cal == 0) {
		axis->last_filtered = raw;
		return raw;
	}

	filtered = (alpha * raw + (256 - alpha) * axis->last_filtered) >> 8;
	axis->last_filtered = filtered;
	return filtered;
}

/*----------------------------------------------------------------------------*/
/* Multiplexor Analógico (AMUX)                                               */
/*----------------------------------------------------------------------------*/
static int armiga_amux_select(struct armiga_joypad *joypad, int channel)
{
	struct armiga_amux *amux = &joypad->amux;

	/* Misma polaridad que el driver original: EN=0 habilita el mux */
	if (amux->en)
		gpiod_set_value_cansleep(amux->en, 0);

	if (channel < 0 || channel >= ARMIGA_AMUX_CH_NUM) {
		if (amux->en)
			gpiod_set_value_cansleep(amux->en, 1);
		return -EINVAL;
	}

	/* canal 0: A=0 B=0, 1: A=0 B=1, 2: A=1 B=0, 3: A=1 B=1 */
	gpiod_set_value_cansleep(amux->sel_a, (channel >> 1) & 1);
	gpiod_set_value_cansleep(amux->sel_b, channel & 1);

	/* Conmutacion del mux: 35 ns (on) / 9 ns (off) */
	usleep_range(10, 20);
	return 0;
}

static int armiga_read_raw_axis(struct armiga_joypad *joypad, struct armiga_axis *axis)
{
	int raw_value = 0;
	int ret;

	if (joypad->use_amux) {
		ret = armiga_amux_select(joypad, axis->channel);
		if (ret < 0)
			return 0;

		ret = iio_read_channel_raw(joypad->amux.iio_ch, &raw_value);
	} else {
		ret = iio_read_channel_processed(joypad->iio_channels[axis->channel], &raw_value);
	}

	if (ret < 0)
		return 0;

	if (axis->scale > 0)
		raw_value *= axis->scale;

	return raw_value;
}

/*----------------------------------------------------------------------------*/
/* Procesamiento Analógico y Zona Muerta Radial Escalada                      */
/*----------------------------------------------------------------------------*/
static void armiga_joypad_process_sticks(struct armiga_joypad *joypad)
{
	int i;

	for (i = 0; i < joypad->num_axes; i += 2) {
		struct armiga_axis *ax = &joypad->axes[i];
		struct armiga_axis *ay = (i + 1 < joypad->num_axes) ? &joypad->axes[i + 1] : NULL;
		int raw_x, raw_y;
		int x, y;
		int mag;

		raw_x = armiga_read_raw_axis(joypad, ax);
		if (!raw_x)
			continue;
		x = armiga_filter_ema(joypad, ax, raw_x) - ax->cal;

		if (ay) {
			raw_y = armiga_read_raw_axis(joypad, ay);
			if (!raw_y)
				continue;
			y = armiga_filter_ema(joypad, ay, raw_y) - ay->cal;

			/* Deadzone Radial Escalado */
			mag = int_sqrt((long)x * x + (long)y * y);
			if (joypad->deadzone > 0) {
				if (mag <= joypad->deadzone) {
					x = 0;
					y = 0;
				} else if (mag > 0 && ax->max > joypad->deadzone) {
					x = (((ax->max * x) / mag) * (mag - joypad->deadzone)) /
					    (ax->max - joypad->deadzone);
					y = (((ay->max * y) / mag) * (mag - joypad->deadzone)) /
					    (ay->max - joypad->deadzone);
				}
			}

			/* Curvas no lineales cuadrante positivo / negativo */
			if (ay->tuning_n && y < 0)
				y = ARMIGA_TUNING_FACTOR(y, ay->tuning_n);
			if (ay->tuning_p && y > 0)
				y = ARMIGA_TUNING_FACTOR(y, ay->tuning_p);

			y = ARMIGA_CLAMP(y, ay->min, ay->max);
			input_report_abs(joypad->input, ay->code, ay->invert ? -y : y);
		}

		if (ax->tuning_n && x < 0)
			x = ARMIGA_TUNING_FACTOR(x, ax->tuning_n);
		if (ax->tuning_p && x > 0)
			x = ARMIGA_TUNING_FACTOR(x, ax->tuning_p);

		x = ARMIGA_CLAMP(x, ax->min, ax->max);
		input_report_abs(joypad->input, ax->code, ax->invert ? -x : x);
	}
}

/*----------------------------------------------------------------------------*/
/* Botones Digitales (IRQs y Polling Fallback)                                */
/*----------------------------------------------------------------------------*/
static irqreturn_t armiga_button_irq_handler(int irq, void *dev_id)
{
	struct armiga_joypad *joypad = dev_id;
	int i;

	for (i = 0; i < joypad->num_buttons; i++) {
		struct armiga_button *btn = &joypad->buttons[i];

		if (btn->irq == irq) {
			int val = gpiod_get_value_cansleep(btn->desc);
			if (val >= 0 && val != btn->old_value) {
				input_event(joypad->input, btn->report_type, btn->linux_code, val);
				input_sync(joypad->input);
				btn->old_value = val;
			}
			break;
		}
	}
	return IRQ_HANDLED;
}

static void armiga_joypad_check_buttons_poll(struct armiga_joypad *joypad)
{
	int i;
	bool sync_needed = false;

	for (i = 0; i < joypad->num_buttons; i++) {
		struct armiga_button *btn = &joypad->buttons[i];

		if (!btn->use_irq) {
			int val = gpiod_get_value_cansleep(btn->desc);
			if (val >= 0 && val != btn->old_value) {
				input_event(joypad->input, btn->report_type, btn->linux_code, val);
				btn->old_value = val;
				sync_needed = true;
			}
		}
	}

	if (sync_needed)
		input_sync(joypad->input);
}

/*----------------------------------------------------------------------------*/
/* Bucle de sondeo analógico                                                  */
/*----------------------------------------------------------------------------*/
static void armiga_joypad_poll(struct input_dev *input)
{
	struct armiga_joypad *joypad = input_get_drvdata(input);

	if (READ_ONCE(joypad->enabled)) {
		armiga_joypad_process_sticks(joypad);
		if (joypad->any_button_polling)
			armiga_joypad_check_buttons_poll(joypad);
		input_sync(input);
	}
}

/*----------------------------------------------------------------------------*/
/* Rumble / Haptic PWM Support                                                */
/*----------------------------------------------------------------------------*/
static int armiga_pwm_rumble_start(struct armiga_joypad *joypad)
{
	struct pwm_state state;
	int err;

	pwm_get_state(joypad->pwm, &state);
	pwm_set_relative_duty_cycle(&state, joypad->rumble_level, 0xffff);
	state.enabled = true;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0))
	err = pwm_apply_might_sleep(joypad->pwm, &state);
#else
	err = pwm_apply_state(joypad->pwm, &state);
#endif
	if (err)
		dev_err(joypad->dev, "Error al aplicar PWM rumble: %d\n", err);

	return err;
}

static void armiga_pwm_rumble_stop(struct armiga_joypad *joypad)
{
	pwm_disable(joypad->pwm);
}

static void armiga_rumble_work_fn(struct work_struct *work)
{
	struct armiga_joypad *joypad = container_of(work, struct armiga_joypad, rumble_work);

	guard(mutex)(&joypad->lock);

	if (!joypad->rumble_enabled || joypad->rumble_level == 0) {
		armiga_pwm_rumble_stop(joypad);
		return;
	}

	armiga_pwm_rumble_start(joypad);
}

static int armiga_rumble_play_effect(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct armiga_joypad *joypad = data;
	u32 level;

	if (effect->type != FF_RUMBLE)
		return 0;

	scoped_guard(mutex, &joypad->lock) {
		if (!joypad->rumble_enabled)
			return 0;

		if (effect->u.rumble.strong_magnitude)
			level = effect->u.rumble.strong_magnitude + joypad->boost_strong;
		else
			level = effect->u.rumble.weak_magnitude + joypad->boost_weak;

		joypad->rumble_level = (u16)ARMIGA_CLAMP(level, 0, 0xffff);
	}

	schedule_work(&joypad->rumble_work);
	return 0;
}

/*----------------------------------------------------------------------------*/
/* Ciclo de vida del dispositivo Input                                        */
/*----------------------------------------------------------------------------*/
static int armiga_joypad_open(struct input_dev *input)
{
	struct armiga_joypad *joypad = input_get_drvdata(input);
	int i;

	/* Calibrar centro de ejes al abrir */
	for (i = 0; i < joypad->num_axes; i++) {
		struct armiga_axis *ax = &joypad->axes[i];
		int raw = armiga_read_raw_axis(joypad, ax);
		if (raw > 0) {
			ax->cal = raw;
			ax->last_filtered = raw;
			dev_info(joypad->dev, "Eje '%s' centrado en %d mV\n", ax->name, ax->cal);
		}
	}

	/* Sincronizar estado inicial de botones */
	for (i = 0; i < joypad->num_buttons; i++) {
		struct armiga_button *btn = &joypad->buttons[i];
		int val = gpiod_get_value_cansleep(btn->desc);
		if (val >= 0) {
			btn->old_value = val;
			input_event(input, btn->report_type, btn->linux_code, val);
		}
	}
	input_sync(input);

	guard(mutex)(&joypad->lock);
	joypad->enabled = true;
	dev_info(joypad->dev, "Joypad abierto y habilitado\n");
	return 0;
}

static void armiga_joypad_close(struct input_dev *input)
{
	struct armiga_joypad *joypad = input_get_drvdata(input);

	guard(mutex)(&joypad->lock);
	joypad->enabled = false;

	if (joypad->has_rumble) {
		cancel_work_sync(&joypad->rumble_work);
		armiga_pwm_rumble_stop(joypad);
	}
	dev_info(joypad->dev, "Joypad cerrado\n");
}

/*----------------------------------------------------------------------------*/
/* Inicialización de hardware y DT                                            */
/*----------------------------------------------------------------------------*/
static int armiga_setup_axis(struct device *dev, struct armiga_axis *axis,
			     const char *name, int code, int channel, int scale)
{
	char prop[32];
	u32 tuning;

	axis->name = name;
	axis->code = code;
	axis->channel = channel;
	axis->scale = scale;
	axis->max = (ARMIGA_ADC_MAX_VOLTAGE / 2) * scale;
	axis->min = -axis->max;

	snprintf(prop, sizeof(prop), "invert-abs%s", name);
	axis->invert = device_property_read_bool(dev, prop);

	snprintf(prop, sizeof(prop), "abs_%s-p-tuning", name);
	if (!device_property_read_u32(dev, prop, &tuning))
		axis->tuning_p = tuning;
	else
		axis->tuning_p = ARMIGA_ADC_TUNING_DEFAULT;

	snprintf(prop, sizeof(prop), "abs_%s-n-tuning", name);
	if (!device_property_read_u32(dev, prop, &tuning))
		axis->tuning_n = tuning;
	else
		axis->tuning_n = ARMIGA_ADC_TUNING_DEFAULT;

	return 0;
}

static int armiga_parse_buttons(struct armiga_joypad *joypad)
{
	struct device *dev = joypad->dev;
	struct fwnode_handle *child;
	int idx = 0;

	joypad->num_buttons = device_get_child_node_count(dev);
	if (joypad->num_buttons == 0)
		return 0;

	joypad->buttons = devm_kcalloc(dev, joypad->num_buttons,
				      sizeof(struct armiga_button), GFP_KERNEL);
	if (!joypad->buttons)
		return -ENOMEM;

	/* Iterador seguro RAII para hijos en Device Tree / FWNODE */
	device_for_each_child_node_scoped(dev, child) {
		struct armiga_button *btn = &joypad->buttons[idx++];
		int irq, err;

		fwnode_property_read_string(child, "label", &btn->label);
		btn->desc = devm_fwnode_gpiod_get(dev, child, NULL, GPIOD_IN, btn->label);
		if (IS_ERR(btn->desc))
			return dev_err_probe(dev, PTR_ERR(btn->desc),
					      "Fallo al pedir GPIO %s\n", btn->label);

		if (fwnode_property_read_u32(child, "linux,code", &btn->linux_code))
			return dev_err_probe(dev, -EINVAL, "Botón sin linux,code: %s\n", btn->label);

		if (fwnode_property_read_u32(child, "linux,input-type", &btn->report_type))
			btn->report_type = EV_KEY;

		/* Intentar usar interrupción nativa */
		irq = gpiod_to_irq(btn->desc);
		if (irq > 0) {
			err = devm_request_threaded_irq(dev, irq, NULL,
							armiga_button_irq_handler,
							IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
							btn->label ?: "armiga-btn", joypad);
			if (err == 0) {
				btn->irq = irq;
				btn->use_irq = true;
				continue;
			}
		}

		/* Fallback a polling si el pin no soporta IRQ */
		btn->use_irq = false;
		joypad->any_button_polling = true;
	}

	return 0;
}

static int armiga_setup_input_device(struct armiga_joypad *joypad)
{
	struct device *dev = joypad->dev;
	struct input_dev *input;
	const char *name = ARMIGA_DRV_NAME;
	int i, err;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input_set_drvdata(input, joypad);
	joypad->input = input;

	device_property_read_string(dev, "joypad-name", &name);
	input->name = name;
	input->phys = ARMIGA_DRV_NAME "/input0";

	input->open = armiga_joypad_open;
	input->close = armiga_joypad_close;

	/* Configuración de ejes analógicos */
	if (joypad->num_axes > 0) {
		__set_bit(EV_ABS, input->evbit);
		for (i = 0; i < joypad->num_axes; i++) {
			struct armiga_axis *ax = &joypad->axes[i];
			input_set_abs_params(input, ax->code, ax->min, ax->max,
					     joypad->fuzz, joypad->flat);
		}
	}

	/* Configuración de botones */
	if (joypad->num_buttons > 0) {
		__set_bit(EV_KEY, input->evbit);
		for (i = 0; i < joypad->num_buttons; i++) {
			struct armiga_button *btn = &joypad->buttons[i];
			input_set_capability(input, btn->report_type, btn->linux_code);
		}
	}

	/* Configuración de Rumble */
	if (joypad->has_rumble) {
		input_set_capability(input, EV_FF, FF_RUMBLE);
		err = input_ff_create_memless(input, joypad, armiga_rumble_play_effect);
		if (err)
			return dev_err_probe(dev, err, "Fallo al registrar FF memless rumble\n");
	}

	/* Polling para ejes analógicos y botones sin IRQ */
	err = input_setup_polling(input, armiga_joypad_poll);
	if (err)
		return dev_err_probe(dev, err, "Fallo al configurar polling del input\n");

	input_set_poll_interval(input, joypad->poll_interval_ms);

	return input_register_device(input);
}

/*----------------------------------------------------------------------------*/
/* Atributos Sysfs Limpios                                                    */
/*----------------------------------------------------------------------------*/
static ssize_t rumble_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct armiga_joypad *joypad = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", joypad->rumble_enabled ? 1 : 0);
}

static ssize_t rumble_enable_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct armiga_joypad *joypad = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;

	guard(mutex)(&joypad->lock);
	if (joypad->rumble_enabled != enable) {
		joypad->rumble_enabled = enable;
		if (!enable) {
			joypad->rumble_level = 0;
			cancel_work_sync(&joypad->rumble_work);
			armiga_pwm_rumble_stop(joypad);
		}
	}
	return count;
}
static DEVICE_ATTR_RW(rumble_enable);

static struct attribute *armiga_joypad_attrs[] = {
	&dev_attr_rumble_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(armiga_joypad);

/*----------------------------------------------------------------------------*/
/* Driver Probe / Remove                                                      */
/*----------------------------------------------------------------------------*/
static int armiga_joypad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct armiga_joypad *joypad;
	u32 scale = 1;
	u32 amux_count = 0;
	u32 amux_map[ARMIGA_AMUX_CH_NUM] = { 0, 1, 2, 3 };
	int err;

	joypad = devm_kzalloc(dev, sizeof(*joypad), GFP_KERNEL);
	if (!joypad)
		return -ENOMEM;

	joypad->dev = dev;
	mutex_init(&joypad->lock);
	platform_set_drvdata(pdev, joypad);

	/* Lectura de propiedades generales de DT */
	device_property_read_u32(dev, "button-adc-fuzz", &joypad->fuzz);
	device_property_read_u32(dev, "button-adc-flat", &joypad->flat);
	device_property_read_u32(dev, "button-adc-scale", &scale);
	device_property_read_u32(dev, "button-adc-deadzone", &joypad->deadzone);

	joypad->poll_interval_ms = 16; /* ~60 Hz por defecto */
	device_property_read_u32(dev, "poll-interval", &joypad->poll_interval_ms);

	joypad->ema_alpha = ARMIGA_EMA_ALPHA_DEFAULT;
	device_property_read_u32(dev, "armiga,filter-alpha", &joypad->ema_alpha);

	/* Determinar arquitectura analógica: AMUX vs Canales IIO directos */
	device_property_read_u32(dev, "amux-count", &amux_count);
	joypad->use_amux = amux_count > 0;
	if (joypad->use_amux) {
		struct armiga_amux *amux = &joypad->amux;

		amux->iio_ch = devm_iio_channel_get(dev, "amux_adc");
		if (IS_ERR(amux->iio_ch))
			return dev_err_probe(dev, PTR_ERR(amux->iio_ch), "Fallo al obtener canal amux_adc\n");

		amux->sel_a = devm_gpiod_get(dev, "amux-a", GPIOD_OUT_LOW);
		if (IS_ERR(amux->sel_a))
			return dev_err_probe(dev, PTR_ERR(amux->sel_a), "Fallo al pedir gpio amux-a\n");

		amux->sel_b = devm_gpiod_get(dev, "amux-b", GPIOD_OUT_LOW);
		if (IS_ERR(amux->sel_b))
			return dev_err_probe(dev, PTR_ERR(amux->sel_b), "Fallo al pedir gpio amux-b\n");

		amux->en = devm_gpiod_get_optional(dev, "amux-en", GPIOD_OUT_LOW);
		if (IS_ERR(amux->en))
			return dev_err_probe(dev, PTR_ERR(amux->en), "Fallo al pedir gpio amux-en\n");

		{
			static const struct {
				const char *name;
				int code;
			} amux_axes[ARMIGA_AMUX_CH_NUM] = {
				{ "ry", ABS_RY }, { "rx", ABS_RX }, { "y", ABS_Y }, { "x", ABS_X },
			};
			int i;

			/* Canal del mux de cada eje (orden RY, RX, Y, X); por defecto 0 1 2 3 */
			device_property_read_u32_array(dev, "amux-channel-mapping", amux_map,
						       ARMIGA_AMUX_CH_NUM);

			joypad->num_axes = min_t(u32, amux_count, ARMIGA_AMUX_CH_NUM);
			joypad->axes = devm_kcalloc(dev, joypad->num_axes,
						    sizeof(struct armiga_axis), GFP_KERNEL);
			if (!joypad->axes)
				return -ENOMEM;

			for (i = 0; i < joypad->num_axes; i++)
				armiga_setup_axis(dev, &joypad->axes[i], amux_axes[i].name,
						  amux_axes[i].code, amux_map[i], scale);
		}
	} else {
		/* SARADC multicanal nativo */
		int ch_count = of_property_count_strings(dev->of_node, "io-channel-names");
		if (ch_count > 0) {
			const char *uname;
			int i;

			joypad->num_axes = min(ch_count, ARMIGA_MAX_AXES);
			joypad->iio_channels = devm_kcalloc(dev, joypad->num_axes,
							    sizeof(struct iio_channel *), GFP_KERNEL);
			joypad->axes = devm_kcalloc(dev, joypad->num_axes,
						    sizeof(struct armiga_axis), GFP_KERNEL);
			if (!joypad->iio_channels || !joypad->axes)
				return -ENOMEM;

			for (i = 0; i < joypad->num_axes; i++) {
				if (of_property_read_string_index(dev->of_node, "io-channel-names", i, &uname) < 0)
					return dev_err_probe(dev, -EINVAL, "Error en io-channel-names[%d]\n", i);

				joypad->iio_channels[i] = devm_iio_channel_get(dev, uname);
				if (IS_ERR(joypad->iio_channels[i]))
					return dev_err_probe(dev, PTR_ERR(joypad->iio_channels[i]),
							      "Error obteniendo canal IIO: %s\n", uname);
			}

			/* Mapeo predeterminado de ejes */
			armiga_setup_axis(dev, &joypad->axes[0], "x",  ABS_X,  0, scale);
			armiga_setup_axis(dev, &joypad->axes[1], "y",  ABS_Y,  1, scale);
			if (joypad->num_axes >= 4) {
				armiga_setup_axis(dev, &joypad->axes[2], "rx", ABS_RX, 2, scale);
				armiga_setup_axis(dev, &joypad->axes[3], "ry", ABS_RY, 3, scale);
			}
		}
	}

	/* Configuración de botones */
	err = armiga_parse_buttons(joypad);
	if (err)
		return err;

	/* Soporte de Rumble (PWM) */
	if (device_property_present(dev, "pwm-names") || device_property_present(dev, "pwms")) {
		joypad->pwm = devm_pwm_get(dev, "enable");
		if (!IS_ERR(joypad->pwm)) {
			joypad->has_rumble = true;
			joypad->rumble_enabled = true;
			INIT_WORK(&joypad->rumble_work, armiga_rumble_work_fn);
			device_property_read_u32(dev, "rumble-boost-weak", &joypad->boost_weak);
			device_property_read_u32(dev, "rumble-boost-strong", &joypad->boost_strong);
		}
	}

	/* Registro en subsistema de Input */
	err = armiga_setup_input_device(joypad);
	if (err)
		return err;

	dev_info(dev, "Armiga Joypad inicializado correctamente (%d ejes, %d botones)\n",
		 joypad->num_axes, joypad->num_buttons);
	return 0;
}

/*----------------------------------------------------------------------------*/
/* Tabla de Coincidencias y Registro del Módulo                               */
/*----------------------------------------------------------------------------*/
static const struct of_device_id armiga_joypad_of_match[] = {
	{ .compatible = "armiga,joypad", },
	{ .compatible = "armiga,singleadc-joypad", },
	{ .compatible = "rocknix-singleadc-joypad", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, armiga_joypad_of_match);

static struct platform_driver armiga_joypad_driver = {
	.probe = armiga_joypad_probe,
	.driver = {
		.name = ARMIGA_DRV_NAME,
		.dev_groups = armiga_joypad_groups,
		.of_match_table = armiga_joypad_of_match,
	},
};

module_platform_driver(armiga_joypad_driver);

MODULE_AUTHOR("Armiga Project <https://github.com/armiga>");
MODULE_DESCRIPTION("Driver unificado de gamepad para plataformas Armiga");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" ARMIGA_DRV_NAME);
MODULE_IMPORT_NS("IIO_CONSUMER");

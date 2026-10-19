/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Armiga Joypad Driver Header
 *
 * Copyright (C) 2026 Armiga Project
 */

#ifndef _ARMIGA_JOYPAD_H_
#define _ARMIGA_JOYPAD_H_

#include <linux/types.h>

#define ARMIGA_DRV_NAME		"armiga-joypad"

/* Configuración predeterminada de ADC */
#define ARMIGA_ADC_MAX_VOLTAGE		1800
#define ARMIGA_ADC_TUNING_DEFAULT	180
#define ARMIGA_EMA_ALPHA_DEFAULT	256  /* sin filtro: respuesta identica al driver original (base 256) */

/* Canales máximos admitidos para multiplexor o IIO directo */
#define ARMIGA_MAX_AXES			4
#define ARMIGA_AMUX_CH_NUM		4

#endif /* _ARMIGA_JOYPAD_H_ */

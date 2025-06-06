/*
 * MS5611 pressure and temperature sensor driver
 *
 * Copyright (c) Tomasz Duszynski <tduszyns@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _MS5611_H
#define _MS5611_H

#include <linux/device.h>
#include <linux/iio/iio.h>
#include <linux/mutex.h>

struct regulator;

#define MS5611_RESET			0x1e
#define MS5611_READ_ADC			0x00
#define MS5611_READ_PROM_WORD		0xA0
#define MS5611_PROM_WORDS_NB		8

enum {
	MS5611,
	MS5607,
};

/*
 * OverSampling Rate descriptor.
 * Warning: cmd MUST be kept aligned on a word boundary (see
 * m5611_spi_read_adc_temp_and_pressure in ms5611_spi.c).
 */
struct ms5611_osr {
	unsigned long conv_usec;
	u8 cmd;
	unsigned short rate;
};

struct ms5611_state {
	void *client;
	struct mutex lock;

	const struct ms5611_osr *pressure_osr;
	const struct ms5611_osr *temp_osr;

	u16 prom[MS5611_PROM_WORDS_NB];

	int (*reset)(struct device *dev);
	int (*read_prom_word)(struct device *dev, int index, u16 *word);
	int (*read_adc_temp_and_pressure)(struct device *dev,
					  s32 *temp, s32 *pressure);

	int (*compensate_temp_and_pressure)(struct ms5611_state *st, s32 *temp,
					  s32 *pressure);
	struct regulator *vdd;
};

int ms5611_probe(struct iio_dev *indio_dev, struct device *dev,
                 const char* name, int type);
int ms5611_remove(struct iio_dev *indio_dev);

#endif /* _MS5611_H */

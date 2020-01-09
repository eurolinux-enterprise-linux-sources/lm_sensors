/*
 * sensord
 *
 * A daemon that periodically logs sensor information to syslog.
 *
 * Copyright (c) 1999-2002 Merlin Hughes <merlin@merlin.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "args.h"
#include "sensord.h"
#include "lib/error.h"

#define DO_READ 0
#define DO_SCAN 1
#define DO_SET 2
#define DO_RRD 3

static const char *chipName(const sensors_chip_name *chip)
{
	static char buffer[256];
	if (sensors_snprintf_chip_name(buffer, 256, chip) < 0)
		return NULL;
	return buffer;
}

static int idChip(const sensors_chip_name *chip)
{
	const char *adapter;

	sensorLog(LOG_INFO, "Chip: %s", chipName (chip));
	adapter = sensors_get_adapter_name(&chip->bus);
	if (adapter)
		sensorLog(LOG_INFO, "Adapter: %s", adapter);

	return 0;
}

static int doKnownChip(const sensors_chip_name *chip,
		       const ChipDescriptor *descriptor, int action)
{
	const FeatureDescriptor *features = descriptor->features;
	int index0, subindex;
	int ret = 0;
	double tmp;

	if (action == DO_READ)
		ret = idChip(chip);
	for (index0 = 0; (ret == 0) && features[index0].format; ++ index0) {
		const FeatureDescriptor *feature = features + index0;
		int alarm, beep;
		char *label = NULL;

		if (!(label = sensors_get_label(chip, feature->feature))) {
			sensorLog(LOG_ERR,
				  "Error getting sensor label: %s/%s",
				  chip->prefix, feature->feature->name);
			ret = 22;
		} else {
			double values[MAX_DATA];

			alarm = 0;
			if (!ret && feature->alarmNumber != -1) {
				if ((ret = sensors_get_value(chip,
							     feature->alarmNumber,
							     &tmp))) {
					sensorLog(LOG_ERR,
						  "Error getting sensor data: %s/#%d: %s",
						  chip->prefix,
						  feature->alarmNumber,
						  sensors_strerror(ret));
					ret = 20;
				} else {
					alarm = (int) (tmp + 0.5);
				}
			}
			if ((action == DO_SCAN) && !alarm)
				continue;

			beep = 0;
			if (!ret && feature->beepNumber != -1) {
				if ((ret = sensors_get_value(chip,
							     feature->beepNumber,
							     &tmp))) {
					sensorLog(LOG_ERR,
						  "Error getting sensor data: %s/#%d: %s",
						  chip->prefix,
						  feature->beepNumber,
						  sensors_strerror(ret));
					ret = 21;
				} else {
					beep = (int) (tmp + 0.5);
				}
			}

			for (subindex = 0; !ret &&
				     (feature->dataNumbers[subindex] >= 0); ++ subindex) {
				if ((ret = sensors_get_value(chip, feature->dataNumbers[subindex], values + subindex))) {
					sensorLog(LOG_ERR, "Error getting sensor data: %s/#%d: %s", chip->prefix, feature->dataNumbers[subindex], sensors_strerror(ret));
					ret = 23;
				}
			}
			if (ret == 0) {
				if (action == DO_RRD) { // arse = "N:"
					if (feature->rrd) {
						const char *rrded = feature->rrd (values);
						strcat(strcat (rrdBuff, ":"),
						       rrded ? rrded : "U");
					}
				} else {
					const char *formatted = feature->format (values, alarm, beep);
					if (formatted) {
						if (action == DO_READ) {
							sensorLog(LOG_INFO, "  %s: %s", label, formatted);
						} else {
							sensorLog(LOG_ALERT, "Sensor alarm: Chip %s: %s: %s", chipName(chip), label, formatted);
						}
					}
				}
			}
		}
		if (label)
			free(label);
	}
	return ret;
}

static int setChip(const sensors_chip_name *chip)
{
	int ret = 0;
	if ((ret = idChip(chip))) {
		sensorLog(LOG_ERR, "Error identifying chip: %s",
			  chip->prefix);
	} else if ((ret = sensors_do_chip_sets(chip))) {
		sensorLog(LOG_ERR, "Error performing chip sets: %s: %s",
			  chip->prefix, sensors_strerror(ret));
		ret = 50;
	} else {
		sensorLog(LOG_INFO, "Set.");
	}
	return ret;
}

static int doChip(const sensors_chip_name *chip, int action)
{
	int ret = 0;
	if (action == DO_SET) {
		ret = setChip(chip);
	} else {
		int index0, chipindex = -1;
		for (index0 = 0; knownChips[index0].features; ++ index0)
			/*
			 * Trick: we compare addresses here. We know it works
			 * because both pointers were returned by
			 * sensors_get_detected_chips(), so they refer to
			 * libsensors internal structures, which do not move.
			 */
			if (knownChips[index0].name == chip) {
				chipindex = index0;
				break;
			}
		if (chipindex >= 0)
			ret = doKnownChip(chip, &knownChips[chipindex],
					  action);
	}
	return ret;
}

static int doChips(int action)
{
	const sensors_chip_name *chip;
	int i, j, ret = 0;

	for (j = 0; (ret == 0) && (j < sensord_args.numChipNames); ++ j) {
		i = 0;
		while ((ret == 0) &&
		       ((chip = sensors_get_detected_chips(&sensord_args.chipNames[j], &i)) != NULL)) {
			ret = doChip(chip, action);
		}
	}

	return ret;
}

int readChips(void)
{
	int ret = 0;

	sensorLog(LOG_DEBUG, "sensor read started");
	ret = doChips(DO_READ);
	sensorLog(LOG_DEBUG, "sensor read finished");

	return ret;
}

int scanChips(void)
{
	int ret = 0;

	sensorLog(LOG_DEBUG, "sensor sweep started");
	ret = doChips(DO_SCAN);
	sensorLog(LOG_DEBUG, "sensor sweep finished");

	return ret;
}

int setChips(void)
{
	int ret = 0;

	sensorLog(LOG_DEBUG, "sensor set started");
	ret = doChips(DO_SET);
	sensorLog(LOG_DEBUG, "sensor set finished");

	return ret;
}

/* TODO: loadavg entry */

int rrdChips(void)
{
	int ret = 0;

	strcpy(rrdBuff, "N");

	sensorLog(LOG_DEBUG, "sensor rrd started");
	ret = doChips(DO_RRD);
	sensorLog(LOG_DEBUG, "sensor rrd finished");

	return ret;
}

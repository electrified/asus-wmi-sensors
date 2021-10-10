// SPDX-License-Identifier: GPL-2.0+
/*
 * HWMON driver for ASUS motherboards that provides sensor readouts via WMI
 * interface present in the UEFI of the X370/X470/B450/X399 Ryzen motherboards.
 *
 * Copyright (C) 2018-2019 Ed Brindley <kernel@maidavale.org>
 *
 * WMI interface provides:
 * CPU Core Voltage,
 * CPU SOC Voltage,
 * DRAM Voltage,
 * VDDP Voltage,
 * 1.8V PLL Voltage,
 * +12V Voltage,
 * +5V Voltage,
 * 3VSB Voltage,
 * VBAT Voltage,
 * AVCC3 Voltage,
 * SB 1.05V Voltage,
 * CPU Core Voltage,
 * CPU SOC Voltage,
 * DRAM Voltage,
 * CPU Fan RPM,
 * Chassis Fan 1 RPM,
 * Chassis Fan 2 RPM,
 * Chassis Fan 3 RPM,
 * HAMP Fan RPM,
 * Water Pump RPM,
 * CPU OPT RPM,
 * Water Flow RPM,
 * AIO Pump RPM,
 * CPU Temperature,
 * CPU Socket Temperature,
 * Motherboard Temperature,
 * Chipset Temperature,
 * Tsensor 1 Temperature,
 * CPU VRM Temperature,
 * Water In,
 * Water Out,
 * CPU VRM Output Current.
 *
 */
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/units.h>
#include <linux/wmi.h>

#define ASUSWMI_MONITORING_GUID		"466747A0-70EC-11DE-8A39-0800200C9A66"
#define ASUSWMI_METHODID_GET_VALUE	0x52574543
#define ASUSWMI_METHODID_UPDATE_BUFFER	0x51574543
#define ASUSWMI_METHODID_GET_INFO	0x50574543
#define ASUSWMI_METHODID_GET_NUMBER		0x50574572
#define ASUSWMI_METHODID_GET_VERSION		0x50574574

#define ASUS_WMI_MAX_STR_SIZE	32

#define DMI_EXACT_MATCH_ASUS_BOARD_NAME(name) \
	{ \
		.matches = { \
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, \
					"ASUSTeK COMPUTER INC."), \
			DMI_EXACT_MATCH(DMI_BOARD_NAME, name), \
		}, \
	}

static const struct dmi_system_id asus_wmi_dmi_table[] __initconst = {
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("PRIME X399-A"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("PRIME X470-PRO"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG CROSSHAIR VI EXTREME"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG CROSSHAIR VI HERO"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG CROSSHAIR VI HERO (WI-FI AC)"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG CROSSHAIR VII HERO"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG CROSSHAIR VII HERO (WI-FI)"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG STRIX B450-E GAMING"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG STRIX B450-F GAMING"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG STRIX B450-I GAMING"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG STRIX X399-E GAMING"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG STRIX X470-F GAMING"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG STRIX X470-I GAMING"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG ZENITH EXTREME"),
	DMI_EXACT_MATCH_ASUS_BOARD_NAME("ROG ZENITH EXTREME ALPHA"),
	{}
};
MODULE_DEVICE_TABLE(dmi, asus_wmi_dmi_table);

enum asus_wmi_sensor_class {
	VOLTAGE = 0x0,
	TEMPERATURE_C = 0x1,
	FAN_RPM = 0x2,
	CURRENT = 0x3,
	WATER_FLOW = 0x4,
};

enum asus_wmi_location {
	CPU = 0x0,
	CPU_SOC = 0x1,
	DRAM = 0x2,
	MOTHERBOARD = 0x3,
	CHIPSET = 0x4,
	AUX = 0x5,
	VRM = 0x6,
	COOLER = 0x7
};

enum asus_wmi_type {
	SIGNED_INT = 0x0,
	UNSIGNED_INT = 0x1,
	SCALED = 0x3,
};

enum asus_wmi_source {
	SIO = 0x1,
	EC = 0x2
};

static enum hwmon_sensor_types asus_data_types[] = {
	[VOLTAGE] = hwmon_in,
	[TEMPERATURE_C] = hwmon_temp,
	[FAN_RPM] = hwmon_fan,
	[CURRENT] = hwmon_curr,
	[WATER_FLOW] = hwmon_fan,
};

static u32 hwmon_attributes[] = {
	[hwmon_chip] = HWMON_C_REGISTER_TZ,
	[hwmon_temp] = HWMON_T_INPUT | HWMON_T_LABEL,
	[hwmon_in] = HWMON_I_INPUT | HWMON_I_LABEL,
	[hwmon_curr] = HWMON_C_INPUT | HWMON_C_LABEL,
	[hwmon_fan] = HWMON_F_INPUT | HWMON_F_LABEL,
};

/**
 * struct asus_wmi_sensor_info - sensor info.
 * @id: sensor id.
 * @data_type: sensor class e.g. voltage, temp etc.
 * @location: sensor location.
 * @name: sensor name.
 * @source: sensor source.
 * @type: sensor type signed, unsigned etc.
 * @cached_value: cached sensor value.
 */
struct asus_wmi_sensor_info {
	u32 id;
	int data_type;
	int location;
	char name[ASUS_WMI_MAX_STR_SIZE];
	int source;
	int type;
	u32 cached_value;
};

struct asus_wmi_wmi_info {
	u8 buffer;
	unsigned long source_last_updated[3];	/* in jiffies */
	int sensor_count;

	const struct asus_wmi_sensor_info **info[hwmon_max];
	struct asus_wmi_sensor_info **info_by_id;
};

struct asus_wmi_sensors {
	/* lock access to instrnal cache */
	struct mutex lock;
	struct asus_wmi_wmi_info wmi;
};

/*
 * Universal method for calling WMI method
 */
static int asus_wmi_call_method(u32 method_id, u32 *args, struct acpi_buffer *output)
{
	struct acpi_buffer input = {(acpi_size) sizeof(*args), args };
	acpi_status status;

	status = wmi_evaluate_method(ASUSWMI_MONITORING_GUID, 0, method_id, &input, output);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

/*
 * Gets the version of the ASUS sensors interface implemented
 */
static int asus_wmi_get_version(u32 *version)
{
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	u32 args[] = {0, 0, 0};
	union acpi_object *obj;
	int err;

	err = asus_wmi_call_method(ASUSWMI_METHODID_GET_VERSION, args, &output);
	if (err)
		return err;

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_INTEGER)
		return -EIO;

	*version = obj->integer.value;

	return 0;
}

/*
 * Gets the number of sensor items
 */
static int asus_wmi_get_item_count(u32 *count)
{
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	u32 args[] = {0, 0, 0};
	union acpi_object *obj;
	int err;

	err = asus_wmi_call_method(ASUSWMI_METHODID_GET_NUMBER, args, &output);
	if (err)
		return err;

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_INTEGER)
		return -EIO;

	*count = obj->integer.value;

	return 0;
}

static int asus_wmi_hwmon_add_chan_info(struct hwmon_channel_info *asus_wmi_hwmon_chan,
					struct device *dev, int num,
					enum hwmon_sensor_types type, u32 config)
{
	u32 *cfg;

	cfg = devm_kcalloc(dev, num + 1, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	asus_wmi_hwmon_chan->type = type;
	asus_wmi_hwmon_chan->config = cfg;
	memset32(cfg, config, num);

	return 0;
}

/*
 * For a given sensor item returns details e.g. type (voltage/temperature/fan speed etc), bank etc
 */
static int asus_wmi_sensor_info(int index, struct asus_wmi_sensor_info *s)
{
	union acpi_object name_obj, data_type_obj, location_obj, source_obj, type_obj;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	u32 args[] = {index, 0};
	union acpi_object *obj;
	int err;

	err = asus_wmi_call_method(ASUSWMI_METHODID_GET_INFO, args, &output);
	if (err)
		return err;

	s->id = index;

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_PACKAGE)
		return -EIO;

	if (obj->package.count != 5)
		return 1;

	name_obj = obj->package.elements[0];

	if (name_obj.type != ACPI_TYPE_STRING)
		return 1;

	strncpy(s->name, name_obj.string.pointer, sizeof(s->name) - 1);

	data_type_obj = obj->package.elements[1];

	if (data_type_obj.type != ACPI_TYPE_INTEGER)
		return 1;

	s->data_type = data_type_obj.integer.value;

	location_obj = obj->package.elements[2];

	if (location_obj.type != ACPI_TYPE_INTEGER)
		return 1;

	s->location = location_obj.integer.value;

	source_obj = obj->package.elements[3];

	if (source_obj.type != ACPI_TYPE_INTEGER)
		return 1;

	s->source = source_obj.integer.value;

	type_obj = obj->package.elements[4];

	if (type_obj.type != ACPI_TYPE_INTEGER)
		return 1;

	s->type = type_obj.integer.value;

	return 0;
}

static int asus_wmi_update_buffer(u8 source)
{
	u32 args[] = {source, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	return asus_wmi_call_method(ASUSWMI_METHODID_UPDATE_BUFFER, args, &output);
}

static int asus_wmi_get_sensor_value(u8 index, u32 *value)
{
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	u32 args[] = {index, 0};
	union acpi_object *obj;
	int err;

	err = asus_wmi_call_method(ASUSWMI_METHODID_GET_VALUE, args, &output);
	if (err)
		return err;

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_INTEGER)
		return -EIO;

	*value = obj->integer.value;

	return 0;
}

static void asus_wmi_update_values_for_source(u8 source, struct asus_wmi_sensors *sensor_data)
{
	int ret = 0;
	int value = 0;
	int i;
	struct asus_wmi_sensor_info *sensor;

	for (i = 0; i < sensor_data->wmi.sensor_count; i++) {
		sensor = sensor_data->wmi.info_by_id[i];
		if (sensor && sensor->source == source) {
			ret = asus_wmi_get_sensor_value(sensor->id, &value);
			if (!ret)
				sensor->cached_value = value;
		}
	}
}

static int asus_wmi_scale_sensor_value(u32 value, int data_type)
{
	/* FAN_RPM and WATER_FLOW don't need scaling */
	switch (data_type) {
	case VOLTAGE:
		return DIV_ROUND_CLOSEST(value, 1000);
	case TEMPERATURE_C:
		return value * 1000;
	case CURRENT:
		return value * 1000;
	}
	return value;
}

static int asus_wmi_get_cached_value_or_update(const struct asus_wmi_sensor_info *sensor,
					       struct asus_wmi_sensors *sensor_data,
					       u32 *value)
{
	int ret;

	if (time_after(jiffies, sensor_data->wmi.source_last_updated[sensor->source] + HZ)) {
		ret = asus_wmi_update_buffer(sensor->source);
		if (ret)
			return -EIO;

		sensor_data->wmi.buffer = sensor->source;

		asus_wmi_update_values_for_source(sensor->source, sensor_data);
		sensor_data->wmi.source_last_updated[sensor->source] = jiffies;
	}

	*value = sensor->cached_value;
	return 0;
}

/*
 * Now follow the functions that implement the hwmon interface
 */

static int asus_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	int ret;
	u32 value = 0;
	const struct asus_wmi_sensor_info *sensor;

	struct asus_wmi_sensors *sensor_data = dev_get_drvdata(dev);

	sensor = *(sensor_data->wmi.info[type] + channel);

	mutex_lock(&sensor_data->lock);
	ret = asus_wmi_get_cached_value_or_update(sensor, sensor_data, &value);
	mutex_unlock(&sensor_data->lock);
	if (!ret)
		*val = asus_wmi_scale_sensor_value(value, sensor->data_type);

	return ret;
}

static int asus_wmi_hwmon_read_string(struct device *dev,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel, const char **str)
{
	const struct asus_wmi_sensor_info *sensor;
	struct asus_wmi_sensors *sensor_data = dev_get_drvdata(dev);

	sensor = *(sensor_data->wmi.info[type] + channel);
	*str = sensor->name;

	return 0;
}

static umode_t asus_wmi_hwmon_is_visible(const void *drvdata,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	const struct asus_wmi_sensor_info *sensor;
	const struct asus_wmi_sensors *sensor_data = drvdata;

	sensor = *(sensor_data->wmi.info[type] + channel);
	if (sensor && sensor->name)
		return 0444;

	return 0;
}

static const struct hwmon_ops asus_wmi_hwmon_ops = {
	.is_visible = asus_wmi_hwmon_is_visible,
	.read = asus_wmi_hwmon_read,
	.read_string = asus_wmi_hwmon_read_string,
};

static struct hwmon_chip_info asus_wmi_chip_info = {
	.ops = &asus_wmi_hwmon_ops,
	.info = NULL,
};

static int asus_wmi_configure_sensor_setup(struct platform_device *pdev,
					   struct asus_wmi_sensors *sensor_data)
{
	int err;
	int i, idx;
	int nr_count[hwmon_max] = {0}, nr_types = 0;
	struct device *hwdev;
	struct device *dev = &pdev->dev;
	struct hwmon_channel_info *asus_wmi_hwmon_chan;
	struct asus_wmi_sensor_info *temp_sensor;
	enum hwmon_sensor_types type;
	const struct hwmon_channel_info **ptr_asus_wmi_ci;
	const struct hwmon_chip_info *chip_info;

	sensor_data->wmi.buffer = -1;
	temp_sensor = devm_kcalloc(dev, 1, sizeof(*temp_sensor), GFP_KERNEL);
	if (!temp_sensor)
		return -ENOMEM;

	for (i = 0; i < sensor_data->wmi.sensor_count; i++) {
		err = asus_wmi_sensor_info(i, temp_sensor);
		if (err)
			return -EINVAL;

		switch (temp_sensor->data_type) {
		case TEMPERATURE_C:
		case VOLTAGE:
		case CURRENT:
		case FAN_RPM:
		case WATER_FLOW:
			type = asus_data_types[temp_sensor->data_type];
			if (!nr_count[type])
				nr_types++;
			nr_count[type]++;
			break;
		}
	}

	if (nr_count[hwmon_temp])
		nr_count[hwmon_chip]++, nr_types++;

	asus_wmi_hwmon_chan = devm_kcalloc(dev, nr_types,
					   sizeof(*asus_wmi_hwmon_chan),
					   GFP_KERNEL);
	if (!asus_wmi_hwmon_chan)
		return -ENOMEM;

	ptr_asus_wmi_ci = devm_kcalloc(dev, nr_types + 1,
				       sizeof(*ptr_asus_wmi_ci), GFP_KERNEL);
	if (!ptr_asus_wmi_ci)
		return -ENOMEM;

	asus_wmi_chip_info.info = ptr_asus_wmi_ci;
	chip_info = &asus_wmi_chip_info;

	sensor_data->wmi.info_by_id = devm_kcalloc(dev, sensor_data->wmi.sensor_count,
						   sizeof(*sensor_data->wmi.info_by_id),
						   GFP_KERNEL);

	if (!sensor_data->wmi.info_by_id)
		return -ENOMEM;

	for (type = 0; type < hwmon_max; type++) {
		if (!nr_count[type])
			continue;

		asus_wmi_hwmon_add_chan_info(asus_wmi_hwmon_chan, dev,
					     nr_count[type], type,
					     hwmon_attributes[type]);
		*ptr_asus_wmi_ci++ = asus_wmi_hwmon_chan++;

		sensor_data->wmi.info[type] = devm_kcalloc(dev,
							   nr_count[type],
							   sizeof(*sensor_data->wmi.info),
							   GFP_KERNEL);
		if (!sensor_data->wmi.info[type])
			return -ENOMEM;
	}

	for (i = sensor_data->wmi.sensor_count - 1; i >= 0 ; i--) {
		temp_sensor = devm_kzalloc(dev, sizeof(*temp_sensor), GFP_KERNEL);
		if (!temp_sensor)
			return -ENOMEM;

		err = asus_wmi_sensor_info(i, temp_sensor);
		if (err)
			continue;

		switch (temp_sensor->data_type) {
		case TEMPERATURE_C:
		case VOLTAGE:
		case CURRENT:
		case FAN_RPM:
		case WATER_FLOW:
			type = asus_data_types[temp_sensor->data_type];
			idx = --nr_count[type];
			*(sensor_data->wmi.info[type] + idx) = temp_sensor;
			sensor_data->wmi.info_by_id[i] = temp_sensor;
			break;
		}
	}

	dev_dbg(&pdev->dev, "board has %d sensors",
		sensor_data->wmi.sensor_count);

	hwdev = devm_hwmon_device_register_with_info(dev, KBUILD_MODNAME,
						     sensor_data, chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwdev);
}

static int asus_wmi_probe(struct platform_device *pdev)
{
	struct asus_wmi_sensors *sensor_data;
	struct device *dev = &pdev->dev;
	u32 version = 0;

	sensor_data = devm_kzalloc(dev, sizeof(struct asus_wmi_sensors),
				   GFP_KERNEL);
	if (!sensor_data)
		return -ENOMEM;

	if (!wmi_has_guid(ASUSWMI_MONITORING_GUID))
		return -ENODEV;

	if (asus_wmi_get_version(&version))
		return -ENODEV;

	if (asus_wmi_get_item_count(&sensor_data->wmi.sensor_count))
		return -ENODEV;

	if (sensor_data->wmi.sensor_count  <= 0 || version < 2) {
		dev_info(dev, "version: %u with %d sensors is unsupported\n",
			 version, sensor_data->wmi.sensor_count);

		return -ENODEV;
	}

	mutex_init(&sensor_data->lock);

	platform_set_drvdata(pdev, sensor_data);

	return asus_wmi_configure_sensor_setup(pdev,
					       sensor_data);
}

static struct platform_driver asus_wmi_sensors_platform_driver = {
	.driver = {
		.name	= "asus-wmi-sensors",
	},
	.probe = asus_wmi_probe,
};

static struct platform_device *sensors_pdev;

static int __init asus_wmi_init(void)
{
	if (!dmi_check_system(asus_wmi_dmi_table))
		return -ENODEV;

	sensors_pdev = platform_create_bundle(&asus_wmi_sensors_platform_driver,
					      asus_wmi_probe,
					      NULL, 0,
					      NULL, 0);

	return PTR_ERR_OR_ZERO(sensors_pdev);
}
module_init(asus_wmi_init);

static void __exit asus_wmi_exit(void)
{
	platform_device_unregister(sensors_pdev);
	platform_driver_unregister(&asus_wmi_sensors_platform_driver);
}
module_exit(asus_wmi_exit);

MODULE_AUTHOR("Ed Brindley <kernel@maidavale.org>");
MODULE_DESCRIPTION("Asus WMI Sensors Driver");
MODULE_LICENSE("GPL");

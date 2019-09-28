// SPDX-License-Identifier: GPL-2.0+
/*
 * Asus WMI sensors HWMON driver
 *
 * Copyright (C) 2018-2019 Ed Brindley <kernel@maidavale.org>
 */
#define PLATFORM_DRIVER

#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/wmi.h>

MODULE_AUTHOR("Ed Brindley <kernel@maidavale.org>");
MODULE_DESCRIPTION("Asus WMI Sensors Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("3");

#define ASUS_HW_GUID "466747A0-70EC-11DE-8A39-0800200C9A66"

#define CROSSHAIR_6 "CROSSHAIR VI HERO"
#define CROSSHAIR_6_WIFI "ROG CROSSHAIR VI HERO (WI-FI AC)"
#define CROSSHAIR_6_EXTREME "ROG CROSSHAIR VI EXTREME"
#define CROSSHAIR_7 "ROG CROSSHAIR VII HERO"
#define CROSSHAIR_7_WIFI "ROG CROSSHAIR VII HERO (WI-FI)"
#define ZENITH_EXTREME "ROG ZENITH EXTREME"
#define ZENITH_EXTREME_ALPHA "ROG ZENITH EXTREME ALPHA"
#define PRIME_X470_PRO "PRIME X470-PRO"
#define PRIME_X399_A "PRIME X399-A"
#define STRIX_X399_E "ROG STRIX X399-E GAMING"
#define STRIX_B450_E "ROG STRIX B450-E GAMING"
#define STRIX_B450_F "ROG STRIX B450-F GAMING"
#define STRIX_B450_I "ROG STRIX B450-I GAMING"
#define STRIX_X470_I "ROG STRIX X470-I GAMING"
#define STRIX_X470_F "ROG STRIX X470-F GAMING"

#define METHODID_SENSOR_GET_VALUE     		0x52574543
#define METHODID_SENSOR_UPDATE_BUFFER     	0x51574543
#define METHODID_SENSOR_GET_INFO     		0x50574543
#define METHODID_SENSOR_GET_NUMBER     		0x50574572
#define METHODID_SENSOR_GET_BUFFER_ADDRESS  0x50574573
#define METHODID_SENSOR_GET_VERSION     	0x50574574

#define ASUS_WMI_MAX_STR_SIZE	32

#define HWMON_MAX	9

enum asus_wmi_sensor_class {
	VOLTAGE = 0x0,
	TEMPERATURE_C = 0x1,
	FAN_RPM = 0x2,
	CURRENT = 0x3,
	WATER_FLOW = 0x4,
	//BOOL = 0x5 //TODO
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
	//BOOL = 0x2, //TODO
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

struct asus_wmi_sensor_info {
	u32 id;
	int data_type; // asus_wmi_sensor_class e.g. voltage, temp etc
	int location; // asus_wmi_location
	char name[ASUS_WMI_MAX_STR_SIZE];
	int source; // asus_wmi_source
	int type; // asus_wmi_type signed, unsigned etc
	u32 cached_value;
};

struct asus_wmi_sensors {
	#ifdef PLATFORM_DRIVER
	struct platform_driver platform_driver;
 	struct platform_device *platform_device;
	#else
	struct wmi_driver wmi_driver;
	struct wmi_device *wmi_device;
	#endif

	u8 buffer;
	unsigned long source_last_updated[3];	/* in jiffies */
	u8 sensor_count;

	struct mutex lock;
	const struct asus_wmi_sensor_info **info[HWMON_MAX];
	struct asus_wmi_sensor_info **info_by_id;
};

/*
 * Universal method for calling WMI method
 * @method_id: 
 * @args:
 * @output:
 */
static int asus_wmi_call_method(u32 method_id, u32 *args, struct acpi_buffer *output) 
{
	struct acpi_buffer input = {(acpi_size) sizeof(*args), args };

	acpi_status status;
	status = wmi_evaluate_method(ASUS_HW_GUID,
				     0,
					 method_id,
				     &input, output);
	if (ACPI_FAILURE(status)) {
		return -EIO;
	}

	return 0;
}

/*
 * Gets the version of the ASUS sensors interface implemented
 */
static int get_version(u32 *version)
{
	u32 args[] = {0, 0, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(METHODID_SENSOR_GET_VERSION, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER) {
			*version = obj->integer.value;	
		}
	}
	return status;
}

/*
 * Gets the number of sensor items
 */
static int get_item_count(u32 *count) 
{
	u32 args[] = {0, 0, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(METHODID_SENSOR_GET_NUMBER, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER) {
			*count = obj->integer.value;	
		}
	}
	return status;
}

/*
 * For a given sensor item returns details e.g. type (voltage/temperature/fan speed etc), bank etc
 */
static int info(int index, struct asus_wmi_sensor_info *s) 
{
	u32 args[] = {index, 0};
	union acpi_object name_obj, data_type_obj, location_obj, source_obj, type_obj;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;

	int status = asus_wmi_call_method(METHODID_SENSOR_GET_INFO, args, &output);

	if (!status) {
		s->id = index;

		obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_PACKAGE) {
			if(obj->package.count != 5) {
				return 1;
			}
			name_obj = obj->package.elements[0];

			if (name_obj.type != ACPI_TYPE_STRING) {
				return 1;
			}
			strncpy(s->name, name_obj.string.pointer, sizeof s->name - 1);

			data_type_obj = obj->package.elements[1];

			if (data_type_obj.type != ACPI_TYPE_INTEGER) {
				return 1;
			}
			s->data_type = data_type_obj.integer.value;

			location_obj = obj->package.elements[2];

			if (location_obj.type != ACPI_TYPE_INTEGER) {
				return 1;
			}
			s->location = location_obj.integer.value;

			source_obj = obj->package.elements[3];

			if (source_obj.type != ACPI_TYPE_INTEGER) {
				return 1;
			}
			s->source = source_obj.integer.value;

			type_obj = obj->package.elements[4];

			if (type_obj.type != ACPI_TYPE_INTEGER) {
				return 1;
			}
			s->type = type_obj.integer.value;
		}
	}
	return status;
}

static int update_buffer(u8 source)
{
	u32 args[] = {source, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	return asus_wmi_call_method(METHODID_SENSOR_UPDATE_BUFFER, args, &output);
}

static int get_sensor_value(u8 index, u32 *value)
{
	u32 args[] = {index, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(METHODID_SENSOR_GET_VALUE, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER) {
			*value = obj->integer.value;
		}
	}
	return status;
}

static void update_values_for_source(u8 source, struct asus_wmi_sensors *asus_wmi_sensors) {
	int ret = 0;
	int value = 0;
	int i;
	struct asus_wmi_sensor_info *sensor;

	for (i = 0; i < asus_wmi_sensors->sensor_count;i++) {
		sensor = asus_wmi_sensors->info_by_id[i];
		if(sensor && sensor->source == source) {
			ret = get_sensor_value(sensor->id, &value);
			if (!ret) {
				sensor->cached_value = value;
			}
		}
	}
}

static int scale_sensor_value(u32 value, int data_type) {
	switch (data_type) {
	case VOLTAGE:
		return DIV_ROUND_CLOSEST(value, 1000);
	case TEMPERATURE_C:
		return value * 1000;
	case CURRENT:
		return value * 1000;
	}
	return value; // FAN_RPM and WATER_FLOW don't need scaling
}

static int get_cached_value_or_update(const struct asus_wmi_sensor_info *sensor, struct asus_wmi_sensors *asus_wmi_sensors, u32 *value) {
	int ret;

	if (time_after(jiffies, asus_wmi_sensors->source_last_updated[sensor->source] + HZ)) {
		ret = update_buffer(sensor->source);

		if (ret) {
			pr_err("update_buffer failure\n");
			return -EIO;
		}
		asus_wmi_sensors->buffer = sensor->source;

		update_values_for_source(sensor->source, asus_wmi_sensors);
		asus_wmi_sensors->source_last_updated[sensor->source] = jiffies;
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

	struct asus_wmi_sensors *asus_wmi_sensors = dev_get_drvdata(dev);

	sensor = *(asus_wmi_sensors->info[type] + channel);

	mutex_lock(&asus_wmi_sensors->lock);

	ret = get_cached_value_or_update(sensor, asus_wmi_sensors, &value);
	mutex_unlock(&asus_wmi_sensors->lock);

	if (!ret) {
		*val = scale_sensor_value(value, sensor->data_type);
	}

	return ret;
}

static int
asus_wmi_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, const char **str)
{
	const struct asus_wmi_sensor_info *sensor;
	struct asus_wmi_sensors *asus_wmi_sensors = dev_get_drvdata(dev);

	sensor = *(asus_wmi_sensors->info[type] + channel);
	*str = sensor->name;

	return 0;
}

static umode_t
asus_wmi_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type,
		      u32 attr, int channel)
{
	const struct asus_wmi_sensor_info *sensor;
	const struct asus_wmi_sensors *asus_wmi_sensors = drvdata;

	sensor = *(asus_wmi_sensors->info[type] + channel);
	if (sensor && sensor->name)
		return S_IRUGO;

	return 0;
}

static int asus_wmi_hwmon_add_chan_info(struct hwmon_channel_info *asus_wmi_hwmon_chan,
				    struct device *dev, int num,
				    enum hwmon_sensor_types type, u32 config)
{
	int i;
	u32 *cfg = devm_kcalloc(dev, num + 1, sizeof(*cfg), GFP_KERNEL);

	if (!cfg)
		return -ENOMEM;

	asus_wmi_hwmon_chan->type = type;
	asus_wmi_hwmon_chan->config = cfg;
	for (i = 0; i < num; i++, cfg++)
		*cfg = config;

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

static int configure_sensor_setup(struct asus_wmi_sensors *asus_wmi_sensors)
{
	int err;
	int i, idx;
	int nr_count[HWMON_MAX] = {0}, nr_types = 0;
	u32 nr_sensors = 0;
	struct device *hwdev; 
	#ifdef PLATFORM_DRIVER
	struct device *dev = &asus_wmi_sensors->platform_device->dev;
	#else
	struct device *dev = &asus_wmi_sensors->wmi_device->dev;
	#endif
	struct hwmon_channel_info *asus_wmi_hwmon_chan;
	struct asus_wmi_sensor_info *temp_sensor;
	enum hwmon_sensor_types type;
	const struct hwmon_channel_info **ptr_asus_wmi_ci;
	const struct hwmon_chip_info *chip_info;

	asus_wmi_sensors->buffer = -1;
	mutex_init(&asus_wmi_sensors->lock);

	temp_sensor = devm_kcalloc(dev, 1, sizeof(*temp_sensor), GFP_KERNEL);
	if (!temp_sensor) {
		pr_err("Alloc fail\n");
		return -ENOMEM;
	}

	get_item_count(&nr_sensors);
	asus_wmi_sensors->sensor_count = nr_sensors;

	pr_debug("sensor count %u\n", nr_sensors);

	for (i = 0; i < nr_sensors; i++) {
		err = info(i, temp_sensor);
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

	asus_wmi_hwmon_chan = devm_kcalloc(dev, nr_types, sizeof(*asus_wmi_hwmon_chan),
				       GFP_KERNEL);
	if (!asus_wmi_hwmon_chan)
		return -ENOMEM;

	ptr_asus_wmi_ci = devm_kcalloc(dev, nr_types + 1, sizeof(*ptr_asus_wmi_ci),
				   GFP_KERNEL);
	if (!ptr_asus_wmi_ci)
		return -ENOMEM;

	asus_wmi_chip_info.info = ptr_asus_wmi_ci;
	chip_info = &asus_wmi_chip_info;
	
	asus_wmi_sensors->info_by_id =
		devm_kcalloc(dev, nr_sensors, sizeof(*asus_wmi_sensors->info_by_id), GFP_KERNEL);

	if (!asus_wmi_sensors->info_by_id)
		return -ENOMEM;

	for (type = 0; type < HWMON_MAX; type++) {
		if (!nr_count[type])
			continue;

		asus_wmi_hwmon_add_chan_info(asus_wmi_hwmon_chan, dev, nr_count[type],
					 type, hwmon_attributes[type]);
		*ptr_asus_wmi_ci++ = asus_wmi_hwmon_chan++;

		asus_wmi_sensors->info[type] =
			devm_kcalloc(dev, nr_count[type], sizeof(*asus_wmi_sensors->info), GFP_KERNEL);
		if (!asus_wmi_sensors->info[type])
			return -ENOMEM;
	}

	for (i = nr_sensors - 1; i >= 0 ; i--) {
		temp_sensor = devm_kzalloc(dev, sizeof(*temp_sensor), GFP_KERNEL);
		if (!temp_sensor) {
			pr_err("asuswmisensors: Alloc fail\n");
			return -ENOMEM;
		}

		err = info(i, temp_sensor);
		if (err) {
			pr_err("asuswmisensors: sensor error\n");
			continue;
		}

		pr_debug("asuswmisensors: setting sensor info\n");

		switch (temp_sensor->data_type) {
		case TEMPERATURE_C:
		case VOLTAGE:
		case CURRENT:
		case FAN_RPM:
		case WATER_FLOW:
			type = asus_data_types[temp_sensor->data_type];
			idx = --nr_count[type];
			*(asus_wmi_sensors->info[type] + idx) = temp_sensor;
			asus_wmi_sensors->info_by_id[i] = temp_sensor;
			break;
		}
	}

	hwdev = devm_hwmon_device_register_with_info(dev, "asuswmisensors",
						     asus_wmi_sensors, chip_info,
						     NULL);

	return PTR_ERR_OR_ZERO(hwdev);
}

static int is_board_supported(void) {
	const char *board_vendor, *board_name, *bios_version;
	u32 version = 0;

	board_vendor = dmi_get_system_info(DMI_BOARD_VENDOR);
	board_name = dmi_get_system_info(DMI_BOARD_NAME);
	bios_version = dmi_get_system_info(DMI_BIOS_VERSION);

	if(get_version(&version)) {
		pr_err("asuswmisensors: Error getting version\n");
		return -ENODEV;
	}

	if (board_vendor && board_name && bios_version) {
		pr_info("asuswmisensors: Vendor: %s Board: %s BIOS version: %s WMI version: %u", board_vendor, board_name, bios_version, version);

		if (version >= 3 || (version >= 2 && (
			strcmp(board_name, CROSSHAIR_7_WIFI) == 0 ||
			strcmp(board_name, CROSSHAIR_7) == 0 ||
			strcmp(board_name, CROSSHAIR_6_WIFI) == 0 ||
			strcmp(board_name, CROSSHAIR_6) == 0 ||
			strcmp(board_name, CROSSHAIR_6_EXTREME) == 0 ||
			strcmp(board_name, ZENITH_EXTREME) == 0 ||
			strcmp(board_name, ZENITH_EXTREME_ALPHA) == 0 ||
			strcmp(board_name, PRIME_X399_A) == 0 ||
			strcmp(board_name, PRIME_X470_PRO) == 0 ||
			strcmp(board_name, STRIX_X399_E) == 0 ||
			strcmp(board_name, STRIX_B450_E) == 0 ||
			strcmp(board_name, STRIX_B450_F) == 0 ||
			strcmp(board_name, STRIX_B450_I) == 0 ||
			strcmp(board_name, STRIX_X470_I) == 0 ||
			strcmp(board_name, STRIX_X470_F) == 0))) {

			pr_info("asuswmisensors: Supported board");
			return 0;
		}
	}
	pr_info("asuswmisensors: Unsupported board");
	return -ENODEV;
}

#ifndef PLATFORM_DRIVER

static int asus_wmi_sensors_probe(struct wmi_device *wdev)
{
	struct device *dev = &wdev->dev;
	struct asus_wmi_sensors *asus_wmi_sensors;

	pr_info("asuswmisensors: WMI GUID matched - probing");

	if (is_board_supported()) {
		return -ENODEV;
	}

	asus_wmi_sensors = devm_kzalloc(dev, sizeof(struct asus_wmi_sensors), GFP_KERNEL);
	if (!asus_wmi_sensors)
		return -ENOMEM;

	asus_wmi_sensors->wmi_device = wdev;

	dev_set_drvdata(dev, asus_wmi_sensors);
	return configure_sensor_setup(asus_wmi_sensors);
}

static int asus_wmi_sensors_remove(struct wmi_device *wdev)
{
	struct asus_wmi_sensors *asus;

	asus = dev_get_drvdata(&wdev->dev);

	return 0;
}

static const struct wmi_device_id asus_wmi_sensors_id_table[] = {
	{ .guid_string = ASUS_HW_GUID },
	{ },
};

static struct wmi_driver asus_wmi_sensors = {
	.driver = {
		.name = "asus-wmi-sensors",
	},
	.probe = asus_wmi_sensors_probe,
	.remove = asus_wmi_sensors_remove,
	.id_table = asus_wmi_sensors_id_table,
};

module_wmi_driver(asus_wmi_sensors);
#endif

#ifdef PLATFORM_DRIVER
static struct platform_device *asus_wmi_sensors_platform_device;

static int asus_wmi_probe(struct platform_device *pdev)
{
	if (!wmi_has_guid(ASUS_HW_GUID)) {
		pr_info("asuswmisensors: ASUSHW GUID not found\n");
		return -ENODEV;
	}

	if (is_board_supported()) {
		return -ENODEV;
	}

	pr_info("asuswmisensors: ASUS WMI sensors driver loaded\n");
	return 0;
}

static struct platform_driver asus_wmi_sensors_platform_driver = {
	.driver = {
		.name	= "asus-wmi-sensors",
	},
	.probe		= asus_wmi_probe
};

static int __init asus_wmi_init(void)
{
	struct asus_wmi_sensors *asus_wmi_sensors;

	asus_wmi_sensors_platform_device = platform_create_bundle(&asus_wmi_sensors_platform_driver,
						 asus_wmi_probe,
						 NULL, 0, NULL, 0);

	if (IS_ERR(asus_wmi_sensors_platform_device))
		return PTR_ERR(asus_wmi_sensors_platform_device);

	asus_wmi_sensors = devm_kzalloc(&asus_wmi_sensors_platform_device->dev, sizeof(struct asus_wmi_sensors), GFP_KERNEL);
	if (!asus_wmi_sensors)
		return -ENOMEM;

	asus_wmi_sensors->platform_device = asus_wmi_sensors_platform_device;
	asus_wmi_sensors->platform_driver = asus_wmi_sensors_platform_driver;

	platform_set_drvdata(asus_wmi_sensors->platform_device, asus_wmi_sensors);
	
	return configure_sensor_setup(asus_wmi_sensors);
}

static void __exit asus_wmi_exit(void)
 {
 	platform_device_unregister(asus_wmi_sensors_platform_device);
 	platform_driver_unregister(&asus_wmi_sensors_platform_driver);
 }
 
 module_init(asus_wmi_init);
 module_exit(asus_wmi_exit);
 #endif

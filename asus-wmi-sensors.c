#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/wmi.h>

MODULE_AUTHOR("Ed Brindley <kernel@maidavale.org>");
MODULE_DESCRIPTION("Asus WMI Sensors Driver");
MODULE_LICENSE("GPL");

#define ASUS_HW_GUID "466747A0-70EC-11DE-8A39-0800200C9A66"

#define sensor_get_value     1381451075
#define sensor_update_buffer     1364673859
#define sensor_get_info     1347896643
#define sensor_get_number     1347896690
#define sensor_get_buffer_address     1347896691
#define sensor_get_version     1347896692

#define ASUS_WMI_MAX_STR_SIZE	32

enum asus_wmi_sensor_class {
	VOLTAGE = 0x0,
	TEMPERATURE_C = 0x1,
	FAN_RPM = 0x2,
	CURRENT = 0x3,
	WATER_FLOW = 0x4,
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
	int data_type;
	int location;
	char name[ASUS_WMI_MAX_STR_SIZE];
	int source;
	int type;
};

struct asus_wmi_sensors {
	struct wmi_device *wdev;
	struct wmi_driver *wmi_driver;

	const struct asus_wmi_sensor_info **info[hwmon_max];
};

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

static int get_version(u32 *version)
{
	u32 args[] = {0, 0, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(sensor_get_version, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER) {
			*version = obj->integer.value;	
		}
	}
	return status;
}

static int get_item_count(u32 *count) 
{
	u32 args[] = {0, 0, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(sensor_get_number, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER) {
			*count = obj->integer.value;	
		}
	}
	return status;
}

static int info(int index, struct asus_wmi_sensor_info *s) 
{
	u32 args[] = {index, 0};
	union acpi_object name_obj, data_type_obj, location_obj, source_obj, type_obj;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;

	int status = asus_wmi_call_method(sensor_get_info, args, &output);

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
			// u32 length = sub_obj.string.length;

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

static int update_buffer(int source)
{
	u32 args[] = {source, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	return asus_wmi_call_method(sensor_update_buffer, args, &output);
}

static int getValue(int index, u32 *value)
{
	u32 args[] = {index, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(sensor_get_value, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER) {
			*value = obj->integer.value;
		}
	}
	return status;
}

static int asus_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	int ret;
	u32 value = 0;
	const struct asus_wmi_sensor_info *sensor;

	struct asus_wmi_sensors *asus_wmi_sensors = dev_get_drvdata(dev);

	sensor = *(asus_wmi_sensors->info[type] + channel);
	ret = update_buffer(sensor->source);

	if (ret)
		return ret;

	ret = getValue(sensor->id, &value);

	if (!ret)
		*val = value;

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

static int asus_wmi_sensors_remove(struct wmi_device *wdev)
{
	struct asus_wmi_sensors *asus;

	asus = dev_get_drvdata(&wdev->dev);

	// kfree(asus);
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

static int asus_wmi_sensors_probe(struct wmi_device *wdev)
{
	struct wmi_driver *wdrv = container_of(wdev->dev.driver, struct wmi_driver, driver);
	struct asus_wmi_sensors *asus_wmi_sensors;
	int err;
	int i, idx;
	int nr_count[hwmon_max] = {0}, nr_types = 0;
	u32 nr_sensors = 0;
	struct device *hwdev, *dev = &wdev->dev;
	struct hwmon_channel_info *asus_wmi_hwmon_chan;
	struct asus_wmi_sensor_info *temp_sensor;
	enum hwmon_sensor_types type;
	const struct hwmon_channel_info **ptr_asus_wmi_ci;
	const struct hwmon_chip_info *chip_info;

	asus_wmi_sensors = devm_kzalloc(dev, sizeof(struct asus_wmi_sensors), GFP_KERNEL);
	if (!asus_wmi_sensors)
		return -ENOMEM;

	asus_wmi_sensors->wmi_driver = wdrv;
	asus_wmi_sensors->wdev = wdev;

	dev_set_drvdata(&wdev->dev, asus_wmi_sensors);

	temp_sensor = devm_kcalloc(dev, 1, sizeof(*temp_sensor), GFP_KERNEL);
	if (!temp_sensor) {
		pr_info("Alloc fail\n");
		return -ENOMEM;
	}

	get_item_count(&nr_sensors);
	
	pr_info("item count %u\n", nr_sensors);

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
	
	for (type = 0; type < hwmon_max; type++) {
		if (!nr_count[type])
			continue;
		pr_info("setting type info for type %u count %u \n", type, nr_count[type]);

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
			pr_info("Alloc fail\n");
			return -ENOMEM;
		}

		err = info(i, temp_sensor);
		if (err) {
			pr_info("sensor error\n");
			continue;
		}

		pr_info("setting sensor info\n");

		switch (temp_sensor->data_type) {
		case TEMPERATURE_C:
		case VOLTAGE:
		case CURRENT:
		case FAN_RPM:
		case WATER_FLOW:
			type = asus_data_types[temp_sensor->data_type];
			idx = --nr_count[type];
			*(asus_wmi_sensors->info[type] + idx) = temp_sensor;
			break;
		}
	}

	hwdev = devm_hwmon_device_register_with_info(dev, "asushwwmi",
						     asus_wmi_sensors, chip_info,
						     NULL);

	return PTR_ERR_OR_ZERO(hwdev);
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

// SPDX-License-Identifier: GPL-2.0+
/*
 * HWMON driver for ASUS motherboards that publish some sensor values
 * via the embedded controller registers
 *
 * Copyright (C) 2021 Eugene Shalygin <eugene.shalygin@gmail.com>
 * Copyright (C) 2018-2019 Ed Brindley <kernel@maidavale.org>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define DRVNAME "asus_wmi_sensors"

#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/wmi.h>

#define ASUSWMI_MONITORING_GUID		"466747A0-70EC-11DE-8A39-0800200C9A66"
#define ASUSWMI_METHODID_BLOCK_READ_EC		0x42524543 /* BREC */
#define ASUSWMI_METHODID_GET_VALUE	0x52574543
#define ASUSWMI_METHODID_UPDATE_BUFFER	0x51574543
#define ASUSWMI_METHODID_GET_INFO	0x50574543
#define ASUSWMI_METHODID_GET_NUMBER		0x50574572
#define ASUSWMI_METHODID_GET_BUFFER_ADDRESS	0x50574573
#define ASUSWMI_METHODID_GET_VERSION		0x50574574

#define ASUS_WMI_MAX_STR_SIZE	32

#define HWMON_MAX	9

#define ASUS_WMI_BLOCK_READ_REGISTERS_MAX 0x10 /* from the ASUS DSDT source */
/* from the ASUS_WMI_BLOCK_READ_REGISTERS_MAX value */
#define ASUS_WMI_MAX_BUF_LEN 0x80
#define MAX_SENSOR_LABEL_LENGTH 0x10

#define ASUSWMI_SENSORS_MAX 11
#define ASUS_EC_KNOWN_EC_REGISTERS 14
#define HWMON_MAX	9

enum asus_wmi_ec_board {
	BOARD_R_C8H, // ROG Crosshair VIII Hero
	BOARD_R_C8DH, // ROG Crosshair VIII Dark Hero
	BOARD_R_C8F, // ROG Crosshair VIII Formula
	BOARD_RS_X570_E_G, // ROG STRIX X570-E GAMING
	BOARD_RS_B550_E_G, // ROG STRIX B550-E GAMING
};

/* boards with EC support */
static const char *const asus_wmi_ec_boards_names[] = {
	[BOARD_R_C8H] = "ROG CROSSHAIR VIII HERO",
	[BOARD_R_C8DH] = "ROG CROSSHAIR VIII DARK HERO",
	[BOARD_R_C8F] = "ROG CROSSHAIR VIII FORMULA",
	[BOARD_RS_X570_E_G] = "ROG STRIX X570-E GAMING",
	[BOARD_RS_B550_E_G] = "ROG STRIX B550-E GAMING",
};

/* boards with wmi sensors support */
static const char *const asus_wmi_boards_names[] = {
	"ROG CROSSHAIR VII HERO (WI-FI)",
	"ROG CROSSHAIR VII HERO",
	"ROG CROSSHAIR VI HERO (WI-FI AC)",
	"CROSSHAIR VI HERO",
	"ROG CROSSHAIR VI EXTREME",
	"ROG ZENITH EXTREME",
	"ROG ZENITH EXTREME ALPHA",
	"PRIME X399-A",
	"PRIME X470-PRO",
	"ROG STRIX X399-E GAMING",
	"ROG STRIX B450-E GAMING",
	"ROG STRIX B450-F GAMING",
	"ROG STRIX B450-I GAMING",
	"ROG STRIX X470-I GAMING",
	"ROG STRIX X470-F GAMING",
};

enum asus_wmi_sensor_class {
	VOLTAGE = 0x0,
	TEMPERATURE_C = 0x1,
	FAN_RPM = 0x2,
	CURRENT = 0x3,
	WATER_FLOW = 0x4,
	// BOOL = 0x5 //TODO
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
	// BOOL = 0x2, //TODO
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

union asus_wmi_ec_sensor_address {
	u32 value;
	struct {
		u8 index;
		u8 bank;
		u8 size;
		u8 dummy;
	} addr;
};

struct asus_wmi_ec_sensor_info {
	char label[MAX_SENSOR_LABEL_LENGTH];
	enum hwmon_sensor_types type;
	union asus_wmi_ec_sensor_address addr;
	u32 cached_value;
};

struct asus_wmi_ec_info {
	struct asus_wmi_ec_sensor_info sensors[ASUSWMI_SENSORS_MAX];
	/* UTF-16 string to pass to BRxx() WMI function */
	char read_arg[((ASUS_WMI_BLOCK_READ_REGISTERS_MAX * 4) + 1) * 2];
	u8 read_buffer[ASUS_WMI_BLOCK_READ_REGISTERS_MAX];
	u8 nr_sensors; /* number of board EC sensors */
	/* number of EC registers to read (sensor might span more than 1 register) */
	u8 nr_registers;
	unsigned long last_updated; /* in jiffies */
};

struct asus_wmi_wmi_info {
	u8 buffer;
	unsigned long source_last_updated[3];	/* in jiffies */
	u8 sensor_count;

	const struct asus_wmi_sensor_info **info[HWMON_MAX];
	struct asus_wmi_sensor_info **info_by_id;
};

struct asus_wmi_sensors {
	/* lock access to instrnal cache */
	struct mutex lock;
	struct asus_wmi_ec_info ec;
	struct asus_wmi_wmi_info wmi;

	int ec_board;
	int wmi_board;
};

struct asus_wmi_data {
	int ec_board;
	int wmi_board;
	int wmi_count;
};

static inline union asus_wmi_ec_sensor_address asus_wmi_ec_make_sensor_address(u8 size,
									       u8 bank,
									       u8 index)
{
	union asus_wmi_ec_sensor_address res;

	res.value = (size << 16) + (bank << 8) + index;
	return res;
}

static inline void asus_wmi_ec_set_sensor_info(struct asus_wmi_ec_sensor_info *sensor_info,
					       const char *label,
					       enum hwmon_sensor_types type,
					       union asus_wmi_ec_sensor_address addr,
					       u8 *nr_regs)
{
	sensor_info->type = type;
	strcpy(sensor_info->label, label);
	sensor_info->cached_value = 0;
	sensor_info->addr.value = addr.value;
	*nr_regs += sensor_info->addr.addr.size;
}

static void asus_wmi_ec_fill_board_sensors(struct asus_wmi_ec_info *ec, int board)
{
	struct asus_wmi_ec_sensor_info *si;

	si = ec->sensors;
	ec->nr_registers = 0;

	switch (board) {
	case BOARD_RS_B550_E_G:
	case BOARD_RS_X570_E_G:
	case BOARD_R_C8H:
	case BOARD_R_C8DH:
	case BOARD_R_C8F:
		asus_wmi_ec_set_sensor_info(si++, "Chipset", hwmon_temp,
					    asus_wmi_ec_make_sensor_address(1, 0x00, 0x3A),
					    &ec->nr_registers);
		asus_wmi_ec_set_sensor_info(si++, "CPU", hwmon_temp,
					    asus_wmi_ec_make_sensor_address(1, 0x00, 0x3B),
					    &ec->nr_registers);
		asus_wmi_ec_set_sensor_info(si++, "Motherboard", hwmon_temp,
					    asus_wmi_ec_make_sensor_address(1, 0x00, 0x3C),
					    &ec->nr_registers);
		asus_wmi_ec_set_sensor_info(si++, "T_Sensor", hwmon_temp,
					    asus_wmi_ec_make_sensor_address(1, 0x00, 0x3D),
					    &ec->nr_registers);
		asus_wmi_ec_set_sensor_info(si++, "VRM", hwmon_temp,
					    asus_wmi_ec_make_sensor_address(1, 0x00, 0x3E),
					    &ec->nr_registers);
	}

	switch (board) {
	case BOARD_RS_X570_E_G:
	case BOARD_R_C8H:
	case BOARD_R_C8DH:
	case BOARD_R_C8F:
		asus_wmi_ec_set_sensor_info(si++, "CPU_Opt", hwmon_fan,
					    asus_wmi_ec_make_sensor_address(2, 0x00, 0xB0),
					    &ec->nr_registers);
		asus_wmi_ec_set_sensor_info(si++, "CPU", hwmon_curr,
					    asus_wmi_ec_make_sensor_address(1, 0x00, 0xF4),
					    &ec->nr_registers);
	}

	switch (board) {
	case BOARD_RS_X570_E_G:
	case BOARD_R_C8H:
	case BOARD_R_C8F:
		asus_wmi_ec_set_sensor_info(si++, "Chipset", hwmon_fan,
					    asus_wmi_ec_make_sensor_address(2, 0x00, 0xB4),
					    &ec->nr_registers);
	}

	switch (board) {
	case BOARD_R_C8H:
	case BOARD_R_C8DH:
	case BOARD_R_C8F:
		asus_wmi_ec_set_sensor_info(si++, "Water", hwmon_fan,
					    asus_wmi_ec_make_sensor_address(2, 0x00, 0xBC),
					    &ec->nr_registers);
		asus_wmi_ec_set_sensor_info(si++, "Water_In", hwmon_temp,
					    asus_wmi_ec_make_sensor_address(1, 0x01, 0x00),
					    &ec->nr_registers);
		asus_wmi_ec_set_sensor_info(si++, "Water_Out", hwmon_temp,
					    asus_wmi_ec_make_sensor_address(1, 0x01, 0x01),
					    &ec->nr_registers);
	}

	ec->nr_sensors = si - ec->sensors;
}

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
	u32 args[] = {0, 0, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(ASUSWMI_METHODID_GET_VERSION, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER)
			*version = obj->integer.value;
	}
	return status;
}

/*
 * Gets the number of sensor items
 */
static int asus_wmi_get_item_count(u32 *count)
{
	u32 args[] = {0, 0, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(ASUSWMI_METHODID_GET_NUMBER, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER)
			*count = obj->integer.value;
	}
	return status;
}

/*
 * The next four functions converts to/from BRxx string argument format
 * The format of the string is as follows:
 * The string consists of two-byte UTF-16 characters
 * The value of the very first byte int the string is equal to the total length
 * of the next string in bytes, thus excluding the first two-byte character
 * The rest of the string encodes pairs of (bank, index) pairs, where both
 * values are byte-long (0x00 to 0xFF)
 * Numbers are encoded as UTF-16 hex values
 */

static inline char *asus_wmi_ec_hex_utf_16_le_pack(char *buf, u8 byte)
{
	*buf++ = hex_asc_hi(byte);
	*buf++ = 0;
	*buf++ = hex_asc_lo(byte);
	*buf++ = 0;
	return buf;
}

static void asus_wmi_ec_decode_reply_buffer(const u8 *inp, u8 *out)
{
	u8 len = ACPI_MIN(ASUS_WMI_MAX_BUF_LEN, inp[0] / 4);
	const u8 *data = inp + 2;
	u8 i;

	for (i = 0; i < len; ++i, data += 4)
		out[i] = (hex_to_bin(data[0]) << 4) + hex_to_bin(data[2]);
}

static void asus_wmi_ec_encode_registers(u16 *registers, u8 len, char *out)
{
	u8 i;

	// assert(len <= 30)
	*out++ = len * 8;
	*out++ = 0;
	for (i = 0; i < len; ++i) {
		out = asus_wmi_ec_hex_utf_16_le_pack(out, (registers[i] & 0xFF00) >> 8);
		out = asus_wmi_ec_hex_utf_16_le_pack(out, (registers[i] & 0x00FF));
	}
}

static void asus_wmi_ec_make_block_read_query(struct asus_wmi_ec_info *ec)
{
	u16 registers[ASUS_EC_KNOWN_EC_REGISTERS];
	u8 i, j, register_idx = 0;

	/* if we can get values for all the registers in a single query,
	 * the query will not change from call to call
	 */
	if (ec->nr_registers <= ASUS_WMI_BLOCK_READ_REGISTERS_MAX &&
	    ec->read_arg[0] > 0) {
		/* no need to update */
		return;
	}

	for (i = 0; i < ec->nr_sensors; ++i) {
		for (j = 0; j < ec->sensors[i].addr.addr.size;
		     ++j, ++register_idx) {
			registers[register_idx] =
				(ec->sensors[i].addr.addr.bank << 8) +
				ec->sensors[i].addr.addr.index + j;
		}
	}

	asus_wmi_ec_encode_registers(registers, ec->nr_registers, ec->read_arg);
}

static int asus_wmi_ec_block_read(u32 method_id, const char *query, u8 *out)
{
	struct acpi_buffer input;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER,
				      NULL }; // TODO use pre-allocated buffer
	acpi_status status;
	union acpi_object *obj;

	/* the first byte of the BRxx() argument string has to be the string size */
	input.length = (acpi_size)query[0] + 2;
	input.pointer = (void *)query;
	status = wmi_evaluate_method(ASUSWMI_MONITORING_GUID, 0, method_id, &input,
				     &output);

	if (ACPI_FAILURE(status)) {
		acpi_os_free(output.pointer);
		return -EIO;
	}

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER) {
		pr_err("unexpected reply type from ASUS ACPI code");
		acpi_os_free(output.pointer);
		return -EIO;
	}
	asus_wmi_ec_decode_reply_buffer(obj->buffer.pointer, out);
	acpi_os_free(output.pointer);
	return 0;
}

static int asus_wmi_ec_update_ec_sensors(struct asus_wmi_ec_info *ec)
{
	struct asus_wmi_ec_sensor_info *si;
	u32 value;
	int status;
	u8 i_sensor, read_reg_ct, i_sensor_register;

	asus_wmi_ec_make_block_read_query(ec);
	status = asus_wmi_ec_block_read(ASUSWMI_METHODID_BLOCK_READ_EC,
					ec->read_arg,
					ec->read_buffer);
	if (status)
		return status;

	read_reg_ct = 0;
	for (i_sensor = 0; i_sensor < ec->nr_sensors; ++i_sensor) {
		si = &ec->sensors[i_sensor];
		value = ec->read_buffer[read_reg_ct++];
		for (i_sensor_register = 1;
		     i_sensor_register < si->addr.addr.size;
		     ++i_sensor_register) {
			value <<= 8;
			value += ec->read_buffer[read_reg_ct++];
		}
		si->cached_value = value;
	}
	return 0;
}

static int asus_wmi_ec_scale_sensor_value(u32 value, int data_type)
{
	switch (data_type) {
	case hwmon_curr:
	case hwmon_temp:
	case hwmon_in:
		return value * 1000;
	default:
		return value;
	}
}

static u8 asus_wmi_ec_find_sensor_index(const struct asus_wmi_ec_info *ec,
					enum hwmon_sensor_types type, int channel)
{
	u8 i;

	for (i = 0; i < ec->nr_sensors; ++i) {
		if (ec->sensors[i].type == type) {
			if (channel == 0)
				return i;

			--channel;
		}
	}
	return 0xFF;
}

static int asus_wmi_ec_get_cached_value_or_update(int sensor_index,
						  struct asus_wmi_sensors *state,
						  u32 *value)
{
	int ret;

	if (time_after(jiffies, state->ec.last_updated + HZ)) {
		ret = asus_wmi_ec_update_ec_sensors(&state->ec);

		if (ret) {
			pr_err("asus_wmi_ec_update_ec_sensors() failure\n");
			return -EIO;
		}

		state->ec.last_updated = jiffies;
	}

	*value = state->ec.sensors[sensor_index].cached_value;
	return 0;
}

/*
 * Now follow the functions that implement the hwmon interface
 */

static int asus_wmi_ec_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
				  u32 attr, int channel, long *val)
{
	int ret;
	u32 value = 0;
	struct asus_wmi_sensors *sensor_data = dev_get_drvdata(dev);

	u8 sidx = asus_wmi_ec_find_sensor_index(&sensor_data->ec, type, channel);

	mutex_lock(&sensor_data->lock);

	ret = asus_wmi_ec_get_cached_value_or_update(sidx, sensor_data, &value);
	mutex_unlock(&sensor_data->lock);

	if (!ret)
		*val = asus_wmi_ec_scale_sensor_value(value, sensor_data->ec.sensors[sidx].type);

	return ret;
}

static int asus_wmi_ec_hwmon_read_string(struct device *dev,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel, const char **str)
{
	struct asus_wmi_sensors *sensor_data = dev_get_drvdata(dev);

	u8 sensor_index = asus_wmi_ec_find_sensor_index(&sensor_data->ec, type, channel);
	*str = sensor_data->ec.sensors[sensor_index].label;

	return 0;
}

static umode_t asus_wmi_ec_hwmon_is_visible(const void *drvdata,
					    enum hwmon_sensor_types type, u32 attr,
					    int channel)
{
	const struct asus_wmi_sensors *sensor_data = drvdata;

	return asus_wmi_ec_find_sensor_index(&sensor_data->ec, type, channel) != 0xFF ?
			     0444 :
			     0;
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

static const struct hwmon_ops asus_wmi_ec_hwmon_ops = {
	.is_visible = asus_wmi_ec_hwmon_is_visible,
	.read = asus_wmi_ec_hwmon_read,
	.read_string = asus_wmi_ec_hwmon_read_string,
};

static struct hwmon_chip_info asus_wmi_ec_chip_info = {
	.ops = &asus_wmi_ec_hwmon_ops,
	.info = NULL,
};

static int asus_wmi_ec_configure_sensor_setup(struct platform_device *pdev,
					      struct asus_wmi_sensors *sensor_data)
{
	int i;
	int nr_count[HWMON_MAX] = { 0 }, nr_types = 0;
	struct device *hwdev;
	struct device *dev = &pdev->dev;
	struct hwmon_channel_info *asus_wmi_hwmon_chan;
	const struct hwmon_channel_info **ptr_asus_wmi_ci;
	const struct hwmon_chip_info *chip_info;
	const struct asus_wmi_ec_sensor_info *si;
	enum hwmon_sensor_types type;

	if (sensor_data->ec_board < 0)
		return 0;

	asus_wmi_ec_fill_board_sensors(&sensor_data->ec, sensor_data->ec_board);

	if (!sensor_data->ec.nr_sensors)
		return -ENODEV;

	for (i = 0; i < sensor_data->ec.nr_sensors; ++i) {
		si = &sensor_data->ec.sensors[i];
		if (!nr_count[si->type])
			++nr_types;
		++nr_count[si->type];
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

	asus_wmi_ec_chip_info.info = ptr_asus_wmi_ci;
	chip_info = &asus_wmi_ec_chip_info;

	for (type = 0; type < HWMON_MAX; type++) {
		if (!nr_count[type])
			continue;

		asus_wmi_hwmon_add_chan_info(asus_wmi_hwmon_chan, dev,
					     nr_count[type], type,
					     hwmon_attributes[type]);
		*ptr_asus_wmi_ci++ = asus_wmi_hwmon_chan++;
	}

	pr_info("%s board has %d EC sensors that span %d registers",
		asus_wmi_ec_boards_names[sensor_data->ec_board],
		sensor_data->ec.nr_sensors,
		sensor_data->ec.nr_registers);

	hwdev = devm_hwmon_device_register_with_info(dev, "asuswmiecsensors",
						     sensor_data, chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwdev);
}

/*
 * For a given sensor item returns details e.g. type (voltage/temperature/fan speed etc), bank etc
 */
static int asus_wmi_sensor_info(int index, struct asus_wmi_sensor_info *s)
{
	u32 args[] = {index, 0};
	union acpi_object name_obj, data_type_obj, location_obj, source_obj, type_obj;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;

	int status = asus_wmi_call_method(ASUSWMI_METHODID_GET_INFO, args, &output);

	if (!status) {
		s->id = index;

		obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_PACKAGE) {
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
		}
	}
	return status;
}

static int asus_wmi_update_buffer(u8 source)
{
	u32 args[] = {source, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	return asus_wmi_call_method(ASUSWMI_METHODID_UPDATE_BUFFER, args, &output);
}

static int asus_wmi_get_sensor_value(u8 index, u32 *value)
{
	u32 args[] = {index, 0};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int status = asus_wmi_call_method(ASUSWMI_METHODID_GET_VALUE, args, &output);

	if (!status) {
		union acpi_object *obj = (union acpi_object *)output.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER)
			*value = obj->integer.value;
	}
	return status;
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
	int nr_count[HWMON_MAX] = {0}, nr_types = 0;
	struct device *hwdev;
	struct device *dev = &pdev->dev;
	struct hwmon_channel_info *asus_wmi_hwmon_chan;
	struct asus_wmi_sensor_info *temp_sensor;
	enum hwmon_sensor_types type;
	const struct hwmon_channel_info **ptr_asus_wmi_ci;
	const struct hwmon_chip_info *chip_info;

	if (sensor_data->wmi.sensor_count <= 0 || sensor_data->wmi_board < 0)
		return 0;

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

	for (type = 0; type < HWMON_MAX; type++) {
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
		if (err) {
			pr_err("sensor error\n");
			continue;
		}

		pr_debug("setting sensor info\n");

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

	pr_info("%s board has %d sensors",
		asus_wmi_boards_names[sensor_data->wmi_board],
		sensor_data->wmi.sensor_count);

	hwdev = devm_hwmon_device_register_with_info(dev, "asuswmisensors",
						     sensor_data, chip_info,
						     NULL);

	return PTR_ERR_OR_ZERO(hwdev);
}

static int asus_wmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct asus_wmi_data *data = dev_get_platdata(dev);
	struct asus_wmi_sensors *sensor_data;
	int err;

	sensor_data = devm_kzalloc(dev, sizeof(struct asus_wmi_sensors),
				   GFP_KERNEL);
	if (!sensor_data)
		return -ENOMEM;

	mutex_init(&sensor_data->lock);
	sensor_data->ec_board = data->ec_board;
	sensor_data->wmi_board = data->wmi_board;
	sensor_data->wmi.sensor_count = data->wmi_count;

	platform_set_drvdata(pdev, sensor_data);

	/* ec init */
	err = asus_wmi_ec_configure_sensor_setup(pdev,
						 sensor_data);
	if (err)
		return err;

	/* old version */
	err = asus_wmi_configure_sensor_setup(pdev,
					      sensor_data);

	return err;
}

static struct platform_driver asus_wmi_sensors_platform_driver = {
	.driver = {
		.name	= "asus-wmi-sensors",
	},
	.probe = asus_wmi_probe
};

static struct platform_device *sensors_pdev;

static int __init asus_wmi_init(void)
{
	const char *board_vendor, *board_name;
	u32 version = 0;
	struct asus_wmi_data data;

	data.wmi_board = -1;
	data.wmi_count = 0;
	data.ec_board = -1;

	board_vendor = dmi_get_system_info(DMI_BOARD_VENDOR);
	board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (board_vendor && board_name &&
	    !strcmp(board_vendor, "ASUSTeK COMPUTER INC.")) {
		if (!wmi_has_guid(ASUSWMI_MONITORING_GUID))
			return -ENODEV;

		data.ec_board = match_string(asus_wmi_ec_boards_names,
					     ARRAY_SIZE(asus_wmi_ec_boards_names),
					     board_name);
		data.wmi_board = match_string(asus_wmi_boards_names,
					      ARRAY_SIZE(asus_wmi_boards_names),
					      board_name);

		if (data.wmi_board >= 0) {
			if (asus_wmi_get_item_count(&data.wmi_count))
				return -ENODEV;

			if (asus_wmi_get_version(&version))
				return -ENODEV;

			if (data.wmi_count  <= 0 || version < 2) {
				pr_err("Board: %s WMI wmi version: %u with %d sensors is unsupported\n",
				       board_name, version, data.wmi_count);

				data.wmi_board = -ENODEV;
			}
		}
	}

	/* Nothing to support */
	if (data.ec_board < 0 && data.wmi_board < 0)
		return -ENODEV;

	sensors_pdev = platform_create_bundle(&asus_wmi_sensors_platform_driver,
					      asus_wmi_probe,
					      NULL, 0,
					      &data, sizeof(struct asus_wmi_data));

	if (IS_ERR(sensors_pdev))
		return PTR_ERR(sensors_pdev);

	return 0;
}

static void __exit asus_wmi_exit(void)
{
	platform_device_unregister(sensors_pdev);
	platform_driver_unregister(&asus_wmi_sensors_platform_driver);
}

MODULE_AUTHOR("Ed Brindley <kernel@maidavale.org>");
MODULE_AUTHOR("Eugene Shalygin <eugene.shalygin@gmail.com>");
MODULE_DESCRIPTION("Asus WMI Sensors Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1");

module_init(asus_wmi_init);
module_exit(asus_wmi_exit);
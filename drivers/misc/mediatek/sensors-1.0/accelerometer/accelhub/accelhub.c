/* accelhub motion sensor driver
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <hwmsensor.h>
#include "accelhub.h"
#include <accel.h>
#include <SCP_sensorHub.h>
#include "SCP_power_monitor.h"

#define DEBUG 1
#define SW_CALIBRATION
#define ACCELHUB_AXIS_X          0
#define ACCELHUB_AXIS_Y          1
#define ACCELHUB_AXIS_Z          2
#define ACCELHUB_AXES_NUM        3
#define ACCELHUB_DATA_LEN        6
#define ACCELHUB_DEV_NAME        "accel_hub_pl"	/* name must different with accel accelhub */
/* dadadadada */
typedef enum {
	ACCELHUB_TRC_FILTER = 0x01,
	ACCELHUB_TRC_RAWDATA = 0x02,
	ACCELHUB_TRC_IOCTL = 0x04,
	ACCELHUB_TRC_CALI = 0X08,
	ACCELHUB_TRC_INFO = 0X10,
} ACCELHUB_TRC;
struct accelhub_ipi_data {
	/*misc */
	atomic_t trace;
	atomic_t suspend;
	int32_t static_cali[ACCELHUB_AXES_NUM];
	int32_t dynamic_cali[ACCELHUB_AXES_NUM];
	int direction;
	struct work_struct init_done_work;
	atomic_t scp_init_done;
	atomic_t first_ready_after_boot;
	bool factory_enable;
	bool android_enable;
};

static struct acc_init_info accelhub_init_info;

static struct accelhub_ipi_data *obj_ipi_data;

static int gsensor_init_flag = -1;

#define GSE_TAG                  "[Gsensor] "
#define GSE_FUN(f)               pr_err(GSE_TAG"%s\n", __func__)
#define GSE_ERR(fmt, args...)    pr_err(GSE_TAG"%s %d : "fmt, __func__, __LINE__, ##args)
#define GSE_LOG(fmt, args...)    pr_err(GSE_TAG fmt, ##args)

static int gsensor_get_data(int *x, int *y, int *z, int *status);

int accelhub_SetPowerMode(bool enable)
{
	int err = 0;

	err = sensor_enable_to_hub(ID_ACCELEROMETER, enable);
	if (err < 0) {
		GSE_ERR("SCP_sensorHub_req_send fail!\n");
		return err;
	}
	return err;
}

static int accelhub_ReadCalibration(int dat[ACCELHUB_AXES_NUM])
{
	struct accelhub_ipi_data *obj = obj_ipi_data;

	dat[ACCELHUB_AXIS_X] = obj->static_cali[ACCELHUB_AXIS_X];
	dat[ACCELHUB_AXIS_Y] = obj->static_cali[ACCELHUB_AXIS_Y];
	dat[ACCELHUB_AXIS_Z] = obj->static_cali[ACCELHUB_AXIS_Z];

	return 0;
}

static int accelhub_ResetCalibration(void)
{
	struct accelhub_ipi_data *obj = obj_ipi_data;
	int err = 0;
	unsigned char dat[2];

	err = sensor_set_cmd_to_hub(ID_ACCELEROMETER, CUST_ACTION_RESET_CALI, dat);
	if (err < 0) {
		GSE_ERR("sensor_set_cmd_to_hub fail, (ID: %d),(action: %d)\n", ID_ACCELEROMETER,
			CUST_ACTION_RESET_CALI);
	}

	memset(obj->static_cali, 0x00, sizeof(obj->static_cali));

	return err;
}

static int accelhub_ReadCalibrationEx(int act[ACCELHUB_AXES_NUM], int raw[ACCELHUB_AXES_NUM])
{
	/*raw: the raw calibration data; act: the actual calibration data */
	struct accelhub_ipi_data *obj = obj_ipi_data;

	raw[ACCELHUB_AXIS_X] = obj->static_cali[ACCELHUB_AXIS_X];
	raw[ACCELHUB_AXIS_Y] = obj->static_cali[ACCELHUB_AXIS_Y];
	raw[ACCELHUB_AXIS_Z] = obj->static_cali[ACCELHUB_AXIS_Z];

	act[ACCELHUB_AXIS_X] = raw[ACCELHUB_AXIS_X];
	act[ACCELHUB_AXIS_Y] = raw[ACCELHUB_AXIS_Y];
	act[ACCELHUB_AXIS_Z] = raw[ACCELHUB_AXIS_Z];

	return 0;
}

static int accelhub_WriteCalibration_scp(int dat[ACCELHUB_AXES_NUM])
{
	int err = 0;

	err = sensor_set_cmd_to_hub(ID_ACCELEROMETER, CUST_ACTION_SET_CALI, dat);
	if (err < 0)
		GSE_ERR("sensor_set_cmd_to_hub fail, (ID: %d),(action: %d)\n", ID_ACCELEROMETER, CUST_ACTION_SET_CALI);
	return err;
}

static int accelhub_WriteCalibration(int dat[ACCELHUB_AXES_NUM])
{
	struct accelhub_ipi_data *obj = obj_ipi_data;
	int err = 0;
	int cali[ACCELHUB_AXES_NUM], raw[ACCELHUB_AXES_NUM];

	err = accelhub_ReadCalibrationEx(cali, raw);
	if (err) {
		GSE_ERR("read offset fail, %d\n", err);
		return err;
	}

	GSE_LOG("OLDOFF: (%+3d %+3d %+3d), cali: (%+3d %+3d %+3d)\n",
		raw[ACCELHUB_AXIS_X], raw[ACCELHUB_AXIS_Y], raw[ACCELHUB_AXIS_Z],
		obj->static_cali[ACCELHUB_AXIS_X], obj->static_cali[ACCELHUB_AXIS_Y],
		obj->static_cali[ACCELHUB_AXIS_Z]);

	err = accelhub_WriteCalibration_scp(dat);
	if (err < 0) {
		GSE_ERR("accelhub_WriteCalibration_scp fail\n");
		return err;
	}
	/*calculate the real offset expected by caller */
	cali[ACCELHUB_AXIS_X] += dat[ACCELHUB_AXIS_X];
	cali[ACCELHUB_AXIS_Y] += dat[ACCELHUB_AXIS_Y];
	cali[ACCELHUB_AXIS_Z] += dat[ACCELHUB_AXIS_Z];

	GSE_LOG("UPDATE: (%+3d %+3d %+3d)\n", dat[ACCELHUB_AXIS_X], dat[ACCELHUB_AXIS_Y], dat[ACCELHUB_AXIS_Z]);

	obj->static_cali[ACCELHUB_AXIS_X] = cali[ACCELHUB_AXIS_X];
	obj->static_cali[ACCELHUB_AXIS_Y] = cali[ACCELHUB_AXIS_Y];
	obj->static_cali[ACCELHUB_AXIS_Z] = cali[ACCELHUB_AXIS_Z];

	return err;
}

static int accelhub_ReadAllReg(char *buf, int bufsize)
{
	int err = 0;

	err = accelhub_SetPowerMode(true);
	if (err) {
		GSE_ERR("Power on accelhub error %d!\n", err);
		return err;
	}

	/* register map */
	return 0;
}

static int accelhub_ReadChipInfo(char *buf, int bufsize)
{
	u8 databuf[10];

	memset(databuf, 0, sizeof(u8) * 10);

	if ((buf == NULL) || (bufsize <= 30))
		return -1;

	sprintf(buf, "ACCELHUB Chip");
	return 0;
}

static int accelhub_ReadSensorData(char *buf, int bufsize)
{
	struct accelhub_ipi_data *obj = obj_ipi_data;
	uint64_t time_stamp = 0;
	uint64_t time_stamp_gpt = 0;
	struct data_unit_t data;
	int acc[ACCELHUB_AXES_NUM];
	int err = 0;
	int status = 0;

	if (atomic_read(&obj->suspend))
		return -3;

	if (buf == NULL)
		return -1;
	err = sensor_get_data_from_hub(ID_ACCELEROMETER, &data);
	if (err < 0) {
		GSE_ERR("sensor_get_data_from_hub fail!\n");
		return err;
	}
	time_stamp = data.time_stamp;
	time_stamp_gpt = data.time_stamp_gpt;
	acc[ACCELHUB_AXIS_X] = data.accelerometer_t.x;
	acc[ACCELHUB_AXIS_Y] = data.accelerometer_t.y;
	acc[ACCELHUB_AXIS_Z] = data.accelerometer_t.z;
	status				 = data.accelerometer_t.status;

	sprintf(buf, "%04x %04x %04x %04x", acc[ACCELHUB_AXIS_X], acc[ACCELHUB_AXIS_Y], acc[ACCELHUB_AXIS_Z], status);
	if (atomic_read(&obj->trace) & ACCELHUB_TRC_IOCTL)
		GSE_LOG("gsensor data: %s!\n", buf);

	return 0;
}
static ssize_t show_chipinfo_value(struct device_driver *ddri, char *buf)
{
	char strbuf[ACCELHUB_BUFSIZE];

	accelhub_SetPowerMode(true);
	msleep(50);

	accelhub_ReadAllReg(strbuf, ACCELHUB_BUFSIZE);

	accelhub_ReadChipInfo(strbuf, ACCELHUB_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);
}

static ssize_t show_sensordata_value(struct device_driver *ddri, char *buf)
{
	char strbuf[ACCELHUB_BUFSIZE];

	accelhub_ReadSensorData(strbuf, ACCELHUB_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);
}

static ssize_t show_cali_value(struct device_driver *ddri, char *buf)
{
	struct accelhub_ipi_data *obj = obj_ipi_data;
	int len = 0;

	len +=
	    snprintf(buf + len, PAGE_SIZE - len, "[SW ][%d] (%+3d, %+3d, %+3d)\n",
	    1, obj->static_cali[ACCELHUB_AXIS_X], obj->static_cali[ACCELHUB_AXIS_Y],
	    obj->static_cali[ACCELHUB_AXIS_Z]);

	return len;
}

static ssize_t store_trace_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct accelhub_ipi_data *obj = obj_ipi_data;
	int trace = 0;
	int res = 0;

	if (obj == NULL) {
		GSE_ERR("obj is null!!\n");
		return 0;
	}
	if (sscanf(buf, "0x%x", &trace) == 1) {
		atomic_set(&obj->trace, trace);
		res = sensor_set_cmd_to_hub(ID_ACCELEROMETER, CUST_ACTION_SET_TRACE, &trace);
		if (res < 0) {
			GSE_ERR("sensor_set_cmd_to_hub fail, (ID: %d),(action: %d)\n", ID_ACCELEROMETER,
				CUST_ACTION_SET_TRACE);
			return 0;
		}
	} else {
		GSE_ERR("invalid content: '%s', length = %zu\n", buf, count);
		return 0;
	}

	return count;
}

static ssize_t show_chip_orientation(struct device_driver *ddri, char *buf)
{
	ssize_t _tLength = 0;
	struct accelhub_ipi_data *obj = obj_ipi_data;

	_tLength = snprintf(buf, PAGE_SIZE, "default direction = %d\n", obj->direction);

	return _tLength;
}

static ssize_t store_chip_orientation(struct device_driver *ddri, const char *buf, size_t tCount)
{
	int _nDirection = 0, ret = 0;
	struct accelhub_ipi_data *obj = obj_ipi_data;

	if (obj == NULL)
		return 0;
	ret = kstrtoint(buf, 10, &_nDirection);
	if (ret != 0) {
		GSE_LOG("kstrtoint fail\n");
		return 0;
	}
	obj->direction = _nDirection;
	ret = sensor_set_cmd_to_hub(ID_ACCELEROMETER, CUST_ACTION_SET_DIRECTION, &_nDirection);
	if (ret < 0) {
		GSE_ERR("sensor_set_cmd_to_hub fail, (ID: %d),(action: %d)\n", ID_ACCELEROMETER,
			CUST_ACTION_SET_DIRECTION);
		return 0;
	}

	GSE_LOG("[%s] set direction: %d\n", __func__, _nDirection);

	return tCount;
}

static DRIVER_ATTR(chipinfo, S_IRUGO, show_chipinfo_value, NULL);
static DRIVER_ATTR(sensordata, S_IRUGO, show_sensordata_value, NULL);
static DRIVER_ATTR(cali, S_IWUSR | S_IRUGO, show_cali_value, NULL);
static DRIVER_ATTR(trace, S_IWUSR | S_IRUGO, NULL, store_trace_value);
static DRIVER_ATTR(orientation, S_IWUSR | S_IRUGO, show_chip_orientation, store_chip_orientation);

static struct driver_attribute *accelhub_attr_list[] = {
	&driver_attr_chipinfo,	/*chip information */
	&driver_attr_sensordata,	/*dump sensor data */
	&driver_attr_cali,	/*show calibration data */
	&driver_attr_trace,	/*trace log */
	&driver_attr_orientation,
};

static int accelhub_create_attr(struct device_driver *driver)
{
	int idx, err = 0;
	int num = (int)(ARRAY_SIZE(accelhub_attr_list));

	if (driver == NULL)
		return -EINVAL;

	for (idx = 0; idx < num; idx++) {
		err = driver_create_file(driver, accelhub_attr_list[idx]);
		if (err != 0) {
			GSE_ERR("driver_create_file (%s) = %d\n", accelhub_attr_list[idx]->attr.name, err);
			break;
		}
	}
	return err;
}

static int accelhub_delete_attr(struct device_driver *driver)
{
	int idx, err = 0;
	int num = (int)(ARRAY_SIZE(accelhub_attr_list));

	if (driver == NULL)
		return -EINVAL;

	for (idx = 0; idx < num; idx++)
		driver_remove_file(driver, accelhub_attr_list[idx]);

	return err;
}

static void scp_init_work_done(struct work_struct *work)
{
	int32_t cfg_data[6] = {0};
	struct accelhub_ipi_data *obj = obj_ipi_data;
	int err = 0;

	if (atomic_read(&obj->scp_init_done) == 0) {
		GSE_LOG("scp is not ready to send cmd\n");
	} else {
		if (atomic_read(&obj->first_ready_after_boot) == 0) {
			atomic_set(&obj->first_ready_after_boot, 1);
		} else {
			err = accelhub_WriteCalibration_scp(obj->static_cali);
			if (err < 0)
				GSE_ERR("accelhub_WriteCalibration_scp fail\n");
			cfg_data[0] = obj->dynamic_cali[0];
			cfg_data[1] = obj->dynamic_cali[1];
			cfg_data[2] = obj->dynamic_cali[2];

			cfg_data[3] = obj->static_cali[0];
			cfg_data[4] = obj->static_cali[1];
			cfg_data[5] = obj->static_cali[2];
			err = sensor_cfg_to_hub(ID_ACCELEROMETER, (uint8_t *)cfg_data, sizeof(cfg_data));
			if (err < 0)
				GSE_ERR("sensor_cfg_to_hub fail\n");
		}
	}
}

static int gsensor_recv_data(struct data_unit_t *event, void *reserved)
{
	int err = 0;
	struct acc_data data;
	struct accelhub_ipi_data *obj = obj_ipi_data;

	if (READ_ONCE(obj->android_enable) == false)
		return 0;
	data.x = event->accelerometer_t.x;
	data.y = event->accelerometer_t.y;
	data.z = event->accelerometer_t.z;
	data.status = event->accelerometer_t.status;
	data.timestamp = (int64_t)(event->time_stamp + event->time_stamp_gpt);
	data.reserved[0] = event->reserve[0];

	if (event->flush_action == DATA_ACTION)
		err = acc_data_report(&data);
	else if (event->flush_action == FLUSH_ACTION)
		err = acc_flush_report();
	else if (event->flush_action == BIAS_ACTION) {
		data.x = event->accelerometer_t.x_bias;
		data.y = event->accelerometer_t.y_bias;
		data.z = event->accelerometer_t.z_bias;
		obj->dynamic_cali[ACCELHUB_AXIS_X] = event->accelerometer_t.x_bias;
		obj->dynamic_cali[ACCELHUB_AXIS_Y] = event->accelerometer_t.y_bias;
		obj->dynamic_cali[ACCELHUB_AXIS_Z] = event->accelerometer_t.z_bias;
		err = acc_bias_report(&data);
	} else if (event->flush_action == CALI_ACTION) {
		data.x = event->accelerometer_t.x_bias;
		data.y = event->accelerometer_t.y_bias;
		data.z = event->accelerometer_t.z_bias;
		obj->static_cali[ACCELHUB_AXIS_X] = event->accelerometer_t.x_bias;
		obj->static_cali[ACCELHUB_AXIS_Y] = event->accelerometer_t.y_bias;
		obj->static_cali[ACCELHUB_AXIS_Z] = event->accelerometer_t.z_bias;
		err = acc_cali_report(&data);
	}
	return err;
}
static int gsensor_factory_enable_sensor(bool enabledisable, int64_t sample_periods_ms)
{
	int err = 0;
	struct accelhub_ipi_data *obj = obj_ipi_data;

	if (enabledisable == true)
		WRITE_ONCE(obj->factory_enable, true);
	else
		WRITE_ONCE(obj->factory_enable, false);
	if (enabledisable == true) {
		err = sensor_set_delay_to_hub(ID_ACCELEROMETER, sample_periods_ms);
		if (err) {
			GSE_ERR("sensor_set_delay_to_hub failed!\n");
			return -1;
		}
	}
	err = sensor_enable_to_hub(ID_ACCELEROMETER, enabledisable);
	if (err) {
		GSE_ERR("sensor_enable_to_hub failed!\n");
		return -1;
	}
	return 0;
}
static int gsensor_factory_get_data(int32_t data[3], int *status)
{
	return gsensor_get_data(&data[0], &data[1], &data[2], status);
}
static int gsensor_factory_get_raw_data(int32_t data[3])
{
	GSE_ERR("don't support gsensor_factory_get_raw_data!\n");
	return 0;
}
static int gsensor_factory_enable_calibration(void)
{
	return sensor_calibration_to_hub(ID_ACCELEROMETER);
}
static int gsensor_factory_clear_cali(void)
{
	int err = 0;

	err = accelhub_ResetCalibration();
	if (err) {
		GSE_ERR("gsensor_ResetCalibration failed!\n");
		return -1;
	}
	return 0;
}
static int gsensor_factory_set_cali(int32_t data[3])
{
	int err = 0;

	err = accelhub_WriteCalibration(data);
	if (err) {
		GSE_ERR("gsensor_WriteCalibration failed!\n");
		return -1;
	}
	return 0;
}
static int gsensor_factory_get_cali(int32_t data[3])
{
	int err = 0;

	err = accelhub_ReadCalibration(data);
	if (err) {
		GSE_ERR("gsensor_ReadCalibration failed!\n");
		return -1;
	}
	return 0;
}
static int gsensor_factory_do_self_test(void)
{
	return 0;
}

static struct accel_factory_fops gsensor_factory_fops = {
	.enable_sensor = gsensor_factory_enable_sensor,
	.get_data = gsensor_factory_get_data,
	.get_raw_data = gsensor_factory_get_raw_data,
	.enable_calibration = gsensor_factory_enable_calibration,
	.clear_cali = gsensor_factory_clear_cali,
	.set_cali = gsensor_factory_set_cali,
	.get_cali = gsensor_factory_get_cali,
	.do_self_test = gsensor_factory_do_self_test,
};

static struct accel_factory_public gsensor_factory_device = {
	.gain = 1,
	.sensitivity = 1,
	.fops = &gsensor_factory_fops,
};

static int gsensor_open_report_data(int open)
{

	return 0;
}

static int gsensor_enable_nodata(int en)
{
	int err = 0;
	struct accelhub_ipi_data *obj = obj_ipi_data;

	if (en == true)
		WRITE_ONCE(obj->android_enable, true);
	else
		WRITE_ONCE(obj->android_enable, false);

	if (atomic_read(&obj->suspend) == 0) {
		err = accelhub_SetPowerMode(en);
		if (err < 0) {
			GSE_ERR("scp_gsensor_enable_nodata fail!\n");
			return -1;
		}
	}

	GSE_LOG("scp_gsensor_enable_nodata OK!!!\n");
	return 0;
}

static int gsensor_set_delay(u64 ns)
{
#if defined CONFIG_MTK_SCP_SENSORHUB_V1
	int err = 0;
	unsigned int delayms = 0;
	struct accelhub_ipi_data *obj = obj_ipi_data;

	delayms = (unsigned int)ns / 1000 / 1000;
	err = sensor_set_delay_to_hub(ID_ACCELEROMETER, delayms);
	if (err < 0) {
		GSE_ERR("gsensor_set_delay fail!\n");
		return err;
	}
	GSE_LOG("gsensor_set_delay (%d)\n", delayms);
	return 0;
#elif defined CONFIG_NANOHUB
	return 0;
#else
	return 0;
#endif
}

static int gsensor_batch(int flag, int64_t samplingPeriodNs, int64_t maxBatchReportLatencyNs)
{
#if defined CONFIG_MTK_SCP_SENSORHUB_V1
	gsensor_set_delay(samplingPeriodNs);
#endif
	return sensor_batch_to_hub(ID_ACCELEROMETER, flag, samplingPeriodNs, maxBatchReportLatencyNs);
}

static int gsensor_flush(void)
{
	return sensor_flush_to_hub(ID_ACCELEROMETER);
}

static int gsensor_set_cali(uint8_t *data, uint8_t count)
{
	int32_t *buf = (int32_t *)data;
	struct accelhub_ipi_data *obj = obj_ipi_data;

	obj->dynamic_cali[0] = buf[0];
	obj->dynamic_cali[1] = buf[1];
	obj->dynamic_cali[2] = buf[2];

	obj->static_cali[0] = buf[3];
	obj->static_cali[1] = buf[4];
	obj->static_cali[2] = buf[5];

	return sensor_cfg_to_hub(ID_ACCELEROMETER, data, count);
}

static int gsensor_get_data(int *x, int *y, int *z, int *status)
{
	int err = 0;
	char buff[ACCELHUB_BUFSIZE];
	struct accelhub_ipi_data *obj = obj_ipi_data;

	err = accelhub_ReadSensorData(buff, ACCELHUB_BUFSIZE);
	if (err < 0) {
		GSE_ERR("accelhub_ReadSensorData fail!!\n");
		return -1;
	}
	err = sscanf(buff, "%x %x %x %x", x, y, z, status);
	if (err != 4) {
		GSE_ERR("sscanf fail!!\n");
		return -1;
	}

	if (atomic_read(&obj->trace) & ACCELHUB_TRC_RAWDATA)
		GSE_ERR("x = %d, y = %d, z = %d\n", *x, *y, *z);

	return 0;
}

static int scp_ready_event(uint8_t event, void *ptr)
{
	struct accelhub_ipi_data *obj = obj_ipi_data;

	switch (event) {
	case SENSOR_POWER_UP:
		atomic_set(&obj->scp_init_done, 1);
		schedule_work(&obj->init_done_work);
		break;
	case SENSOR_POWER_DOWN:
		atomic_set(&obj->scp_init_done, 0);
		break;
	}
	return 0;
}

static struct scp_power_monitor scp_ready_notifier = {
	.name = "accel",
	.notifier_call = scp_ready_event,
};

static int accelhub_probe(struct platform_device *pdev)
{
	struct accelhub_ipi_data *obj;
	struct acc_control_path ctl = { 0 };
	struct acc_data_path data = { 0 };
	int err = 0;

	GSE_FUN();
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		err = -ENOMEM;
		goto exit;
	}

	memset(obj, 0, sizeof(struct accelhub_ipi_data));

	INIT_WORK(&obj->init_done_work, scp_init_work_done);

	obj_ipi_data = obj;

	platform_set_drvdata(pdev, obj);

	atomic_set(&obj->trace, 0);
	atomic_set(&obj->suspend, 0);
	atomic_set(&obj->scp_init_done, 0);
	atomic_set(&obj->first_ready_after_boot, 0);
	WRITE_ONCE(obj->factory_enable, false);
	WRITE_ONCE(obj->android_enable, false);
	scp_power_monitor_register(&scp_ready_notifier);
	err = scp_sensorHub_data_registration(ID_ACCELEROMETER, gsensor_recv_data);
	if (err < 0) {
		GSE_ERR("scp_sensorHub_data_registration failed\n");
		goto exit_kfree;
	}
	err = accel_factory_device_register(&gsensor_factory_device);
	if (err) {
		GSE_ERR("gsensor_factory_device register failed\n");
		goto exit_kfree;
	}
	err = accelhub_create_attr(&accelhub_init_info.platform_diver_addr->driver);
	if (err) {
		GSE_ERR("create attribute err = %d\n", err);
		goto exit_create_attr_failed;
	}

	ctl.open_report_data = gsensor_open_report_data;
	ctl.enable_nodata = gsensor_enable_nodata;
	ctl.set_delay = gsensor_set_delay;
	ctl.batch = gsensor_batch;
	ctl.flush = gsensor_flush;
	ctl.set_cali = gsensor_set_cali;
#if defined CONFIG_MTK_SCP_SENSORHUB_V1
	ctl.is_report_input_direct = true;
	ctl.is_support_batch = false;
#elif defined CONFIG_NANOHUB
	ctl.is_report_input_direct = true;
	ctl.is_support_batch = true;
#else
#endif
	err = acc_register_control_path(&ctl);
	if (err) {
		GSE_ERR("register acc control path err\n");
		goto exit_create_attr_failed;
	}

	data.get_data = gsensor_get_data;
	data.vender_div = 1000;
	err = acc_register_data_path(&data);
	if (err) {
		GSE_ERR("register acc data path err\n");
		goto exit_create_attr_failed;
	}
	gsensor_init_flag = 0;
	GSE_LOG("%s: OK\n", __func__);
	return 0;

 exit_create_attr_failed:
	accelhub_delete_attr(&(accelhub_init_info.platform_diver_addr->driver));
 exit_kfree:
	kfree(obj);
	obj_ipi_data = NULL;
 exit:
	GSE_ERR("%s: err = %d\n", __func__, err);
	gsensor_init_flag = -1;
	return err;
}

static int accelhub_remove(struct platform_device *pdev)
{
	int err = 0;

	err = accelhub_delete_attr(&accelhub_init_info.platform_diver_addr->driver);
	if (err)
		GSE_ERR("accelhub_delete_attr fail: %d\n", err);
	accel_factory_device_deregister(&gsensor_factory_device);

	kfree(platform_get_drvdata(pdev));
	return 0;
}

static int accelhub_suspend(struct platform_device *pdev, pm_message_t msg)
{
	return 0;
}

static int accelhub_resume(struct platform_device *pdev)
{
	return 0;
}

static struct platform_device accelhub_device = {
	.name = ACCELHUB_DEV_NAME,
	.id = -1,
};

static struct platform_driver accelhub_driver = {
	.driver = {
		   .name = ACCELHUB_DEV_NAME,
		   },
	.probe = accelhub_probe,
	.remove = accelhub_remove,
	.suspend = accelhub_suspend,
	.resume = accelhub_resume,
};

static int gsensor_local_init(void)
{
	GSE_FUN();

	if (platform_driver_register(&accelhub_driver)) {
		GSE_ERR("add driver error\n");
		return -1;
	}
	if (-1 == gsensor_init_flag)
		return -1;
	return 0;
}

static int gsensor_local_remove(void)
{
	GSE_FUN();
	platform_driver_unregister(&accelhub_driver);
	return 0;
}

static struct acc_init_info accelhub_init_info = {
	.name = "accelhub",
	.init = gsensor_local_init,
	.uninit = gsensor_local_remove,
};

static int __init accelhub_init(void)
{

	if (platform_device_register(&accelhub_device)) {
		GSE_ERR("accel platform device error\n");
		return -1;
	}
	acc_driver_add(&accelhub_init_info);
	return 0;
}

static void __exit accelhub_exit(void)
{
	GSE_FUN();
}

module_init(accelhub_init);
module_exit(accelhub_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ACCELHUB gse driver");
MODULE_AUTHOR("hongxu.zhao@mediatek.com");
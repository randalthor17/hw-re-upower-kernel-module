// SPDX-License-Identifier: GPL-2.0-only
/*
 * HID driver for the ValueTop Tempest DF6 case display
 *
 *   5131:2007  segmented LCD showing CPU temperature, CPU cooler RPM and
 *              GPU temperature
 *
 * The display has no input and no battery, so nothing in the kernel ecosystem
 * consumes it.  The driver only owns the device and refreshes it: userspace
 * (tools/valuetop-feed.sh) writes the three readings to sysfs, the driver
 * sends them to the display once a second.  Which hwmon chip is "the CPU" or
 * "the cooler" is policy and stays in userspace.
 *
 * The display holds nothing by itself, and a frozen reading would look live.
 * If no attribute has been written for VT_STALE_TIMEOUT (5 s) the driver sends
 * all zeros, which cannot be mistaken for a real reading, and goes back to the
 * stored values with the next write.
 *
 *   /sys/bus/hid/devices/0003:5131:2007.XXXX/
 *     cpu_temp      degrees C, 0-255 (displayed modulo 100: the LCD has two digits)
 *     gpu_temp      degrees C, 0-255 (displayed modulo 100)
 *     cpu_fan_rpm   0-9999
 *
 * Output report layout (64 bytes, no report ID on the wire; byte 0 is the
 * report-ID slot that usbhid strips when it is 0):
 *   buf[0]      report ID (0)
 *   buf[2]      CPU temperature
 *   buf[6..7]   big-endian CPU cooler RPM (patched, see vt_patch_rpm())
 *   buf[10]     GPU temperature
 *   rest        padding
 *
 * The display firmware misrenders some RPM values.  With tens_ones = rpm % 100
 * the value shown for a requested i is:
 *   i < 2560:         i
 *   2560 <= i < 6000: i + 40 if tens_ones < 60, else i - 60
 *   6000 <= i < 7680: i + 80 if tens_ones < 20, else i - 20
 *   i >= 7680:        i + 20 if tens_ones < 80, else i - 80
 * so the request is pre-compensated.  2560-2599 and 7660-7679 cannot be shown.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define USB_VENDOR_ID_VALUETOP		0x5131
#define USB_DEVICE_ID_VALUETOP_DF6	0x2007

#define VT_REPORT_LEN		64	/* including the report-ID slot */
#define VT_REFRESH_INTERVAL	HZ
#define VT_STALE_TIMEOUT	(5 * HZ)
#define VT_MAX_RPM		9999

struct vt {
	struct hid_device *hdev;
	spinlock_t lock;		/* protects the readings */
	u8 cpu_temp;
	u8 gpu_temp;
	u16 cpu_rpm;
	unsigned long last_update;	/* jiffies of the last sysfs write */
	bool stale;			/* refresh work only */
	struct delayed_work refresh;
};

/* ------------------------------------------------------------------ */
/* RPM compensation                                                    */
/* ------------------------------------------------------------------ */

static unsigned int vt_patch_rpm(unsigned int want)
{
	unsigned int c;

	if (want < 2560)
		return want;

	c = want - 40;
	if (want >= 40 && c >= 2560 && c < 6000 && c % 100 < 60)
		return c;
	c = want + 60;
	if (c >= 2560 && c < 6000 && c % 100 >= 60)
		return c;

	c = want - 80;
	if (want >= 80 && c >= 6000 && c < 7680 && c % 100 < 20)
		return c;
	c = want + 20;
	if (c >= 6000 && c < 7680 && c % 100 >= 20)
		return c;

	c = want - 20;
	if (want >= 20 && c >= 7680 && c < 10000 && c % 100 < 80)
		return c;
	c = want + 80;
	if (c >= 7680 && c < 10000 && c % 100 >= 80)
		return c;

	return want;
}

/* ------------------------------------------------------------------ */
/* Refresh                                                             */
/* ------------------------------------------------------------------ */

static void vt_refresh_work(struct work_struct *work)
{
	struct vt *v = container_of(work, struct vt, refresh.work);
	u8 *buf;
	unsigned int rpm = 0;
	bool stale;
	int ret;

	/* USB needs a heap buffer; zeroed, which is also the "no data" frame */
	buf = kzalloc(VT_REPORT_LEN, GFP_KERNEL);
	if (!buf)
		goto again;

	spin_lock_irq(&v->lock);
	stale = time_after(jiffies, v->last_update + VT_STALE_TIMEOUT);
	if (!stale) {
		rpm = vt_patch_rpm(v->cpu_rpm);
		/* two digits: 100 C shows as 00, 105 C as 05, so an overheating
		 * part is plainly wrong next to a normal reading */
		buf[2] = v->cpu_temp % 100;
		buf[10] = v->gpu_temp % 100;
	}
	spin_unlock_irq(&v->lock);
	if (stale != v->stale) {
		v->stale = stale;
		if (stale)
			hid_info(v->hdev, "valuetop: no data for %d s, showing zeros\n",
				 (int)(VT_STALE_TIMEOUT / HZ));
		else
			hid_info(v->hdev, "valuetop: data resumed\n");
	}
	buf[6] = rpm >> 8;
	buf[7] = rpm & 0xff;

	ret = hid_hw_output_report(v->hdev, buf, VT_REPORT_LEN);
	if (ret < 0)
		hid_warn_ratelimited(v->hdev, "valuetop: send failed (%d)\n",
				     ret);
	kfree(buf);
again:
	schedule_delayed_work(&v->refresh, VT_REFRESH_INTERVAL);
}

/* ------------------------------------------------------------------ */
/* sysfs                                                               */
/* ------------------------------------------------------------------ */

static struct vt *vt_from_dev(struct device *dev)
{
	return hid_get_drvdata(to_hid_device(dev));
}

#define VT_ATTR(_name, _field, _max)					\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct vt *v = vt_from_dev(dev);				\
	unsigned int val;						\
									\
	spin_lock_irq(&v->lock);					\
	val = v->_field;						\
	spin_unlock_irq(&v->lock);					\
	return sysfs_emit(buf, "%u\n", val);				\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct vt *v = vt_from_dev(dev);				\
	unsigned int val;						\
	int ret = kstrtouint(buf, 0, &val);				\
									\
	if (ret)							\
		return ret;						\
	if (val > (_max))						\
		return -ERANGE;						\
	spin_lock_irq(&v->lock);					\
	v->_field = val;						\
	v->last_update = jiffies;					\
	spin_unlock_irq(&v->lock);					\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

VT_ATTR(cpu_temp, cpu_temp, 255);
VT_ATTR(gpu_temp, gpu_temp, 255);
VT_ATTR(cpu_fan_rpm, cpu_rpm, VT_MAX_RPM);

static struct attribute *vt_attrs[] = {
	&dev_attr_cpu_temp.attr,
	&dev_attr_gpu_temp.attr,
	&dev_attr_cpu_fan_rpm.attr,
	NULL,
};

static const struct attribute_group vt_group = {
	.attrs = vt_attrs,
};

/* ------------------------------------------------------------------ */
/* Probe / remove                                                      */
/* ------------------------------------------------------------------ */

static int vt_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct vt *v;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	v = devm_kzalloc(&hdev->dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->hdev = hdev;
	spin_lock_init(&v->lock);
	v->last_update = jiffies;
	INIT_DELAYED_WORK(&v->refresh, vt_refresh_work);
	hid_set_drvdata(hdev, v);

	/* hidraw stays available for other tools */
	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;
	ret = hid_hw_open(hdev);
	if (ret)
		goto err_stop;
	ret = devm_device_add_group(&hdev->dev, &vt_group);
	if (ret)
		goto err_close;

	schedule_delayed_work(&v->refresh, HZ / 2);
	hid_info(hdev, "valuetop: ValueTop display ready\n");
	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void vt_remove(struct hid_device *hdev)
{
	struct vt *v = hid_get_drvdata(hdev);

	cancel_delayed_work_sync(&v->refresh);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id vt_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_VALUETOP, USB_DEVICE_ID_VALUETOP_DF6) },
	{ }
};
MODULE_DEVICE_TABLE(hid, vt_devices);

static struct hid_driver vt_driver = {
	.name = "valuetop-display",
	.id_table = vt_devices,
	.probe = vt_probe,
	.remove = vt_remove,
};
module_hid_driver(vt_driver);

MODULE_DESCRIPTION("ValueTop Tempest DF6 case display: CPU/GPU temperature and fan RPM");
MODULE_LICENSE("GPL");

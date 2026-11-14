// SPDX-License-Identifier: GPL-2.0-only
/*
 * HID driver for the ATK / VXE R1 NK mouse
 *
 *   373b:124c  wired      "ATK VXE R1 NK"
 *   373b:1216  NearLink   "ATK NK mouse NANO dongle"
 *
 * Exposes the one thing existing desktop software consumes (UPower etc.):
 *   - power_supply   battery percent, voltage and charging state
 *
 * DPI, report rate, buttons, macros and lighting are deliberately left to
 * userspace configuration tools talking to hidraw; nothing in the kernel
 * ecosystem reads them.
 *
 * Only the R1 NK (cid 2, mid 8) is vouched for.  The receiver USB ID is shared
 * with other NearLink mice, so through the dongle the model is read from the
 * mouse, and the battery is only registered once it matches.
 *
 * Protocol notes: see the reverse-engineering docs (PROTOCOL.md).
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define USB_VENDOR_ID_ATK		0x373b
#define USB_DEVICE_ID_ATK_VXE_R1_NK	0x124c
#define USB_DEVICE_ID_ATK_NEARLINK_RX	0x1216

#define VXE_REPORT_ID		8
#define VXE_FRAME_LEN		63	/* after the report ID */
#define VXE_MAGIC_REQ		0x72
#define VXE_MAGIC_EVT		0x73

#define VXE_POLL_INTERVAL	(30 * HZ)
#define VXE_RETRY_INTERVAL	(5 * HZ)

#define CMD_BATTERY		0x07
#define CMD_CHARGING		0x0f
#define CMD_IDENTITY		0x4a
#define CMD_RX_LINK		0x81

struct vxe {
	struct hid_device *hdev;
	bool receiver;			/* NearLink dongle (target 1) */

	/* ---- transport ---- */
	struct mutex io_lock;
	spinlock_t rx_lock;
	struct completion rx_done;
	bool rx_active;
	u8 rx_cmd;
	u8 rx_reply[VXE_FRAME_LEN];

	/* ---- state (state_lock) ---- */
	spinlock_t state_lock;
	bool identified;		/* confirmed R1 NK */
	bool online;			/* mouse answers (dongle: linked) */
	bool bat_known;
	int bat_percent, bat_mv;
	bool charging;

	bool features_up;
	int last_link;
	struct power_supply *psy;
	struct delayed_work poll;
};

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

/*
 * Interrupt-OUT if the interface has one, else a SET_REPORT control transfer.
 * USB needs a kmalloc'd buffer, and callers may pass stack memory.
 */
static int vxe_send_output(struct hid_device *hdev, u8 *buf, size_t len)
{
	u8 *tmp = kmemdup(buf, len, GFP_KERNEL);
	int ret;

	if (!tmp)
		return -ENOMEM;
	ret = hid_hw_output_report(hdev, tmp, len);
	if (ret == -ENOSYS || ret == -EINVAL)
		ret = hid_hw_raw_request(hdev, tmp[0], tmp, len,
					 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	kfree(tmp);
	return ret;
}

/*
 * One command/reply exchange.  @target 0 = device addressed, 1 = mouse behind
 * the receiver.  On success the 63-byte reply (without report ID) is copied
 * to @reply; the payload starts at reply[6].
 */
static int vxe_xfer(struct vxe *v, u8 target, u8 cmd, u8 param_len, u8 cmd_len,
		    const u8 *args, size_t nargs, u8 *reply)
{
	u8 buf[VXE_FRAME_LEN + 1] = { VXE_REPORT_ID };
	unsigned int sum = 0;
	unsigned long flags;
	int ret = -ETIMEDOUT, try, i;
	unsigned int tmo = v->receiver ? 1000 : 300;

	if (nargs > 56)
		return -EINVAL;
	buf[2] = VXE_MAGIC_REQ;
	buf[3] = param_len;
	buf[4] = 0;
	buf[5] = target;
	buf[6] = cmd;
	buf[7] = cmd_len;
	if (nargs)
		memcpy(buf + 8, args, nargs);
	for (i = 2; i < sizeof(buf); i++)
		sum += buf[i];
	buf[1] = sum & 0xff;

	mutex_lock(&v->io_lock);
	for (try = 0; try < 3; try++) {
		spin_lock_irqsave(&v->rx_lock, flags);
		v->rx_cmd = cmd;
		v->rx_active = true;
		reinit_completion(&v->rx_done);
		spin_unlock_irqrestore(&v->rx_lock, flags);

		ret = vxe_send_output(v->hdev, buf, sizeof(buf));
		if (ret >= 0) {
			if (wait_for_completion_timeout(&v->rx_done,
							msecs_to_jiffies(tmo)))
				ret = 0;
			else
				ret = -ETIMEDOUT;
		}

		spin_lock_irqsave(&v->rx_lock, flags);
		v->rx_active = false;
		if (!ret) {
			if (v->rx_reply[1] != 0)
				ret = -EIO;	/* error status from device */
			else if (reply)
				memcpy(reply, v->rx_reply, VXE_FRAME_LEN);
		}
		spin_unlock_irqrestore(&v->rx_lock, flags);
		if (!ret)
			break;
	}
	mutex_unlock(&v->io_lock);
	return ret;
}

/* talk to the mouse (through the dongle if there is one) */
static int vxe_mouse(struct vxe *v, u8 cmd, u8 plen, u8 clen, const u8 *args,
		     size_t n, u8 *reply)
{
	return vxe_xfer(v, v->receiver ? 1 : 0, cmd, plen, clen, args, n, reply);
}

static int vxe_raw_event(struct hid_device *hdev, struct hid_report *report,
			 u8 *data, int size)
{
	struct vxe *v = hid_get_drvdata(hdev);
	unsigned long flags;

	if (!v || size < VXE_FRAME_LEN + 1 || data[0] != VXE_REPORT_ID)
		return 0;
	data++;		/* skip report ID */

	/* replies start with 0x72; unsolicited events carry 0x73 in byte 1 */
	if (data[0] != VXE_MAGIC_REQ || data[1] == VXE_MAGIC_EVT)
		return 0;
	spin_lock_irqsave(&v->rx_lock, flags);
	if (v->rx_active && data[4] == v->rx_cmd) {
		memcpy(v->rx_reply, data, VXE_FRAME_LEN);
		complete(&v->rx_done);
	}
	spin_unlock_irqrestore(&v->rx_lock, flags);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Device queries                                                      */
/* ------------------------------------------------------------------ */

static int vxe_refresh_battery(struct vxe *v)
{
	u8 r[VXE_FRAME_LEN];
	int ret, pct, mv, chg;

	ret = vxe_mouse(v, CMD_BATTERY, 2, 1, NULL, 0, r);
	if (ret)
		return ret;
	pct = r[6];
	mv = r[7] | (r[8] << 8);
	ret = vxe_mouse(v, CMD_CHARGING, 2, 1, NULL, 0, r);
	chg = ret ? 0 : r[6] == 1;

	spin_lock_irq(&v->state_lock);
	v->bat_known = true;
	v->bat_percent = min(pct, 100);
	v->bat_mv = mv;
	v->charging = chg;
	spin_unlock_irq(&v->state_lock);
	return 0;
}

/* Returns 0 if the mouse is the R1 NK (cid 2, mid 8) */
static int vxe_identify(struct vxe *v)
{
	u8 r[VXE_FRAME_LEN], arg = 0;
	int ret;

	ret = vxe_mouse(v, CMD_IDENTITY, 8, 6, &arg, 1, r);
	if (ret)
		return ret;
	/* payload: operation, cid, mid (32-bit LE) */
	if (r[7] != 2 || (r[8] | (r[9] << 8) | (r[10] << 16) | (r[11] << 24)) != 8)
		return -ENODEV;
	return 0;
}

static bool vxe_receiver_linked(struct vxe *v)
{
	u8 r[VXE_FRAME_LEN];

	return !vxe_xfer(v, 0, CMD_RX_LINK, 3, 1, NULL, 0, r) && r[6];
}

/* ------------------------------------------------------------------ */
/* Power supply                                                        */
/* ------------------------------------------------------------------ */

static enum power_supply_property vxe_psy_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int vxe_psy_get(struct power_supply *psy, enum power_supply_property p,
		       union power_supply_propval *val)
{
	struct vxe *v = power_supply_get_drvdata(psy);
	bool known, online, chg;
	int pct, mv;

	spin_lock_irq(&v->state_lock);
	known = v->bat_known;
	online = v->online;
	chg = v->charging;
	pct = v->bat_percent;
	mv = v->bat_mv;
	spin_unlock_irq(&v->state_lock);

	switch (p) {
	case POWER_SUPPLY_PROP_STATUS:
		if (chg)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (!v->receiver)
			val->intval = pct >= 100 ? POWER_SUPPLY_STATUS_FULL :
						   POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = known && online;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (!known)
			return -ENODATA;
		val->intval = pct;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (!known)
			return -ENODATA;
		val->intval = mv * 1000;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "ATK VXE R1 NK";
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "ATK";
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc vxe_psy_desc_tmpl = {
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = vxe_psy_props,
	.num_properties = ARRAY_SIZE(vxe_psy_props),
	.get_property = vxe_psy_get,
};

static void vxe_unregister_features(struct vxe *v)
{
	if (!v->features_up)
		return;
	v->features_up = false;
	power_supply_unregister(v->psy);
	v->psy = NULL;
}

/* Called once the battery has been read: UPower ignores batteries without data */
static int vxe_register_features(struct vxe *v)
{
	struct hid_device *hdev = v->hdev;
	struct power_supply_desc *desc;
	struct power_supply_config pcfg = { .drv_data = v };

	desc = devm_kmemdup(&hdev->dev, &vxe_psy_desc_tmpl, sizeof(*desc),
			    GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	desc->name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "hid-%s-battery",
				    dev_name(&hdev->dev));
	if (!desc->name)
		return -ENOMEM;

	v->psy = power_supply_register(&hdev->dev, desc, &pcfg);
	if (IS_ERR(v->psy)) {
		int ret = PTR_ERR(v->psy);

		v->psy = NULL;
		return ret;
	}
	v->features_up = true;
	return 0;
}

static void vxe_set_online(struct vxe *v, bool on)
{
	bool changed;

	spin_lock_irq(&v->state_lock);
	changed = v->online != on;
	v->online = on;
	spin_unlock_irq(&v->state_lock);
	if (changed && v->psy)
		power_supply_changed(v->psy);
}

/* ------------------------------------------------------------------ */
/* Polling                                                             */
/* ------------------------------------------------------------------ */

static void vxe_poll_work(struct work_struct *work)
{
	struct vxe *v = container_of(work, struct vxe, poll.work);
	bool linked = true;

	if (v->receiver) {
		linked = vxe_receiver_linked(v);
		if (linked != v->last_link) {
			hid_info(v->hdev, "vxe: receiver link %s\n",
				 linked ? "up" : "down");
			v->last_link = linked;
		}
	}

	if (!linked) {
		vxe_set_online(v, false);
		goto again;
	}

	if (!v->identified) {
		int ret = vxe_identify(v);

		if (ret) {
			/* asleep (timeout) or a different model on a shared receiver */
			hid_info_ratelimited(v->hdev, "vxe: identify failed (%d)\n", ret);
			goto again;
		}
		v->identified = true;
	}

	if (vxe_refresh_battery(v)) {
		/* a sleeping mouse simply does not answer */
		vxe_set_online(v, false);
		goto again;
	}
	vxe_set_online(v, true);
	if (!v->features_up) {
		if (vxe_register_features(v))
			hid_warn(v->hdev, "vxe: registration failed\n");
		else
			hid_info(v->hdev, "vxe: ATK VXE R1 NK identified (%s)\n",
				 v->receiver ? "NearLink" : "wired");
	}
	if (v->psy)
		power_supply_changed(v->psy);
again:
	schedule_delayed_work(&v->poll, v->features_up ? VXE_POLL_INTERVAL :
							  VXE_RETRY_INTERVAL);
}

/* ------------------------------------------------------------------ */
/* Probe / remove                                                      */
/* ------------------------------------------------------------------ */

static int vxe_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct vxe *v;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	/* only the interface with the vendor report 8 is ours; the rest is input */
	if (!hdev->report_enum[HID_OUTPUT_REPORT].report_id_hash[VXE_REPORT_ID] ||
	    !hdev->report_enum[HID_INPUT_REPORT].report_id_hash[VXE_REPORT_ID])
		return hid_hw_start(hdev, HID_CONNECT_DEFAULT);

	v = devm_kzalloc(&hdev->dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->hdev = hdev;
	v->receiver = hdev->product == USB_DEVICE_ID_ATK_NEARLINK_RX;
	v->online = !v->receiver;
	mutex_init(&v->io_lock);
	spin_lock_init(&v->rx_lock);
	spin_lock_init(&v->state_lock);
	init_completion(&v->rx_done);
	INIT_DELAYED_WORK(&v->poll, vxe_poll_work);
	hid_set_drvdata(hdev, v);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;
	ret = hid_hw_open(hdev);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}
	schedule_delayed_work(&v->poll, HZ / 2);
	return 0;
}

static void vxe_remove(struct hid_device *hdev)
{
	struct vxe *v = hid_get_drvdata(hdev);

	if (v) {
		cancel_delayed_work_sync(&v->poll);
		vxe_unregister_features(v);
		hid_hw_close(hdev);
	}
	hid_hw_stop(hdev);
}

static const struct hid_device_id vxe_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ATK, USB_DEVICE_ID_ATK_VXE_R1_NK) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ATK, USB_DEVICE_ID_ATK_NEARLINK_RX) },
	{ }
};
MODULE_DEVICE_TABLE(hid, vxe_devices);

static struct hid_driver vxe_driver = {
	.name = "atk-vxe",
	.id_table = vxe_devices,
	.probe = vxe_probe,
	.remove = vxe_remove,
	.raw_event = vxe_raw_event,
};
module_hid_driver(vxe_driver);

MODULE_DESCRIPTION("ATK VXE R1 NK mouse: battery");
MODULE_LICENSE("GPL");

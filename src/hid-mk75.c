// SPDX-License-Identifier: GPL-2.0-only
/*
 * HID driver for the Lingbao MK75 keyboard (BY Tech / AULA F75 / AU75 family)
 *
 *   258a:010c  wired            "BY Tech Gaming Keyboard"
 *   3554:fa09  2.4 GHz dongle   "Compx 2.4G Wireless Receiver"
 *
 * Exposes only what existing desktop software consumes (UPower etc.):
 *   - power_supply   battery capacity and charging state
 *   - led_class      mk75::kbd_backlight (brightness 0-4)
 *
 * Everything else the keyboard can do (effects, side light, keymaps, per-key
 * RGB, ...) is deliberately left to userspace tools talking to hidraw.
 * Bluetooth is not supported (protocol unknown).
 *
 * Hardware caveats from the protocol docs:
 *   - Every config write makes the keyboard blank its LEDs for ~2 s.
 *   - Writes over the cable are ignored while the keyboard is in 2.4 GHz or
 *     Bluetooth mode (-EBUSY here).
 *   - A sleeping keyboard returns a mostly-zero config; it is never written
 *     back (config end markers are checked first).
 *   - 258a:010c and 3554:fa09 are shared with other products, so nothing is
 *     registered until the config block reads back in the documented layout.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define USB_VENDOR_ID_BYTECH_MK75	0x258a
#define USB_DEVICE_ID_MK75_WIRED	0x010c
#define USB_VENDOR_ID_COMPX		0x3554
#define USB_DEVICE_ID_COMPX_RX_FA09	0xfa09

#define MK75_USB_REPORT_ID		6
#define MK75_USB_REPORT_LEN		520
#define MK75_USB_EVENT_ID		3
#define MK75_RX_REPORT_ID		0x13
#define MK75_RX_REPORT_LEN		20

#define MK75_CFG_LEN			128
#define MK75_POLL_INTERVAL		(30 * HZ)
#define MK75_MAX_FAILS			4	/* dongle: polls before we drop out */

/* config block byte offsets */
#define CFG_GLOBAL_SWITCH	5
#define CFG_GLOBAL_BRIGHT	6
#define CFG_PERKEY_ENABLE	9
#define CFG_EFFECT		10
#define CFG_CONN_MODE		14
#define CFG_END_MARK		56
#define CFG_EFFECT_PAIRS	56	/* [brightness, speed/colour] for effect e at 56 + 2e */
#define CFG_PERKEY_BRIGHT	117
#define CFG_TRAILER		126

#define EFFECT_OFF		0
#define EFFECT_NEON_STREAM	11	/* factory default */
#define EFFECT_SELF_DEFINE	21

#define POWER_ON_BATTERY	0x01
#define POWER_CHARGING		0x10
#define POWER_CHARGED		0x11

struct mk75 {
	struct hid_device *hdev;
	bool wireless;			/* talking to the 2.4 GHz dongle */

	/* ---- transport ---- */
	struct mutex io_lock;		/* one transfer at a time */
	spinlock_t rx_lock;		/* protects the rx_* fields below */
	struct completion rx_done;
	bool rx_active;
	bool rx_ack_mode;
	u8 rx_cmd;
	u8 rx_expect;
	u8 rx_ack_idx;
	int rx_err;
	size_t rx_len;
	u8 rx_buf[MK75_CFG_LEN + 16];

	/* ---- cached config ---- */
	struct mutex cfg_lock;		/* outer lock, taken before io_lock */
	u8 cfg[MK75_CFG_LEN];
	bool cfg_valid;
	u8 saved_effect;

	/* ---- power ---- */
	spinlock_t state_lock;
	struct power_supply *psy;
	int capacity;
	u8 power_flags;
	bool power_known;
	bool link_up;			/* dongle only */

	/* ---- features ---- */
	bool features_up;
	int last_err;
	int fails;
	struct led_classdev kbd_led;
	struct delayed_work poll;
	struct work_struct event_work;
};

/* ------------------------------------------------------------------ */
/* Wired transport: 520-byte feature report 6                          */
/* ------------------------------------------------------------------ */

static int mk75_usb_get(struct mk75 *k, u8 cmd, u8 index, u16 len, u8 *out)
{
	u8 *buf;
	int ret;

	if (len > 512)
		return -EINVAL;
	buf = kzalloc(MK75_USB_REPORT_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&k->io_lock);
	buf[0] = MK75_USB_REPORT_ID;
	buf[1] = cmd | 0x80;
	buf[2] = index;
	buf[4] = 1;			/* total blocks */
	buf[5] = 0;			/* block */
	buf[6] = len & 0xff;
	buf[7] = len >> 8;

	msleep(20);
	ret = hid_hw_raw_request(k->hdev, MK75_USB_REPORT_ID, buf,
				 MK75_USB_REPORT_LEN, HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);
	if (ret < 0)
		goto out;
	msleep(20);
	ret = hid_hw_raw_request(k->hdev, MK75_USB_REPORT_ID, buf,
				 MK75_USB_REPORT_LEN, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0)
		goto out;
	if (ret < 8 + len || buf[0] != MK75_USB_REPORT_ID ||
	    buf[1] != (cmd | 0x80)) {
		ret = -EPROTO;
		goto out;
	}
	memcpy(out, buf + 8, len);
	ret = 0;
out:
	mutex_unlock(&k->io_lock);
	kfree(buf);
	return ret;
}

static int mk75_usb_set(struct mk75 *k, u8 cmd, u8 index, const u8 *data,
			u16 len)
{
	u8 *buf;
	int ret = 0, i;

	if (len > 512)
		return -EINVAL;
	buf = kzalloc(MK75_USB_REPORT_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = MK75_USB_REPORT_ID;
	buf[1] = cmd;
	buf[2] = index;
	buf[4] = 1;
	buf[5] = 0;
	buf[6] = len & 0xff;
	buf[7] = len >> 8;
	memcpy(buf + 8, data, len);

	mutex_lock(&k->io_lock);
	msleep(20);
	for (i = 0; i < 3; i++) {
		ret = hid_hw_raw_request(k->hdev, MK75_USB_REPORT_ID, buf,
					 MK75_USB_REPORT_LEN, HID_FEATURE_REPORT,
					 HID_REQ_SET_REPORT);
		if (ret >= 0) {
			ret = 0;
			break;
		}
		msleep(70);
	}
	mutex_unlock(&k->io_lock);
	kfree(buf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Wireless transport: 20-byte report 0x13 through the dongle          */
/* ------------------------------------------------------------------ */

/*
 * Interrupt-OUT if the interface has one, else a SET_REPORT control transfer.
 * USB needs a kmalloc'd buffer, and callers may pass stack memory.
 */
static int mk75_send_output(struct hid_device *hdev, u8 *buf, size_t len)
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

static int mk75_rx_send(struct mk75 *k, u8 cmd, u8 packages, u8 index,
			u8 layer, const u8 *payload, u8 plen)
{
	u8 f[MK75_RX_REPORT_LEN] = { 0 };
	unsigned int sum = 0;
	int i, ret;

	f[0] = MK75_RX_REPORT_ID;
	f[1] = cmd;
	f[2] = packages;
	f[3] = index;
	f[4] = (layer << 4) | (plen & 0x0f);
	if (plen)
		memcpy(f + 5, payload, plen);
	for (i = 0; i < 19; i++)
		sum += f[i];
	f[19] = sum & 0xff;

	ret = mk75_send_output(k->hdev, f, sizeof(f));
	return ret < 0 ? ret : 0;
}

/* Read a whole multi-package reply; returns bytes read or -errno. */
static int mk75_rx_get(struct mk75 *k, u8 cmd, u8 layer, u8 *out, size_t max,
		       unsigned int timeout_ms)
{
	int ret = -ETIMEDOUT, try;
	size_t n = 0;

	mutex_lock(&k->io_lock);
	for (try = 0; try < 3; try++) {
		unsigned long flags;

		spin_lock_irqsave(&k->rx_lock, flags);
		k->rx_cmd = cmd;
		k->rx_expect = 0;
		k->rx_len = 0;
		k->rx_err = 0;
		k->rx_ack_mode = false;
		k->rx_active = true;
		reinit_completion(&k->rx_done);
		spin_unlock_irqrestore(&k->rx_lock, flags);

		ret = mk75_rx_send(k, cmd, 1, 0, layer, NULL, 0);
		if (!ret) {
			if (wait_for_completion_timeout(&k->rx_done,
					msecs_to_jiffies(timeout_ms)))
				ret = 0;
			else
				ret = -ETIMEDOUT;
		}

		spin_lock_irqsave(&k->rx_lock, flags);
		k->rx_active = false;
		if (!ret)
			ret = k->rx_err;
		n = k->rx_len;
		if (!ret)
			memcpy(out, k->rx_buf, min(n, max));
		spin_unlock_irqrestore(&k->rx_lock, flags);

		if (!ret)
			break;
		msleep(70);
	}
	mutex_unlock(&k->io_lock);
	return ret ? ret : (int)min(n, max);
}

static int mk75_rx_set(struct mk75 *k, u8 cmd, u8 layer, const u8 *data,
		       size_t len)
{
	bool ack = cmd != 0x06 && cmd != 0x08 && cmd < 0x80;
	size_t n = max_t(size_t, 1, DIV_ROUND_UP(len, 14));
	size_t i;
	int ret = 0;

	mutex_lock(&k->io_lock);
	for (i = 0; i < n; i++) {
		u8 plen = min_t(size_t, 14, len - i * 14);
		int try;

		for (try = 0; try < 3; try++) {
			unsigned long flags;

			spin_lock_irqsave(&k->rx_lock, flags);
			k->rx_cmd = cmd;
			k->rx_ack_idx = i;
			k->rx_err = 0;
			k->rx_ack_mode = true;
			k->rx_active = ack;
			reinit_completion(&k->rx_done);
			spin_unlock_irqrestore(&k->rx_lock, flags);

			ret = mk75_rx_send(k, cmd, n, i, layer,
					   data + i * 14, plen);
			if (!ret && ack) {
				if (!wait_for_completion_timeout(&k->rx_done,
						msecs_to_jiffies(500)))
					ret = -ETIMEDOUT;
				else
					ret = k->rx_err;
			}

			spin_lock_irqsave(&k->rx_lock, flags);
			k->rx_active = false;
			spin_unlock_irqrestore(&k->rx_lock, flags);

			if (!ret)
				break;
		}
		if (ret)
			break;
		msleep(15);
	}
	mutex_unlock(&k->io_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Config block access                                                 */
/* ------------------------------------------------------------------ */

static bool mk75_cfg_sane(const u8 *cfg)
{
	return cfg[CFG_END_MARK] == 0xff && cfg[CFG_END_MARK + 1] == 0xff &&
	       cfg[CFG_TRAILER] == 0x5a && cfg[CFG_TRAILER + 1] == 0xa5;
}

/* cfg_lock held. Refreshes the cache. */
static int mk75_cfg_read(struct mk75 *k)
{
	u8 tmp[MK75_CFG_LEN];
	int ret;

	if (k->wireless) {
		ret = mk75_rx_get(k, 0x44, 0, tmp, sizeof(tmp), 5000);
		if (ret >= 0 && ret != MK75_CFG_LEN)
			ret = -EPROTO;
	} else {
		ret = mk75_usb_get(k, 0x04, 0, MK75_CFG_LEN, tmp);
	}
	if (ret < 0)
		return ret;

	/* A sleeping keyboard returns a mostly zero block: never use it. */
	if (!mk75_cfg_sane(tmp))
		return -EAGAIN;

	memcpy(k->cfg, tmp, sizeof(tmp));
	k->cfg_valid = true;
	return 0;
}

static int mk75_cfg_write(struct mk75 *k, const u8 *cfg)
{
	if (!mk75_cfg_sane(cfg))
		return -EINVAL;
	if (k->wireless)
		return mk75_rx_set(k, 0x04, 0, cfg, MK75_CFG_LEN);
	return mk75_usb_set(k, 0x04, 0, cfg, MK75_CFG_LEN);
}

/* Read-modify-write the config block; @fn edits a scratch copy. */
static int mk75_cfg_update(struct mk75 *k,
			   int (*fn)(struct mk75 *, u8 *, unsigned long),
			   unsigned long arg)
{
	u8 work[MK75_CFG_LEN];
	int ret;

	mutex_lock(&k->cfg_lock);
	ret = mk75_cfg_read(k);
	if (ret)
		goto out;
	memcpy(work, k->cfg, sizeof(work));
	/* Writes over the cable are ignored unless the keyboard is in wired mode */
	if (!k->wireless && work[CFG_CONN_MODE] != 0) {
		ret = -EBUSY;
		goto out;
	}
	ret = fn(k, work, arg);
	if (ret < 0)
		goto out;
	if (!memcmp(work, k->cfg, sizeof(work))) {
		ret = 0;
		goto out;
	}
	ret = mk75_cfg_write(k, work);
	if (!ret)
		memcpy(k->cfg, work, sizeof(work));
out:
	mutex_unlock(&k->cfg_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Backlight LED                                                       */
/* ------------------------------------------------------------------ */

/* The byte that holds the active effect's brightness (0-4), or NULL */
static u8 *mk75_eff_brightness(u8 *cfg)
{
	u8 e = cfg[CFG_EFFECT];

	if (cfg[CFG_GLOBAL_SWITCH])
		return &cfg[CFG_GLOBAL_BRIGHT];
	if (e >= 1 && e <= 18)
		return &cfg[CFG_EFFECT_PAIRS + 2 * e];
	if (e == EFFECT_SELF_DEFINE || e == 32 || e == 38)
		return &cfg[CFG_PERKEY_BRIGHT];
	return NULL;
}

static unsigned int mk75_brightness_from_cfg(const u8 *cfg)
{
	u8 *b;

	if (cfg[CFG_EFFECT] == EFFECT_OFF)
		return 0;
	b = mk75_eff_brightness((u8 *)cfg);
	return b ? min_t(u8, *b, 4) : 0;
}

/* 0 turns the effect off (remembering it); >0 restores it at that brightness */
static int mk75_kbd_set_fn(struct mk75 *k, u8 *cfg, unsigned long v)
{
	u8 *b;

	if (v == 0) {
		if (cfg[CFG_EFFECT] != EFFECT_OFF)
			k->saved_effect = cfg[CFG_EFFECT];
		cfg[CFG_EFFECT] = EFFECT_OFF;
		cfg[CFG_PERKEY_ENABLE] = 0;
		return 0;
	}
	if (cfg[CFG_EFFECT] == EFFECT_OFF) {
		cfg[CFG_EFFECT] = k->saved_effect ?: EFFECT_NEON_STREAM;
		cfg[CFG_PERKEY_ENABLE] = cfg[CFG_EFFECT] == EFFECT_SELF_DEFINE;
	}
	b = mk75_eff_brightness(cfg);
	if (b)
		*b = v;
	return 0;
}

static int mk75_kbd_led_set(struct led_classdev *led, enum led_brightness v)
{
	struct mk75 *k = container_of(led, struct mk75, kbd_led);
	int ret = mk75_cfg_update(k, mk75_kbd_set_fn, v);

	if (ret)
		hid_warn(k->hdev, "mk75: backlight write failed (%d)\n", ret);
	return ret;
}

/* cfg_lock held; @notify tells userspace the brightness changed in hardware */
static void mk75_sync_led(struct mk75 *k, bool notify)
{
	unsigned int kb;

	if (!k->cfg_valid || !k->features_up)
		return;
	if (k->cfg[CFG_EFFECT])
		k->saved_effect = k->cfg[CFG_EFFECT];
	kb = mk75_brightness_from_cfg(k->cfg);
	if (kb == k->kbd_led.brightness)
		return;
	k->kbd_led.brightness = kb;
#if IS_ENABLED(CONFIG_LEDS_BRIGHT_HW_CHANGED)
	if (notify)
		led_classdev_notify_brightness_hw_changed(&k->kbd_led, kb);
#endif
}

/* ------------------------------------------------------------------ */
/* Power supply                                                        */
/* ------------------------------------------------------------------ */

static enum power_supply_property mk75_psy_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int mk75_psy_get(struct power_supply *psy,
			enum power_supply_property prop,
			union power_supply_propval *val)
{
	struct mk75 *k = power_supply_get_drvdata(psy);
	unsigned long flags;
	u8 pf;
	int cap;
	bool known, link;

	spin_lock_irqsave(&k->state_lock, flags);
	pf = k->power_flags;
	cap = k->capacity;
	known = k->power_known;
	link = !k->wireless || k->link_up;
	spin_unlock_irqrestore(&k->state_lock, flags);

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		if (pf == POWER_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (pf == POWER_CHARGED)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (pf == POWER_ON_BATTERY)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = known && link;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (!known)
			return -ENODATA;
		val->intval = cap;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "Lingbao MK75";
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Lingbao";
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc mk75_psy_desc_tmpl = {
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mk75_psy_props,
	.num_properties = ARRAY_SIZE(mk75_psy_props),
	.get_property = mk75_psy_get,
};

static void mk75_set_power(struct mk75 *k, int cap, int flags)
{
	unsigned long irq;
	bool changed;

	spin_lock_irqsave(&k->state_lock, irq);
	changed = !k->power_known || k->capacity != cap ||
		  (flags >= 0 && k->power_flags != flags);
	k->capacity = clamp(cap, 0, 100);
	if (flags >= 0)
		k->power_flags = flags;
	k->power_known = true;
	spin_unlock_irqrestore(&k->state_lock, irq);

	if (changed && k->psy)
		power_supply_changed(k->psy);
}

static int mk75_poll_power(struct mk75 *k)
{
	u8 p[2];
	int ret;

	if (k->wireless) {
		/* A sleeping keyboard may not answer; keep the wait short. */
		ret = mk75_rx_get(k, 0x4a, 0, p, sizeof(p), 1000);
		if (ret >= 0 && ret < 2)
			ret = -EPROTO;
	} else {
		ret = mk75_usb_get(k, 0x07, 2, 2, p);
	}
	if (ret < 0)
		return ret;
	if (k->wireless) {
		unsigned long flags;

		spin_lock_irqsave(&k->state_lock, flags);
		k->link_up = true;	/* it answered, so the link is up */
		spin_unlock_irqrestore(&k->state_lock, flags);
	}
	mk75_set_power(k, p[0], p[1]);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Feature registration, polling, events                               */
/* ------------------------------------------------------------------ */

static void mk75_unregister_features(struct mk75 *k)
{
	if (!k->features_up)
		return;
	k->features_up = false;
	led_classdev_unregister(&k->kbd_led);
	power_supply_unregister(k->psy);
	k->psy = NULL;
}

/* Called once the config block has been read, so the LED has a value. */
static int mk75_register_features(struct mk75 *k)
{
	struct hid_device *hdev = k->hdev;
	struct power_supply_desc *desc;
	struct power_supply_config pcfg = { .drv_data = k };
	int ret;

	desc = devm_kmemdup(&hdev->dev, &mk75_psy_desc_tmpl, sizeof(*desc),
			    GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	desc->name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "hid-%s-battery",
				    dev_name(&hdev->dev));
	if (!desc->name)
		return -ENOMEM;

	k->kbd_led.name = "mk75::kbd_backlight";
	k->kbd_led.max_brightness = 4;
	k->kbd_led.brightness = mk75_brightness_from_cfg(k->cfg);
	k->kbd_led.brightness_set_blocking = mk75_kbd_led_set;
	k->kbd_led.flags = LED_BRIGHT_HW_CHANGED | LED_RETAIN_AT_SHUTDOWN;

	/* UPower ignores batteries that are added before they have data */
	mk75_poll_power(k);
	k->psy = power_supply_register(&hdev->dev, desc, &pcfg);
	if (IS_ERR(k->psy)) {
		ret = PTR_ERR(k->psy);
		k->psy = NULL;
		return ret;
	}
	ret = led_classdev_register(&hdev->dev, &k->kbd_led);
	if (ret) {
		power_supply_unregister(k->psy);
		k->psy = NULL;
		return ret;
	}
	k->features_up = true;
	return 0;
}

static void mk75_poll_work(struct work_struct *work)
{
	struct mk75 *k = container_of(work, struct mk75, poll.work);
	int ret;

	mutex_lock(&k->cfg_lock);
	ret = mk75_cfg_read(k);
	mutex_unlock(&k->cfg_lock);

	if (ret != k->last_err) {	/* log changes only, not every retry */
		hid_info(k->hdev, "mk75: config read %s (%d)\n",
			 ret ? "failed" : "ok", ret);
		k->last_err = ret;
	}

	if (!ret) {
		k->fails = 0;
		if (!k->features_up) {
			/* Only claim the device once its config reads as an MK75's */
			if (mk75_register_features(k))
				hid_warn(k->hdev, "mk75: registration failed\n");
			else
				hid_info(k->hdev, "mk75: keyboard identified (%s)\n",
					 k->wireless ? "2.4 GHz dongle" : "wired");
		}
		if (k->features_up) {
			mutex_lock(&k->cfg_lock);
			mk75_sync_led(k, true);
			mutex_unlock(&k->cfg_lock);
			mk75_poll_power(k);
		}
	} else if (k->wireless && k->features_up && ++k->fails >= MK75_MAX_FAILS) {
		/*
		 * The keyboard stopped answering through the dongle (asleep, or
		 * switched to the cable): drop out so a stale second
		 * kbd_backlight and battery do not linger.  We re-register as
		 * soon as it answers again.
		 */
		hid_info(k->hdev, "mk75: keyboard gone from the dongle\n");
		mk75_unregister_features(k);
		k->fails = 0;
	}
	schedule_delayed_work(&k->poll, MK75_POLL_INTERVAL);
}

static void mk75_event_work(struct work_struct *work)
{
	struct mk75 *k = container_of(work, struct mk75, event_work);

	if (!k->features_up)
		return;
	mutex_lock(&k->cfg_lock);
	if (!mk75_cfg_read(k))
		mk75_sync_led(k, true);
	mutex_unlock(&k->cfg_lock);
}

static int mk75_raw_event(struct hid_device *hdev, struct hid_report *report,
			  u8 *data, int size)
{
	struct mk75 *k = hid_get_drvdata(hdev);
	unsigned long flags;

	if (!k || size < 1)
		return 0;

	/* wired: lighting changed by a Fn key: 03 08 00 01 */
	if (!k->wireless && data[0] == MK75_USB_EVENT_ID && size >= 4) {
		if (data[1] == 0x08 && data[3] == 0x01)
			schedule_work(&k->event_work);
		return 0;
	}

	if (!k->wireless || data[0] != MK75_RX_REPORT_ID ||
	    size < MK75_RX_REPORT_LEN)
		return 0;

	/* unsolicited: 13 0a 01 00 len type value extra */
	if (data[1] == 0x0a) {
		switch (data[5]) {
		case 2:
			spin_lock_irqsave(&k->state_lock, flags);
			k->link_up = data[6];
			spin_unlock_irqrestore(&k->state_lock, flags);
			if (k->psy)
				power_supply_changed(k->psy);
			schedule_delayed_work(&k->poll, 0);
			break;
		case 5:
			/* flag byte is not understood; poll 0x4A for the status */
			mk75_set_power(k, data[6], -1);
			if (k->features_up)
				mod_delayed_work(system_wq, &k->poll, HZ / 2);
			break;
		}
		return 0;
	}

	spin_lock_irqsave(&k->rx_lock, flags);
	if (!k->rx_active || (data[1] & 0x7f) != k->rx_cmd)
		goto out;

	if (k->rx_ack_mode) {
		k->rx_err = (data[2] & 0x80) || data[3] != k->rx_ack_idx ?
			    -EIO : 0;
		complete(&k->rx_done);
		goto out;
	}
	if (data[2] & 0x80) {
		k->rx_err = -EIO;
		complete(&k->rx_done);
	} else if (data[3] != k->rx_expect) {
		k->rx_err = -EPROTO;
		complete(&k->rx_done);
	} else {
		size_t n = data[4] & 0x0f;

		if (n > 14 || k->rx_len + n > sizeof(k->rx_buf)) {
			k->rx_err = -EOVERFLOW;
			complete(&k->rx_done);
		} else {
			memcpy(k->rx_buf + k->rx_len, data + 5, n);
			k->rx_len += n;
			k->rx_expect++;
			if (data[3] == (data[2] & 0x7f) - 1)
				complete(&k->rx_done);
		}
	}
out:
	spin_unlock_irqrestore(&k->rx_lock, flags);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Probe / remove                                                      */
/* ------------------------------------------------------------------ */

static bool mk75_has_report(struct hid_device *hdev, unsigned int type, u8 id)
{
	return hdev->report_enum[type].report_id_hash[id] != NULL;
}

static int mk75_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct mk75 *k;
	bool wired_ctl, rx_ctl;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	wired_ctl = hdev->vendor == USB_VENDOR_ID_BYTECH_MK75 &&
		    mk75_has_report(hdev, HID_FEATURE_REPORT, MK75_USB_REPORT_ID);
	rx_ctl = hdev->vendor == USB_VENDOR_ID_COMPX &&
		 mk75_has_report(hdev, HID_OUTPUT_REPORT, MK75_RX_REPORT_ID);

	if (!wired_ctl && !rx_ctl)	/* an ordinary input interface */
		return hid_hw_start(hdev, HID_CONNECT_DEFAULT);

	k = devm_kzalloc(&hdev->dev, sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	k->hdev = hdev;
	k->wireless = rx_ctl;
	mutex_init(&k->io_lock);
	mutex_init(&k->cfg_lock);
	spin_lock_init(&k->rx_lock);
	spin_lock_init(&k->state_lock);
	init_completion(&k->rx_done);
	INIT_DELAYED_WORK(&k->poll, mk75_poll_work);
	INIT_WORK(&k->event_work, mk75_event_work);
	hid_set_drvdata(hdev, k);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;
	ret = hid_hw_open(hdev);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}
	schedule_delayed_work(&k->poll, HZ);
	return 0;
}

static void mk75_remove(struct hid_device *hdev)
{
	struct mk75 *k = hid_get_drvdata(hdev);

	if (k) {
		cancel_delayed_work_sync(&k->poll);
		cancel_work_sync(&k->event_work);
		mk75_unregister_features(k);
		hid_hw_close(hdev);
	}
	hid_hw_stop(hdev);
}

static const struct hid_device_id mk75_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_BYTECH_MK75, USB_DEVICE_ID_MK75_WIRED) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_COMPX, USB_DEVICE_ID_COMPX_RX_FA09) },
	{ }
};
MODULE_DEVICE_TABLE(hid, mk75_devices);

static struct hid_driver mk75_driver = {
	.name = "mk75",
	.id_table = mk75_devices,
	.probe = mk75_probe,
	.remove = mk75_remove,
	.raw_event = mk75_raw_event,
};
module_hid_driver(mk75_driver);

MODULE_DESCRIPTION("Lingbao MK75 keyboard: battery and backlight");
MODULE_LICENSE("GPL");

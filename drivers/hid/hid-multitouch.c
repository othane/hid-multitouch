/*
 *  HID driver for multitouch panels
 *
 *  Copyright (c) 2010 Stephane Chatty <chatty@enac.fr>
 *
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include "usbhid/usbhid.h"


MODULE_AUTHOR("Stephane Chatty <chatty@enac.fr>");
MODULE_DESCRIPTION("HID multitouch panels");
MODULE_LICENSE("GPL");

#include "hid-ids.h"

#define MAX_TRKID	USHRT_MAX


struct mt_slot {
	__u16 x, y, p;
	bool valid;	/* did we just get valid contact data for this slot? */
	bool prev_valid;/* was this slot previously valid/active? */
	__u16 trkid;	/* the tracking ID that was assigned to this slot */
};

struct mt_device {
	struct mt_class *mtclass;/* our mt device class */
	struct mt_slot *slots;	/* buffer with all slots */
	__u8 optfeatures;	/* what optional mt features do we get? */
	__u8 curcontact; 	/* index of the current contact */
	__u8 maxcontact;	/* expected last contact index */ 
	bool curvalid; 		/* is the current contact valid? */
	__u16 curcontactid; 	/* ContactID of the current contact */
	__u16 curx, cury, curp;	/* other attributes of the current contact */
	__u16 lasttrkid;	/* the last tracking ID we assigned */
};

struct mt_class {
	int (*compute_slot)(struct mt_device *);
	__u8 maxcontacts;
	__s8 inputmode;	/* InputMode HID feature number, -1 if non-existent */
};

/* classes of device behavior */
#define DUAL1 0
#define DUAL2 1
#define CYPRESS 2
#define MOSART 3

/* contact data that only some devices report */
#define PRESSURE 	(1 << 0)
#define SIZE		(1 << 1)

/*
 * these device-dependent functions determine what slot corresponds
 * to a valid contact that was just read.
 */

static int slot_from_contactid(struct mt_device *td)
{
	return td->curcontactid;
}

static int slot_from_contactnumber(struct mt_device *td)
{
	return td->curcontact;
}

static int cypress_compute_slot(struct mt_device *td)
{
	if (td->curcontactid != 0 || td->curcontact == 0)
		return td->curcontactid;
	else 
		return -1;
}


static int mosart_compute_slot(struct mt_device *td)
{
	return td->curcontactid - 1;
}

struct mt_class mt_classes[] = {
	/* DUAL1 */		{ slot_from_contactid, 2, -1 },
	/* DUAL2 */		{ slot_from_contactnumber, 2, -1 },
	/* CYPRESS */		{ cypress_compute_slot, 10, 3 },
	/* MOSART */		{ mosart_compute_slot, 2, 7 },
};


static int mt_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	switch (usage->hid & HID_USAGE_PAGE) {

	case HID_UP_GENDESK:
		switch (usage->hid) {
		case HID_GD_X:
			hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_POSITION_X);
			input_set_abs_params(hi->input, ABS_MT_POSITION_X,
						field->logical_minimum,
						field->logical_maximum, 0, 0);
			/* touchscreen emulation */
			input_set_abs_params(hi->input, ABS_X,
						field->logical_minimum,
						field->logical_maximum, 0, 0);
			return 1;
		case HID_GD_Y:
			hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_POSITION_Y);
			input_set_abs_params(hi->input, ABS_MT_POSITION_Y,
						field->logical_minimum,
						field->logical_maximum, 0, 0);
			/* touchscreen emulation */
			input_set_abs_params(hi->input, ABS_Y,
						field->logical_minimum,
						field->logical_maximum, 0, 0);
			return 1;
		}
		return 0;

	case HID_UP_DIGITIZER:
		switch (usage->hid) {
		case HID_DG_INRANGE:
		case HID_DG_CONFIDENCE:
			return -1;
		case HID_DG_TIPSWITCH:
			hid_map_usage(hi, usage, bit, max, EV_KEY, BTN_TOUCH);
			input_set_capability(hi->input, EV_KEY, BTN_TOUCH);
			return 1;
		case HID_DG_CONTACTID:
			hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_TRACKING_ID);
			input_set_abs_params(hi->input, ABS_MT_TRACKING_ID,
						0, MAX_TRKID, 0, 0);
			if (!hi->input->mt)
				input_mt_create_slots(hi->input,
						td->mtclass->maxcontacts);

		case HID_DG_TIPPRESSURE:
			hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_PRESSURE);
			td->optfeatures |= PRESSURE;
			return 1;
		case HID_DG_CONTACTCOUNT:
		case HID_DG_CONTACTMAX:
			return -1;
		}
		/* let hid-input decide for the others */
		return 0;

	case 0xff000000:
		/* we do not want to map these: no input-oriented meaning */
		return -1;
	}

	return 0;
}

static int mt_input_mapped(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	if (usage->type == EV_KEY || usage->type == EV_ABS)
		set_bit(usage->type, hi->input->evbit);

	return -1;
}

/*
 * this function is called when a whole contact has been processed,
 * so that it can assign it to a slot and store the data there
 */
static void mt_complete_slot(struct mt_device *td)
{
	if (td->curvalid) {
		struct mt_slot *slot;
		int slotnum = td->mtclass->compute_slot(td);
		if (slotnum >= 0 && slotnum <= td->mtclass->maxcontacts - 1) {
			slot = td->slots + slotnum;

			slot->valid = true;
			slot->x = td->curx;
			slot->y = td->cury;
			slot->p = td->curp;
		}
	}
	td->curcontact++;
}


/*
 * this function is called when a whole packet has been received and processed,
 * so that it can decide what to send to the input layer.
 */
static void mt_emit_event(struct mt_device *td, struct input_dev *input)
{
	struct mt_slot *oldest = 0; /* touchscreen emulation: oldest touch */
	int i;

	for (i = 0; i < td->mtclass->maxcontacts; ++i) {
		struct mt_slot *s = td->slots + i;
		if (!s->valid) {
			/*
			 * this slot does not contain useful data,
			 * notify its closure if necessary
			 */
			if (s->prev_valid) {
				input_mt_slot(input, i);	
				input_event(input, EV_ABS,
						ABS_MT_TRACKING_ID, -1);
				s->prev_valid = false;
			}
			continue;
		}
		if (!s->prev_valid)
			s->trkid = td->lasttrkid++;
		input_mt_slot(input, i);
		input_event(input, EV_ABS, ABS_MT_TRACKING_ID, s->trkid);
		input_event(input, EV_ABS, ABS_MT_POSITION_X, s->x);
		input_event(input, EV_ABS, ABS_MT_POSITION_Y, s->y);
		if (td->optfeatures & PRESSURE)
			input_event(input, EV_ABS, ABS_MT_PRESSURE, s->p);
		s->prev_valid = true;
		s->valid = false;

		/* touchscreen emulation: is this the oldest contact? */
		if (!oldest || ((s->trkid - oldest->trkid) & (SHRT_MAX + 1)))
			oldest = s;
	}

	/* touchscreen emulation */
	if (oldest) {
		input_event(input, EV_KEY, BTN_TOUCH, 1);
		input_event(input, EV_ABS, ABS_X, oldest->x);
		input_event(input, EV_ABS, ABS_Y, oldest->y);
	} else {
		input_event(input, EV_KEY, BTN_TOUCH, 0);
	}

	input_sync(input);
	td->curcontact = 0;
}



static int mt_event(struct hid_device *hid, struct hid_field *field,
				struct hid_usage *usage, __s32 value)
{
	struct mt_device *td = hid_get_drvdata(hid);

	if (hid->claimed & HID_CLAIMED_INPUT) {
		struct input_dev *input = field->hidinput->input;
		switch (usage->hid) {
		case HID_DG_INRANGE:
			break;	
		case HID_DG_TIPSWITCH:
			td->curvalid = value;
			break;
		case HID_DG_CONFIDENCE:
			break;
		case HID_DG_CONTACTID:
			td->curcontactid = value;
			break;	
		case HID_DG_TIPPRESSURE:
			td->curp = value;
			break;
		case HID_GD_X:
			td->curx = value;
			break;
		case HID_GD_Y:
			td->cury = value;
			/* works for devices where Y is last in a contact */
			mt_complete_slot(td);
			break;
		case HID_DG_CONTACTCOUNT:
			/*
			 * works for devices where contact count is
			 * the last field in a message
			 */
			if (value)
				td->maxcontact = value - 1;
			if (td->curcontact > td->maxcontact)
				mt_emit_event(td, input);
			break;
		case HID_DG_CONTACTMAX:
			break;

		default:
			/* fallback to the generic hidinput handling */
			return 0;
		}
	}

	/* we have handled the hidinput part, now remains hiddev */
	if (hid->claimed & HID_CLAIMED_HIDDEV && hid->hiddev_hid_event)
		hid->hiddev_hid_event(hid, field, usage, value);

	return 1;
}

static int mt_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct mt_device *td;

#if 0
	/*
         * todo: activate this as soon as the patch where the quirk below
         * is defined is commited. This will allow the driver to correctly
         * support devices that emit events over several HID messages.
         */
	hdev->quirks |= HID_QUIRK_NO_INPUT_SYNC;
#endif

	td = kzalloc(sizeof(struct mt_device), GFP_KERNEL);
	if (!td) {
		dev_err(&hdev->dev, "cannot allocate multitouch data\n");
		return -ENOMEM;
	}
	td->mtclass = mt_classes + id->driver_data;
	td->slots = kzalloc(td->mtclass->maxcontacts * sizeof(struct mt_slot),
				GFP_KERNEL);
	if (!td->slots) {
		dev_err(&hdev->dev, "cannot allocate multitouch data\n");
		ret = -ENOMEM;
		goto fail;
	}
	hid_set_drvdata(hdev, td);

	ret = hid_parse(hdev);
	if (ret != 0)
		goto fail;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret != 0)
		goto fail;

	if (td->mtclass->inputmode >= 0) {
		struct hid_report *r;
		struct hid_report_enum *re = hdev->report_enum
						+ HID_FEATURE_REPORT;
		r = re->report_id_hash[td->mtclass->inputmode];
		if (r) {
			r->field[0]->value[0] = 0x02;
			usbhid_submit_report(hdev, r, USB_DIR_OUT);
		}
	}
	return 0;

fail:
	kfree(td);
	return ret;
}

static void mt_remove(struct hid_device *hdev)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	hid_hw_stop(hdev);
	kfree(td->slots);
	kfree(td);
	hid_set_drvdata(hdev, NULL);
}

static const struct hid_device_id mt_devices[] = {

	/* PixCir-based panels */
	{ .driver_data = DUAL1,
		HID_USB_DEVICE(USB_VENDOR_ID_HANVON,
			USB_DEVICE_ID_HANVON_MULTITOUCH) },
	{ .driver_data = DUAL1,
		HID_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_PIXCIR_MULTI_TOUCH) },

	/* Cando panels */
	{ .driver_data = DUAL2,
		HID_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_MULTI_TOUCH) },
	{ .driver_data = DUAL2,
		HID_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_MULTI_TOUCH_11_6) },

	/* Cypress panel */
	{ .driver_data = CYPRESS,
		HID_USB_DEVICE(USB_VENDOR_ID_CYPRESS,
			USB_DEVICE_ID_CYPRESS_TRUETOUCH) },

	/* MosArt panels */
	{ .driver_data = MOSART,
		HID_USB_DEVICE(USB_VENDOR_ID_ASUS,
			USB_DEVICE_ID_ASUS_T91MT)},
	{ .driver_data = MOSART,
		HID_USB_DEVICE(USB_VENDOR_ID_ASUS,
			USB_DEVICE_ID_ASUSTEK_MULTITOUCH_YFO) },

	{ }
};
MODULE_DEVICE_TABLE(hid, mt_devices);

static const struct hid_usage_id mt_grabbed_usages[] = {
	{ HID_ANY_ID, HID_ANY_ID, HID_ANY_ID },
	{ HID_ANY_ID - 1, HID_ANY_ID - 1, HID_ANY_ID - 1}
};

static struct hid_driver mt_driver = {
	.name = "hid-multitouch",
	.id_table = mt_devices,
	.probe = mt_probe,
	.remove = mt_remove,
	.input_mapping = mt_input_mapping,
	.input_mapped = mt_input_mapped,
	.usage_table = mt_grabbed_usages,
	.event = mt_event,
};

static int __init mt_init(void)
{
	return hid_register_driver(&mt_driver);
}

static void __exit mt_exit(void)
{
	hid_unregister_driver(&mt_driver);
}

module_init(mt_init);
module_exit(mt_exit);


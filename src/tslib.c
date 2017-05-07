/*
 * (c) 2017 Martin Kepplinger <martink@posteo.de>
 * (c) 2007 Clement Chauplannaz, Thales e-Transactions <chauplac@gmail.com>
 * (c) 2006 Sascha Hauer, Pengutronix <s.hauer@pengutronix.de>
 *
 * derived from the xf86-input-void driver
 * Copyright 1999 by Frederic Lepied, France. <Lepied@XFree86.org>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is  hereby granted without fee, provided that
 * the  above copyright   notice appear  in   all  copies and  that both  that
 * copyright  notice   and   this  permission   notice  appear  in  supporting
 * documentation, and that   the  name of  Frederic   Lepied not  be  used  in
 * advertising or publicity pertaining to distribution of the software without
 * specific,  written      prior  permission.     Frederic  Lepied   makes  no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * FREDERIC  LEPIED DISCLAIMS ALL   WARRANTIES WITH REGARD  TO  THIS SOFTWARE,
 * INCLUDING ALL IMPLIED   WARRANTIES OF MERCHANTABILITY  AND   FITNESS, IN NO
 * EVENT  SHALL FREDERIC  LEPIED BE   LIABLE   FOR ANY  SPECIAL, INDIRECT   OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA  OR PROFITS, WHETHER  IN  AN ACTION OF  CONTRACT,  NEGLIGENCE OR OTHER
 * TORTIOUS  ACTION, ARISING    OUT OF OR   IN  CONNECTION  WITH THE USE    OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 */

/* tslib input driver */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <misc.h>
#include <xf86.h>
#if !defined(DGUX)
#include <xisb.h>
#endif
#include <xf86_OSproc.h>
#include <xf86Xinput.h>
#include <exevents.h>
#include <X11/keysym.h>
#include <mipointer.h>
#include <randrstr.h>
#include <xserver-properties.h>

#include <sys/time.h>
#include <time.h>
#include <stdint.h>

#if defined (__FreeBSD__)
#include <dev/evdev/input.h>
#else
#include <linux/input.h>
#endif

#include <tslib.h>
/* test old legacy interface with tslib 1.10+
#undef TSLIB_VERSION_MT
 */

#ifdef TSLIB_VERSION_MT
#include <errno.h>
#endif

#define TOUCH_MAX_SLOTS 15
#define TOUCH_SAMPLES_READ 5
#define MAXBUTTONS 11 /* > 10 */

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 23
#define HAVE_THREADED_INPUT	1
#endif

struct ts_priv {
	struct tsdev *ts;
	int height;
	int width;
	struct ts_sample last;
	ValuatorMask *valuators;
	int8_t abs_x_only;

#ifdef TSLIB_VERSION_MT
	struct ts_sample_mt **samp_mt;
	struct ts_sample_mt *last_mt;
#endif
};

static void ReadInputLegacy(InputInfoPtr local)
{
	struct ts_priv *priv = (struct ts_priv *) (local->private);
	struct ts_sample samp;
	int ret;
	int type;

	while ((ret = ts_read(priv->ts, &samp, 1)) == 1) {
		ValuatorMask *m = priv->valuators;

		if (priv->last.pressure == 0 && samp.pressure > 0) {
			type = XI_TouchBegin;
		} else if (priv->last.pressure > 0 && samp.pressure == 0) {
			type = XI_TouchEnd;
		} else if (priv->last.pressure > 0 && samp.pressure > 0) {
			type = XI_TouchUpdate;
		}

		valuator_mask_zero(m);

		if (type != XI_TouchEnd) {
			valuator_mask_set_double(m, 0, samp.x);
			valuator_mask_set_double(m, 1, samp.y);
		}

		xf86PostTouchEvent(local->dev, 0, type, 0, m);

		memcpy(&priv->last, &samp, sizeof(struct ts_sample));
	}
	if (ret < 0) {
		xf86IDrvMsg(local, X_ERROR, "ts_read failed\n");
		return;
	}

}

#ifdef TSLIB_VERSION_MT
static void ReadHandleMTSample(InputInfoPtr local, int nr, int slot)
{
	struct ts_priv *priv = (struct ts_priv *) (local->private);
	int type;
	static unsigned int next_touchid;
	static unsigned int touchids[TOUCH_MAX_SLOTS] = {0};
	ValuatorMask *m = priv->valuators;

	if (priv->last_mt[slot].pressure == 0 && priv->samp_mt[nr][slot].pressure > 0) {
		type = XI_TouchBegin;
		touchids[slot] = next_touchid++;
	} else if (priv->last_mt[slot].pressure > 0 && priv->samp_mt[nr][slot].pressure == 0) {
		type = XI_TouchEnd;
	} else if (priv->last_mt[slot].pressure > 0 && priv->samp_mt[nr][slot].pressure > 0) {
		type = XI_TouchUpdate;
	}

	valuator_mask_zero(m);

	if (type != XI_TouchEnd) {
		valuator_mask_set_double(m, 0, priv->samp_mt[nr][slot].x);
		valuator_mask_set_double(m, 1, priv->samp_mt[nr][slot].y);
	}

	xf86PostTouchEvent(local->dev, touchids[slot], type, 0, m);
}

static void ReadInputMT(InputInfoPtr local)
{
	struct ts_priv *priv = (struct ts_priv *) (local->private);
	int ret;
	int i, j;

	while (1) {
		ret = ts_read_mt(priv->ts, priv->samp_mt,
				 TOUCH_MAX_SLOTS, TOUCH_SAMPLES_READ);
		if (ret == -ENOSYS) /* tslib module_raw without MT support */
			ReadInputLegacy(local);
		else if (ret <= 0)
			return;

		for (i = 0; i < ret; i++) {
			for (j = 0; j < TOUCH_MAX_SLOTS; j++) {
				if (priv->samp_mt[i][j].valid != 1)
					continue;

				ReadHandleMTSample(local, i, j);

				memcpy(&priv->last_mt[j],
				       &priv->samp_mt[i][j],
				       sizeof(struct ts_sample_mt));
			}
		}
	}
}
#endif /* TSLIB_VERSION_MT */

static void ReadInput(InputInfoPtr local)
{
#ifdef TSLIB_VERSION_MT
	ReadInputMT(local);
#else
	ReadInputLegacy(local);
#endif
}

static void init_button_labels(Atom *labels, size_t size)
{
        assert(size > 10);

        memset(labels, 0, size * sizeof(Atom));
        labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
        labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
        labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
        labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
        labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
        labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
        labels[7] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_SIDE);
        labels[8] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_EXTRA);
        labels[9] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_FORWARD);
        labels[10] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_BACK);
}

static int xf86TslibControlProc(DeviceIntPtr device, int what)
{
	InputInfoPtr pInfo;
	unsigned char map[MAXBUTTONS + 1];
	Atom labels[MAXBUTTONS];
	int i, axiswidth, axisheight;
	struct ts_priv *priv;

#ifdef DEBUG
	xf86IDrvMsg(pInfo, X_ERROR, "%s\n", __FUNCTION__);
#endif
	pInfo = device->public.devicePrivate;
	priv = pInfo->private;

	switch (what) {
	case DEVICE_INIT:
		device->public.on = FALSE;

		memset(map, 0, sizeof(map));
		for (i = 0; i < MAXBUTTONS; i++)
			map[i + 1] = i + 1;

		init_button_labels(labels, ARRAY_SIZE(labels));

		if (InitButtonClassDeviceStruct(device,
						MAXBUTTONS,
						labels,
						map) == FALSE) {
			xf86IDrvMsg(pInfo, X_ERROR,
				    "unable to allocate Button class device\n");
			return !Success;
		}

		if (InitValuatorClassDeviceStruct(device,
						  2,
						  labels,
						  0, Absolute) == FALSE) {
			xf86IDrvMsg(pInfo, X_ERROR,
				    "unable to allocate Valuator class device\n");
			return !Success;
		}

		axiswidth = priv->width;
		axisheight = priv->height;
		if (priv->abs_x_only) {
			InitValuatorAxisStruct(device, 0,
					       XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X),
					       0,		/* min val */
					       axiswidth - 1,	/* max val */
					       axiswidth,	/* resolution */
					       0,		/* min_res */
					       axiswidth,	/* max_res */
					       Absolute);

			InitValuatorAxisStruct(device, 1,
					       XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y),
					       0,		/* min val */
					       axisheight - 1,	/* max val */
					       axisheight,	/* resolution */
					       0,		/* min_res */
					       axisheight,	/* max_res */
					       Absolute);
		} else {
			InitValuatorAxisStruct(device, 0,
					       XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_POSITION_X),
					       0,		/* min val */
					       axiswidth - 1,	/* max val */
					       axiswidth,	/* resolution */
					       0,		/* min_res */
					       axiswidth,	/* max_res */
					       Absolute);

			InitValuatorAxisStruct(device, 1,
					       XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_POSITION_Y),
					       0,		/* min val */
					       axisheight - 1,	/* max val */
					       axisheight,	/* resolution */
					       0,		/* min_res */
					       axisheight,	/* max_res */
					       Absolute);
		}

		if (InitTouchClassDeviceStruct(device,
					       TOUCH_MAX_SLOTS,
					       XIDirectTouch,
					       2 /* axes */) == FALSE) {
			xf86IDrvMsg(pInfo, X_ERROR,
				    "Unable to allocate TouchClassDeviceStruct\n");
			return !Success;
		}

	case DEVICE_ON:
#if HAVE_THREADED_INPUT
		xf86AddEnabledDevice(pInfo);
#else
		AddEnabledDevice(pInfo->fd);
#endif
		device->public.on = TRUE;
		break;

	case DEVICE_OFF:
	case DEVICE_CLOSE:
#if HAVE_THREADED_INPUT
		xf86RemoveEnabledDevice(pInfo);
#else
		RemoveEnabledDevice(pInfo->fd);
#endif
		device->public.on = FALSE;
		break;
	}
	return Success;
}

static void
xf86TslibUninit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
	struct ts_priv *priv = (struct ts_priv *)(pInfo->private);
#ifdef DEBUG
	xf86IDrvMsg(pInfo, X_ERROR, "%s\n", __FUNCTION__);
#endif

#ifdef TSLIB_VERSION_MT
	int i;

	for (i = 0; i < TOUCH_SAMPLES_READ; i++)
		free(priv->samp_mt[i]);

	free(priv->samp_mt);
	free(priv->last_mt);
#endif
	valuator_mask_free(&priv->valuators);
	xf86TslibControlProc(pInfo->dev, DEVICE_OFF);
	ts_close(priv->ts);
	free(pInfo->private);
	pInfo->private = NULL;
	xf86DeleteInput(pInfo, 0);
}

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define BIT(nr)                 (1UL << (nr))
#define BIT_MASK(nr)            (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)            ((nr) / BITS_PER_LONG)
#define BITS_PER_BYTE           8
#define BITS_PER_LONG           (sizeof(long) * BITS_PER_BYTE)
#define BITS_TO_LONGS(nr)       DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

#ifndef ABS_CNT /* < 2.6.24 kernel headers */
# define ABS_CNT (ABS_MAX+1)
#endif

static int xf86TslibInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
	struct ts_priv *priv;
	char *s;
	int i;
	struct input_absinfo abs_x;
	struct input_absinfo abs_y;
#ifdef TSLIB_VERSION_MT
	struct ts_lib_version_data *ver = ts_libversion();
#endif
	long absbit[BITS_TO_LONGS(ABS_CNT)];

	priv = calloc(1, sizeof (struct ts_priv));
	if (!priv)
		return BadValue;

	pInfo->type_name = XI_TOUCHSCREEN;
	pInfo->control_proc = NULL;
	pInfo->read_input = ReadInput;
	pInfo->device_control = xf86TslibControlProc;
	pInfo->switch_mode = NULL;
	pInfo->private = priv;
	pInfo->dev = NULL;

	s = xf86SetStrOption(pInfo->options, "path", NULL);
	if (!s)
		s = xf86SetStrOption(pInfo->options, "Device", NULL);

#ifdef TSLIB_VERSION_MT
	priv->ts = ts_setup(s, 1);
	if (!priv->ts) {
		xf86IDrvMsg(pInfo, X_ERROR, "ts_setup failed (device=%s)\n", s);
		xf86DeleteInput(pInfo, 0);
		return BadValue;
	}

	xf86IDrvMsg(pInfo, X_INFO, "using libts version %X\n", ver->version_num);
#else
	priv->ts = ts_open(s, 1);
	if (!priv->ts) {
		xf86IDrvMsg(pInfo, X_ERROR, "ts_open failed (device=%s)\n", s);
		xf86DeleteInput(pInfo, 0);
		return BadValue;
	}

	if (ts_config(priv->ts)) {
		xf86IDrvMsg(pInfo, X_ERROR, "ts_config failed\n");
		xf86DeleteInput(pInfo, 0);
		return BadValue;
	}
#endif

	pInfo->fd = ts_fd(priv->ts);

	/* process generic options */
	xf86CollectInputOptions(pInfo, NULL);
	xf86ProcessCommonOptions(pInfo, pInfo->options);

	priv->valuators = valuator_mask_new(6);
	if (!priv->valuators)
		return BadValue;

#ifdef TSLIB_VERSION_MT
	priv->samp_mt = malloc(TOUCH_SAMPLES_READ * sizeof(struct ts_sample_mt *));
	if (!priv->samp_mt)
		return BadValue;

	for (i = 0; i < TOUCH_SAMPLES_READ; i++) {
		priv->samp_mt[i] = calloc(TOUCH_MAX_SLOTS, sizeof(struct ts_sample_mt));
		if (!priv->samp_mt[i])
			return BadValue;
	}

	priv->last_mt = calloc(TOUCH_MAX_SLOTS, sizeof(struct ts_sample_mt));
	if (!priv->last_mt)
		return BadValue;

#endif /* TSLIB_VERSION_MT */

	if (ioctl(pInfo->fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) < 0) {
		xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOCGBIT failed");
		return BadValue;
	}

	if (!(absbit[BIT_WORD(ABS_MT_POSITION_X)] & BIT_MASK(ABS_MT_POSITION_X)) ||
	    !(absbit[BIT_WORD(ABS_MT_POSITION_Y)] & BIT_MASK(ABS_MT_POSITION_Y))) {
		if (!(absbit[BIT_WORD(ABS_X)] & BIT_MASK(ABS_X)) ||
		    !(absbit[BIT_WORD(ABS_Y)] & BIT_MASK(ABS_Y))) {
			xf86IDrvMsg(pInfo, X_ERROR, "no touchscreen device");
			return BadValue;
		} else {
			priv->abs_x_only = 1;
		}
	} else {
		priv->abs_x_only = 0;
	}

	if (priv->abs_x_only) {
		if (ioctl(pInfo->fd, EVIOCGABS(ABS_X), &abs_x) < 0) {
			xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOGABS failed");
			return BadValue;
		}
		if (ioctl(pInfo->fd, EVIOCGABS(ABS_Y), &abs_y) < 0) {
			xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOGABS failed");
			return BadValue;
		}
		priv->width = abs_x.maximum;
		priv->height = abs_y.maximum;
	} else {
		if (ioctl(pInfo->fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) < 0) {
			xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOGABS failed");
			return BadValue;
		}
		if (ioctl(pInfo->fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) < 0) {
			xf86IDrvMsg(pInfo, X_ERROR, "ioctl EVIOGABS failed");
			return BadValue;
		}
		priv->width = abs_x.maximum;
		priv->height = abs_y.maximum;
	}

	/* Return the configured device */
	return Success;
}

_X_EXPORT InputDriverRec TSLIB = {
	.driverVersion	= 1,
	.driverName	= "tslib",
	.PreInit	= xf86TslibInit,
	.UnInit		= xf86TslibUninit,
	.module		= NULL,
	.default_options= NULL,
#ifdef XI86_DRV_CAP_SERVER_FD
	0			/* TODO add this capability */
#endif
};

static pointer xf86TslibPlug(pointer module, pointer options, int *errmaj,
			     int *errmin)
{
	xf86AddInputDriver(&TSLIB, module, 0);
	return module;
}

static XF86ModuleVersionInfo xf86TslibVersionRec = {
	"tslib",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
	ABI_CLASS_XINPUT,
	ABI_XINPUT_VERSION,
	MOD_CLASS_XINPUT,
	{0, 0, 0, 0}	/* signature, to be patched into the file by a tool */
};

_X_EXPORT XF86ModuleData tslibModuleData = {
	.vers = &xf86TslibVersionRec,
	.setup = xf86TslibPlug,
	.teardown = NULL
};

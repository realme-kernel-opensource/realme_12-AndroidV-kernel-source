// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2021 Oplus

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/videodev2.h>
#include <linux/pinctrl/consumer.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <linux/pm_runtime.h>

#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
#include "flashlight-core.h"

#include <linux/power_supply.h>
#endif

#define AW36515_23083_NAME	"aw36515_23083"
#define AW36515_23083_I2C_ADDR	(0x63)

/* registers definitions */
#define REG_ENABLE		0x01
#define REG_LED0_FLASH_BR	0x03
#define REG_LED1_FLASH_BR	0x04
#define REG_LED0_TORCH_BR	0x05
#define REG_LED1_TORCH_BR	0x06
#define REG_FLASH_TOUT		0x08
#define REG_FLAG1		0x0A
#define REG_FLAG2		0x0B
#define REG_DEVICE_ID   0x0C

/* fault mask */
#define FAULT_TIMEOUT	(1<<0)
#define FAULT_THERMAL_SHUTDOWN	(1<<2)
#define FAULT_LED0_SHORT_CIRCUIT	(1<<5)
#define FAULT_LED1_SHORT_CIRCUIT	(1<<4)

/*  FLASH Brightness
 *	min 3.91 mA, step 7.83 mA, max 2000 mA
 */
#define AW36515_23083_FLASH_BRT_MIN 3910
#define AW36515_23083_FLASH_BRT_STEP 7830
#define AW36515_23083_FLASH_BRT_MAX 2000000
#define AW36515_23083_FLASH_BRT_uA_TO_REG(a)	\
	((a) < AW36515_23083_FLASH_BRT_MIN ? 0 :	\
	 (((a) - AW36515_23083_FLASH_BRT_MIN) / AW36515_23083_FLASH_BRT_STEP))
#define AW36515_23083_FLASH_BRT_REG_TO_uA(a)		\
	((a) * AW36515_23083_FLASH_BRT_STEP + AW36515_23083_FLASH_BRT_MIN)

/*  FLASH TIMEOUT DURATION
 *	min 40ms, step 40ms,200ms, max 1600ms
 */
#define AW36515_23083_FLASH_TOUT_MIN 40
#define AW36515_23083_FLASH_TOUT_STEP 200
#define AW36515_23083_FLASH_TOUT_MAX 1600

/*  TORCH BRT
 *	min 0.98 mA, step 1.96 mA, max 500 mA
 */
#define AW36515_23083_TORCH_BRT_MIN 980
#define AW36515_23083_TORCH_BRT_STEP 1960
#define AW36515_23083_TORCH_BRT_MAX 500000
#define AW36515_23083_TORCH_BRT_uA_TO_REG(a)	\
	((a) < AW36515_23083_TORCH_BRT_MIN ? 0 :	\
	 (((a) - AW36515_23083_TORCH_BRT_MIN) / AW36515_23083_TORCH_BRT_STEP))
#define AW36515_23083_TORCH_BRT_REG_TO_uA(a)		\
	((a) * AW36515_23083_TORCH_BRT_STEP + AW36515_23083_TORCH_BRT_MIN)

enum aw36515_23083_led_id {
	AW36515_23083_LED0 = 0,
	AW36515_23083_LED1,
	AW36515_23083_LED_MAX
};

enum v4l2_flash_led_nums {
	aw36515_CONTROL_LED0 = 2,
	aw36515_CONTROL_LED1,
};

/* struct aw36515_23083_platform_data
 *
 * @max_flash_timeout: flash timeout
 * @max_flash_brt: flash mode led brightness
 * @max_torch_brt: torch mode led brightness
 */
struct aw36515_23083_platform_data {
	u32 max_flash_timeout;
	u32 max_flash_brt[AW36515_23083_LED_MAX];
	u32 max_torch_brt[AW36515_23083_LED_MAX];
};


enum led_enable {
	MODE_SHDN = 0x0,
	MODE_TORCH = 0x08,
	MODE_FLASH = 0x0C,
};

/**
 * struct aw36515_23083_flash
 *
 * @dev: pointer to &struct device
 * @pdata: platform data
 * @regmap: reg. map for i2c
 * @lock: muxtex for serial access.
 * @led_mode: V4L2 LED mode
 * @ctrls_led: V4L2 controls
 * @subdev_led: V4L2 subdev
 */
struct aw36515_23083_flash {
	struct device *dev;
	struct aw36515_23083_platform_data *pdata;
	struct regmap *regmap;
	struct mutex lock;

	enum v4l2_flash_led_mode led_mode;
	struct v4l2_ctrl_handler ctrls_led[AW36515_23083_LED_MAX];
	struct v4l2_subdev subdev_led[AW36515_23083_LED_MAX];
	struct device_node *dnode[AW36515_23083_LED_MAX];
//	struct pinctrl *aw36515_23083_hwen_pinctrl;
//	struct pinctrl_state *aw36515_23083_hwen_high;
//	struct pinctrl_state *aw36515_23083_hwen_low;
#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
	struct flashlight_device_id flash_dev_id[AW36515_23083_LED_MAX];
#endif
};

/* define usage count */
static int use_count;
static unsigned int flash_device_id;

static struct aw36515_23083_flash *aw36515_23083_flash_data;

#define to_aw36515_23083_flash(_ctrl, _no)	\
	container_of(_ctrl->handler, struct aw36515_23083_flash, ctrls_led[_no])

/* define pinctrl */
#define aw36515_23083_PINCTRL_PIN_HWEN 0
#define aw36515_23083_PINCTRL_PINSTATE_LOW 0
#define aw36515_23083_PINCTRL_PINSTATE_HIGH 1
#define aw36515_23083_PINCTRL_STATE_HWEN_HIGH "hwen_high"
#define aw36515_23083_PINCTRL_STATE_HWEN_LOW  "hwen_low"
/******************************************************************************
 * Pinctrl configuration
 *****************************************************************************/
//static int aw36515_23083_pinctrl_init(struct aw36515_23083_flash *flash)
//{
//	int ret = 0;
//
//	/* get pinctrl */
//	flash->aw36515_23083_hwen_pinctrl = devm_pinctrl_get(flash->dev);
//	if (IS_ERR(flash->aw36515_23083_hwen_pinctrl)) {
//		pr_info("Failed to get flashlight pinctrl.\n");
//		ret = PTR_ERR(flash->aw36515_23083_hwen_pinctrl);
//		return ret;
//	}
//
//	/* Flashlight HWEN pin initialization */
//	flash->aw36515_23083_hwen_high = pinctrl_lookup_state(
//			flash->aw36515_23083_hwen_pinctrl,
//			aw36515_23083_PINCTRL_STATE_HWEN_HIGH);
//	if (IS_ERR(flash->aw36515_23083_hwen_high)) {
//		pr_info("Failed to init (%s)\n",
//			aw36515_23083_PINCTRL_STATE_HWEN_HIGH);
//		ret = PTR_ERR(flash->aw36515_23083_hwen_high);
//	}
//	flash->aw36515_23083_hwen_low = pinctrl_lookup_state(
//			flash->aw36515_23083_hwen_pinctrl,
//			aw36515_23083_PINCTRL_STATE_HWEN_LOW);
//	if (IS_ERR(flash->aw36515_23083_hwen_low)) {
//		pr_info("Failed to init (%s)\n", aw36515_23083_PINCTRL_STATE_HWEN_LOW);
//		ret = PTR_ERR(flash->aw36515_23083_hwen_low);
//	}
//
//	return ret;
//}

//static int aw36515_23083_pinctrl_set(struct aw36515_23083_flash *flash, int pin, int state)
//{
//	int ret = 0;
//
//	if (IS_ERR(flash->aw36515_23083_hwen_pinctrl)) {
//		pr_info("pinctrl is not available\n");
//		return -1;
//	}
//
//	switch (pin) {
//	case aw36515_23083_PINCTRL_PIN_HWEN:
//		if (state == aw36515_23083_PINCTRL_PINSTATE_LOW &&
//				!IS_ERR(flash->aw36515_23083_hwen_low))
//			pinctrl_select_state(flash->aw36515_23083_hwen_pinctrl,
//					flash->aw36515_23083_hwen_low);
//		else if (state == aw36515_23083_PINCTRL_PINSTATE_HIGH &&
//				!IS_ERR(flash->aw36515_23083_hwen_high))
//			pinctrl_select_state(flash->aw36515_23083_hwen_pinctrl,
//					flash->aw36515_23083_hwen_high);
//		else
//			pr_info("set err, pin(%d) state(%d)\n", pin, state);
//		break;
//	default:
//		pr_info("set err, pin(%d) state(%d)\n", pin, state);
//		break;
//	}
//	pr_info("pin(%d) state(%d)\n", pin, state);
//
//	return ret;
//}

/* enable mode control */
static int aw36515_23083_mode_ctrl(struct aw36515_23083_flash *flash)
{
	int rval = -EINVAL;

	pr_info("%s mode:%d", __func__, flash->led_mode);
	switch (flash->led_mode) {
	case V4L2_FLASH_LED_MODE_NONE:
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x0C, MODE_SHDN);
		break;
	case V4L2_FLASH_LED_MODE_FLASH:
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x0C, MODE_FLASH);
		break;
	case V4L2_FLASH_LED_MODE_TORCH:
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x0C, MODE_TORCH);
		break;
	default :
		break;
	}
	return rval;
}

/* led1/2 enable/disable */
static int aw36515_23083_enable_ctrl(struct aw36515_23083_flash *flash,
			      enum aw36515_23083_led_id led_no, bool on)
{
	int rval;

	pr_info("%s: enable:%d ", __func__, on);
	if (on) {
		rval = regmap_update_bits(flash->regmap,
				REG_ENABLE, 0x01, 0x01);
//		rval = regmap_update_bits(flash->regmap,
//			REG_ENABLE, 0x02, 0x02);
	} else {
		rval = regmap_update_bits(flash->regmap,
				REG_ENABLE, 0x01, 0x00);
//		rval = regmap_update_bits(flash->regmap,
//			REG_ENABLE, 0x02, 0x00);
	}
	pr_debug("%s: return val:%d", __func__,  rval);
	return rval;
}

//static int aw36515_23083_select_led(struct aw36515_23083_flash *flash,
//										int led_num)
//{
//	int rval;
//	pr_info("engineer cam select led->%d", led_num);
//	if (led_num == aw36515_CONTROL_LED0) {
//		rval = regmap_update_bits(flash->regmap, REG_ENABLE, 0x01, 0x01);
//		rval = regmap_update_bits(flash->regmap, REG_ENABLE, 0x02, 0x00);
//	} else if (led_num == aw36515_CONTROL_LED1) {
//		rval = regmap_update_bits(flash->regmap, REG_ENABLE, 0x01, 0x00);
//		rval = regmap_update_bits(flash->regmap, REG_ENABLE, 0x02, 0x02);
//	} else {
//		rval = regmap_update_bits(flash->regmap, REG_ENABLE, 0x01, 0x00);
//		rval = regmap_update_bits(flash->regmap, REG_ENABLE, 0x02, 0x00);
//	}
//	 return rval;
//}

/* torch1/2 brightness control */
static int aw36515_23083_torch_brt_ctrl(struct aw36515_23083_flash *flash,
				 enum aw36515_23083_led_id led_no, unsigned int brt)
{
	int rval;
	u8 br_bits;

	pr_info("%s set brt:%u for both torch leds", __func__, brt);

	if (brt < AW36515_23083_TORCH_BRT_MIN)
		return aw36515_23083_enable_ctrl(flash, led_no, false);

	if (brt >= AW36515_23083_TORCH_BRT_MAX) {
		brt = AW36515_23083_TORCH_BRT_MAX;
		pr_info("%s limit to torch max %d", __func__, AW36515_23083_TORCH_BRT_MAX);
	}

	br_bits = AW36515_23083_TORCH_BRT_uA_TO_REG(brt);

	rval = regmap_update_bits(flash->regmap,
				REG_LED0_TORCH_BR, 0xFF, br_bits);
//	rval = regmap_update_bits(flash->regmap,
//				REG_LED1_TORCH_BR, 0xFF, br_bits);

	pr_info("%s: return val:%d  br_bits = 0x%x", __func__,  rval, br_bits);
	return rval;
}

/* flash1/2 brightness control */
static int aw36515_23083_flash_brt_ctrl(struct aw36515_23083_flash *flash,
				 enum aw36515_23083_led_id led_no, unsigned int brt)
{
	int rval;
	u8 br_bits;

	pr_info("%s: set brt:%u for both leds", __func__,  brt);

	if (brt < AW36515_23083_FLASH_BRT_MIN)
		return aw36515_23083_enable_ctrl(flash, led_no, false);

	if (brt >= AW36515_23083_FLASH_BRT_MAX) {
		brt = AW36515_23083_FLASH_BRT_MAX;
		pr_info("%s limit to flash max %d", __func__, AW36515_23083_FLASH_BRT_MAX);
	}

	br_bits = AW36515_23083_FLASH_BRT_uA_TO_REG(brt);


	rval = regmap_update_bits(flash->regmap,
				  REG_LED0_FLASH_BR, 0xFF, br_bits);
//	rval = regmap_update_bits(flash->regmap,
//				  REG_LED1_FLASH_BR, 0xFF, br_bits);

	pr_info("%s: return val:%d  br_bits = 0x%x", __func__,  rval, br_bits);
	return rval;
}

/* flash1/2 timeout control */
static int aw36515_23083_flash_tout_ctrl(struct aw36515_23083_flash *flash,
				unsigned int tout)
{
	int rval;
	u8 tout_bits = 0x00;

	if (tout > 1600) {
		tout = 1600;
		pr_info("%s: limit to max tout = %d", __func__, tout);
	}

	if (tout <= 40) {
		tout_bits = 0x00;
	} else if (tout <= 400) {
		tout_bits = (tout - 40) / 40;
	} else if (tout <= 1600) {
		tout_bits = (tout - 400) / 200 + 0x09;
	}

	rval = regmap_update_bits(flash->regmap,
				  REG_FLASH_TOUT, 0x0F, tout_bits);

	pr_info("%s: tout = %d , tout_bits = 0x%x, ", __func__, tout, tout_bits);

	return rval;
}

/* v4l2 controls  */
static int aw36515_23083_get_ctrl(struct v4l2_ctrl *ctrl, enum aw36515_23083_led_id led_no)
{
	struct aw36515_23083_flash *flash = to_aw36515_23083_flash(ctrl, led_no);
	int rval = -EINVAL;

	mutex_lock(&flash->lock);

	if (ctrl->id == V4L2_CID_FLASH_FAULT) {
		s32 fault = 0;
		unsigned int reg_val = 0;

		rval = regmap_read(flash->regmap, REG_FLAG1, &reg_val);
		if (rval < 0)
			goto out;
		if (reg_val & FAULT_LED0_SHORT_CIRCUIT)
			fault |= V4L2_FLASH_FAULT_SHORT_CIRCUIT;
		if (reg_val & FAULT_LED1_SHORT_CIRCUIT)
			fault |= V4L2_FLASH_FAULT_SHORT_CIRCUIT;
		if (reg_val & FAULT_THERMAL_SHUTDOWN)
			fault |= V4L2_FLASH_FAULT_OVER_TEMPERATURE;
		if (reg_val & FAULT_TIMEOUT)
			fault |= V4L2_FLASH_FAULT_TIMEOUT;
		ctrl->cur.val = fault;
	}

out:
	mutex_unlock(&flash->lock);
	return rval;
}

static int aw36515_23083_set_ctrl(struct v4l2_ctrl *ctrl, enum aw36515_23083_led_id led_no)
{
	struct aw36515_23083_flash *flash = to_aw36515_23083_flash(ctrl, led_no);
	int rval = -EINVAL;

	unsigned int boost_reg_val;
	rval = regmap_read(flash->regmap, REG_FLAG2, &boost_reg_val);
	pr_info("%s: led_no:%d ctrID:0x%x, ctrlVal:0x%x, boost_reg_val:0x%x", __func__, led_no, ctrl->id, ctrl->val, boost_reg_val);
	mutex_lock(&flash->lock);

	switch (ctrl->id) {
	case V4L2_CID_FLASH_LED_MODE:
		flash->led_mode = ctrl->val;
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH)
			rval = aw36515_23083_mode_ctrl(flash);
		else
			rval = 0;
		if (flash->led_mode == V4L2_FLASH_LED_MODE_NONE)
			aw36515_23083_enable_ctrl(flash, led_no, false);
		else if (flash->led_mode == V4L2_FLASH_LED_MODE_TORCH)
			rval = aw36515_23083_enable_ctrl(flash, led_no, true);
		break;

	case V4L2_CID_FLASH_STROBE_SOURCE:
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x20, (ctrl->val) << 5);
		if (rval < 0)
			goto err_out;
		break;

	case V4L2_CID_FLASH_STROBE:
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH) {
			rval = -EBUSY;
			goto err_out;
		}
		flash->led_mode = V4L2_FLASH_LED_MODE_FLASH;
		rval = aw36515_23083_mode_ctrl(flash);
		rval = aw36515_23083_enable_ctrl(flash, led_no, true);
		break;

	case V4L2_CID_FLASH_STROBE_STOP:
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH) {
			rval = -EBUSY;
			goto err_out;
		}
		aw36515_23083_enable_ctrl(flash, led_no, false);
		flash->led_mode = V4L2_FLASH_LED_MODE_NONE;
		rval = aw36515_23083_mode_ctrl(flash);
		break;

	case V4L2_CID_FLASH_TIMEOUT:
		rval = aw36515_23083_flash_tout_ctrl(flash, ctrl->val);
		break;

	case V4L2_CID_FLASH_INTENSITY:
		rval = aw36515_23083_flash_brt_ctrl(flash, led_no, ctrl->val);
		break;

	case V4L2_CID_FLASH_TORCH_INTENSITY:
		rval = aw36515_23083_torch_brt_ctrl(flash, led_no, ctrl->val);
		break;
	default :
		break;
	}

err_out:
	mutex_unlock(&flash->lock);
	return rval;
}

static int aw36515_23083_led1_get_ctrl(struct v4l2_ctrl *ctrl)
{
	return aw36515_23083_get_ctrl(ctrl, AW36515_23083_LED1);
}

static int aw36515_23083_led1_set_ctrl(struct v4l2_ctrl *ctrl)
{
	return aw36515_23083_set_ctrl(ctrl, AW36515_23083_LED1);
}

static int aw36515_23083_led0_get_ctrl(struct v4l2_ctrl *ctrl)
{
	return aw36515_23083_get_ctrl(ctrl, AW36515_23083_LED0);
}

static int aw36515_23083_led0_set_ctrl(struct v4l2_ctrl *ctrl)
{
	return aw36515_23083_set_ctrl(ctrl, AW36515_23083_LED0);
}

static const struct v4l2_ctrl_ops aw36515_23083_led_ctrl_ops[AW36515_23083_LED_MAX] = {
	[AW36515_23083_LED0] = {
			.g_volatile_ctrl = aw36515_23083_led0_get_ctrl,
			.s_ctrl = aw36515_23083_led0_set_ctrl,
			},
	[AW36515_23083_LED1] = {
			.g_volatile_ctrl = aw36515_23083_led1_get_ctrl,
			.s_ctrl = aw36515_23083_led1_set_ctrl,
			}
};

static int aw36515_23083_init_controls(struct aw36515_23083_flash *flash,
				enum aw36515_23083_led_id led_no)
{
	struct v4l2_ctrl *fault;
	u32 max_flash_brt = flash->pdata->max_flash_brt[led_no];
	u32 max_torch_brt = flash->pdata->max_torch_brt[led_no];
	struct v4l2_ctrl_handler *hdl = &flash->ctrls_led[led_no];
	const struct v4l2_ctrl_ops *ops = &aw36515_23083_led_ctrl_ops[led_no];

	v4l2_ctrl_handler_init(hdl, 8);

	/* flash mode */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_FLASH_LED_MODE,
			       V4L2_FLASH_LED_MODE_TORCH, ~0x7,
			       V4L2_FLASH_LED_MODE_NONE);
	flash->led_mode = V4L2_FLASH_LED_MODE_NONE;

	/* flash source */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_FLASH_STROBE_SOURCE,
			       0x1, ~0x3, V4L2_FLASH_STROBE_SOURCE_SOFTWARE);

	/* flash strobe */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_STROBE, 0, 0, 0, 0);

	/* flash strobe stop */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_STROBE_STOP, 0, 0, 0, 0);

	/* flash strobe timeout */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_TIMEOUT,
			  AW36515_23083_FLASH_TOUT_MIN,
			  flash->pdata->max_flash_timeout,
			  AW36515_23083_FLASH_TOUT_STEP,
			  flash->pdata->max_flash_timeout);

	/* flash brt */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_INTENSITY,
			  AW36515_23083_FLASH_BRT_MIN, max_flash_brt,
			  AW36515_23083_FLASH_BRT_STEP, max_flash_brt);

	/* torch brt */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_TORCH_INTENSITY,
			  AW36515_23083_TORCH_BRT_MIN, max_torch_brt,
			  AW36515_23083_TORCH_BRT_STEP, max_torch_brt);

	/* fault */
	fault = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_FAULT, 0,
				  V4L2_FLASH_FAULT_OVER_VOLTAGE
				  | V4L2_FLASH_FAULT_OVER_TEMPERATURE
				  | V4L2_FLASH_FAULT_SHORT_CIRCUIT
				  | V4L2_FLASH_FAULT_TIMEOUT, 0, 0);
	if (fault != NULL)
		fault->flags |= V4L2_CTRL_FLAG_VOLATILE;

	if (hdl->error)
		return hdl->error;

	flash->subdev_led[led_no].ctrl_handler = hdl;
	return 0;
}

/* initialize device */
static const struct v4l2_subdev_ops aw36515_23083_ops = {
	.core = NULL,
};

static const struct regmap_config aw36515_23083_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xF,
};

static void aw36515_23083_v4l2_i2c_subdev_init(struct v4l2_subdev *sd,
		struct i2c_client *client,
		const struct v4l2_subdev_ops *ops)
{
	v4l2_subdev_init(sd, ops);
	sd->flags |= V4L2_SUBDEV_FL_IS_I2C;
	/* the owner is the same as the i2c_client's driver owner */
	sd->owner = client->dev.driver->owner;
	sd->dev = &client->dev;
	/* i2c_client and v4l2_subdev point to one another */
	v4l2_set_subdevdata(sd, client);
	i2c_set_clientdata(client, sd);
	/* initialize name */
	snprintf(sd->name, sizeof(sd->name), "%s %d-%04x",
		client->dev.driver->name, i2c_adapter_id(client->adapter),
		client->addr);
}

static int aw36515_23083_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	int ret;

	pr_info("%s\n", __func__);

	ret = pm_runtime_get_sync(sd->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(sd->dev);
		return ret;
	}

	return 0;
}

static int aw36515_23083_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pr_info("%s\n", __func__);

	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops aw36515_23083_int_ops = {
	.open = aw36515_23083_open,
	.close = aw36515_23083_close,
};

static int aw36515_23083_subdev_init(struct aw36515_23083_flash *flash,
			      enum aw36515_23083_led_id led_no, char *led_name)
{
	struct i2c_client *client = to_i2c_client(flash->dev);
	struct device_node *np = flash->dev->of_node, *child;
	const char *fled_name = "flash";
	int rval;

	// pr_info("%s %d", __func__, led_no);

	aw36515_23083_v4l2_i2c_subdev_init(&flash->subdev_led[led_no],
				client, &aw36515_23083_ops);
	flash->subdev_led[led_no].flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	flash->subdev_led[led_no].internal_ops = &aw36515_23083_int_ops;
	strscpy(flash->subdev_led[led_no].name, led_name,
		sizeof(flash->subdev_led[led_no].name));

	for (child = of_get_child_by_name(np, fled_name); child;
			child = of_find_node_by_name(child, fled_name)) {
		int rv;
		u32 reg = 0;

		rv = of_property_read_u32(child, "reg", &reg);
		if (rv)
			continue;

		if (reg == led_no) {
			flash->dnode[led_no] = child;
			flash->subdev_led[led_no].fwnode =
				of_fwnode_handle(flash->dnode[led_no]);
		}
	}

	rval = aw36515_23083_init_controls(flash, led_no);
	if (rval)
		goto err_out;
	rval = media_entity_pads_init(&flash->subdev_led[led_no].entity, 0, NULL);
	if (rval < 0)
		goto err_out;
	flash->subdev_led[led_no].entity.function = MEDIA_ENT_F_FLASH;

	rval = v4l2_async_register_subdev(&flash->subdev_led[led_no]);
	if (rval < 0)
		goto err_out;

	return rval;

err_out:
	v4l2_ctrl_handler_free(&flash->ctrls_led[led_no]);
	return rval;
}

/* flashlight init */
static int aw36515_23083_init(struct aw36515_23083_flash *flash)
{
	int rval = 0;
	unsigned int reg_val;

//	aw36515_23083_pinctrl_set(flash, aw36515_23083_PINCTRL_PIN_HWEN, aw36515_23083_PINCTRL_PINSTATE_HIGH);

	/* set timeout */
	rval = aw36515_23083_flash_tout_ctrl(flash, 400);
	if (rval < 0)
		return rval;
	/* output disable */
	flash->led_mode = V4L2_FLASH_LED_MODE_NONE;
	rval = aw36515_23083_mode_ctrl(flash);
	if (rval < 0)
		return rval;

	rval = regmap_update_bits(flash->regmap,
				  REG_LED0_TORCH_BR, 0xFF, 0x00);
	if (rval < 0)
		return rval;
	rval = regmap_update_bits(flash->regmap,
				  REG_LED0_FLASH_BR, 0xFF, 0x00);
	if (rval < 0)
		return rval;
	/* reset faults */
	rval = regmap_read(flash->regmap, REG_FLAG1, &reg_val);
	return rval;
}

/* flashlight uninit */
static int aw36515_23083_uninit(struct aw36515_23083_flash *flash)
{
//	aw36515_23083_pinctrl_set(flash,
//			aw36515_23083_PINCTRL_PIN_HWEN, aw36515_23083_PINCTRL_PINSTATE_LOW);

	return 0;
}

static int aw36515_23083_flash_open(void)
{
	int rval = -EINVAL;
	rval = regmap_read(aw36515_23083_flash_data->regmap, REG_DEVICE_ID, &flash_device_id);
	pr_info("%s: flash_device_id :%x", __func__, flash_device_id);
	return 0;
}

static int aw36515_23083_flash_release(void)
{
	return 0;
}

static int aw36515_23083_ioctl(unsigned int cmd, unsigned long arg)
{
	struct flashlight_dev_arg *fl_arg;
	int channel;
//	int ret;

	fl_arg = (struct flashlight_dev_arg *)arg;
	channel = fl_arg->channel;

	switch (cmd) {
	case FLASH_IOC_SET_ONOFF:
		pr_info("FLASH_IOC_SET_ONOFF(%d): %d\n",
				channel, (int)fl_arg->arg);
		if ((int)fl_arg->arg) {
			aw36515_23083_torch_brt_ctrl(aw36515_23083_flash_data, channel, 25000);
			aw36515_23083_flash_data->led_mode = V4L2_FLASH_LED_MODE_TORCH;
			aw36515_23083_mode_ctrl(aw36515_23083_flash_data);
		} else {
			aw36515_23083_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
			aw36515_23083_mode_ctrl(aw36515_23083_flash_data);
			aw36515_23083_enable_ctrl(aw36515_23083_flash_data, channel, false);
		}
		break;
/*
	case OPLUS_FLASH_IOC_SELECT_LED_NUM:
		if (fl_arg->arg == aw36515_CONTROL_LED0 || fl_arg->arg == aw36515_CONTROL_LED1) {
			aw36515_23083_flash_data->led_mode = V4L2_FLASH_LED_MODE_FLASH;
			 ret = aw36515_23083_select_led(aw36515_23083_flash_data, fl_arg->arg);
			if (ret < 0) {
				pr_err("engineer cam set led[%d] fail", fl_arg->arg);
				return ret;
			}
		} else {
			aw36515_23083_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
			aw36515_23083_mode_ctrl(aw36515_23083_flash_data);
			aw36515_23083_enable_ctrl(aw36515_23083_flash_data, channel, false);
		}
		break;
*/
	default:
		pr_info("No such command and arg(%d): (%d, %d)\n",
				channel, _IOC_NR(cmd), (int)fl_arg->arg);
		return -ENOTTY;
	}

	return 0;
}

static int aw36515_23083_set_driver(int set)
{
	int ret = 0;

	/* set chip and usage count */
	//mutex_lock(&aw36515_23083_mutex);
	if (set) {
		if (!use_count)
			ret = aw36515_23083_init(aw36515_23083_flash_data);
		use_count++;
		pr_debug("Set driver: %d\n", use_count);
	} else {
		use_count--;
		if (!use_count)
			ret = aw36515_23083_uninit(aw36515_23083_flash_data);
		if (use_count < 0)
			use_count = 0;
		pr_debug("Unset driver: %d\n", use_count);
	}
	//mutex_unlock(&aw36515_23083_mutex);

	return 0;
}

static ssize_t aw36515_23083_strobe_store(struct flashlight_arg arg)
{
	aw36515_23083_set_driver(1);
	//aw36515_23083_set_level(arg.channel, arg.level);
	//aw36515_23083_timeout_ms[arg.channel] = 0;
	//aw36515_23083_enable(arg.channel);
	aw36515_23083_torch_brt_ctrl(aw36515_23083_flash_data, arg.channel,
				arg.level * 25000);
	aw36515_23083_flash_data->led_mode = V4L2_FLASH_LED_MODE_TORCH;
	aw36515_23083_mode_ctrl(aw36515_23083_flash_data);
	aw36515_23083_enable_ctrl(aw36515_23083_flash_data, arg.channel, true);
	msleep(arg.dur);
	//aw36515_23083_disable(arg.channel);
	aw36515_23083_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
	aw36515_23083_mode_ctrl(aw36515_23083_flash_data);
	aw36515_23083_enable_ctrl(aw36515_23083_flash_data, arg.channel, false);
	aw36515_23083_set_driver(0);
	return 0;
}

static struct flashlight_operations aw36515_23083_flash_ops = {
	aw36515_23083_flash_open,
	aw36515_23083_flash_release,
	aw36515_23083_ioctl,
	aw36515_23083_strobe_store,
	aw36515_23083_set_driver
};

static int aw36515_23083_parse_dt(struct aw36515_23083_flash *flash)
{
	struct device_node *np, *cnp;
	struct device *dev = flash->dev;
	u32 decouple = 0;
	int i = 0;

	if (!dev || !dev->of_node)
		return -ENODEV;

	np = dev->of_node;
	for_each_child_of_node(np, cnp) {
		if (of_property_read_u32(cnp, "type",
					&flash->flash_dev_id[i].type))
			goto err_node_put;
		if (of_property_read_u32(cnp,
					"ct", &flash->flash_dev_id[i].ct))
			goto err_node_put;
		if (of_property_read_u32(cnp,
					"part", &flash->flash_dev_id[i].part))
			goto err_node_put;
		snprintf(flash->flash_dev_id[i].name, FLASHLIGHT_NAME_SIZE,
				flash->subdev_led[i].name);
		flash->flash_dev_id[i].channel = i;
		flash->flash_dev_id[i].decouple = decouple;

		pr_info("Parse dt (type,ct,part,name,channel,decouple)=(%d,%d,%d,%s,%d,%d).\n",
				flash->flash_dev_id[i].type,
				flash->flash_dev_id[i].ct,
				flash->flash_dev_id[i].part,
				flash->flash_dev_id[i].name,
				flash->flash_dev_id[i].channel,
				flash->flash_dev_id[i].decouple);
		if (flashlight_dev_register_by_device_id(&flash->flash_dev_id[i],
			&aw36515_23083_flash_ops))
			return -EFAULT;
		i++;
	}

	return 0;

err_node_put:
	of_node_put(cnp);
	return -EINVAL;
}

static int aw36515_23083_probe(struct i2c_client *client,
			const struct i2c_device_id *devid)
{
	struct aw36515_23083_flash *flash;
	struct aw36515_23083_platform_data *pdata = dev_get_platdata(&client->dev);
	int rval;

	pr_info("%s:%d", __func__, __LINE__);

	flash = devm_kzalloc(&client->dev, sizeof(*flash), GFP_KERNEL);
	if (flash == NULL)
		return -ENOMEM;

	flash->regmap = devm_regmap_init_i2c(client, &aw36515_23083_regmap);
	if (IS_ERR(flash->regmap)) {
		rval = PTR_ERR(flash->regmap);
		return rval;
	}

	/* if there is no platform data, use chip default value */
	if (pdata == NULL) {
		pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
		if (pdata == NULL)
			return -ENODEV;
		pdata->max_flash_timeout = AW36515_23083_FLASH_TOUT_MAX;
		/* led 1 */
		pdata->max_flash_brt[AW36515_23083_LED0] = AW36515_23083_FLASH_BRT_MAX;
		pdata->max_torch_brt[AW36515_23083_LED0] = AW36515_23083_TORCH_BRT_MAX;
		/* led 2 */
		pdata->max_flash_brt[AW36515_23083_LED1] = AW36515_23083_FLASH_BRT_MAX;
		pdata->max_torch_brt[AW36515_23083_LED1] = AW36515_23083_TORCH_BRT_MAX;
	}
	flash->pdata = pdata;
	flash->dev = &client->dev;
	mutex_init(&flash->lock);
	aw36515_23083_flash_data = flash;

//	rval = aw36515_23083_pinctrl_init(flash);
//	if (rval < 0)
//		return rval;

	rval = aw36515_23083_subdev_init(flash, AW36515_23083_LED0, "aw36515_23083-led0");
	if (rval < 0)
		return rval;

//	rval = aw36515_23083_subdev_init(flash, AW36515_23083_LED1, "aw36515_23083-led1");
//	if (rval < 0)
//		return rval;

	pm_runtime_enable(flash->dev);

	rval = aw36515_23083_parse_dt(flash);

	i2c_set_clientdata(client, flash);


	rval = regmap_update_bits(flash->regmap,
				REG_LED0_TORCH_BR, 0xFF, 0x33);//torch current 100mA

	pr_info("%s:%d", __func__, __LINE__);
	return 0;
}

static int aw36515_23083_remove(struct i2c_client *client)
{
	struct aw36515_23083_flash *flash = i2c_get_clientdata(client);
	unsigned int i;

	for (i = AW36515_23083_LED0; i < AW36515_23083_LED_MAX; i++) {
		v4l2_device_unregister_subdev(&flash->subdev_led[i]);
		v4l2_ctrl_handler_free(&flash->ctrls_led[i]);
		media_entity_cleanup(&flash->subdev_led[i].entity);
	}

	pm_runtime_disable(&client->dev);

	pm_runtime_set_suspended(&client->dev);
	return 0;
}

static int __maybe_unused aw36515_23083_suspend(struct device *dev)
{
	//struct i2c_client *client = to_i2c_client(dev);
	//struct aw36515_23083_flash *flash = i2c_get_clientdata(client);

	pr_info("%s %d", __func__, __LINE__);

	return 0;
}

static int __maybe_unused aw36515_23083_resume(struct device *dev)
{
	//struct i2c_client *client = to_i2c_client(dev);
	//struct aw36515_23083_flash *flash = i2c_get_clientdata(client);

	pr_info("%s %d", __func__, __LINE__);

	return 0;
}

static const struct i2c_device_id aw36515_23083_id_table[] = {
	{AW36515_23083_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, aw36515_23083_id_table);

static const struct of_device_id aw36515_23083_of_table[] = {
	{ .compatible = "mediatek,aw36515_23083" },
	{ },
};
MODULE_DEVICE_TABLE(of, aw36515_23083_of_table);

static const struct dev_pm_ops aw36515_23083_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(aw36515_23083_suspend, aw36515_23083_resume, NULL)
};

static struct i2c_driver aw36515_23083_i2c_driver = {
	.driver = {
		   .name = AW36515_23083_NAME,
		   .pm = &aw36515_23083_pm_ops,
		   .of_match_table = aw36515_23083_of_table,
		   },
	.probe = aw36515_23083_probe,
	.remove = aw36515_23083_remove,
	.id_table = aw36515_23083_id_table,
};

module_i2c_driver(aw36515_23083_i2c_driver);

MODULE_AUTHOR("XXX");
MODULE_DESCRIPTION("aw36515_23083 LED flash v4l2 driver");
MODULE_LICENSE("GPL");

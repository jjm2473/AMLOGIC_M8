/*
 * AMLOGIC backlight driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the named License,
 * or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
 *
 * Author:  Wang Han <han.wang@amlogic.com>
 *  
 * Modify:  Evoke Zhang <evoke.zhang@amlogic.com>
 * compatible dts
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/backlight.h>
#include <linux/slab.h>
#include <linux/amlogic/aml_bl.h>
#include <linux/workqueue.h>
#include <mach/power_gate.h>
#ifdef CONFIG_ARCH_MESON6    
#include <mach/mod_gate.h>
#endif /* CONFIG_ARCH_MESON6 */
#include <linux/amlogic/aml_gpio_consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/delay.h>
#include <linux/amlogic/aml_lcd_bl.h>

//#define MESON_BACKLIGHT_DEBUG
#ifdef MESON_BACKLIGHT_DEBUG
#define DPRINT(...) printk(KERN_INFO __VA_ARGS__)
#define DTRACE()    DPRINT(KERN_INFO "%s()\n", __FUNCTION__)
#else
#define DPRINT(...)
#define DTRACE()
#endif /* MESON_BACKLIGHT_DEBUG */

#define BL_LEVEL_DEFAULT					BL_LEVEL_MID
#define BL_NAME 							"backlight"
#define bl_gpio_request(gpio) 				amlogic_gpio_request(gpio, BL_NAME)
#define bl_gpio_free(gpio) 					amlogic_gpio_free(gpio, BL_NAME)
#define bl_gpio_direction_input(gpio) 		amlogic_gpio_direction_input(gpio, BL_NAME)
#define bl_gpio_direction_output(gpio, val) amlogic_gpio_direction_output(gpio, val, BL_NAME)
#define bl_gpio_get_value(gpio) 			amlogic_get_value(gpio, BL_NAME)
#define bl_gpio_set_value(gpio,val) 		amlogic_set_value(gpio, val, BL_NAME)

#ifdef LCD_BACKLIGHT_SUPPORT
/* for lcd backlight power */
typedef enum {
	BL_CTL_GPIO = 0,
	BL_CTL_PWM_NEGATIVE,
	BL_CTL_PWM_POSITIVE,
	BL_CTL_PWM_COMBO,
	BL_CTL_MAX,
} BL_Ctrl_Method_t;

static const char* bl_ctrl_method_table[]={
	"gpio",
	"pwm_negative",
	"pwm_positive",
	"pwm_combo",
	"null"
};

typedef enum {
	BL_PWM_A = 0,
	BL_PWM_B,
	BL_PWM_C,
	BL_PWM_D,
	BL_PWM_MAX,
} BL_PWM_t;

typedef struct {
	unsigned level_default;
	unsigned level_mid;
	unsigned level_mid_mapping;
	unsigned level_min;
	unsigned level_max;
	unsigned short power_on_delay;
    unsigned char method;
	int gpio;
	unsigned dim_max;
	unsigned dim_min;
	unsigned char pwm_port;
	unsigned char pwm_gpio_used;
	unsigned pwm_cnt;
	unsigned pwm_pre_div;
	unsigned pwm_max;
	unsigned pwm_min;
	
	unsigned combo_level_switch;
	unsigned char combo_high_port;
	unsigned char combo_high_method;
	unsigned char combo_low_port;
	unsigned char combo_low_method;
	unsigned combo_high_cnt;
	unsigned combo_high_pre_div;
	unsigned combo_high_duty_max;
	unsigned combo_high_duty_min;
	unsigned combo_low_cnt;
	unsigned combo_low_pre_div;
	unsigned combo_low_duty_max;
	unsigned combo_low_duty_min;
	
	struct pinctrl *p;
	struct workqueue_struct *workqueue;
	struct delayed_work bl_delayed_work;
} Lcd_Bl_Config_t;

static Lcd_Bl_Config_t bl_config;
static unsigned bl_level = BL_LEVEL_DEFAULT;
static int bl_status = 1;
static int bl_real_status = 1;

#define FIN_FREQ				(24 * 1000)

static DEFINE_MUTEX(bl_power_mutex);
static void power_on_bl(void)
{
	struct pinctrl_state *s;
	int ret;
	
	mutex_lock(&bl_power_mutex);
	DPRINT("%s: bl_level=%u, bl_status=%s, bl_real_status=%s\n", __FUNCTION__, bl_level, (bl_status ? "ON" : "OFF"), (bl_real_status ? "ON" : "OFF"));
	if ((bl_level == 0) || (bl_status == 0)) {
		goto exit_power_on_bl;
	}
	
	if (bl_config.method == BL_CTL_GPIO) {
		aml_set_reg32_bits(P_LED_PWM_REG0, 1, 12, 2);
		mdelay(20);
		bl_gpio_direction_output(bl_config.gpio, 1);
	}
	else if ((bl_config.method == BL_CTL_PWM_NEGATIVE) || (bl_config.method == BL_CTL_PWM_POSITIVE)) {
		switch (bl_config.pwm_port) {
			case BL_PWM_A:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, bl_config.combo_high_pre_div, 8, 7);  //pwm_a_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 4, 2);  //pwm_a_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 15, 1);  //pwm_a_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 0, 1);  //enable pwm_a
				break;
			case BL_PWM_B:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, bl_config.combo_high_pre_div, 16, 7);  //pwm_b_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 6, 2);  //pwm_b_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 23, 1);  //pwm_b_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 1, 1);  //enable pwm_b
				break;
			case BL_PWM_C:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, bl_config.combo_high_pre_div, 8, 7);  //pwm_c_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 4, 2);  //pwm_c_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 15, 1);  //pwm_c_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 0, 1);  //enable pwm_c
				break;
			case BL_PWM_D:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, bl_config.combo_high_pre_div, 16, 7);  //pwm_d_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 6, 2);  //pwm_d_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 23, 1);  //pwm_d_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 1, 1);  //enable pwm_d
				break;
			default:
				break;
		}
	
		if (IS_ERR(bl_config.p)) {
			printk("set backlight pinmux error.\n");
			goto exit_power_on_bl;
		}
		s = pinctrl_lookup_state(bl_config.p, "default");	//select pinctrl
		if (IS_ERR(s)) {
			printk("set backlight pinmux error.\n");
			devm_pinctrl_put(bl_config.p);
			goto exit_power_on_bl;
		}
	
		ret = pinctrl_select_state(bl_config.p, s);	//set pinmux and lock pins
		if (ret < 0) {
			printk("set backlight pinmux error.\n");
			devm_pinctrl_put(bl_config.p);
			goto exit_power_on_bl;
		}
		mdelay(20);
		if (bl_config.pwm_gpio_used) {
			if (bl_config.gpio)
				bl_gpio_direction_output(bl_config.gpio, 1);
		}
	}
	else if (bl_config.method == BL_CTL_PWM_COMBO) {
		switch (bl_config.combo_high_port) {
			case BL_PWM_A:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, bl_config.combo_high_pre_div, 8, 7);  //pwm_a_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 4, 2);  //pwm_a_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 15, 1);  //pwm_a_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 0, 1);  //enable pwm_a
				break;
			case BL_PWM_B:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, bl_config.combo_high_pre_div, 16, 7);  //pwm_b_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 6, 2);  //pwm_b_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 23, 1);  //pwm_b_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 1, 1);  //enable pwm_b
				break;
			case BL_PWM_C:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, bl_config.combo_high_pre_div, 8, 7);  //pwm_c_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 4, 2);  //pwm_c_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 15, 1);  //pwm_c_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 0, 1);  //enable pwm_c
				break;
			case BL_PWM_D:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, bl_config.combo_high_pre_div, 16, 7);  //pwm_d_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 6, 2);  //pwm_d_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 23, 1);  //pwm_d_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 1, 1);  //enable pwm_d
				break;
			default:
				break;
		}
		switch (bl_config.combo_low_port) {
			case BL_PWM_A:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, bl_config.combo_high_pre_div, 8, 7);  //pwm_a_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 4, 2);  //pwm_a_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 15, 1);  //pwm_a_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 0, 1);  //enable pwm_a
				break;
			case BL_PWM_B:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, bl_config.combo_high_pre_div, 16, 7);  //pwm_b_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 6, 2);  //pwm_b_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 23, 1);  //pwm_b_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 1, 1, 1);  //enable pwm_b
				break;
			case BL_PWM_C:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, bl_config.combo_high_pre_div, 8, 7);  //pwm_c_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 4, 2);  //pwm_c_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 15, 1);  //pwm_c_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 0, 1);  //enable pwm_c
				break;
			case BL_PWM_D:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, bl_config.combo_high_pre_div, 16, 7);  //pwm_d_clk_div
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 6, 2);  //pwm_d_clk_sel
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 23, 1);  //pwm_d_clk_en
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 1, 1, 1);  //enable pwm_d
				break;
			default:
				break;
		}
	
		if (IS_ERR(bl_config.p)) {
			printk("set backlight pinmux error.\n");
			goto exit_power_on_bl;
		}
		s = pinctrl_lookup_state(bl_config.p, "pwm_combo");	//select pinctrl	
		if (IS_ERR(s)) {
			printk("set backlight pinmux error.\n");
			devm_pinctrl_put(bl_config.p);
			goto exit_power_on_bl;
		}
	
		ret = pinctrl_select_state(bl_config.p, s);	//set pinmux and lock pins
		if (ret < 0) {
			printk("set backlight pinmux error.\n");
			devm_pinctrl_put(bl_config.p);
			goto exit_power_on_bl;
		}
	}
	else {
		printk("Wrong backlight control method\n");
		goto exit_power_on_bl;
	}
	bl_real_status = 1;
	printk("backlight power on\n");
	
exit_power_on_bl:
	mutex_unlock(&bl_power_mutex);
}

void bl_power_on(int bl_flag)
{
	mutex_lock(&bl_power_mutex);
	if (bl_flag == LCD_BL_FLAG)
		bl_status = 1;
	
	DPRINT("%s(bl_flag=%s): bl_level=%u, bl_status=%s, bl_real_status=%s\n", __FUNCTION__, (bl_flag ? "LCD_BL_FLAG" : "DRV_BL_FLAG"), bl_level, (bl_status ? "ON" : "OFF"), (bl_real_status ? "ON" : "OFF"));
	if ((bl_level == 0) || (bl_real_status == 1)) {
		mutex_unlock(&bl_power_mutex);
		return;
	}
	
	mutex_unlock(&bl_power_mutex);
	queue_delayed_work(bl_config.workqueue, &bl_config.bl_delayed_work, msecs_to_jiffies(bl_config.power_on_delay)); 
	
	DPRINT("bl_power_on...\n");
}

void bl_power_off(int bl_flag)
{
	mutex_lock(&bl_power_mutex);
	
	if (bl_flag == LCD_BL_FLAG)
		bl_status = 0;
	
	DPRINT("%s(bl_flag=%s): bl_level=%u, bl_status=%s, bl_real_status=%s\n", __FUNCTION__, (bl_flag ? "LCD_BL_FLAG" : "DRV_BL_FLAG"), bl_level, (bl_status ? "ON" : "OFF"), (bl_real_status ? "ON" : "OFF"));	
	if (bl_real_status == 0) {
		mutex_unlock(&bl_power_mutex);
		return;
	}
	
	if (bl_config.method == BL_CTL_GPIO) {
		bl_gpio_direction_output(bl_config.gpio, 0);
	}
	else if ((bl_config.method == BL_CTL_PWM_NEGATIVE) || (bl_config.method == BL_CTL_PWM_POSITIVE)) {
		if (bl_config.pwm_gpio_used) {
			if (bl_config.gpio)
				bl_gpio_direction_output(bl_config.gpio, 0);
		}
		switch (bl_config.pwm_port) {
			case BL_PWM_A:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 0, 1);  //disable pwm_a
				break;
			case BL_PWM_B:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 1, 1);  //disable pwm_b
				break;
			case BL_PWM_C:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 0, 1);  //disable pwm_c
				break;
			case BL_PWM_D:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 1, 1);	//disable pwm_d
				break;
			default:
				break;
		}
	}
	else if (bl_config.method == BL_CTL_PWM_COMBO) {
		switch (bl_config.combo_high_port) {
			case BL_PWM_A:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 0, 1);  //disable pwm_a
				break;
			case BL_PWM_B:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 1, 1);  //disable pwm_b
				break;
			case BL_PWM_C:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 0, 1);  //disable pwm_c
				break;
			case BL_PWM_D:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 1, 1);	//disable pwm_d
				break;
			default:
				break;
		}
		switch (bl_config.combo_low_port) {
			case BL_PWM_A:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 0, 1);  //disable pwm_a
				break;
			case BL_PWM_B:
				aml_set_reg32_bits(P_PWM_MISC_REG_AB, 0, 1, 1);  //disable pwm_b
				break;
			case BL_PWM_C:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 0, 1);  //disable pwm_c
				break;
			case BL_PWM_D:
				aml_set_reg32_bits(P_PWM_MISC_REG_CD, 0, 1, 1);	//disable pwm_d
				break;
			default:
				break;
		}
	}
	bl_real_status = 0;
	printk("backlight power off\n");
	mutex_unlock(&bl_power_mutex);
}

static DEFINE_MUTEX(bl_level_mutex);
static void set_backlight_level(unsigned level)
{
	unsigned pwm_hi = 0, pwm_lo = 0;

	mutex_lock(&bl_level_mutex);
	
	DPRINT("set_backlight_level: %u, last level: %u\n", level, bl_level);
	level = (level > bl_config.level_max ? bl_config.level_max : (level < bl_config.level_min ? (level < BL_LEVEL_OFF ? 0 : bl_config.level_min) : level));
	bl_level = level;

	if (bl_level == 0) {
		if (bl_real_status == 1)
			bl_power_off(DRV_BL_FLAG);
	}
	else {
		//mapping
		if (level > bl_config.level_mid)
			level = ((level - bl_config.level_mid) * (bl_config.level_max - bl_config.level_mid_mapping)) / (bl_config.level_max - bl_config.level_mid) + bl_config.level_mid_mapping;
		else
			level = ((level - bl_config.level_min) * (bl_config.level_mid_mapping - bl_config.level_min)) / (bl_config.level_mid - bl_config.level_min) + bl_config.level_min;
		DPRINT("level mapping=%u\n", level);
		if (bl_config.method == BL_CTL_GPIO) {
			level = bl_config.dim_min - ((level - bl_config.level_min) * (bl_config.dim_min - bl_config.dim_max)) / (bl_config.level_max - bl_config.level_min);
			aml_set_reg32_bits(P_LED_PWM_REG0, level, 0, 4);
		}
		else if ((bl_config.method == BL_CTL_PWM_NEGATIVE) || (bl_config.method == BL_CTL_PWM_POSITIVE)) {
			level = (bl_config.pwm_max - bl_config.pwm_min) * (level - bl_config.level_min) / (bl_config.level_max - bl_config.level_min) + bl_config.pwm_min;
			if (bl_config.method == BL_CTL_PWM_NEGATIVE) {
				pwm_hi = bl_config.pwm_cnt - level;
				pwm_lo = level;
			}
			else {
				pwm_hi = level;
				pwm_lo = bl_config.pwm_cnt - level;
			}
			switch (bl_config.pwm_port) {
				case BL_PWM_A:
					aml_write_reg32(P_PWM_PWM_A, (pwm_hi << 16) | (pwm_lo));
					break;
				case BL_PWM_B:
					aml_write_reg32(P_PWM_PWM_B, (pwm_hi << 16) | (pwm_lo));
					break;
				case BL_PWM_C:
					aml_write_reg32(P_PWM_PWM_C, (pwm_hi << 16) | (pwm_lo));
					break;
				case BL_PWM_D:
					aml_write_reg32(P_PWM_PWM_D, (pwm_hi << 16) | (pwm_lo));
					break;
				default:
					break;
			}
		}
		else if (bl_config.method == BL_CTL_PWM_COMBO) {
			if (level >= bl_config.combo_level_switch) {
				//pre_set combo_low duty max
				if (bl_config.combo_low_method == BL_CTL_PWM_NEGATIVE) {
					pwm_hi = bl_config.combo_low_cnt - bl_config.combo_low_duty_max;
					pwm_lo = bl_config.combo_low_duty_max;
				}
				else {
					pwm_hi = bl_config.combo_low_duty_max;
					pwm_lo = bl_config.combo_low_cnt - bl_config.combo_low_duty_max;
				}
				switch (bl_config.combo_low_port) {
					case BL_PWM_A:
						aml_write_reg32(P_PWM_PWM_A, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_B:
						aml_write_reg32(P_PWM_PWM_B, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_C:
						aml_write_reg32(P_PWM_PWM_C, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_D:
						aml_write_reg32(P_PWM_PWM_D, (pwm_hi << 16) | (pwm_lo));
						break;
					default:
						break;
				}
				
				//set combo_high duty
				level = (bl_config.combo_high_duty_max - bl_config.combo_high_duty_min) * (level - bl_config.combo_level_switch) / (bl_config.level_max - bl_config.combo_level_switch) + bl_config.combo_high_duty_min;
				if (bl_config.combo_high_method == BL_CTL_PWM_NEGATIVE) {
					pwm_hi = bl_config.combo_high_cnt - level;
					pwm_lo = level;
				}
				else {
					pwm_hi = level;
					pwm_lo = bl_config.combo_high_cnt - level;
				}
				switch (bl_config.combo_high_port) {
					case BL_PWM_A:
						aml_write_reg32(P_PWM_PWM_A, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_B:
						aml_write_reg32(P_PWM_PWM_B, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_C:
						aml_write_reg32(P_PWM_PWM_C, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_D:
						aml_write_reg32(P_PWM_PWM_D, (pwm_hi << 16) | (pwm_lo));
						break;
					default:
						break;
				}
			}
			else {
				//pre_set combo_high duty min
				if (bl_config.combo_high_method == BL_CTL_PWM_NEGATIVE) {
					pwm_hi = bl_config.combo_high_cnt - bl_config.combo_high_duty_min;
					pwm_lo = bl_config.combo_high_duty_min;
				}
				else {
					pwm_hi = bl_config.combo_high_duty_min;;
					pwm_lo = bl_config.combo_high_cnt - bl_config.combo_high_duty_min;
				}
				switch (bl_config.combo_high_port) {
					case BL_PWM_A:
						aml_write_reg32(P_PWM_PWM_A, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_B:
						aml_write_reg32(P_PWM_PWM_B, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_C:
						aml_write_reg32(P_PWM_PWM_C, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_D:
						aml_write_reg32(P_PWM_PWM_D, (pwm_hi << 16) | (pwm_lo));
						break;
					default:
						break;
				}
				
				//set combo_low duty
				level = (bl_config.combo_low_duty_max - bl_config.combo_low_duty_min) * (level - bl_config.level_min) / (bl_config.combo_level_switch - bl_config.level_min) + bl_config.combo_low_duty_min;
				if (bl_config.combo_low_method == BL_CTL_PWM_NEGATIVE) {
					pwm_hi = bl_config.combo_low_cnt - level;
					pwm_lo = level;
				}
				else {
					pwm_hi = level;
					pwm_lo = bl_config.combo_low_cnt - level;
				}
				switch (bl_config.combo_low_port) {
					case BL_PWM_A:
						aml_write_reg32(P_PWM_PWM_A, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_B:
						aml_write_reg32(P_PWM_PWM_B, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_C:
						aml_write_reg32(P_PWM_PWM_C, (pwm_hi << 16) | (pwm_lo));
						break;
					case BL_PWM_D:
						aml_write_reg32(P_PWM_PWM_D, (pwm_hi << 16) | (pwm_lo));
						break;
					default:
						break;
				}
			}
		}
		if ((bl_status == 1) && (bl_real_status == 0))
			bl_power_on(DRV_BL_FLAG);
	}
	mutex_unlock(&bl_level_mutex);
}

static unsigned get_backlight_level(void)
{
	DPRINT("%s: %d\n", __FUNCTION__, bl_level);
	return bl_level;
}
#else
typedef struct {
	unsigned level_default;
	struct pinctrl *p;
} Bl_Config_t;

static Bl_Config_t bl_config;

void set_backlight_level(unsigned level)
{
	return;
}

unsigned get_backlight_level(void)
{
	return 0;
}
#endif

struct aml_bl {
    const struct aml_bl_platform_data   *pdata;
    struct backlight_device         *bldev;
    struct platform_device          *pdev;
};

static int aml_bl_update_status(struct backlight_device *bd)
{
    struct aml_bl *amlbl = bl_get_data(bd);
    int brightness = bd->props.brightness;

    //DPRINT("%s() brightness=%d\n", __FUNCTION__, brightness);
    //DPRINT("%s() pdata->set_bl_level=%p\n", __FUNCTION__, amlbl->pdata->set_bl_level);

    if (brightness < 0)
        brightness = 0;
    else if (brightness > 255)
        brightness = 255;

    if (amlbl->pdata->set_bl_level)
        amlbl->pdata->set_bl_level(brightness);

    return 0;
}

static int aml_bl_get_brightness(struct backlight_device *bd)
{
    struct aml_bl *amlbl = bl_get_data(bd);

    DPRINT("%s() pdata->get_bl_level=%p\n", __FUNCTION__, amlbl->pdata->get_bl_level);

    if (amlbl->pdata->get_bl_level)
        return amlbl->pdata->get_bl_level();
    else
        return 0;
}

static const struct backlight_ops aml_bl_ops = {
    .get_brightness = aml_bl_get_brightness,
    .update_status  = aml_bl_update_status,
};

#ifdef CONFIG_USE_OF
static struct aml_bl_platform_data meson_backlight_platform =
{
    //.power_on_bl = power_on_backlight,
    //.power_off_bl = power_off_backlight,
    .get_bl_level = get_backlight_level,
    .set_bl_level = set_backlight_level,
	.max_brightness = BL_LEVEL_MAX,
	.dft_brightness = BL_LEVEL_DEFAULT,
};

#define AMLOGIC_BL_DRV_DATA ((kernel_ulong_t)&meson_backlight_platform)

static const struct of_device_id backlight_dt_match[] = {
	{
		.compatible = "amlogic,backlight",
		.data = (void *)AMLOGIC_BL_DRV_DATA
	},
	{},
};
#else
#define backlight_dt_match NULL
#endif

#ifdef CONFIG_USE_OF
static inline struct aml_bl_platform_data *bl_get_driver_data(struct platform_device *pdev)
{
	const struct of_device_id *match;
	
	if(pdev->dev.of_node) {
		DPRINT("***of_device: get backlight driver data.***\n");		
		match = of_match_node(backlight_dt_match, pdev->dev.of_node);
		return (struct aml_bl_platform_data *)match->data;
	}
	return NULL;
}
#endif

#ifdef LCD_BACKLIGHT_SUPPORT
static inline int _get_backlight_config(struct platform_device *pdev)
{
	int ret=0;
	int val;
	const char *str;
	unsigned int bl_para[3];
	unsigned pwm_freq, pwm_cnt, pwm_pre_div;
	int i;
	
	if (pdev->dev.of_node) {
		ret = of_property_read_u32_array(pdev->dev.of_node,"bl_level_default_uboot_kernel", &bl_para[0], 2);
		if(ret){
			printk("faild to get bl_level_default_uboot_kernel\n");
			bl_config.level_default = BL_LEVEL_DEFAULT;
		}
		else {
			bl_config.level_default = bl_para[1];
		}
		DPRINT("bl level default kernel=%u\n", bl_config.level_default);
		ret = of_property_read_u32_array(pdev->dev.of_node, "bl_level_middle_mapping", &bl_para[0], 2);
		if (ret) {
			printk("faild to get bl_level_middle_mapping!\n");
			bl_config.level_mid = BL_LEVEL_MID;
			bl_config.level_mid_mapping = BL_LEVEL_MID_MAPPED;
		}
		else {
			bl_config.level_mid = bl_para[0];
			bl_config.level_mid_mapping = bl_para[1];
		}
		DPRINT("bl level mid=%u, mid_mapping=%u\n", bl_config.level_mid, bl_config.level_mid_mapping);
		ret = of_property_read_u32_array(pdev->dev.of_node,"bl_level_max_min", &bl_para[0],2);
		if(ret){
			printk("faild to get bl_level_max_min\n");
			bl_config.level_min = BL_LEVEL_MIN;
			bl_config.level_max = BL_LEVEL_MAX;
		}
		else {
			bl_config.level_max = bl_para[0];
			bl_config.level_min = bl_para[1];
		}
		DPRINT("bl level max=%u, min=%u\n", bl_config.level_max, bl_config.level_min);
		
		ret = of_property_read_u32(pdev->dev.of_node, "bl_power_on_delay", &val);
		if (ret) {
			printk("faild to get bl_power_on_delay\n");
			bl_config.power_on_delay = 100;
		}
		else {
			val = val & 0xffff;
			bl_config.power_on_delay = (unsigned short)val;
		}
		DPRINT("bl power_on_delay: %ums\n", bl_config.power_on_delay);
		ret = of_property_read_u32(pdev->dev.of_node, "bl_ctrl_method", &val);
		if (ret) {
			printk("faild to get bl_ctrl_method\n");
#if (MESON_CPU_TYPE == MESON_CPU_TYPE_MESON6)
			bl_config.method = BL_CTL_GPIO;
#elif (MESON_CPU_TYPE == MESON_CPU_TYPE_MESON8)
			bl_config.method = BL_CTL_PWM_NEGATIVE;
#endif
		}
		else {
			val = (val >= BL_CTL_MAX) ? (BL_CTL_MAX-1) : val;
			bl_config.method = (unsigned char)val;
		}
		DPRINT("bl control_method: %s(%u)\n", bl_ctrl_method_table[bl_config.method], bl_config.method);
		
		ret = of_property_read_string_index(pdev->dev.of_node, "bl_pwm_port_gpio_used", 1, &str);
		if (ret) {
			printk("faild to get bl_pwm_port_gpio_used!\n");
			bl_config.pwm_gpio_used = 0;
		}
		else {
			if (strncmp(str, "1", 1) == 0)
				bl_config.pwm_gpio_used = 1;
			else
				bl_config.pwm_gpio_used = 0;
			DPRINT("bl_pwm gpio_used: %u\n", bl_config.pwm_gpio_used);
		}
		ret = of_property_read_string_index(pdev->dev.of_node, "bl_pwm_port_gpio_used", 0, &str);
		if (ret) {
			printk("faild to get bl_pwm_port_gpio_used!\n");
#if (MESON_CPU_TYPE == MESON_CPU_TYPE_MESON6)
			bl_config.pwm_port = BL_PWM_D;
#elif (MESON_CPU_TYPE == MESON_CPU_TYPE_MESON8)
			bl_config.pwm_port = BL_PWM_C;
#endif
		}
		else {
			if (strcmp(str, "PWM_A") == 0)
				bl_config.pwm_port = BL_PWM_A;
			else if (strcmp(str, "PWM_B") == 0)
				bl_config.pwm_port = BL_PWM_B;
			else if (strcmp(str, "PWM_C") == 0)
				bl_config.pwm_port = BL_PWM_C;
			else if (strcmp(str, "PWM_D") == 0)
				bl_config.pwm_port = BL_PWM_D;
			DPRINT("bl pwm_port: %s(%u)\n", str, bl_config.pwm_port);
		}
		if ((bl_config.method == BL_CTL_GPIO) || ((bl_config.pwm_gpio_used == 1) && ((bl_config.method == BL_CTL_PWM_NEGATIVE) || (bl_config.method == BL_CTL_PWM_POSITIVE)))) {
			ret = of_property_read_string(pdev->dev.of_node, "bl_gpio_port", &str);
			if (ret) {
				printk("faild to get bl_gpio_port!\n");
#if (MESON_CPU_TYPE == MESON_CPU_TYPE_MESON6)
				str = "GPIOD_1";
#elif (MESON_CPU_TYPE == MESON_CPU_TYPE_MESON8)
				str = "GPIODV_28";
#endif
			}
			val = amlogic_gpio_name_map_num(str);
			if (val > 0) {
				ret = bl_gpio_request(val);
				if (ret) {
				  printk("faild to alloc bl gpio (%s)!\n", str);
				}
				bl_config.gpio = val;
				DPRINT("bl gpio = %s(%d)\n", str, bl_config.gpio);
			}
			else {
				bl_config.gpio = -1;
			}
		}
		ret = of_property_read_u32_array(pdev->dev.of_node,"bl_gpio_dim_max_min",&bl_para[0],2);
		if (ret) {
			printk("faild to get bl_gpio_dim_max_min\n");
			bl_config.dim_max = 0x0;
			bl_config.dim_min = 0xf;
		}
		else {
			bl_config.dim_max = bl_para[0];
			bl_config.dim_min = bl_para[1];
		}
		DPRINT("bl dim max=%u, min=%u\n", bl_config.dim_max, bl_config.dim_min);
		ret = of_property_read_u32(pdev->dev.of_node,"bl_pwm_freq",&val);
		if (ret) {
#if (MESON_CPU_TYPE == MESON_CPU_TYPE_MESON6)
			pwm_freq = 1000;
#elif (MESON_CPU_TYPE == MESON_CPU_TYPE_MESON8)
			pwm_freq = 300000;
#endif
			printk("faild to get bl_pwm_freq, default set to %u\n", pwm_freq);
		}
		else {	
			pwm_freq = ((val >= (FIN_FREQ * 500)) ? (FIN_FREQ * 500) : val);
		}
		for (i=0; i<0x7f; i++) {
			pwm_pre_div = i;
			pwm_cnt = FIN_FREQ * 1000 / (pwm_freq * (pwm_pre_div + 1)) - 2;
			if (pwm_cnt <= 0xffff)
				break;
		}			
		bl_config.pwm_cnt = pwm_cnt;
		bl_config.pwm_pre_div = pwm_pre_div;
		DPRINT("bl pwm_frequency=%u, cnt=%u, div=%u\n", pwm_freq, bl_config.pwm_cnt, bl_config.pwm_pre_div);
		ret = of_property_read_u32_array(pdev->dev.of_node,"bl_pwm_duty_max_min",&bl_para[0],2);
		if (ret) {
			printk("faild to get bl_pwm_duty_max_min\n");
			bl_para[0] = 100;
			bl_para[1] = 20;
		}		
		bl_config.pwm_max = (bl_config.pwm_cnt * bl_para[0] / 100);
		bl_config.pwm_min = (bl_config.pwm_cnt * bl_para[1] / 100);
		DPRINT("bl pwm_duty max=%u\%, min=%u\%\n", bl_para[0], bl_para[1]);
		
		//pwm combo
		ret = of_property_read_u32(pdev->dev.of_node,"bl_pwm_combo_high_low_level_switch",&val);
		if (ret) {
			printk("faild to get bl_pwm_combo_high_low_level_switch\n");
			val = bl_config.level_mid;
		}
		if (val > bl_config.level_mid)
			val = ((val - bl_config.level_mid) * (bl_config.level_max - bl_config.level_mid_mapping)) / (bl_config.level_max - bl_config.level_mid) + bl_config.level_mid_mapping;
		else
			val = ((val - bl_config.level_min) * (bl_config.level_mid_mapping - bl_config.level_min)) / (bl_config.level_mid - bl_config.level_min) + bl_config.level_min;
		bl_config.combo_level_switch = val;
		DPRINT("bl pwm_combo level switch =%u\n", bl_config.combo_level_switch);
		ret = of_property_read_string_index(pdev->dev.of_node, "bl_pwm_combo_high_port_method", 0, &str);
		if (ret) {
			printk("faild to get bl_pwm_combo_high_port_method!\n");
			str = "PWM_C";
			bl_config.combo_high_port = BL_PWM_C;
		}
		else {
			if (strcmp(str, "PWM_A") == 0)
				bl_config.combo_high_port = BL_PWM_A;
			else if (strcmp(str, "PWM_B") == 0)
				bl_config.combo_high_port = BL_PWM_B;
			else if (strcmp(str, "PWM_C") == 0)
				bl_config.combo_high_port = BL_PWM_C;
			else if (strcmp(str, "PWM_D") == 0)
				bl_config.combo_high_port = BL_PWM_D;
		}
		DPRINT("bl pwm_combo high port: %s(%u)\n", str, bl_config.combo_high_port);
		ret = of_property_read_string_index(pdev->dev.of_node, "bl_pwm_combo_high_port_method", 1, &str);
		if (ret) {
			printk("faild to get bl_pwm_combo_high_port_method!\n");
			str = "1";
			bl_config.combo_high_method = BL_CTL_PWM_NEGATIVE;
		}
		else {
			if (strncmp(str, "1", 1) == 0)
				bl_config.combo_high_method = BL_CTL_PWM_NEGATIVE;
			else
				bl_config.combo_high_method = BL_CTL_PWM_POSITIVE;
		}
		DPRINT("bl pwm_combo high method: %s(%u)\n", bl_ctrl_method_table[bl_config.combo_high_method], bl_config.combo_high_method);
		ret = of_property_read_string_index(pdev->dev.of_node, "bl_pwm_combo_low_port_method", 0, &str);
		if (ret) {
			printk("faild to get bl_pwm_combo_low_port_method!\n");
			str = "PWM_D";
			bl_config.combo_low_port = BL_PWM_D;
		}
		else {
			if (strcmp(str, "PWM_A") == 0)
				bl_config.combo_low_port = BL_PWM_A;
			else if (strcmp(str, "PWM_B") == 0)
				bl_config.combo_low_port = BL_PWM_B;
			else if (strcmp(str, "PWM_C") == 0)
				bl_config.combo_low_port = BL_PWM_C;
			else if (strcmp(str, "PWM_D") == 0)
				bl_config.combo_low_port = BL_PWM_D;
		}
		DPRINT("bl pwm_combo high port: %s(%u)\n", str, bl_config.combo_low_port);
		ret = of_property_read_string_index(pdev->dev.of_node, "bl_pwm_combo_low_port_method", 1, &str);
		if (ret) {
			printk("faild to get bl_pwm_combo_low_port_method!\n");
			str = "1";
			bl_config.combo_low_method = BL_CTL_PWM_NEGATIVE;
		}
		else {
			if (strncmp(str, "1", 1) == 0)
				bl_config.combo_low_method = BL_CTL_PWM_NEGATIVE;
			else
				bl_config.combo_low_method = BL_CTL_PWM_POSITIVE;
		}
		DPRINT("bl pwm_combo low method: %s(%u)\n", bl_ctrl_method_table[bl_config.combo_low_method], bl_config.combo_low_method);
		ret = of_property_read_u32_array(pdev->dev.of_node,"bl_pwm_combo_high_freq_duty_max_min",&bl_para[0],3);
		if (ret) {
			printk("faild to get bl_pwm_combo_high_freq_duty_max_min\n");
			bl_para[0] = 300000;	//freq=300k
			bl_para[1] = 100;
			bl_para[2] = 50;
		}
		pwm_freq = ((bl_para[0] >= (FIN_FREQ * 500)) ? (FIN_FREQ * 500) : bl_para[0]);
		for (i=0; i<0x7f; i++) {
			pwm_pre_div = i;
			pwm_cnt = FIN_FREQ * 1000 / (pwm_freq * (pwm_pre_div + 1)) - 2;
			if (pwm_cnt <= 0xffff)
				break;
		}			
		bl_config.combo_high_cnt = pwm_cnt;
		bl_config.combo_high_pre_div = pwm_pre_div;
		bl_config.combo_high_duty_max = (bl_config.combo_high_cnt * bl_para[1] / 100);
		bl_config.combo_high_duty_min = (bl_config.combo_high_cnt * bl_para[2] / 100);
		DPRINT("bl pwm_combo high freq=%uHz, duty_max=%u\%, duty_min=%u\%\n", pwm_freq, bl_para[1], bl_para[2]);
		ret = of_property_read_u32_array(pdev->dev.of_node,"bl_pwm_combo_low_freq_duty_max_min",&bl_para[0],3);
		if (ret) {
			printk("faild to get bl_pwm_combo_low_freq_duty_max_min\n");
			bl_para[0] = 1000;	//freq=1k
			bl_para[1] = 100;
			bl_para[2] = 50;
		}
		pwm_freq = ((bl_para[0] >= (FIN_FREQ * 500)) ? (FIN_FREQ * 500) : bl_para[0]);
		for (i=0; i<0x7f; i++) {
			pwm_pre_div = i;
			pwm_cnt = FIN_FREQ * 1000 / (pwm_freq * (pwm_pre_div + 1)) - 2;
			if (pwm_cnt <= 0xffff)
				break;
		}			
		bl_config.combo_low_cnt = pwm_cnt;
		bl_config.combo_low_pre_div = pwm_pre_div;
		bl_config.combo_low_duty_max = (bl_config.combo_low_cnt * bl_para[1] / 100);
		bl_config.combo_low_duty_min = (bl_config.combo_low_cnt * bl_para[2] / 100);
		DPRINT("bl pwm_combo low freq=%uHz, duty_max=%u\%, duty_min=%u\%\n", pwm_freq, bl_para[1], bl_para[2]);
		
		//pinmux
		bl_config.p = devm_pinctrl_get(&pdev->dev);
		if (IS_ERR(bl_config.p))
			printk("get backlight pinmux error.\n");
	}
	return ret;
}
#else
static inline int _get_backlight_config(struct platform_device *pdev)
{
	return 0;
}	
#endif

static int aml_bl_probe(struct platform_device *pdev)
{
    struct backlight_properties props;
    const struct aml_bl_platform_data *pdata;
    struct backlight_device *bldev;
    struct aml_bl *amlbl;
    int retval;

    DTRACE();

    amlbl = kzalloc(sizeof(struct aml_bl), GFP_KERNEL);
    if (!amlbl)
    {
        printk(KERN_ERR "%s() kzalloc error\n", __FUNCTION__);
        return -ENOMEM;
    }

    amlbl->pdev = pdev;
	
#ifdef CONFIG_USE_OF	
	pdata = bl_get_driver_data(pdev);
#else	
    pdata = pdev->dev.platform_data;
#endif	
    if (!pdata) {
        printk(KERN_ERR "%s() missing platform data\n", __FUNCTION__);
        retval = -ENODEV;
        goto err;
    }

#ifdef CONFIG_USE_OF
	_get_backlight_config(pdev);
#endif
	
    amlbl->pdata = pdata;

    DPRINT("%s() pdata->bl_init=%p\n", __FUNCTION__, pdata->bl_init);
    DPRINT("%s() pdata->power_on_bl=%p\n", __FUNCTION__, pdata->power_on_bl);
    DPRINT("%s() pdata->power_off_bl=%p\n", __FUNCTION__, pdata->power_off_bl);
    DPRINT("%s() pdata->set_bl_level=%p\n", __FUNCTION__, pdata->set_bl_level);
    DPRINT("%s() pdata->get_bl_level=%p\n", __FUNCTION__, pdata->get_bl_level);
    DPRINT("%s() pdata->max_brightness=%d\n", __FUNCTION__, pdata->max_brightness);
    DPRINT("%s() pdata->dft_brightness=%d\n", __FUNCTION__, pdata->dft_brightness);

    memset(&props, 0, sizeof(struct backlight_properties));
#ifdef CONFIG_USE_OF
	props.max_brightness = (bl_config.level_max > 0 ? bl_config.level_max : BL_LEVEL_MAX);
#else
    props.max_brightness = (pdata->max_brightness > 0 ? pdata->max_brightness : BL_LEVEL_MAX);
#endif
    props.type = BACKLIGHT_RAW;
    bldev = backlight_device_register("aml-bl", &pdev->dev, amlbl, &aml_bl_ops, &props);
    if (IS_ERR(bldev)) {
        printk(KERN_ERR "failed to register backlight\n");
        retval = PTR_ERR(bldev);
        goto err;
    }

    amlbl->bldev = bldev;

    platform_set_drvdata(pdev, amlbl);

    bldev->props.power = FB_BLANK_UNBLANK;
#ifdef CONFIG_USE_OF
	bldev->props.brightness = (bl_config.level_default > 0 ? bl_config.level_default : BL_LEVEL_DEFAULT);
#else
    bldev->props.brightness = (pdata->dft_brightness > 0 ? pdata->dft_brightness : BL_LEVEL_DEFAULT);
#endif

    if (pdata->bl_init)
        pdata->bl_init();
    if (pdata->power_on_bl)
        pdata->power_on_bl();
    if (pdata->set_bl_level)
        pdata->set_bl_level(bldev->props.brightness);
	
	//init workqueue	
	INIT_DELAYED_WORK(&bl_config.bl_delayed_work, power_on_bl);
	//bl_config.workqueue = create_singlethread_workqueue("bl_power_on_queue");
	bl_config.workqueue = create_workqueue("bl_power_on_queue");
	if (bl_config.workqueue == NULL) {
		printk("can't create bl work queue\n");
	}
	
	printk("aml bl probe OK.\n");
    return 0;

err:
    kfree(amlbl);
    return retval;
}

static int __exit aml_bl_remove(struct platform_device *pdev)
{
    struct aml_bl *amlbl = platform_get_drvdata(pdev);

    DTRACE();
	
	if (bl_config.workqueue)
		destroy_workqueue(bl_config.workqueue);
	
    backlight_device_unregister(amlbl->bldev);
    platform_set_drvdata(pdev, NULL);
    kfree(amlbl);

    return 0;
}

static struct platform_driver aml_bl_driver = {
    .driver = {
        .name = "aml-bl",
        .owner = THIS_MODULE,
#ifdef CONFIG_USE_OF
		.of_match_table = backlight_dt_match,
#endif
    },
    .probe = aml_bl_probe,
    .remove = __exit_p(aml_bl_remove),
};

static int __init aml_bl_init(void)
{
    DTRACE();
    if (platform_driver_register(&aml_bl_driver)) {
		printk("failed to register bl driver module\n");
        return -ENODEV;
	}
	
	return 0;
}

static void __exit aml_bl_exit(void)
{
    DTRACE();
    platform_driver_unregister(&aml_bl_driver);
}

module_init(aml_bl_init);
module_exit(aml_bl_exit);

MODULE_DESCRIPTION("Meson Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Amlogic, Inc.");

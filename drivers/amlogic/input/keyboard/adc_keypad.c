/*
 * drivers/amlogic/input/adc_keypad/adc_keypad.c
 *
 * ADC Keypad Driver
 *
 * Copyright (C) 2010 Amlogic, Inc.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * author :   Robin Zhu
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <asm/irq.h>
#include <asm/io.h>

#include <mach/am_regs.h>
#include <mach/pinmux.h>
#include <linux/major.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/amlogic/saradc.h>
#include <linux/amlogic/input/adc_keypad.h>
#include <linux/of.h>

struct kp {
        int (*led_control)(void *param);
	int *led_control_param;

	struct input_dev *input;
	struct timer_list timer;
	unsigned int cur_keycode;
    unsigned int report_code;
	unsigned int tmp_code;
	int count;	
	int config_major;
	char config_name[20];
	struct class *config_class;
	struct device *config_dev;
	int chan[SARADC_CHAN_NUM];
	int chan_num;
	struct adc_key *key;
	int key_num;
	struct work_struct work_update;
};

#ifndef CONFIG_OF
#define CONFIG_OF
#endif

static struct kp *gp_kp=NULL;

static int timer_count = 0;

static int kp_search_key(struct kp *kp)
{
	struct adc_key *key;
	int value, i, j;
	
	for (i=0; i<kp->chan_num; i++) {
		value = get_adc_sample(kp->chan[i]);
		if (value < 0) {
			continue;
		}
		key = kp->key;
	 	for (j=0; j<kp->key_num; j++) {
			if ((key->chan == kp->chan[i])
			&& (value >= key->value - key->tolerance)
			&& (value <= key->value + key->tolerance)) {
				return key->code;
			}
			key++;
		}
	}
	
	return 0;
}

static void kp_work(struct kp *kp)
{
	int code = kp_search_key(kp);

	if ((!code) && (!kp->cur_keycode)) {
		return;
	}
	else if (code != kp->tmp_code) {
		kp->tmp_code = code;
		kp->count = 0;
	}
	else if(++kp->count == 2) {
		if (kp->cur_keycode != code) {
			if (!code) {
				printk("key %d up\n", kp->report_code);
				input_report_key(kp->input, kp->report_code, 0);
				input_sync(kp->input);
			}
			else if (!kp->cur_keycode) {
				printk("key %d down\n", code);
				input_report_key(kp->input, code, 1);
				kp->report_code = code;
				input_sync(kp->input);
	//				if (kp->led_control && (code!=KEY_PAGEUP) && (code!=KEY_PAGEDOWN)){
	//					kp->led_control_param[0] = 1;
	//					kp->led_control_param[1] = code;
	//		    			timer_count = kp->led_control(kp->led_control_param);
	//				}
				}
				else {
	//				if (kp->led_control){
	//					kp->led_control_param[0] = 2;
	//					kp->led_control_param[1] = code;
	//		    			timer_count = kp->led_control(kp->led_control_param);
	//				}
				}
			kp->cur_keycode = code;
		}
	}
}

static void update_work_func(struct work_struct *work)
{
    struct kp *kp_data = container_of(work, struct kp, work_update);

#if 1
    kp_work(kp_data);

    if (kp_data->led_control ){
	if (timer_count>0)
	{
		timer_count++;
	}
	if (50 == timer_count){
		kp_data->led_control_param[0] = 0;
		timer_count = kp_data->led_control(kp_data->led_control_param);
	}
    }
#else
    unsigned int result;
    result = get_adc_sample();
    if (result>=0x3e0){
        if (kp_data->cur_keycode != 0){
            input_report_key(kp_data->input,kp_data->cur_keycode, 0);	
            kp_data->cur_keycode = 0;
		        printk("adc ch4 sample = %x, keypad released.\n", result);
        }
    }
	else if (result>=0x0 && result<0x60 ) {
		if (kp_data->cur_keycode!=KEY_HOME){
    	  kp_data->cur_keycode = KEY_HOME;
    	  input_report_key(kp_data->input,kp_data->cur_keycode, 1);	
    	  printk("adc ch4 sample = %x, keypad pressed.\n", result);
		}
    }
	else if (result>=0x110 && result<0x170 ) {
		if (kp_data->cur_keycode!=KEY_ENTER){
    	  kp_data->cur_keycode = KEY_ENTER;
    	  input_report_key(kp_data->input,kp_data->cur_keycode, 1);	
    	  printk("adc ch4 sample = %x, keypad pressed.\n", result);
		}
    }
	else if (result>=0x240 && result<0x290 ) {
		if (kp_data->cur_keycode!= KEY_LEFTMETA ){
    	  kp_data->cur_keycode = KEY_LEFTMETA;
    	  input_report_key(kp_data->input,kp_data->cur_keycode, 1);	
    	  printk("adc ch4 sample = %x, keypad pressed.\n", result);
		}
    }
	else if (result>=0x290 && result<0x380 ) {
		if (kp_data->cur_keycode!= KEY_TAB ){
    	  kp_data->cur_keycode = KEY_TAB;
    	  input_report_key(kp_data->input,kp_data->cur_keycode, 1);	
    	  printk("adc ch4 sample = %x, keypad pressed.\n", result);
    }
    }
    else{
		printk("adc ch4 sample = unknown key %x, pressed.\n", result);
    }
#endif

}

static void kp_timer_sr(unsigned long data)
{
    struct kp *kp_data=(struct kp *)data;
    schedule_work(&(kp_data->work_update));
    mod_timer(&kp_data->timer,jiffies+msecs_to_jiffies(25));
}

static int
adckpd_config_open(struct inode *inode, struct file *file)
{
    file->private_data = gp_kp;
    return 0;
}

static int
adckpd_config_release(struct inode *inode, struct file *file)
{
    file->private_data=NULL;
    return 0;
}

static const struct file_operations keypad_fops = {
    .owner      = THIS_MODULE,
    .open       = adckpd_config_open,
    .release    = adckpd_config_release,
};

static int register_keypad_dev(struct kp  *kp)
{
    int ret=0;
    strcpy(kp->config_name,"am_adc_kpd");
    ret=register_chrdev(0, kp->config_name, &keypad_fops);
    if(ret<=0)
    {
        printk("register char device error\r\n");
        return  ret ;
    }
    kp->config_major=ret;
    printk("adc keypad major:%d\r\n",ret);
    kp->config_class=class_create(THIS_MODULE,kp->config_name);
    kp->config_dev=device_create(kp->config_class,	NULL,
    		MKDEV(kp->config_major,0),NULL,kp->config_name);
    return ret;
}

static int kp_probe(struct platform_device *pdev)
{
    struct kp *kp;
    struct input_dev *input_dev;
    int i, j, ret, key_size, name_len;
    int new_chan_flag;
    struct adc_key *key;
    struct adc_kp_platform_data *pdata = NULL;
    int *key_param = NULL;
    int state = 0;

		printk("==%s==\n", __func__);

#ifdef CONFIG_OF
	 if (!pdev->dev.of_node) {
				printk("adc_key: pdev->dev.of_node == NULL!\n");
				state =  -EINVAL;
				goto get_key_node_fail;
		}
		ret = of_property_read_u32(pdev->dev.of_node,"key_num",&key_size);
    if (ret) {
		  printk("adc_key: faild to get key_num!\n");
		  state =  -EINVAL;
		  goto get_key_node_fail;
	  }
	  ret = of_property_read_u32(pdev->dev.of_node,"name_len",&name_len);
    if (ret) {
		  printk("adc_key: faild to get name_len!\n");
		  name_len = 20;
	  }
    pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
    if (!pdata) {
        dev_err(&pdev->dev, "platform data is required!\n");
        state = -EINVAL;
        goto get_key_node_fail;
    }
   
		pdata->key = kzalloc(sizeof(*(pdata->key))*key_size, GFP_KERNEL);
		if (!(pdata->key)) {
			dev_err(&pdev->dev, "platform key is required!\n");
			goto get_key_mem_fail;
		}

		pdata->key_num = key_size;
    for (i=0; i<key_size; i++) {
				ret = of_property_read_string_index(pdev->dev.of_node, "key_name", i, &(pdata->key[i].name));
				//printk("adc_key: %d %s\n",i, (pdata->key[i].name));
				if(ret < 0){
					printk("adc_key: find key_name=%d finished\n", i);
					break;
				}
		}
		key_param = kzalloc(4*(sizeof(*key_param))*(pdata->key_num), GFP_KERNEL);
    if(!key_param) {
			printk("adc_key: key_param can not get mem\n");
			goto get_param_mem_fail;
		}
    ret = of_property_read_u32_array(pdev->dev.of_node,"key_code",key_param, pdata->key_num);
    if (ret) {
		  printk("adc_key: faild to get key_code!\n");
		  goto get_key_param_failed;
	  }
    ret = of_property_read_u32_array(pdev->dev.of_node,"key_chan",key_param+pdata->key_num, pdata->key_num);
    if (ret) {
		  printk("adc_key: faild to get key_chan!\n");
		  goto get_key_param_failed;
	  }
	  ret = of_property_read_u32_array(pdev->dev.of_node,"key_val",key_param+pdata->key_num*2, pdata->key_num);
    if (ret) {
		  printk("adc_key: faild to get key_val!\n");
		  goto get_key_param_failed;
	  }
	  ret = of_property_read_u32_array(pdev->dev.of_node,"key_tolerance",key_param+pdata->key_num*3, pdata->key_num);
    if (ret) {
		  printk("adc_key: faild to get tolerance!\n");
		  goto get_key_param_failed;
	  }

	  for (i=0; i<pdata->key_num; i++) {
			pdata->key[i].code = *(key_param+i);
			pdata->key[i].chan = *(key_param+pdata->key_num+i);
			pdata->key[i].value = *(key_param+pdata->key_num*2+i);
			pdata->key[i].tolerance = *(key_param+pdata->key_num*3+i);
	  }

#else
		pdata = pdev->dev.platform_data;
#endif
    kp = kzalloc(sizeof(struct kp), GFP_KERNEL);
    kp->led_control_param = kzalloc((sizeof(int)*pdata->led_control_param_num), GFP_KERNEL);
    input_dev = input_allocate_device();
    if (!kp ||!kp->led_control_param || !input_dev) {
        kfree(kp->led_control_param);
        kfree(kp);
        input_free_device(input_dev);
        state = -ENOMEM;
        goto get_key_param_failed;
    }
    gp_kp=kp;

    platform_set_drvdata(pdev, pdata);
    kp->input = input_dev;
    kp->cur_keycode = 0;
		kp->tmp_code = 0;
		kp->count = 0;
     
    INIT_WORK(&(kp->work_update), update_work_func);
     
    setup_timer(&kp->timer, kp_timer_sr, (unsigned int)kp) ;
    mod_timer(&kp->timer, jiffies+msecs_to_jiffies(100));

    /* setup input device */
    set_bit(EV_KEY, input_dev->evbit);
    set_bit(EV_REP, input_dev->evbit);
        
    kp->key = pdata->key;
    kp->key_num = pdata->key_num;
    if (pdata->led_control){
    	kp->led_control = pdata->led_control;
    }

    key = pdata->key;
    kp->chan_num = 0;
    for (i=0; i<kp->key_num; i++) {
        set_bit(key->code, input_dev->keybit);
        /* search the key chan */
        new_chan_flag = 1;
        for (j=0; j<kp->chan_num; j++) {
            if (key->chan == kp->chan[j]) {
                new_chan_flag = 0;
                break;
            }
        }
        if (new_chan_flag) {
            kp->chan[kp->chan_num] = key->chan;
            printk(KERN_INFO "chan #%d used for ADC key\n", key->chan);
            kp->chan_num++;
        }    
        printk(KERN_INFO "%s key(%d) registed.\n", key->name, key->code);
        key++;
    }
    
    input_dev->name = "adc_keypad";
    input_dev->phys = "adc_keypad/input0";
    input_dev->dev.parent = &pdev->dev;

    input_dev->id.bustype = BUS_ISA;
    input_dev->id.vendor = 0x0001;
    input_dev->id.product = 0x0001;
    input_dev->id.version = 0x0100;

    input_dev->rep[REP_DELAY]=0xffffffff;
    input_dev->rep[REP_PERIOD]=0xffffffff;

    input_dev->keycodesize = sizeof(unsigned short);
    input_dev->keycodemax = 0x1ff;

    ret = input_register_device(kp->input);
    if (ret < 0) {
        printk(KERN_ERR "Unable to register keypad input device.\n");
		    kfree(kp->led_control_param);
		    kfree(kp);
		    input_free_device(input_dev);
		    state = -EINVAL;
		    goto get_key_param_failed;
    }
    printk("adc keypad register input device completed.\r\n");
    register_keypad_dev(gp_kp);
    kfree(key_param);
    return 0;

    get_key_param_failed:
			kfree(key_param);
    get_param_mem_fail:
			kfree(pdata->key);
    get_key_mem_fail:
			kfree(pdata);
    get_key_node_fail:
    return state;
}

static int kp_remove(struct platform_device *pdev)
{
    struct adc_kp_platform_data *pdata = platform_get_drvdata(pdev);
    struct kp *kp = gp_kp;

#if 0
    if (kp->p_led_timer){
    	del_timer_sync(kp->p_led_timer);
    	kfree(kp->p_led_timer);
	kp->p_led_timer = NULL;
    }
#endif
    kp->led_control(0);
    input_unregister_device(kp->input);
    input_free_device(kp->input);
    unregister_chrdev(kp->config_major,kp->config_name);
    if(kp->config_class)
    {
        if(kp->config_dev)
        device_destroy(kp->config_class,MKDEV(kp->config_major,0));
        class_destroy(kp->config_class);
    }
    kfree(kp->led_control_param);
    kfree(kp);
#ifdef CONFIG_OF
	kfree(pdata->key);
	kfree(pdata);
#endif
    gp_kp=NULL ;
    return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id key_dt_match[]={
	{	.compatible = "amlogic,adc_keypad",
	},
	{},
};
#else
#define key_dt_match NULL
#endif

static struct platform_driver kp_driver = {
    .probe      = kp_probe,
    .remove     = kp_remove,
    .suspend    = NULL,
    .resume     = NULL,
    .driver     = {
        .name   = "m1-adckp",
        .of_match_table = key_dt_match,
    },
};

static int __init kp_init(void)
{
    printk(KERN_INFO "ADC Keypad Driver init.\n");
    return platform_driver_register(&kp_driver);
}

static void __exit kp_exit(void)
{
    printk(KERN_INFO "ADC Keypad Driver exit.\n");
    platform_driver_unregister(&kp_driver);
}

module_init(kp_init);
module_exit(kp_exit);

MODULE_AUTHOR("Robin Zhu");
MODULE_DESCRIPTION("ADC Keypad Driver");
MODULE_LICENSE("GPL");





/*
 * AMLOGIC DVB frontend driver.
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
 */

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/fcntl.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <linux/poll.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
//#include <linux/videodev2.h>
//s#include <linux/pinctrl/consumer.h>
//#include <linux/amlogic/aml_gpio_consumer.h>

#include "aml_fe.h"

#define pr_dbg(fmt, args...)\
	do{\
		if(debug_fe)\
			printk("FE: " fmt, ## args);\
	}while(0)
#define pr_error(fmt, args...) printk("FE: " fmt, ## args)

MODULE_PARM_DESC(debug_fe, "\n\t\t Enable frontend debug information");
static int debug_fe = 1;
module_param(debug_fe, int, S_IRUGO);

#define AFC_BEST_LOCK      50
#define ATV_AFC_1_0MHZ   1000000
#define ATV_AFC_2_0MHZ	 2000000

#define AML_FE_MAX_RES		50

static int slow_mode=0;
module_param(slow_mode,int,0644);
MODULE_DESCRIPTION("search the channel by slow_mode,by add +1MHz\n");

static struct aml_fe_drv *tuner_drv_list = NULL;
static struct aml_fe_drv *atv_demod_drv_list = NULL;
static struct aml_fe_drv *dtv_demod_drv_list = NULL;
static struct aml_fe_man  fe_man;

#ifndef CONFIG_OF
static struct platform_device *g_aml_fe_pdev = NULL;
static u32	 aml_fe_num_resources = 0;
static struct resource aml_fe_resource[AML_FE_MAX_RES];
static char *aml_fe_buf = NULL;
static size_t aml_fe_size;
#endif

static u32 aml_fe_suspended = 0;

static DEFINE_SPINLOCK(lock);
static int aml_fe_afc_closer(struct dvb_frontend *fe,int minafcfreq,int maxafcfqreq);

typedef int (*hook_func_t)(void);
void aml_fe_hook_cvd(hook_func_t atv_mode,hook_func_t cvd_hv_lock)
{
//	aml_fe_hook_atv_status = atv_mode;
//	aml_fe_hook_hv_lock = cvd_hv_lock;
//	printk("[aml_fe]%s \n",__func__);
}


static struct aml_fe_drv** aml_get_fe_drv_list(aml_fe_dev_type_t type)
{
	switch(type){
		case AM_DEV_TUNER:
			return &tuner_drv_list;
		case AM_DEV_ATV_DEMOD:
			return &atv_demod_drv_list;
		case AM_DEV_DTV_DEMOD:
			return &dtv_demod_drv_list;
		default:
			return NULL;
	}
}

int aml_register_fe_drv(aml_fe_dev_type_t type, struct aml_fe_drv *drv)
{
	if(drv){
		struct aml_fe_drv **list = aml_get_fe_drv_list(type);
		unsigned long flags;

		spin_lock_irqsave(&lock, flags);

		drv->next = *list;
		*list = drv;

		drv->ref = 0;

		spin_unlock_irqrestore(&lock, flags);
	}

	return 0;
}

int aml_unregister_fe_drv(aml_fe_dev_type_t type, struct aml_fe_drv *drv)
{
	int ret = 0;

	if(drv){
		struct aml_fe_drv *pdrv, *pprev;
		struct aml_fe_drv **list = aml_get_fe_drv_list(type);
		unsigned long flags;

		spin_lock_irqsave(&lock, flags);

		if(drv->ref){
			for(pprev = NULL, pdrv = *list;
				pdrv;
				pprev = pdrv, pdrv = pdrv->next){
				if(pdrv == drv){
					if(pprev)
						pprev->next = pdrv->next;
					else
						*list = pdrv->next;
					break;
				}
			}
		}else{
			pr_error("fe driver %d is inused\n", drv->id);
			ret = -1;
		}

		spin_unlock_irqrestore(&lock, flags);
	}

	return ret;
}

int aml_fe_analog_set_frontend(struct dvb_frontend* fe)
{
	struct aml_fe *afe = fe->demodulator_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct analog_parameters p;
	int ret = -1;

	p.frequency  = c->frequency;
	p.soundsys   = c->analog.soundsys;
	p.audmode    = c->analog.audmode;
	p.std        = c->analog.std;
	p.reserved   = c->analog.reserved;

	/*set tuner&ademod such as philipse tuner*/
	if(fe->ops.analog_ops.set_params){
		fe->ops.analog_ops.set_params(fe);
		ret = 0;
	}
	if(fe->ops.tuner_ops.set_params){
		ret = fe->ops.tuner_ops.set_params(fe);
	}

	if(ret == 0){
		afe->params.frequency = c->frequency;
		afe->params.inversion = c->inversion;
		afe->params.analog  = c->analog;
	}

	return ret;
}

static int aml_fe_analog_get_frontend(struct dvb_frontend* fe)
{
	struct dtv_frontend_properties *p = &fe->dtv_property_cache;
	struct aml_fe *afe = fe->demodulator_priv;

	p->frequency = afe->params.frequency;
	pr_dbg("[%s] params.frequency:%d\n",__func__,p->frequency);

	return 0;
}

static int aml_fe_analog_read_status(struct dvb_frontend* fe, fe_status_t* status)
{
    int ret = 0;
    if(!status)
        return -1;
    /*atv only demod locked is vaild*/
    if(fe->ops.analog_ops.get_status)
	    fe->ops.analog_ops.get_status(fe, status);
    else if(fe->ops.tuner_ops.get_status)
        ret = fe->ops.tuner_ops.get_status(fe, status);

    return ret;
}

static int aml_fe_analog_read_signal_strength(struct dvb_frontend* fe, u16 *strength)
{
	int ret = -1;
	u16 s;
	s=0;
	if(fe->ops.analog_ops.has_signal){

		fe->ops.analog_ops.has_signal(fe);
		*strength = s;
		ret = 0;
	}else if(fe->ops.tuner_ops.get_rf_strength){
		ret = fe->ops.tuner_ops.get_rf_strength(fe, strength);
	}

	return ret;
}
static int aml_fe_analog_read_signal_snr(struct dvb_frontend* fe, u16 *snr)
{
    if(!snr)
    {
        pr_error("[aml_fe..]%s null pointer error.\n",__func__);
        return -1;
    }
    if(fe->ops.analog_ops.get_snr)
        *snr = (unsigned short)fe->ops.analog_ops.get_snr(fe);
    return 0;
}
static enum dvbfe_algo aml_fe_get_analog_algo(struct dvb_frontend *dev)
{
        return DVBFE_ALGO_CUSTOM;
}
#if 0
//+1 M or the step from api,then get tuner lock status,if lock get demod lock
  // status,if lock update the analog_parameter
static enum dvbfe_search aml_fe_analog_search(struct dvb_frontend *fe, struct dvb_frontend_parameters *p)
{
	fe_status_t tuner_state;
	fe_status_t ade_state;
	int minafcfreq, maxafcfreq;
//set the afc_range and start freq
	if(p->u.analog.afc_range==0)
		p->u.analog.afc_range=ATV_AFC_1_0MHZ;
	tuner_state = FE_TIMEDOUT;
	ade_state   = FE_TIMEDOUT;
	minafcfreq  = p->frequency  - p->u.analog.afc_range;
	maxafcfreq = p->frequency + p->u.analog.afc_range;
	pr_dbg("[%s] is working,afc_range=%d.\n",__func__,p->u.analog.afc_range);
//from the min freq start
	p->frequency = minafcfreq;
	if( fe->ops.set_frontend(fe, p)){
		pr_error("[%s]the func of set_param err.\n",__func__);
		return DVBFE_ALGO_SEARCH_FAILED;
	}
//atuo bettween afc range
	if(likely(fe->ops.tuner_ops.get_status && fe->ops.analog_ops.get_status && fe->ops.set_frontend))
	{
		 while( p->frequency<=maxafcfreq)
		{
			pr_dbg("[%s] p->frequency=[%d] is processing\n",__func__,p->frequency);
			fe->ops.tuner_ops.get_status(fe, &tuner_state);
			fe->ops.analog_ops.get_status(fe, &ade_state);
			if(FE_HAS_LOCK==ade_state && FE_HAS_LOCK==tuner_state){
				if(aml_fe_afc_closer(fe,p,minafcfreq,maxafcfreq)<0){
					return  DVBFE_ALGO_SEARCH_FAILED;
				}

			pr_dbg("[%s] afc end  :p->frequency=[%d] has lock,search success.\n",__func__,p->frequency);
	            	return DVBFE_ALGO_SEARCH_SUCCESS;

			}
 			else
			{
				pr_dbg("[%s] freq is[%d] unlock\n",__func__,p->frequency);
				p->frequency +=  ATV_AFC_1_0MHZ;
				if(p->frequency >maxafcfreq)
				{
					p->frequency -=  ATV_AFC_1_0MHZ;
					pr_dbg("[%s] p->frequency=[%d] over maxafcfreq=[%d].search failed.\n",__func__,p->frequency,maxafcfreq);
					return DVBFE_ALGO_SEARCH_FAILED;
				}
				if( fe->ops.set_frontend(fe, p)){
					pr_error("[%s] the func of set_frontend err.\n",__func__);
					return  DVBFE_ALGO_SEARCH_FAILED;
				}
			}
		}
        }

	   return DVBFE_ALGO_SEARCH_FAILED;
}
#endif
//this func set two ways to search the channel
//1.if the afc_range>1Mhz,set the freq  more than once
//2. if the afc_range<=1MHz,set the freq only once ,on the mid freq

static enum dvbfe_search aml_fe_analog_search(struct dvb_frontend *fe, struct dvb_frontend_parameters *p)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	fe_status_t tuner_state;
	fe_status_t ade_state;
	int minafcfreq, maxafcfreq;
	int frist_step;
	int afc_step;
	struct aml_fe *fee;
	fee = fe->demodulator_priv;

	if(c->analog.flag & ANALOG_FLAG_MANUL_SCAN){
		pr_dbg("[%s]:set the mannul scan mode and current freq [%d]\n",__func__,c->frequency);
		if( fe->ops.set_frontend(fe)){
		    pr_dbg("[%s]the func of set_param err.\n",__func__);
		    return DVBFE_ALGO_SEARCH_FAILED;
		}
		fe->ops.tuner_ops.get_status(fe, &tuner_state);
		fe->ops.analog_ops.get_status(fe, &ade_state);
		if(FE_HAS_LOCK==ade_state && FE_HAS_LOCK==tuner_state){
		    pr_dbg("[%s] manul scan mode:p->frequency=[%d] has lock,search success.\n",__func__,c->frequency);
		    return DVBFE_ALGO_SEARCH_SUCCESS;
		}
		else
		    return DVBFE_ALGO_SEARCH_FAILED;
	}
	else if(c->analog.afc_range==0)
	{
		pr_dbg("[%s]:afc_range==0,skip the search\n",__func__);
		return DVBFE_ALGO_SEARCH_FAILED;
	}

//set the frist_step
	if(c->analog.afc_range>ATV_AFC_1_0MHZ)
		frist_step=ATV_AFC_1_0MHZ;
	else
		frist_step=c->analog.afc_range;
//set the afc_range and start freq
	tuner_state = FE_TIMEDOUT;
	ade_state   = FE_TIMEDOUT;
	minafcfreq  = c->frequency  - c->analog.afc_range;
	maxafcfreq  = c->frequency + c->analog.afc_range;
	pr_dbg("[%s] is working,afc_range=%d,the received freq=[%d]\n",__func__,c->analog.afc_range,c->frequency);
	pr_dbg("the tuner type is [%d]\n",fee->tuner->drv->id);
//from the min freq start,and set the afc_step
	if(slow_mode || fee->tuner->drv->id ==  AM_TUNER_FQ1216 || AM_TUNER_HTM == fee->tuner->drv->id ){
		pr_dbg("[%s]this is slow mode to search the channel\n",__func__);
		c->frequency = minafcfreq;
		afc_step=ATV_AFC_1_0MHZ;
	}
	else if(!slow_mode && fee->tuner->drv->id== AM_TUNER_SI2176){
		c->frequency = minafcfreq+frist_step;
		afc_step=ATV_AFC_2_0MHZ;
	}
	else{
		pr_dbg("[%s]this is ukown tuner type and on slow_mode to search the channel\n",__func__);
		c->frequency = minafcfreq;
		afc_step=ATV_AFC_1_0MHZ;
	}
	if( fe->ops.set_frontend(fe)){
		pr_error("[%s]the func of set_param err.\n",__func__);
		return DVBFE_ALGO_SEARCH_FAILED;
	}
//atuo bettween afc range
	if(likely(fe->ops.tuner_ops.get_status && fe->ops.analog_ops.get_status && fe->ops.set_frontend))
	{
		 while( c->frequency<=maxafcfreq)
		{
			pr_dbg("[%s] p->frequency=[%d] is processing\n",__func__,c->frequency);
			fe->ops.tuner_ops.get_status(fe, &tuner_state);
			fe->ops.analog_ops.get_status(fe, &ade_state);
			if(FE_HAS_LOCK==ade_state && FE_HAS_LOCK==tuner_state){
				if(aml_fe_afc_closer(fe,minafcfreq,maxafcfreq)==0){
					pr_dbg("[%s] afc end  :p->frequency=[%d] has lock,search success.\n",__func__,c->frequency);
	            	return DVBFE_ALGO_SEARCH_SUCCESS;
				}
			}
			pr_dbg("[%s] freq is[%d] unlock\n",__func__,c->frequency);
			c->frequency +=  afc_step;
			if(c->frequency >maxafcfreq)
			{
				c->frequency -=  afc_step;
				pr_dbg("[%s] p->frequency=[%d] over maxafcfreq=[%d].search failed.\n",__func__,c->frequency,maxafcfreq);
				return DVBFE_ALGO_SEARCH_FAILED;
			}
			if( fe->ops.set_frontend(fe)){
				pr_error("[%s] the func of set_frontend err.\n",__func__);
				return  DVBFE_ALGO_SEARCH_FAILED;
			}
		}
        }

	   return DVBFE_ALGO_SEARCH_FAILED;
}

static int aml_fe_afc_closer(struct dvb_frontend *fe,int minafcfreq,int maxafcfqreq)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	int afc = 100;
	__u32 set_freq;
	int count=10;

	//do the auto afc make sure the afc<50k or the range from api
	if(fe->ops.analog_ops.get_afc &&fe->ops.set_frontend){
		set_freq=c->frequency;

		while(afc > AFC_BEST_LOCK){

			fe->ops.analog_ops.get_afc(fe);
			c->frequency += afc*1000;

			if(unlikely(c->frequency>maxafcfqreq) ){
				 pr_dbg("[%s]:[%d] is exceed maxafcfqreq[%d]\n",__func__,c->frequency,maxafcfqreq);
				 c->frequency=set_freq;
				 return -1;
			}
			if( unlikely(c->frequency<minafcfreq)){
				 pr_dbg("[%s]:[%d ] is exceed minafcfreq[%d]\n",__func__,c->frequency,minafcfreq);
				 c->frequency=set_freq;
				 return -1;
			}
			if(likely(!(count--))){
				 pr_dbg("[%s]:exceed the afc count\n",__func__);
				 c->frequency=set_freq;
				 return -1;
			}

			fe->ops.set_frontend(fe);

			pr_dbg("[aml_fe..]%s get afc %d khz, freq %u.\n",__func__,afc, c->frequency);
		}

		pr_dbg("[aml_fe..]%s get afc %d khz done, freq %u.\n",__func__,afc, c->frequency);
	}

    return 0;
}



static int aml_fe_set_mode(struct dvb_frontend *dev, fe_type_t type)
{
	struct aml_fe *fe;
	aml_fe_mode_t mode;
	unsigned long flags;
	fe = dev->demodulator_priv;
	//type=FE_ATSC;
	switch(type){
		case FE_QPSK:
			mode = AM_FE_QPSK;
			pr_dbg("set mode -> QPSK\n");
			break;
		case FE_QAM:
			pr_dbg("set mode -> QAM\n");
			mode = AM_FE_QAM;
			break;
		case FE_OFDM:
			pr_dbg("set mode -> OFDM\n");
			mode = AM_FE_OFDM;
			break;
		case FE_ATSC:
			pr_dbg("set mode -> ATSC\n");
			mode = AM_FE_ATSC;
			break;
		case FE_ISDBT:
			pr_dbg("set mode -> ISDBT\n");
			mode = AM_FE_ISDBT;
			break;
		case FE_DTMB:
			pr_dbg("set mode -> DTMB\n");
			mode = AM_FE_DTMB;
			break;
		case FE_ANALOG:
			pr_dbg("set mode -> ANALOG\n");
			mode = AM_FE_ANALOG;
			break;
		default:
			pr_error("illegal fe type %d\n", type);
			return -1;
	}

	if(fe->mode == mode)
	{
		pr_dbg("[%s]:the mode is not change!!!!\n",__func__);
		return 0;
	}

	if(fe->mode != AM_FE_UNKNOWN){
		if(fe->dtv_demod && (fe->dtv_demod->drv->capability & fe->mode) && fe->dtv_demod->drv->leave_mode)
				fe->dtv_demod->drv->leave_mode(fe, fe->mode);
		if(fe->atv_demod && (fe->atv_demod->drv->capability & fe->mode) && fe->atv_demod->drv->leave_mode)
				fe->atv_demod->drv->leave_mode(fe, fe->mode);
		if(fe->tuner && (fe->tuner->drv->capability & fe->mode) && fe->tuner->drv->leave_mode)
				fe->tuner->drv->leave_mode(fe, fe->mode);

		if(fe->mode & AM_FE_DTV_MASK)
			aml_dmx_register_frontend(fe->ts, NULL);

		fe->mode = AM_FE_UNKNOWN;
	}

	if(!(mode & fe->capability)){
		int i;

		spin_lock_irqsave(&lock, flags);
		for(i = 0; i < FE_DEV_COUNT; i++){
			if((mode & fe_man.fe[i].capability) && (fe_man.fe[i].dev_id == fe->dev_id))
				break;
		}
		spin_unlock_irqrestore(&lock, flags);

		if(i >= FE_DEV_COUNT){
			pr_error("frontend %p do not support mode %x, capability %x\n", fe, mode, fe->capability);
			return -1;
		}

		fe = &fe_man.fe[i];
		dev->demodulator_priv = fe;
	}

	if(fe->mode & AM_FE_DTV_MASK){
		aml_dmx_register_frontend(fe->ts, NULL);
		fe->mode = 0;
	}

	spin_lock_irqsave(&fe->slock, flags);

	memset(&fe->fe->ops.tuner_ops, 0, sizeof(fe->fe->ops.tuner_ops));
	memset(&fe->fe->ops.analog_ops, 0, sizeof(fe->fe->ops.analog_ops));
	memset(&fe->fe->ops.info,0,sizeof(fe->fe->ops.info));
	fe->fe->ops.release = NULL;
	fe->fe->ops.release_sec = NULL;
	fe->fe->ops.init = NULL;
	fe->fe->ops.sleep = NULL;
	fe->fe->ops.write = NULL;
	fe->fe->ops.tune = NULL;
	fe->fe->ops.get_frontend_algo = NULL;
	fe->fe->ops.set_frontend = NULL;
	fe->fe->ops.get_tune_settings = NULL;
	fe->fe->ops.get_frontend = NULL;
	fe->fe->ops.read_status = NULL;
	fe->fe->ops.read_ber = NULL;
	fe->fe->ops.read_signal_strength = NULL;
	fe->fe->ops.read_snr = NULL;
	fe->fe->ops.read_ucblocks = NULL;
	fe->fe->ops.set_qam_mode = NULL;
	fe->fe->ops.diseqc_reset_overload = NULL;
	fe->fe->ops.diseqc_send_master_cmd = NULL;
	fe->fe->ops.diseqc_recv_slave_reply = NULL;
	fe->fe->ops.diseqc_send_burst = NULL;
	fe->fe->ops.set_tone = NULL;
	fe->fe->ops.set_voltage = NULL;
	fe->fe->ops.enable_high_lnb_voltage = NULL;
	fe->fe->ops.dishnetwork_send_legacy_command = NULL;
	fe->fe->ops.i2c_gate_ctrl = NULL;
	fe->fe->ops.ts_bus_ctrl = NULL;
	fe->fe->ops.search = NULL;
	fe->fe->ops.track = NULL;
	fe->fe->ops.set_property = NULL;
	fe->fe->ops.get_property = NULL;
	memset(&fe->fe->ops.blindscan_ops, 0, sizeof(fe->fe->ops.blindscan_ops));
	fe->fe->ops.asyncinfo.set_frontend_asyncenable = 0;
	if(fe->tuner && fe->tuner->drv && (mode & fe->tuner->drv->capability) && fe->tuner->drv->get_ops){
		fe->tuner->drv->get_ops(fe->tuner, mode, &fe->fe->ops.tuner_ops);
	}

	if(fe->atv_demod && fe->atv_demod->drv && (mode & fe->atv_demod->drv->capability) && fe->atv_demod->drv->get_ops){
		fe->atv_demod->drv->get_ops(fe->atv_demod, mode, &fe->fe->ops.analog_ops);
		fe->fe->ops.set_frontend = aml_fe_analog_set_frontend;
		fe->fe->ops.get_frontend = aml_fe_analog_get_frontend;
		fe->fe->ops.read_status  = aml_fe_analog_read_status;
		fe->fe->ops.read_signal_strength = aml_fe_analog_read_signal_strength;
		fe->fe->ops.read_snr     = aml_fe_analog_read_signal_snr;
		fe->fe->ops.get_frontend_algo = aml_fe_get_analog_algo;
		fe->fe->ops.search       =  aml_fe_analog_search;
	}

	if(fe->dtv_demod && fe->dtv_demod->drv && (mode & fe->dtv_demod->drv->capability) && fe->dtv_demod->drv->get_ops){
		fe->dtv_demod->drv->get_ops(fe->dtv_demod, mode, &fe->fe->ops);
	}

	spin_unlock_irqrestore(&fe->slock, flags);

	if(fe->dtv_demod && (fe->dtv_demod->drv->capability & mode) && fe->dtv_demod->drv->enter_mode)
		fe->dtv_demod->drv->enter_mode(fe, mode);
	if(fe->atv_demod && (fe->atv_demod->drv->capability & mode) && fe->atv_demod->drv->enter_mode)
		fe->atv_demod->drv->enter_mode(fe, mode);
	if(fe->tuner && (fe->tuner->drv->capability & mode) && fe->tuner->drv->enter_mode)
		fe->tuner->drv->enter_mode(fe, mode);

	if(mode & AM_FE_DTV_MASK){
		aml_dmx_register_frontend(fe->ts, fe->fe);
	}
	strcpy(fe->fe->ops.info.name, "amlogic dvb frontend");

	fe->fe->ops.info.type = type;
	fe->mode = mode;

	return 0;
}

static int aml_fe_read_ts(struct dvb_frontend *dev, int *ts)
{
	struct aml_fe *fe;

	fe = dev->demodulator_priv;

	*ts = fe->ts;
	return 0;
}

#ifndef CONFIG_OF
struct resource *aml_fe_platform_get_resource_byname(const char *name)
{
	int i;

	for (i = 0; i < aml_fe_num_resources; i++) {
		struct resource *r = &aml_fe_resource[i];

		if (!strcmp(r->name, name))
			return r;
	}
	return NULL;
}
#endif /*CONFIG_OF*/

static int aml_fe_dev_init(struct aml_dvb *dvb, struct platform_device *pdev, aml_fe_dev_type_t type, struct aml_fe_dev *dev, int id)
{
#ifndef CONFIG_OF
	struct resource *res;
#endif
	char *name = NULL;
	char buf[32];
	int ret;
	u32 value;
	const char *str;
	aml_tuner_type_t tunerid;

	switch(type){
		case AM_DEV_TUNER:
			name = "tuner";
			break;
		case AM_DEV_ATV_DEMOD:
			name = "atv_demod";
			break;
		case AM_DEV_DTV_DEMOD:
			name = "dtv_demod";
			break;
		default:
			break;
	}

	snprintf(buf, sizeof(buf), "%s%d", name, id);
#ifdef CONFIG_OF
	printk("---------------CONFIG_OF-------------\n");
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(ret){
		return 0;
	}else{
		struct aml_fe_drv **list = aml_get_fe_drv_list(type);
		struct aml_fe_drv *drv;
		unsigned long flags;
		tunerid = value;

		spin_lock_irqsave(&lock, flags);

	/*	for(drv = *list; drv; drv = drv->next){
			if(!strcmp(drv->name, str)){
				drv->ref++;
				break;
			}
		}*/
		for(drv = *list; drv; drv = drv->next){
			if(drv->id == tunerid){
				drv->ref++;
				break;
			}
		}

		spin_unlock_irqrestore(&lock, flags);

		if(drv){
				dev->drv = drv;
			}else{
				pr_err("cannot find %s%d driver: %d\n", name, id, tunerid);
				return -1;
			}
	}

#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		struct aml_fe_drv **list = aml_get_fe_drv_list(type);
		struct aml_fe_drv *drv;
		int type = res->start;
		unsigned long flags;

		spin_lock_irqsave(&lock, flags);

		for(drv = *list; drv; drv = drv->next){
			if(drv->id == type){
				drv->ref++;
				break;
			}
		}

		spin_unlock_irqrestore(&lock, flags);

		if(drv){
			dev->drv = drv;
		}else{
			pr_error("cannot find %s%d driver: %d\n", name, id, type);
			return -1;
		}
	}else{
		return 0;
	}
#endif /*CONFIG_OF*/

	snprintf(buf, sizeof(buf), "%s%d_i2c_adap_id", name, id);
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(!ret){
		dev->i2c_adap_id = value;
		dev->i2c_adap = i2c_get_adapter(value);
		pr_dbg("%s: %d\n", buf, dev->i2c_adap_id);
	}else{
		dev->i2c_adap_id = -1;
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int adap = res->start;

		dev->i2c_adap_id = adap;
		dev->i2c_adap = i2c_get_adapter(adap);
	}else{
		dev->i2c_adap_id = -1;
	}
#endif /*CONFIG_OF*/

	snprintf(buf, sizeof(buf), "%s%d_i2c_addr", name, id);
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(!ret){
		dev->i2c_addr = value;
		pr_dbg("%s: %d\n", buf, dev->i2c_addr);
	}else{
		dev->i2c_addr = -1;
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int addr = res->start;

		dev->i2c_addr = addr;
	}else{
		dev->i2c_addr = -1;
	}
#endif

	snprintf(buf, sizeof(buf), "%s%d_reset_gpio", name, id);
#ifdef CONFIG_OF
	ret = of_property_read_string(pdev->dev.of_node, buf, &str);
	if(!ret){
		dev->reset_gpio = amlogic_gpio_name_map_num(str);
		pr_dbg("%s: %s\n", buf, str);
	}else{
		dev->reset_gpio = -1;
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int gpio = res->start;

		dev->reset_gpio = gpio;
	}else{
		dev->reset_gpio = -1;
	}
#endif /*CONFIG_OF*/

	snprintf(buf, sizeof(buf), "%s%d_reset_value", name, id);
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(!ret){
		dev->reset_value = value;
		pr_dbg("%s: %d\n", buf, dev->reset_value);
	}else{
		dev->reset_value = -1;
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int v = res->start;

		dev->reset_value = v;
	}else{
		dev->reset_value = 0;
	}
#endif /*CONFIG_OF*/

	snprintf(buf, sizeof(buf), "%s%d_tunerpower", name, id);
#ifdef CONFIG_OF
	ret = of_property_read_string(pdev->dev.of_node, buf, &str);
	if(!ret){
		dev->tuner_power_gpio = amlogic_gpio_name_map_num(str);
		pr_dbg("%s: %s\n", buf, str);
	}else{
		dev->tuner_power_gpio = -1;
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int gpio = res->start;

		dev->tuner_power_gpio = gpio;
	}else{
		dev->tuner_power_gpio = -1;
	}
#endif /*CONFIG_OF*/

	snprintf(buf, sizeof(buf), "%s%d_lnbpower", name, id);
#ifdef CONFIG_OF
	ret = of_property_read_string(pdev->dev.of_node, buf, &str);
	if(!ret){
		dev->lnb_power_gpio = amlogic_gpio_name_map_num(str);
		pr_dbg("%s: %s\n", buf, str);
	}else{
		dev->lnb_power_gpio = -1;
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int gpio = res->start;

		dev->lnb_power_gpio = gpio;
	}else{
		dev->lnb_power_gpio = -1;
	}
#endif

	snprintf(buf, sizeof(buf), "%s%d_antoverload", name, id);
#ifdef CONFIG_OF
	ret = of_property_read_string(pdev->dev.of_node, buf, &str);
	if(!ret){
		dev->antoverload_gpio = amlogic_gpio_name_map_num(str);
		pr_dbg("%s: %s\n", buf, str);
	}else{
		dev->antoverload_gpio = -1;
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int gpio = res->start;

		dev->antoverload_gpio = gpio;
	}else{
		dev->antoverload_gpio = -1;
	}
#endif /*CONFIG_OF*/

	if(dev->drv->init){
		ret = dev->drv->init(dev);
		if(ret != 0){
			dev->drv = NULL;
            pr_error("[aml_fe..]%s error.\n",__func__);
			return ret;
		}
	}

	return 0;
}

static int aml_fe_dev_release(struct aml_dvb *dvb, aml_fe_dev_type_t type, struct aml_fe_dev *dev)
{
	if(dev->drv && dev->drv->release){
		dev->drv->ref--;
		dev->drv->release(dev);
	}

	dev->drv = NULL;
	return 0;
}
long *mem_buf;
int memstart;
static int aml_fe_man_init(struct aml_dvb *dvb, struct platform_device *pdev, struct aml_fe *fe, int id)
{
#ifndef CONFIG_OF
	struct resource *res;
#endif
	char buf[32];
	int tuner_cap = 0xFFFFFFFF;
	int demod_cap = 0;
	u32 value;
	int ret;


	snprintf(buf, sizeof(buf), "fe%d_tuner", id);

#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(!ret){
		int id = value;
		if((id < 0) || (id >= FE_DEV_COUNT) || !fe_man.tuner[id].drv){
			pr_error("invalid tuner device id %d\n", id);
			return -1;
		}

		fe->tuner = &fe_man.tuner[id];
		fe->init = 1;

		tuner_cap &= fe->tuner->drv->capability;

		pr_dbg("%s: %d\n", buf, id);
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int id = res->start;
		if((id < 0) || (id >= FE_DEV_COUNT) || !fe_man.tuner[id].drv){
			pr_error("invalid tuner device id %d\n", id);
			return -1;
		}

		fe->tuner = &fe_man.tuner[id];
		fe->init = 1;

		tuner_cap &= fe->tuner->drv->capability;
	}
#endif /*CONFIG_OF*/

	snprintf(buf, sizeof(buf), "fe%d_atv_demod", id);
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(!ret){
		int id = value;
		if((id < 0) || (id >= FE_DEV_COUNT) || !fe_man.atv_demod[id].drv){
			pr_error("invalid ATV demod device id %d\n", id);
			return -1;
		}

		fe->atv_demod = &fe_man.atv_demod[id];
		fe->init = 1;

		demod_cap |= fe->atv_demod->drv->capability;
		pr_dbg("%s: %d\n", buf, id);
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int id = res->start;
		if((id < 0) || (id >= FE_DEV_COUNT) || !fe_man.atv_demod[id].drv){
			pr_error("invalid ATV demod device id %d\n", id);
			return -1;
		}

		fe->atv_demod = &fe_man.atv_demod[id];
		fe->init = 1;

		demod_cap |= fe->atv_demod->drv->capability;
	}
#endif /*CONFIG_OF*/

	snprintf(buf, sizeof(buf), "fe%d_dtv_demod", id);
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(!ret){
		int id = value;
		if((id < 0) || (id >= FE_DEV_COUNT) || !fe_man.dtv_demod[id].drv){
			pr_error("invalid DTV demod device id %d\n", id);
			return -1;
		}

		fe->dtv_demod = &fe_man.dtv_demod[id];
		fe->init = 1;

		demod_cap |= fe->dtv_demod->drv->capability;
		pr_dbg("%s: %d\n", buf, id);
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int id = res->start;

		pr_dbg("[dvb] res->start is %d\n",res->start);
		if((id < 0) || (id >= FE_DEV_COUNT) || !fe_man.dtv_demod[id].drv){
			pr_error("invalid DTV demod device id %d\n", id);
			return -1;
		}

		fe->dtv_demod = &fe_man.dtv_demod[id];
		fe->init = 1;

		demod_cap |= fe->dtv_demod->drv->capability;
	}
#endif /*CONFIG_OF*/

	snprintf(buf, sizeof(buf), "fe%d_ts", id);
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(!ret){
		int id = value;
		aml_ts_source_t ts = AM_TS_SRC_TS0;

		switch(id){
			case 0:
				ts = AM_TS_SRC_TS0;
				break;
			case 1:
				ts = AM_TS_SRC_TS1;
				break;
			case 2:
				ts = AM_TS_SRC_TS2;
				break;
			default:
				break;
		}

		fe->ts = ts;
		pr_dbg("%s: %d\n", buf, id);
	}
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int id = res->start;
		aml_ts_source_t ts = AM_TS_SRC_TS0;

		switch(id){
			case 0:
				ts = AM_TS_SRC_TS0;
				break;
			case 1:
				ts = AM_TS_SRC_TS1;
				break;
			case 2:
				ts = AM_TS_SRC_TS2;
				break;
			default:
				break;
		}

		fe->ts = ts;
	}
#endif /*CONFIG_OF*/
	{
		int memend;
		int memsize;
		snprintf(buf, sizeof(buf), "fe%d_mem", id);
		ret = find_reserve_block(pdev->dev.of_node->name,0);
		if(ret < 0){
		    pr_err("aml_fe memory resource undefined.\n");
		}else{
			memstart = (phys_addr_t)get_reserve_block_addr(ret);
			memsize = (phys_addr_t)get_reserve_block_size(ret);
			memend = memstart+memsize;
			mem_buf=(long*)phys_to_virt(memstart/*+0x40000000*/);
		//	mem_buf = (long*)(memstart+0x40000000);
			printk("memend is %x,memstart is %x,memsize is %x\n",memend,memstart,memsize);
			printk("mem reset g\n");
			memset(mem_buf,0,memsize-1);
			printk("mem reset o\n");
	}
	}

	snprintf(buf, sizeof(buf), "fe%d_dev", id);
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, buf, &value);
	if(!ret){
		int id = value;

		if((id >= 0) && (id < FE_DEV_COUNT))
			fe->dev_id = id;
		else
			fe->dev_id = 0;
	}else{
		fe->dev_id = 0;
	}
	pr_dbg("%s: %d\n", buf, fe->dev_id);
#else /*CONFIG_OF*/
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	res = aml_fe_platform_get_resource_byname(buf);
#else
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, buf);
#endif
	if(res){
		int id = res->start;

		if((id >= 0) && (id < FE_DEV_COUNT))
			fe->dev_id = id;
		else
			fe->dev_id = 0;
	}else{
		fe->dev_id = 0;
	}
#endif /*CONFIG_OF*/

	if(fe->init){
		int reg = 1;
		int prev;
		int ret;


		spin_lock_init(&fe->slock);
		fe->mode = AM_FE_UNKNOWN;
		fe->capability = (tuner_cap & demod_cap);
		pr_dbg("fe: %p cap: %x tuner: %x demod: %x\n", fe, fe->capability, tuner_cap, demod_cap);

		for(prev = 0; prev < id; prev++){
			struct aml_fe *prev_fe = &fe_man.fe[prev];

			if(prev_fe->init && (prev_fe->dev_id == fe->dev_id)){
				reg = 0;
				break;
			}
		}
			fe->fe = &fe_man.dev[fe->dev_id];
		if(reg){
			fe->fe->demodulator_priv = fe;
			fe->fe->ops.set_mode = aml_fe_set_mode;
			fe->fe->ops.read_ts  = aml_fe_read_ts;

			ret = dvb_register_frontend(&dvb->dvb_adapter, fe->fe);
			if(ret){
				pr_error("register fe%d failed\n", fe->dev_id);
				return -1;
			}
		}

		if(fe->tuner)
			fe->tuner->fe = fe;
		if(fe->atv_demod)
			fe->atv_demod->fe = fe;
		if(fe->dtv_demod)
			fe->dtv_demod->fe = fe;
	}

	return 0;
}

static int aml_fe_man_release(struct aml_dvb *dvb, struct aml_fe *fe)
{
	if(fe->init){
		aml_dmx_register_frontend(fe->ts, NULL);
		dvb_unregister_frontend(fe->fe);
		dvb_frontend_detach(fe->fe);
		fe->init = 0;
	}

	return 0;
}
static ssize_t tuner_name_show(struct class *cls,struct class_attribute *attr,char *buf)
{
        size_t len = 0;
        struct aml_fe_drv *drv;
        unsigned long flags;

        struct aml_fe_drv **list = aml_get_fe_drv_list(AM_DEV_TUNER);
        spin_lock_irqsave(&lock, flags);
        for(drv = *list; drv; drv = drv->next){
	        len += sprintf(buf+len,"%s\n", drv->name);
        }
        spin_unlock_irqrestore(&lock, flags);
        return len;
}

static ssize_t atv_demod_name_show(struct class *cls,struct class_attribute *attr,char *buf)
{
	size_t len = 0;
	struct aml_fe_drv *drv;
	unsigned long flags;

	struct aml_fe_drv **list = aml_get_fe_drv_list(AM_DEV_ATV_DEMOD);
	spin_lock_irqsave(&lock, flags);
	for(drv = *list; drv; drv = drv->next){
		len += sprintf(buf+len,"%s\n", drv->name);
	}
	spin_unlock_irqrestore(&lock, flags);
	return len;
}

static ssize_t dtv_demod_name_show(struct class *cls,struct class_attribute *attr,char *buf)
{
	size_t len = 0;
	struct aml_fe_drv *drv;
	unsigned long flags;

	struct aml_fe_drv **list = aml_get_fe_drv_list(AM_DEV_DTV_DEMOD);
	spin_lock_irqsave(&lock, flags);
	for(drv = *list; drv; drv = drv->next){
		len += sprintf(buf+len,"%s\n", drv->name);
	}
	spin_unlock_irqrestore(&lock, flags);
	return len;
}

static ssize_t setting_show(struct class *cls,struct class_attribute *attr,char *buf)
{
	int r, total = 0;
	int i;
	struct aml_fe_man *fm = &fe_man;

	r = sprintf(buf, "tuners:\n");
	buf += r;
	total += r;
	for(i=0; i<FE_DEV_COUNT; i++){
		struct aml_fe_dev *dev = &fm->tuner[i];
		if(dev->drv){
			r = sprintf(buf, "\t%d: %s i2s_id: %d i2c_addr: 0x%x reset_gpio: 0x%x reset_level: %d\n",
					i,
					dev->drv->name,
					dev->i2c_adap_id,
					dev->i2c_addr,
					dev->reset_gpio,
					dev->reset_value);
			buf += r;
			total += r;
		}
	}

	r = sprintf(buf, "atv_demod:\n");
	buf += r;
	total += r;
	for(i=0; i<FE_DEV_COUNT; i++){
		struct aml_fe_dev *dev = &fm->atv_demod[i];
		if(dev->drv){
			r = sprintf(buf, "\t%d: %s i2s_id: %d i2c_addr: 0x%x reset_gpio: 0x%x reset_level: %d\n",
					i,
					dev->drv->name,
					dev->i2c_adap_id,
					dev->i2c_addr,
					dev->reset_gpio,
					dev->reset_value);
			buf += r;
			total += r;
		}
	}

	r = sprintf(buf, "dtv_demod:\n");
	buf += r;
	total += r;
	for(i=0; i<FE_DEV_COUNT; i++){
		struct aml_fe_dev *dev = &fm->dtv_demod[i];
		if(dev->drv){
			r = sprintf(buf, "\t%d: %s i2s_id: %d i2c_addr: 0x%x reset_gpio: 0x%x reset_level: %d\n",
					i,
					dev->drv->name,
					dev->i2c_adap_id,
					dev->i2c_addr,
					dev->reset_gpio,
					dev->reset_value);
			buf += r;
			total += r;
		}
	}

	r = sprintf(buf, "frontends:\n");
	buf += r;
	total += r;
	for(i=0; i<FE_DEV_COUNT; i++){
		struct aml_fe *fe = &fm->fe[i];

		if(fe->init){
			r = sprintf(buf, "\t%d: device: %d ts: %d tuner: %s atv_demod: %s dtv_demod: %s\n",
					i,
					fe->dev_id,
					fe->ts,
					fe->tuner ? fe->tuner->drv->name : "none",
					fe->atv_demod ? fe->atv_demod->drv->name : "none",
					fe->dtv_demod ? fe->dtv_demod->drv->name : "none");
			buf += r;
			total += r;
		}
	}

	return total;
}

#ifndef CONFIG_OF

static ssize_t aml_fe_show_dynamic_config(struct class *class, struct class_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	return ret;
}

static ssize_t aml_fe_store_dynamic_config(struct class *class, struct class_attribute *attr, const char *buf, size_t size)
{
	struct aml_fe_man *fe_man_pre = platform_get_drvdata(g_aml_fe_pdev);
	struct aml_dvb *dvb = aml_get_dvb_device();
	int i;
	char *s, *p, *t;

	/*user not release fe, we can not config fe*/
	if(aml_fe_buf && (aml_fe_size == size) && !(memcmp(aml_fe_buf, buf, aml_fe_size))){
		return -1;
	}

	for(i = 0; i < aml_fe_num_resources; i++){
		if(aml_fe_resource[i].name){
			kfree(aml_fe_resource[i].name);
			aml_fe_resource[i].name = NULL;
		}
	}

	aml_fe_num_resources = 0;

	i = 0;
	p = (char*)buf;
	while ((s = strstr(p, "|")) != NULL) {
		*s = '\0';
		pr_dbg("%s\n", p);
		if(i % 2 == 0){
			aml_fe_resource[aml_fe_num_resources].start = simple_strtol(p, NULL, 0);
		}
		else{
			t = kmalloc(strlen(p) + 1, GFP_KERNEL);
			if(t){
				memcpy(t, p, strlen(p) + 1);
				aml_fe_resource[aml_fe_num_resources].name = t;

				aml_fe_num_resources++;
			}
			else{
				pr_error("kmalloc fail\n");
			}
		}
		i++;
		p = s+1;
	}

	pr_dbg("num res %d\n", aml_fe_num_resources);

	for (i = 0; i < aml_fe_num_resources; i++) {
		struct resource *r = &aml_fe_resource[i];

		pr_dbg("%d | %s\n", r->start, r->name);
	}

	/* release pre aml fe man and dev */
	if(fe_man_pre){
		platform_set_drvdata(g_aml_fe_pdev, NULL);

		for(i = 0; i < FE_DEV_COUNT; i++){
			aml_fe_man_release(dvb, &fe_man_pre->fe[i]);
		}

		for(i = 0; i < FE_DEV_COUNT; i++){
			aml_fe_dev_release(dvb, AM_DEV_DTV_DEMOD, &fe_man_pre->dtv_demod[i]);
			aml_fe_dev_release(dvb, AM_DEV_ATV_DEMOD, &fe_man_pre->atv_demod[i]);
			aml_fe_dev_release(dvb, AM_DEV_TUNER, &fe_man_pre->tuner[i]);
		}
	}

	for(i = 0; i < FE_DEV_COUNT; i++){
		if(aml_fe_dev_init(dvb, g_aml_fe_pdev, AM_DEV_TUNER, &fe_man.tuner[i], i)<0)
			return -1;
		if(aml_fe_dev_init(dvb, g_aml_fe_pdev, AM_DEV_ATV_DEMOD, &fe_man.atv_demod[i], i)<0)
			return -1;
		if(aml_fe_dev_init(dvb, g_aml_fe_pdev, AM_DEV_DTV_DEMOD, &fe_man.dtv_demod[i], i)<0)
			return -1;
	}

	for(i = 0; i < FE_DEV_COUNT; i++){
		if(aml_fe_man_init(dvb, g_aml_fe_pdev, &fe_man.fe[i], i)<0)
			return -1;
	}



	platform_set_drvdata(g_aml_fe_pdev, &fe_man);

	/*save config for compare*/
	if(aml_fe_buf){
		kfree(aml_fe_buf);
		aml_fe_buf = NULL;
	}

	aml_fe_buf = kmalloc(size, GFP_KERNEL);
	if(aml_fe_buf){
		aml_fe_size = size;
	}
	else{
		pr_error("kmalloc buf fail\n");
	}

	return size;
}

#endif /*CONFIG_OF*/

static ssize_t aml_fe_show_suspended_flag(struct class *class, struct class_attribute *attr, char *buf)
{
	ssize_t ret = 0;

	ret = sprintf(buf, "%d\n", aml_fe_suspended);

	return ret;
}

static ssize_t aml_fe_store_suspended_flag(struct class *class, struct class_attribute *attr, const char *buf, size_t size)
{
	aml_fe_suspended = simple_strtol(buf, 0, 0);

	return size;
}

static struct class_attribute aml_fe_cls_attrs[] = {
	__ATTR(tuner_name,  S_IRUGO | S_IWUSR, tuner_name_show, NULL),
	__ATTR(atv_demod_name,  S_IRUGO | S_IWUSR, atv_demod_name_show, NULL),
	__ATTR(dtv_demod_name,  S_IRUGO | S_IWUSR, dtv_demod_name_show, NULL),
	__ATTR(setting,  S_IRUGO | S_IWUSR, setting_show, NULL),

#ifndef CONFIG_OF
#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	__ATTR(aml_fe_dynamic_config,  S_IRUGO | S_IWUSR, aml_fe_show_dynamic_config, aml_fe_store_dynamic_config),
#endif
#endif

	__ATTR(aml_fe_suspended_flag,  S_IRUGO | S_IWUSR, aml_fe_show_suspended_flag, aml_fe_store_suspended_flag),
	__ATTR_NULL
};

static struct class aml_fe_class = {
	.name = "amlfe",
	.class_attrs = aml_fe_cls_attrs,
};

static int aml_fe_probe(struct platform_device *pdev)
{
	struct aml_dvb *dvb = aml_get_dvb_device();
	int i;


#if defined(CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG) && !defined(CONFIG_OF)
	platform_set_drvdata(pdev, NULL);
	g_aml_fe_pdev = pdev;
#else
	for(i = 0; i < FE_DEV_COUNT; i++){
		if(aml_fe_dev_init(dvb, pdev, AM_DEV_TUNER, &fe_man.tuner[i], i)<0)
			return -1;
		if(aml_fe_dev_init(dvb, pdev, AM_DEV_ATV_DEMOD, &fe_man.atv_demod[i], i)<0)
			return -1;
		if(aml_fe_dev_init(dvb, pdev, AM_DEV_DTV_DEMOD, &fe_man.dtv_demod[i], i)<0)
			return -1;
	}

	for(i = 0; i < FE_DEV_COUNT; i++){
		if(aml_fe_man_init(dvb, pdev, &fe_man.fe[i], i)<0)
			return -1;
	}

#ifdef CONFIG_OF
	fe_man.pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
#endif

	platform_set_drvdata(pdev, &fe_man);
#endif

	if(class_register(&aml_fe_class) < 0) {
		pr_error("[aml_fe..] register class error\n");
	}

	pr_dbg("[aml_fe..] probe ok.\n");

	return 0;
}

static int aml_fe_remove(struct platform_device *pdev)
{
	struct aml_fe_man *fe_man = platform_get_drvdata(pdev);
	struct aml_dvb *dvb = aml_get_dvb_device();
	int i;

#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	if(fe_man){
#endif
		platform_set_drvdata(pdev, NULL);

		for(i = 0; i < FE_DEV_COUNT; i++){
			aml_fe_man_release(dvb, &fe_man->fe[i]);
		}

		for(i = 0; i < FE_DEV_COUNT; i++){
			aml_fe_dev_release(dvb, AM_DEV_DTV_DEMOD, &fe_man->dtv_demod[i]);
			aml_fe_dev_release(dvb, AM_DEV_ATV_DEMOD, &fe_man->atv_demod[i]);
			aml_fe_dev_release(dvb, AM_DEV_TUNER, &fe_man->tuner[i]);
		}

		if(fe_man->pinctrl)
			devm_pinctrl_put(fe_man->pinctrl);

#if defined (CONFIG_AMLOGIC_DYNAMIC_FEANDDMX_CONFIG)
	}
#endif

	class_unregister(&aml_fe_class);

	return 0;
}

static int aml_fe_suspend(struct platform_device *dev, pm_message_t state)
{
	int i;

	for(i = 0; i < FE_DEV_COUNT; i++){
		struct aml_fe *fe = &fe_man.fe[i];

		if(fe->tuner && fe->tuner->drv && fe->tuner->drv->suspend){
			fe->tuner->drv->suspend(fe->tuner);
		}

		if(fe->atv_demod && fe->atv_demod->drv && fe->atv_demod->drv->suspend){
			fe->atv_demod->drv->suspend(fe->atv_demod);
		}

		if(fe->dtv_demod && fe->dtv_demod->drv && fe->dtv_demod->drv->suspend){
			fe->dtv_demod->drv->suspend(fe->dtv_demod);
		}
	}

	aml_fe_suspended = 1;

	return 0;
}

static int aml_fe_resume(struct platform_device *dev)
{
	int i;

	for(i = 0; i < FE_DEV_COUNT; i++){
		struct aml_fe *fe = &fe_man.fe[i];

		if(fe->tuner && fe->tuner->drv && fe->tuner->drv->resume){
			fe->tuner->drv->resume(fe->tuner);
		}

		if(fe->atv_demod && fe->atv_demod->drv && fe->atv_demod->drv->resume){
			fe->atv_demod->drv->resume(fe->atv_demod);
		}

		if(fe->dtv_demod && fe->dtv_demod->drv && fe->dtv_demod->drv->resume){
			fe->dtv_demod->drv->resume(fe->dtv_demod);
		}
	}

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id aml_fe_dt_match[]={
	{
		.compatible = "amlogic,dvbfe",
	},
	{},
};
#endif /*CONFIG_OF*/

static struct platform_driver aml_fe_driver = {
	.probe		= aml_fe_probe,
	.remove		= aml_fe_remove,
	.suspend        = aml_fe_suspend,
	.resume         = aml_fe_resume,
	.driver		= {
		.name	= "amlogic-dvb-fe",
		.owner	= THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = aml_fe_dt_match,
#endif
	}
};
const char *audmode_to_str(unsigned short audmode)
{
  /*  switch(audmode)
    {
        case V4L2_TUNER_AUDMODE_NULL:
            return "V4L2_TUNER_AUDMODE_NULL";
            break;
        case V4L2_TUNER_MODE_MONO:
            return "V4L2_TUNER_MODE_MONO";
            break;
        case V4L2_TUNER_MODE_STEREO:
            return "V4L2_TUNER_MODE_STEREO";
            break;
        case V4L2_TUNER_MODE_LANG2:
            return "V4L2_TUNER_MODE_LANG2";
            break;
        case V4L2_TUNER_MODE_SAP:
            return "V4L2_TUNER_MODE_SAP";
            break;
        case V4L2_TUNER_SUB_LANG1:
            return "V4L2_TUNER_SUB_LANG1";
            break;
        case V4L2_TUNER_MODE_LANG1_LANG2:
            return "V4L2_TUNER_MODE_LANG1_LANG2";
            break;
        default:
            return "NO AUDMODE";
            break;
    }*/
	return 0;
}
EXPORT_SYMBOL(audmode_to_str);
const char *soundsys_to_str(unsigned short sys)
{
/*    switch(sys)
    {
        case V4L2_TUNER_SYS_NULL:
            return "V4L2_TUNER_SYS_NULL";
            break;
         case V4L2_TUNER_SYS_A2_BG:
            return "V4L2_TUNER_SYS_A2_BG";
            break;
         case V4L2_TUNER_SYS_A2_DK1:
            return "V4L2_TUNER_SYS_A2_DK1";
            break;
         case V4L2_TUNER_SYS_A2_DK2:
            return "V4L2_TUNER_SYS_A2_DK2";
            break;
         case V4L2_TUNER_SYS_A2_DK3:
            return "V4L2_TUNER_SYS_A2_DK3";
            break;
         case V4L2_TUNER_SYS_A2_M:
            return "V4L2_TUNER_SYS_A2_M";
            break;
         case V4L2_TUNER_SYS_NICAM_BG:
            return "V4L2_TUNER_SYS_NICAM_BG";
            break;
        case V4L2_TUNER_SYS_NICAM_I:
            return "V4L2_TUNER_SYS_NICAM_I";
            break;
        case V4L2_TUNER_SYS_NICAM_DK:
            return "V4L2_TUNER_SYS_NICAM_DK";
            break;
        case V4L2_TUNER_SYS_NICAM_L:
            return "V4L2_TUNER_SYS_NICAM_L";
            break;
        case V4L2_TUNER_SYS_EIAJ:
            return "V4L2_TUNER_SYS_EIAJ";
            break;
        case V4L2_TUNER_SYS_BTSC:
            return "V4L2_TUNER_SYS_BTSC";
            break;
        case V4L2_TUNER_SYS_FM_RADIO:
            return "V4L2_TUNER_SYS_FM_RADIO";
            break;
        default:
            return "NO SOUND SYS";
            break;
    }*/
    return 0;
}
EXPORT_SYMBOL(soundsys_to_str);

const char *v4l2_std_to_str(v4l2_std_id std)
{
    switch(std){
        case V4L2_STD_PAL_B:
            return "V4L2_STD_PAL_B";
            break;
        case V4L2_STD_PAL_B1:
            return "V4L2_STD_PAL_B1";
            break;
        case V4L2_STD_PAL_G:
            return "V4L2_STD_PAL_G";
            break;
        case V4L2_STD_PAL_H:
            return "V4L2_STD_PAL_H";
            break;
        case V4L2_STD_PAL_I:
            return "V4L2_STD_PAL_I";
            break;
        case V4L2_STD_PAL_D:
            return "V4L2_STD_PAL_D";
            break;
        case V4L2_STD_PAL_D1:
            return "V4L2_STD_PAL_D1";
            break;
        case V4L2_STD_PAL_K:
            return "V4L2_STD_PAL_K";
            break;
        case V4L2_STD_PAL_M:
            return "V4L2_STD_PAL_M";
            break;
        case V4L2_STD_PAL_N:
            return "V4L2_STD_PAL_N";
            break;
        case V4L2_STD_PAL_Nc:
            return "V4L2_STD_PAL_Nc";
            break;
        case V4L2_STD_PAL_60:
            return "V4L2_STD_PAL_60";
            break;
        case V4L2_STD_NTSC_M:
            return "V4L2_STD_NTSC_M";
            break;
        case V4L2_STD_NTSC_M_JP:
            return "V4L2_STD_NTSC_M_JP";
            break;
        case V4L2_STD_NTSC_443:
            return "V4L2_STD_NTSC_443";
            break;
        case V4L2_STD_NTSC_M_KR:
            return "V4L2_STD_NTSC_M_KR";
            break;
        case V4L2_STD_SECAM_B:
            return "V4L2_STD_SECAM_B";
            break;
         case V4L2_STD_SECAM_D:
            return "V4L2_STD_SECAM_D";
            break;
        case V4L2_STD_SECAM_G:
            return "V4L2_STD_SECAM_G";
            break;
        case V4L2_STD_SECAM_H:
            return "V4L2_STD_SECAM_H";
            break;
        case V4L2_STD_SECAM_K:
            return "V4L2_STD_SECAM_K";
            break;
        case V4L2_STD_SECAM_K1:
            return "V4L2_STD_SECAM_K1";
            break;
        case V4L2_STD_SECAM_L:
            return "V4L2_STD_SECAM_L";
            break;
        case V4L2_STD_SECAM_LC:
            return "V4L2_STD_SECAM_LC";
            break;
        case V4L2_STD_ATSC_8_VSB:
            return "V4L2_STD_ATSC_8_VSB";
            break;
         case V4L2_STD_ATSC_16_VSB:
            return "V4L2_STD_ATSC_16_VSB";
            break;
	    /*
         case V4L2_COLOR_STD_PAL:
            return "V4L2_COLOR_STD_PAL";
            break;
         case V4L2_COLOR_STD_NTSC:
            return "V4L2_COLOR_STD_NTSC";
            break;
        case V4L2_COLOR_STD_SECAM:
            return "V4L2_COLOR_STD_SECAM";
            break;
            */
        case V4L2_STD_MN:
            return "V4L2_STD_MN";
            break;
        case V4L2_STD_B:
            return "V4L2_STD_B";
            break;
         case V4L2_STD_GH:
            return "V4L2_STD_GH";
            break;
        case V4L2_STD_DK:
            return "V4L2_STD_DK";
            break;
        case V4L2_STD_PAL_BG:
            return "V4L2_STD_PAL_BG";
            break;
        case V4L2_STD_PAL_DK:
            return "V4L2_STD_PAL_DK";
            break;
        case V4L2_STD_PAL:
            return "V4L2_STD_PAL";
            break;
        case V4L2_STD_NTSC:
            return "V4L2_STD_NTSC";
            break;
        case V4L2_STD_SECAM_DK:
            return "V4L2_STD_SECAM_DK";
            break;
        case V4L2_STD_SECAM:
            return "V4L2_STD_SECAM";
            break;
        case V4L2_STD_525_60:
            return "V4L2_STD_525_60";
            break;
        case V4L2_STD_625_50:
            return "V4L2_STD_625_50";
            break;
        case V4L2_STD_ATSC:
            return "V4L2_STD_ATSC";
            break;
         case V4L2_STD_ALL:
            return "V4L2_STD_ALL";
            break;
         default:
            return "V4L2_STD_UNKNOWN";
            break;
    }
}
EXPORT_SYMBOL(v4l2_std_to_str);
const char* fe_type_to_str(fe_type_t type)
{
    switch(type)
    {
        case FE_QPSK:
            return "FE_QPSK";
            break;
        case FE_QAM:
            return "FE_QAM";
            break;
        case FE_OFDM:
            return "FE_OFDM";
            break;
        case FE_ATSC:
            return "FE_ATSC";
            break;
        case FE_ANALOG:
            return "FE_ANALOG";
            break;
		case FE_ISDBT:
			return "FE_ISDBT";
			break;
		case FE_DTMB:
            return "FE_DTMB";
            break;
       default:
            return "UNKONW TYPE";
            break;
    }
}
static int __init aml_fe_init(void)
{
	return platform_driver_register(&aml_fe_driver);
}


static void __exit aml_fe_exit(void)
{
	platform_driver_unregister(&aml_fe_driver);
}

module_init(aml_fe_init);
module_exit(aml_fe_exit);


MODULE_DESCRIPTION("amlogic frontend driver");
MODULE_AUTHOR("L+#= +0=1");

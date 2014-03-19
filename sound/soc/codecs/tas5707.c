#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/tas57xx.h>

#include "tas5707.h"

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
static struct early_suspend early_suspend;
static void tas5707_early_suspend(struct early_suspend *h);
static void tas5707_late_resume(struct early_suspend *h);
#endif

#define CODEC_DEBUG printk

#define tas5707_RATES (SNDRV_PCM_RATE_8000 | \
		      SNDRV_PCM_RATE_11025 | \
		      SNDRV_PCM_RATE_16000 | \
		      SNDRV_PCM_RATE_22050 | \
		      SNDRV_PCM_RATE_32000 | \
		      SNDRV_PCM_RATE_44100 | \
		      SNDRV_PCM_RATE_48000)

#define tas5707_FORMATS \
	(SNDRV_PCM_FMTBIT_S16_LE  | SNDRV_PCM_FMTBIT_S16_BE  | \
	 SNDRV_PCM_FMTBIT_S20_3LE | SNDRV_PCM_FMTBIT_S20_3BE | \
	 SNDRV_PCM_FMTBIT_S24_LE  | SNDRV_PCM_FMTBIT_S24_BE)

/* Power-up register defaults */
static const u8 tas5707_regs[DDX_NUM_BYTE_REG] = {
	0x6c, 0x70, 0x00, 0xA0, 0x05, 0x40, 0x00, 0xFF,
	0x30, 0x30,	0xFF, 0x00, 0x00, 0x00, 0x91, 0x00,//0x0F
	0x02, 0xAC, 0x54, 0xAC,	0x54, 0x00, 0x00, 0x00,//0x17
	0x00, 0x30, 0x0F, 0x82, 0x02,
};

static u8 TAS5707_drc1_table[3][9]={
	//0x3A   drc1_ae
	{0x3A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
	//0x3B   drc1_aa
	{0x3B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
	//0x3C   drc1_ad
	{0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
};
static u8 tas5707_drc1_tko_table[3][5]={
	//0x40   drc1_t
	{0x40,0x00,0x00,0x00,0x00},
	//0x41   drc1_k
	{0x41,0x00,0x00,0x00,0x00},
	//0x42   drc1_o
	{0x42,0x00,0x00,0x00,0x00}
};

/* codec private data */
struct tas5707_priv {
	struct snd_soc_codec *codec;
	struct tas5707_platform_data *pdata;

	enum snd_soc_control_type control_type;
	void *control_data;

	//Platform provided EQ configuration
	int num_eq_conf_texts;
	const char **eq_conf_texts;
	int eq_cfg;
	struct soc_enum eq_conf_enum;
        unsigned char Ch1_vol;
        unsigned char Ch2_vol;
	unsigned mclk;
};

static const DECLARE_TLV_DB_SCALE(mvol_tlv, -12700, 50, 1);
static const DECLARE_TLV_DB_SCALE(chvol_tlv, -10300, 50, 1);

static const struct snd_kcontrol_new tas5707_snd_controls[] = {
	SOC_SINGLE_TLV("Master Volume", DDX_MASTER_VOLUME, 0, 0xff, 1, mvol_tlv),
	SOC_SINGLE_TLV("Ch1 Volume", DDX_CHANNEL1_VOL, 0, 0xff, 1, chvol_tlv),
	SOC_SINGLE_TLV("Ch2 Volume", DDX_CHANNEL2_VOL, 0, 0xff, 1, chvol_tlv),
	SOC_SINGLE("Ch1 Switch", DDX_SOFT_MUTE, 0, 1, 1),
	SOC_SINGLE("Ch2 Switch", DDX_SOFT_MUTE, 1, 1, 1),
};

static int tas5707_set_dai_sysclk(struct snd_soc_dai *codec_dai,
		int clk_id, unsigned int freq, int dir)
{
	//struct snd_soc_codec *codec = codec_dai->codec;
	struct snd_soc_codec *codec = codec_dai->codec;
	struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
	tas5707->mclk = freq;
	if(freq == 512* 48000)
		snd_soc_write(codec, DDX_CLOCK_CTL, 0x74);//0x74 = 512fs; 0x6c = 256fs
	else
		snd_soc_write(codec, DDX_CLOCK_CTL, 0x6c);//0x74 = 512fs; 0x6c = 256fs
	return 0;
}

static int tas5707_set_dai_fmt(struct snd_soc_dai *codec_dai,
				  unsigned int fmt)
{
	//struct snd_soc_codec *codec = codec_dai->codec;
	CODEC_DEBUG("~~~~%s\n", __func__);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_NB_IF:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int tas5707_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	//struct snd_soc_pcm_runtime *rtd = substream->private_data;
	//struct snd_soc_codec *codec = rtd->codec;
	unsigned int rate;
	CODEC_DEBUG("~~~~%s\n", __func__);

	rate = params_rate(params);
	pr_debug("rate: %u\n", rate);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_BE:
		pr_debug("24bit\n");
		/* fall through */
	case SNDRV_PCM_FORMAT_S32_LE:
	case SNDRV_PCM_FORMAT_S20_3LE:
	case SNDRV_PCM_FORMAT_S20_3BE:
		pr_debug("20bit\n");

		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S16_BE:
		pr_debug("16bit\n");

		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int tas5707_set_bias_level(struct snd_soc_codec *codec,
				 enum snd_soc_bias_level level)
{
	pr_debug("level = %d\n", level);
	switch (level) {
	case SND_SOC_BIAS_ON:
		break;

	case SND_SOC_BIAS_PREPARE:
		/* Full power on */

		break;

	case SND_SOC_BIAS_STANDBY:
		if (codec->dapm.bias_level == SND_SOC_BIAS_OFF) {
		}

		/* Power up to mute */
		/* FIXME */
		break;

	case SND_SOC_BIAS_OFF:
		/* The chip runs through the power down sequence for us. */
		break;
	}
	codec->dapm.bias_level = level;
	return 0;
}

static const struct snd_soc_dai_ops tas5707_dai_ops = {
	.hw_params	= tas5707_hw_params,
	.set_sysclk	= tas5707_set_dai_sysclk,
	.set_fmt	= tas5707_set_dai_fmt,
};

static struct snd_soc_dai_driver tas5707_dai = {
	.name = "tas5707",
	.playback = {
		.stream_name = "HIFI Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = tas5707_RATES,
		.formats = tas5707_FORMATS,
	},
	.ops = &tas5707_dai_ops,
};
static int tas5707_set_master_vol(struct snd_soc_codec *codec)
{
	struct tas5707_platform_data *pdata = dev_get_platdata(codec->dev);

	//using user BSP defined master vol config;
	if(pdata && pdata->custom_master_vol){
		CODEC_DEBUG("tas5707_set_master_vol::using BSP defined master vol config\n");
		snd_soc_write(codec, DDX_MASTER_VOLUME, pdata->custom_master_vol);
	}
	else
		snd_soc_write(codec, DDX_MASTER_VOLUME, 0x00);
	return 0;
}

//tas5707 DRC for channel L/R
static int tas5707_set_drc1(struct snd_soc_codec *codec)
{
	int i = 0, j = 0;
	u8 *p = NULL;
	struct tas5707_platform_data *pdata = dev_get_platdata(codec->dev);

	//using user BSP defined drc1 config;
	if(pdata && pdata->custom_drc1_table && pdata->custom_drc1_table_len == 24){
		p = pdata->custom_drc1_table;
		CODEC_DEBUG("tas5707_set_drc1::using BSP defined drc1 config\n");
		for(i = 0;i < 3;i++){
			for(j = 0;j < 8;j++)
				TAS5707_drc1_table[i][j+1] = p[i*8 + j];
			snd_soc_bulk_write_raw(codec, DDX_DRC1_AE+i, TAS5707_drc1_table[i], 9);
		}
	}
	if(pdata && pdata->custom_drc1_tko_table && pdata->custom_drc1_tko_table_len == 12){
		p = pdata->custom_drc1_tko_table;
		CODEC_DEBUG("tas5707_set_drc1::using BSP defined drc1 TKO config\n");
		for(i = 0;i < 3;i++){
			for(j = 0;j < 4;j++)
				tas5707_drc1_tko_table[i][j+1]= p[i*4 + j];
			snd_soc_bulk_write_raw(codec, DDX_DRC1_T+i, tas5707_drc1_tko_table[i], 5);
		}
	}
	return 0;
}

static int tas5707_set_drc(struct snd_soc_codec *codec)
{
	struct tas5707_platform_data *pdata = dev_get_platdata(codec->dev);
	char drc_mask = 0;
	u8 tas5707_drc_ctl_table[] = {0x46,0x00,0x00,0x00,0x00};
	if(pdata && pdata->enable_ch1_drc){
		drc_mask |= 0x01;
		tas5707_set_drc1(codec);
	}
	tas5707_drc_ctl_table[4] = drc_mask;
	snd_soc_bulk_write_raw(codec, DDX_DRC_CTL, tas5707_drc_ctl_table, 5);
	return 0;
}

static int tas5707_set_eq_biquad(struct snd_soc_codec *codec)
{
	int i = 0, j = 0, k = 0;
	u8 *p = NULL;
	u8 addr;
	u8 tas5707_bq_table[21];
	struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
	struct tas5707_platform_data *pdata = tas5707->pdata;
	struct tas5707_eq_cfg *cfg = pdata->eq_cfgs;

	CODEC_DEBUG("tas5707_set_eq_biquad::using \
		BSP defined EQ biquad config::%s\n", cfg[tas5707->eq_cfg].name);
	p = cfg[tas5707->eq_cfg].regs;

	for(i = 0;i < 2;i++){
		for(j = 0;j < 7;j++){
			addr = (DDX_CH1_BQ_0 + i*7 + j);
			tas5707_bq_table[0] = addr;
			for(k = 0;k < 20;k++){
				tas5707_bq_table[k+1]= p[i*7*20 + j*20 + k];
				printk(KERN_DEBUG "[%d]=%#x\n",k,tas5707_bq_table[k+1]);
			}
			printk(KERN_DEBUG "\n");
			snd_soc_bulk_write_raw(codec, addr, tas5707_bq_table, 21);
		}
	}
	return 0;
}

static int tas5707_put_eq_enum(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
	struct tas5707_platform_data *pdata = tas5707->pdata;
	int value = ucontrol->value.integer.value[0];

	if (value >= pdata->num_eq_cfgs)
		return -EINVAL;

	tas5707->eq_cfg = value;
	tas5707_set_eq_biquad(codec);

	return 0;
}

static int tas5707_get_eq_enum(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
	ucontrol->value.enumerated.item[0] = tas5707->eq_cfg;

	return 0;
}
static int tas5707_set_eq(struct snd_soc_codec *codec)
{
	int i = 0, ret = 0;
	struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
	struct tas5707_platform_data *pdata = tas5707->pdata;
	u8 tas5707_eq_ctl_table[] = {0x50,0x00,0x00,0x00,0x80};
	struct tas5707_eq_cfg *cfg = pdata->eq_cfgs;

	if(!pdata)
		return -ENOENT;

	if(pdata->num_eq_cfgs){
		struct snd_kcontrol_new control =
			SOC_ENUM_EXT("EQ Mode", tas5707->eq_conf_enum,
					tas5707_get_eq_enum, tas5707_put_eq_enum);

		tas5707->eq_conf_texts = kmalloc(sizeof(char *) * pdata->num_eq_cfgs, GFP_KERNEL);
		if(!tas5707->eq_conf_texts){
			dev_err(codec->dev,
				"Fail to allocate %d EQ config tests\n",
				pdata->num_eq_cfgs);
			return -ENOMEM;
		}

		for (i = 0; i < pdata->num_eq_cfgs; i++)
			tas5707->eq_conf_texts[i] = cfg[i].name;

		tas5707->eq_conf_enum.max = pdata->num_eq_cfgs;
		tas5707->eq_conf_enum.texts = tas5707->eq_conf_texts;

		ret = snd_soc_add_controls(codec, &control, 1);
		if (ret != 0)
			dev_err(codec->dev, "Fail to add EQ mode control: %d\n", ret);
	}

	tas5707_set_eq_biquad(codec);

	tas5707_eq_ctl_table[4] &= 0x7F;
	snd_soc_bulk_write_raw(codec, DDX_BANKSWITCH_AND_EQCTL,
						tas5707_eq_ctl_table, 5);
	return 0;
}

static int getRegConfigDataIndex(struct snd_soc_codec *codec, unsigned int reg) {
    int i = 0;
    struct tas5707_platform_data *pdata = dev_get_platdata(codec->dev);
    struct tas57xx_reg_cfg *reg_cfgs = NULL;

    if (pdata && pdata->init_regs) {
        reg_cfgs = pdata->init_regs;
        for (i = 0; i < pdata->num_init_regs; i++) {
            if (reg_cfgs[i].reg_data[0] == reg) {
                break;
            }
        }
    }

    if (i < pdata->num_init_regs) {
        return i;
    }

    return -1;
}

static int init_reg_write(struct snd_soc_codec *codec, unsigned int reg, const void *data, size_t len) {
    int i = 0, tmp_index = 0;
    unsigned char data_buf[64] = { 0 };
    struct tas5707_platform_data *pdata = dev_get_platdata(codec->dev);
    struct tas57xx_reg_cfg *reg_cfgs = NULL;

    tmp_index = getRegConfigDataIndex(codec, reg);
    if (tmp_index < 0) {
        if (len == 2) {
            return snd_soc_write(codec, *((unsigned char *) data), *((unsigned char *) data + 1));
        } else {
            return snd_soc_bulk_write_raw(codec, reg, data, len);
        }
    } else {
        if (pdata && pdata->init_regs) {
            reg_cfgs = pdata->init_regs;

            data_buf[0] = reg_cfgs[tmp_index].reg_data[0];
            memcpy(&data_buf[1], &reg_cfgs[tmp_index].reg_data[2], reg_cfgs[tmp_index].reg_data[1]);
#if 0
            CODEC_DEBUG("%s, addr = 0x%x, bytes = 0x%x\n", __func__, data_buf[0], reg_cfgs[tmp_index].reg_data[1]);
            for (i = 0; i < reg_cfgs[tmp_index].reg_data[1]; i++) {
                CODEC_DEBUG("%s, data[%d] = 0x%02x\n", __func__, i, data_buf[i + 1]);
            }
#endif
            if (len == 2) {
                return snd_soc_write(codec, data_buf[0], data_buf[1]);
            } else {
                return snd_soc_bulk_write_raw(codec, reg, data_buf, reg_cfgs[tmp_index].reg_data[1] + 1);
            }
        }
    }
}

static int init_snd_soc_bulk_write_raw(struct snd_soc_codec *codec, unsigned int reg, const void *data, size_t len) {
    return init_reg_write(codec, reg, data, len);
}

static int init_snd_soc_write(struct snd_soc_codec *codec, unsigned int reg, unsigned int val) {
    unsigned char data_buf[16] = { 0 };

    data_buf[0] = reg;
    data_buf[1] = val;
    return init_reg_write(codec, reg, data_buf, 2);
}

static int tas5707_init(struct snd_soc_codec *codec)
{
	int ret = 0;
	unsigned char burst_data[][5]= {
		{DDX_INPUT_MUX,0x00,0x01,0x77,0x72},
		{DDX_CH4_SOURCE_SELECT,0x00,0x00,0x42,0x03},
		{DDX_PWM_MUX,0x01,0x02,0x13,0x45},
	};
	struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
	struct tas5707_platform_data *pdata = tas5707->pdata;
	CODEC_DEBUG("tas5707_init\n");
	init_snd_soc_write(codec, DDX_OSC_TRIM, 0x00);
	msleep(50);
	init_snd_soc_write(codec, DDX_CLOCK_CTL, 0x6c);//0x74 = 512fs; 0x6c = 256fs
	init_snd_soc_write(codec, DDX_SYS_CTL_1, 0xa0);
	init_snd_soc_write(codec, DDX_SERIAL_DATA_INTERFACE, 0x05);

/*	init_snd_soc_write(codec, DDX_IC_DELAY_CHANNEL_1, 0xac);
	init_snd_soc_write(codec, DDX_IC_DELAY_CHANNEL_2, 0x54);
	init_snd_soc_write(codec, DDX_IC_DELAY_CHANNEL_3, 0xac);
	init_snd_soc_write(codec, DDX_IC_DELAY_CHANNEL_4, 0x54);
*/
	init_snd_soc_write(codec, DDX_BKND_ERR, 0x02);

	init_snd_soc_bulk_write_raw(codec, DDX_INPUT_MUX, burst_data[0], 5);
	init_snd_soc_bulk_write_raw(codec, DDX_CH4_SOURCE_SELECT, burst_data[1], 5);
	init_snd_soc_bulk_write_raw(codec, DDX_PWM_MUX, burst_data[2], 5);

	//drc
	if((ret = tas5707_set_drc(codec)) < 0)
		CODEC_DEBUG("fail to set tas5707 drc\n");
	//eq
	if((ret = tas5707_set_eq(codec)) < 0)
		CODEC_DEBUG("fail to set tas5707 eq\n");

	init_snd_soc_write(codec, DDX_VOLUME_CONFIG, 0xD1);
	init_snd_soc_write(codec, DDX_SYS_CTL_2, 0x84);
	init_snd_soc_write(codec, DDX_START_STOP_PERIOD, 0x95);
	init_snd_soc_write(codec, DDX_PWM_SHUTDOWN_GROUP, 0x30);
	init_snd_soc_write(codec, DDX_MODULATION_LIMIT, 0x02);
	//normal operation
	if((ret = tas5707_set_master_vol(codec)) < 0)
		CODEC_DEBUG("fail to set tas5707 master vol\n");

	init_snd_soc_write(codec, DDX_CHANNEL1_VOL, tas5707->Ch1_vol);
	init_snd_soc_write(codec, DDX_CHANNEL2_VOL, tas5707->Ch2_vol);
	init_snd_soc_write(codec, DDX_SOFT_MUTE, 0x00);

	return ret;
}
static int tas5707_probe(struct snd_soc_codec *codec)
{
	int ret = 0;
	struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
	struct tas5707_platform_data *pdata = dev_get_platdata(codec->dev);

#ifdef CONFIG_HAS_EARLYSUSPEND
        early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
        early_suspend.suspend = tas5707_early_suspend;
        early_suspend.resume = tas5707_late_resume;
        early_suspend.param = codec;
        register_early_suspend(&early_suspend);
#endif

	tas5707->pdata = pdata;
	//codec->control_data = tas5707->control_data;
	codec->control_type = tas5707->control_type;
    ret = snd_soc_codec_set_cache_io(codec, 8, 8, tas5707->control_type);
    if (ret != 0) {
        dev_err(codec->dev, "Failed to set cache I/O: %d\n", ret);
        return ret;
    }

	//TODO: set the DAP
	tas5707_init(codec);

	return 0;
}

static int tas5707_remove(struct snd_soc_codec *codec)
{
	CODEC_DEBUG("~~~~~~~~~~~~%s", __func__);

#ifdef CONFIG_HAS_EARLYSUSPEND
    unregister_early_suspend(&early_suspend);
#endif

	return 0;
}

#ifdef CONFIG_PM
static int tas5707_suspend(struct snd_soc_codec *codec,pm_message_t state) {
    struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
    struct tas5707_platform_data *pdata = dev_get_platdata(codec->dev);

    CODEC_DEBUG("sound::tas5707_suspend\n");

    if (pdata && pdata->suspend_func) {
        pdata->suspend_func();
    }

    //save volume
    tas5707->Ch1_vol = snd_soc_read(codec, DDX_CHANNEL1_VOL);
    tas5707->Ch2_vol = snd_soc_read(codec, DDX_CHANNEL2_VOL);
    tas5707_set_bias_level(codec, SND_SOC_BIAS_OFF);
    return 0;
}

static int tas5707_resume(struct snd_soc_codec *codec) {
    struct tas5707_priv *tas5707 = snd_soc_codec_get_drvdata(codec);
    struct tas5707_platform_data *pdata = dev_get_platdata(codec->dev);

    CODEC_DEBUG("sound::tas5707_resume\n");

    if (pdata && pdata->resume_func) {
        pdata->resume_func();
    }

    tas5707_init(codec);
    snd_soc_write(codec, DDX_CHANNEL1_VOL, tas5707->Ch1_vol);
    snd_soc_write(codec, DDX_CHANNEL2_VOL, tas5707->Ch2_vol);
    tas5707_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
    return 0;
}
#else
#define tas5707_suspend NULL
#define tas5707_resume NULL
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void tas5707_early_suspend(struct early_suspend *h) {
    struct snd_soc_codec *codec = NULL;
    struct tas5707_platform_data *pdata = NULL;

    CODEC_DEBUG("sound::tas5707_early_suspend\n");

    codec = (struct snd_soc_codec *)(h->param);
    pdata = dev_get_platdata(codec->dev);

    if (pdata && pdata->early_suspend_func) {
        pdata->early_suspend_func();
    }

    snd_soc_write(codec, DDX_MASTER_VOLUME, 0xFF);
}

static void tas5707_late_resume(struct early_suspend *h) {
    struct snd_soc_codec *codec = NULL;
    struct tas5707_platform_data *pdata = NULL;

    CODEC_DEBUG("sound::tas5707_late_resume\n");

    codec = (struct snd_soc_codec *)(h->param);
    pdata = dev_get_platdata(codec->dev);

    if (pdata && pdata->late_resume_func) {
        pdata->late_resume_func();
    }

    tas5707_set_master_vol(codec);
}
#endif

static const struct snd_soc_dapm_widget tas5707_dapm_widgets[] = {
SND_SOC_DAPM_DAC("DAC", "HIFI Playback", SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route tas5707_dapm_routes[] = {
	{ "LEFT", NULL, "DAC" },
	{ "RIGHT", NULL, "DAC" },
};
static const struct snd_soc_codec_driver tas5707_codec = {
	.probe =		tas5707_probe,
	.remove =		tas5707_remove,
	.suspend =		tas5707_suspend,
	.resume =		tas5707_resume,
	.reg_cache_size = DDX_NUM_BYTE_REG,
	.reg_word_size = sizeof(u8),
	.reg_cache_default = tas5707_regs,
	//.volatile_register =	tas5707_reg_is_volatile,
	.set_bias_level = tas5707_set_bias_level,
	.controls =		tas5707_snd_controls,
	.num_controls =		ARRAY_SIZE(tas5707_snd_controls),
	.dapm_widgets =		tas5707_dapm_widgets,
	.num_dapm_widgets =	ARRAY_SIZE(tas5707_dapm_widgets),
	.dapm_routes =		tas5707_dapm_routes,
	.num_dapm_routes =	ARRAY_SIZE(tas5707_dapm_routes),
};

static __devinit int tas5707_i2c_probe(struct i2c_client *i2c,
				      const struct i2c_device_id *id)
{
	struct tas5707_priv *tas5707;
	int ret;
	tas5707 = devm_kzalloc(&i2c->dev, sizeof(struct tas5707_priv),
			      GFP_KERNEL);
	if (!tas5707)
		return -ENOMEM;

	i2c_set_clientdata(i2c, tas5707);
	tas5707->control_type = SND_SOC_I2C;
	//tas5707->control_data = i2c;

	ret = snd_soc_register_codec(&i2c->dev, &tas5707_codec,
			&tas5707_dai, 1);
	if (ret != 0){
		dev_err(&i2c->dev, "Failed to register codec (%d)\n", ret);
	}
	return ret;
}

static __devexit int tas5707_i2c_remove(struct i2c_client *client)
{
	//snd_soc_unregister_codec(&client->dev);
	devm_kfree(&client->dev, i2c_get_clientdata(client));
	return 0;
}

static const struct i2c_device_id tas5707_i2c_id[] = {
	{ "tas5707", 0 },
	{ }
};

static struct i2c_driver tas5707_i2c_driver = {
	.driver = {
		.name = "tas5707",
		.owner = THIS_MODULE,
	},
	.probe =    tas5707_i2c_probe,
	.remove =   __devexit_p(tas5707_i2c_remove),
	.id_table = tas5707_i2c_id,
};

static int __init TAS5707_init(void)
{
	return i2c_add_driver(&tas5707_i2c_driver);
}

static void __exit TAS5707_exit(void)
{
	i2c_del_driver(&tas5707_i2c_driver);
}
module_init(TAS5707_init);
module_exit(TAS5707_exit);

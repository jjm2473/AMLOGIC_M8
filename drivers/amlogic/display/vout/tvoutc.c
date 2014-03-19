/*
 * Amlogic Apollo
 * frame buffer driver
 *
 * Copyright (C) 2009 Amlogic, Inc.
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
 * Author:  Tim Yao <timyao@amlogic.com>
 *
 */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <mach/am_regs.h>

#include <linux/amlogic/vout/vinfo.h>
#include <linux/amlogic/logo/logo.h>
#include "tvoutc.h"
#include "tvconf.h"
#include <linux/clk.h>
#include <plat/io.h>
#include <mach/tvregs.h>
#include <mach/mod_gate.h>
#include <linux/amlogic/vout/enc_clk_config.h>

static u32 curr_vdac_setting=DEFAULT_VDAC_SEQUENCE;

#define  SET_VDAC(index,val)   (aml_write_reg32(CBUS_REG_ADDR(index+VENC_VDAC_DACSEL0),val))
static const unsigned int  signal_set[SIGNAL_SET_MAX][3]=
{
	{	VIDEO_SIGNAL_TYPE_INTERLACE_Y,     // component interlace
		VIDEO_SIGNAL_TYPE_INTERLACE_PB,
		VIDEO_SIGNAL_TYPE_INTERLACE_PR,
	},
	{
		VIDEO_SIGNAL_TYPE_CVBS,            	//cvbs&svideo
		VIDEO_SIGNAL_TYPE_SVIDEO_LUMA,
    	VIDEO_SIGNAL_TYPE_SVIDEO_CHROMA,
	},
	{	VIDEO_SIGNAL_TYPE_PROGRESSIVE_Y,     //progressive.
		VIDEO_SIGNAL_TYPE_PROGRESSIVE_PB,
		VIDEO_SIGNAL_TYPE_PROGRESSIVE_PR,
	},
	{
	    VIDEO_SIGNAL_TYPE_PROGEESSIVE_B,     //Analog RGB for VGA.
		VIDEO_SIGNAL_TYPE_PROGEESSIVE_G,
		VIDEO_SIGNAL_TYPE_PROGEESSIVE_R,
	},

};
static  const  char*   signal_table[]={
	"INTERLACE_Y ", /**< Interlace Y signal */
    	"CVBS",            /**< CVBS signal */
    	"SVIDEO_LUMA",     /**< S-Video luma signal */
    	"SVIDEO_CHROMA",   /**< S-Video chroma signal */
    	"INTERLACE_PB",    /**< Interlace Pb signal */
    	"INTERLACE_PR",    /**< Interlace Pr signal */
    	"INTERLACE_R",     /**< Interlace R signal */
         "INTERLACE_G",     /**< Interlace G signal */
         "INTERLACE_B",     /**< Interlace B signal */
         "PROGRESSIVE_Y",   /**< Progressive Y signal */
         "PROGRESSIVE_PB",  /**< Progressive Pb signal */
         "PROGRESSIVE_PR",  /**< Progressive Pr signal */
         "PROGEESSIVE_R",   /**< Progressive R signal */
         "PROGEESSIVE_G",   /**< Progressive G signal */
         "PROGEESSIVE_B",   /**< Progressive B signal */

	};
int 	 get_current_vdac_setting(void)
{
	return curr_vdac_setting;
}

extern unsigned int clk_util_clk_msr(unsigned int clk_mux);

//120120
void  change_vdac_setting(unsigned int  vdec_setting,vmode_t  mode)
{
	unsigned  int  signal_set_index=0;
	unsigned int  idx=0,bit=5,i;
	switch(mode )
	{
		case VMODE_480I:
		case VMODE_576I:
		signal_set_index=0;
		bit=5;
		break;
		case VMODE_480CVBS:
		case VMODE_576CVBS:
		signal_set_index=1;
		bit=2;
		break;
		case VMODE_SVGA:
		case VMODE_XGA:
		case VMODE_VGA:
		signal_set_index=3;
		bit=5;
		break;
		default :
		signal_set_index=2;
		bit=5;
		break;
	}
	for(i=0;i<3;i++)
	{
		idx=vdec_setting>>(bit<<2)&0xf;
		printk("dac index:%d ,signal:%s\n",idx,signal_table[signal_set[signal_set_index][i]]);
		SET_VDAC(idx,signal_set[signal_set_index][i]);
		bit--;
	}
	curr_vdac_setting=vdec_setting;
}

#if 0
static void enable_vsync_interrupt(void)
{
    printk("enable_vsync_interrupt\n");

    CLEAR_CBUS_REG_MASK(HHI_MPEG_CLK_CNTL, 1<<11);

    if (READ_MPEG_REG(ENCP_VIDEO_EN) & 1) {
        WRITE_MPEG_REG(VENC_INTCTRL, 0x200);

#ifdef CONFIG_ARCH_MESON1
        while ((READ_MPEG_REG(VENC_INTFLAG) & 0x200) == 0) {
            u32 line1, line2;

            line1 = line2 = READ_MPEG_REG(VENC_ENCP_LINE);

            while (line1 >= line2) {
                line2 = line1;
                line1 = READ_MPEG_REG(VENC_ENCP_LINE);
            }

            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            if (READ_MPEG_REG(VENC_INTFLAG) & 0x200) {
                break;
            }

            WRITE_MPEG_REG(ENCP_VIDEO_EN, 0);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);

            WRITE_MPEG_REG(ENCP_VIDEO_EN, 1);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
            READ_MPEG_REG(VENC_INTFLAG);
        }
#else
        while ((READ_MPEG_REG(VENC_INTFLAG) & 0x200) == 0) {
            mdelay(50);
            WRITE_MPEG_REG(ENCP_VIDEO_EN, 0);
            READ_MPEG_REG(VENC_INTFLAG);
            WRITE_MPEG_REG(ENCP_VIDEO_EN, 1);
            printk("recycle TV encoder\n");
        }
#endif
    }
    else{
        WRITE_MPEG_REG(VENC_INTCTRL, 0x2);
    }

    printk("Enable vsync done\n");
}
#endif

int tvoutc_setclk(tvmode_t mode)
{
	struct clk *clk;
	const  reg_t *sd,*hd;
	int xtal;

	sd=tvreg_vclk_sd;
	hd=tvreg_vclk_hd;

	clk=clk_get_sys("clk_xtal", NULL);
	if(!clk)
	{
		printk(KERN_ERR "can't find clk %s for VIDEO PLL SETTING!\n\n","clk_xtal");
		return -1;
	}
	xtal=clk_get_rate(clk);
	xtal=xtal/1000000;
	if(xtal>=24 && xtal <=25)/*current only support 24,25*/
		{
		xtal-=24;
		}
	else
		{
		printk(KERN_WARNING "UNsupport xtal setting for vidoe xtal=%d,default to 24M\n",xtal);
		xtal=0;
		}
	switch(mode)
	{
		case TVMODE_480I:
		case TVMODE_480CVBS:
		case TVMODE_480P:
		case TVMODE_576I:
		case TVMODE_576CVBS:
		case TVMODE_576P:
			  setreg(&sd[xtal]);
			  break;
		case TVMODE_720P:
		case TVMODE_720P_50HZ:
		case TVMODE_1080I:
		case TVMODE_1080I_50HZ:
		case TVMODE_1080P:
		case TVMODE_1080P_50HZ:
			  setreg(&hd[xtal]);
			  if(xtal == 1)
			  {
				WRITE_MPEG_REG(HHI_VID_CLK_DIV, 4);
			  }
			  break;
		default:
			printk(KERN_ERR "unsupport tv mode,video clk is not set!!\n");
	}

	return 0 ;
}

static void set_tvmode_misc(tvmode_t mode)
{
    set_vmode_clk(mode);
}

/*
 * uboot_display_already() uses to judge whether display has already
 * be set in uboot.
 * Here, first read the value of reg P_ENCP_VIDEO_MAX_PXCNT and
 * P_ENCP_VIDEO_MAX_LNCNT, then compare with value of tvregsTab[mode]
 */
static int uboot_display_already(tvmode_t mode)
{
    tvmode_t source = vmode_to_tvmode(get_resolution_vmode());
    if(source == mode)
        return 1;
    else
        return 0;
    /*
    const  reg_t *s = tvregsTab[mode];
    unsigned int pxcnt_tab = 0;
    unsigned int lncnt_tab = 0;

    while(s->reg != MREG_END_MARKER) {
        if(s->reg == P_ENCP_VIDEO_MAX_PXCNT) {
            pxcnt_tab = s->val;
        }
        if(s->reg == P_ENCP_VIDEO_MAX_LNCNT) {
            lncnt_tab = s->val;
        }
        s++;
    }

    if((pxcnt_tab == aml_read_reg32(P_ENCP_VIDEO_MAX_PXCNT)) &&
       (lncnt_tab == aml_read_reg32(P_ENCP_VIDEO_MAX_LNCNT))) {
        return 1;
    } else {
        return 0;
    }
    */
}

int tvoutc_setmode(tvmode_t mode)
{
    const  reg_t *s;
#if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8
    static int uboot_display_flag = 1;
#else
    static int uboot_display_flag = 0;
#endif
    if (mode >= TVMODE_MAX) {
        printk(KERN_ERR "Invalid video output modes.\n");
        return -ENODEV;
    }

#if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6
//TODO
//    switch_mod_gate_by_name("venc", 1);
#endif

    printk("TV mode %s selected.\n", tvinfoTab[mode].id);

    s = tvregsTab[mode];

    if(uboot_display_flag) {
        uboot_display_flag = 0;
        if(uboot_display_already(mode)) {
            printk("already display in uboot\n");
            return 0;
        }
    }
    while (MREG_END_MARKER != s->reg)
        setreg(s++);
    printk("%s[%d]\n", __func__, __LINE__);

    if(mode >= TVMODE_VGA || mode <= TVMODE_SXGA){
        aml_write_reg32(P_PERIPHS_PIN_MUX_0,aml_read_reg32(P_PERIPHS_PIN_MUX_0)|(3<<20));
    }else{
	aml_write_reg32(P_PERIPHS_PIN_MUX_0,aml_read_reg32(P_PERIPHS_PIN_MUX_0)&(~(3<<20)));
    }
    set_tvmode_misc(mode);
#ifdef CONFIG_ARCH_MESON1
	tvoutc_setclk(mode);
    printk("%s[%d]\n", __func__, __LINE__);
    enable_vsync_interrupt();
#endif
#ifdef CONFIG_AM_TV_OUTPUT2
	switch(mode)
	{
		case TVMODE_480I:
		case TVMODE_480CVBS:
		case TVMODE_576I:
		case TVMODE_576CVBS:
        aml_set_reg32_bits(P_VPU_VIU_VENC_MUX_CTRL, 1, 0, 2); //reg0x271a, select ENCI to VIU1
        aml_set_reg32_bits(P_VPU_VIU_VENC_MUX_CTRL, 1, 4, 4); //reg0x271a, Select encI clock to VDIN
        aml_set_reg32_bits(P_VPU_VIU_VENC_MUX_CTRL, 1, 8, 4); //reg0x271a,Enable VIU of ENC_I domain to VDIN;
			  break;
		case TVMODE_480P:
		case TVMODE_576P:
		case TVMODE_720P:
		case TVMODE_720P_50HZ:
		case TVMODE_1080I: //??
		case TVMODE_1080I_50HZ: //??
		case TVMODE_1080P:
		case TVMODE_1080P_50HZ:
		case TVMODE_1080P_24HZ:
        case TVMODE_4K2K_30HZ:
        case TVMODE_4K2K_25HZ:
        case TVMODE_4K2K_24HZ:
        case TVMODE_4K2K_SMPTE:
        aml_set_reg32_bits(P_VPU_VIU_VENC_MUX_CTRL, 2, 0, 2); //reg0x271a, select ENCP to VIU1
        aml_set_reg32_bits(P_VPU_VIU_VENC_MUX_CTRL, 2, 4, 4); //reg0x271a, Select encP clock to VDIN
        aml_set_reg32_bits(P_VPU_VIU_VENC_MUX_CTRL, 2, 8, 4); //reg0x271a,Enable VIU of ENC_P domain to VDIN;
        break;
		default:
			printk(KERN_ERR "unsupport tv mode,video clk is not set!!\n");
	}
#endif

    aml_write_reg32(P_VPP_POSTBLEND_H_SIZE, tvinfoTab[mode].xres);

#ifdef CONFIG_ARCH_MESON3
printk(" clk_util_clk_msr 6 = %d\n", clk_util_clk_msr(6));
printk(" clk_util_clk_msr 7 = %d\n", clk_util_clk_msr(7));
printk(" clk_util_clk_msr 8 = %d\n", clk_util_clk_msr(8));
printk(" clk_util_clk_msr 9 = %d\n", clk_util_clk_msr(9));
printk(" clk_util_clk_msr 10 = %d\n", clk_util_clk_msr(10));
printk(" clk_util_clk_msr 27 = %d\n", clk_util_clk_msr(27));
printk(" clk_util_clk_msr 29 = %d\n", clk_util_clk_msr(29));
#endif

#ifdef CONFIG_ARCH_MESON8
	if( (mode==TVMODE_480CVBS) || (mode==TVMODE_576CVBS) )
	{
		msleep(1000);

		aml_write_reg32(P_HHI_VDAC_CNTL0,0x650001);
		aml_write_reg32(P_HHI_VDAC_CNTL1,0x1);
	}
#endif
//while(1);


    return 0;
}


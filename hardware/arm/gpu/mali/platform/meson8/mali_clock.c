#include <linux/platform_device.h>
#include <linux/version.h>
#include <linux/pm.h>
#include <mach/register.h>
#include <mach/irqs.h>
#include <linux/io.h>
#include <mach/io.h>
#include <plat/io.h>
#include <asm/io.h>
#include "mali_clock.h"

unsigned int mali_default_clock_step = MALI_CLOCK_318;

static DEFINE_SPINLOCK(lock);
int mali_clock_critical(critical_t critical, u64 param)
{
	int ret = 0;
	unsigned long flags;
	
	spin_lock_irqsave(&lock, flags);
	ret = critical(param);
	spin_unlock_irqrestore(&lock, flags);
	return ret;
}

static int critical_clock_set(u64 param)
{
	unsigned int clock = param;
	clrbits_le32(P_HHI_MALI_CLK_CNTL, 1 << 8);
	clrbits_le32(P_HHI_MALI_CLK_CNTL, (0x7F | (0x7 << 9)));
	writel(clock, P_HHI_MALI_CLK_CNTL); /* set clock to 333MHZ.*/
	setbits_le32(P_HHI_MALI_CLK_CNTL, 1 << 8);
	return 0;
}

int mali_clock_set(unsigned int  clock) 
{
	return mali_clock_critical(critical_clock_set, (u64)clock);
}

void disable_clock(void)
{
	unsigned long flags;
	spin_lock_irqsave(&lock, flags);
	clrbits_le32(P_HHI_MALI_CLK_CNTL, 1 << 8);
	spin_unlock_irqrestore(&lock, flags);
	printk("## mali clock off----\n");
}

void enable_clock(void)
{
	u32 ret = 0;
	unsigned long flags;
	spin_lock_irqsave(&lock, flags);
	setbits_le32(P_HHI_MALI_CLK_CNTL, 1 << 8);
	ret = readl(P_HHI_MALI_CLK_CNTL) & (1 << 8);
	spin_unlock_irqrestore(&lock, flags);
	printk("## mali clock on :%x++++\n", ret);
}

u32 get_clock_state(void)
{
	u32 ret = 0;
	unsigned long flags;
	spin_lock_irqsave(&lock, flags);
	ret = readl(P_HHI_MALI_CLK_CNTL) & (1 << 8);
	spin_unlock_irqrestore(&lock, flags);
	return ret;
}

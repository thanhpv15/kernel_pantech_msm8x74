/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/ratelimit.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/qpnp-misc.h>
#include <linux/usb/msm_hsusb.h>
#include <linux/usb/msm_ext_chg.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/cdev.h>
#include <linux/completion.h>

#include <mach/rpm-regulator.h>
#include <mach/rpm-regulator-smd.h>
#include <mach/msm_bus.h>
#include <mach/clk.h>

#ifdef CONFIG_PANTECH_PMIC_ABNORMAL
#include <linux/power/qpnp-charger.h>
#endif /* CONFIG_PANTECH_PMIC_ABNORMAL */

#include "dwc3_otg.h"
#include "core.h"
#include "gadget.h"
#include "debug.h"

#ifdef CONFIG_PANTECH_USB_REDRIVER_EN_CONTROL
#include <linux/gpio.h>
#define REDRIVER_EN 81
#endif

#ifdef CONFIG_PANTECH_USB_VER_SWITCH
extern int usb_reconnect_cb(int type);
#endif

#ifdef CONFIG_PANTECH_USB_DEBUG
int dwc3_logmask_value;
// tarial link_state change history
extern int link_state_array[4];
extern struct dwc3 *temp_dwc3;

#undef dev_dbg
#define dev_dbg(dev, format, arg...)		\
	do{ if(dwc3_logmask_value & USB_DEBUG_MASK) dev_printk(KERN_DEBUG, dev, format, ##arg);}while(0)

#endif

#ifdef CONFIG_PANTECH_USB_BLOCKING_MDMSTATE
extern int get_pantech_mdm_state(void);
extern void set_pantech_mdm_callback(void *cb);

enum mdm_reg_type {
    MDM_REG_TYPE_RETRY,
    MDM_REG_TYPE_CONTROL
};
static enum mdm_reg_type mdm_reg_value;
static void control_otg_mdm_state(bool mdm_enabled);
static void register_mdm_callback(enum mdm_reg_type value);
#endif

#ifdef CONFIG_PANTECH_USB_TUNE_SIGNALING_PARAM
static struct dwc3_msm *context;
static u32 pantech_ssphy_init;
static u32 pantech_rx_equal_data; 
#endif
#ifdef CONFIG_PANTECH_USB_EXTERNAL_ID_PULLUP
#if defined(CONFIG_PANTECH_PMIC_SHARED_DATA) && !defined(CONFIG_MACH_MSM8974_EF56S) \
	&& !defined(CONFIG_MACH_MSM8974_EF63S) && !defined(CONFIG_MACH_MSM8974_EF63K) && !defined(CONFIG_MACH_MSM8974_EF63L)
#include <mach/msm_smsm.h>
int get_oem_hw_rev(void)
{
	oem_pm_smem_vendor1_data_type *smem_vendor1_ptr;

	smem_vendor1_ptr = (oem_pm_smem_vendor1_data_type*) smem_alloc(SMEM_ID_VENDOR1,
			sizeof(oem_pm_smem_vendor1_data_type));
    
    if(smem_vendor1_ptr) {
        pr_debug("%s: hw board rev = %d\n", __func__, smem_vendor1_ptr->hw_rev);
        return smem_vendor1_ptr->hw_rev;
    }

    return 1;
}
#endif /* CONFIG_PANTECH_PMIC_SHARED_DATA .... */
extern int qpnp_misc_usb_id_pull_up_set(struct device *consumer_dev, bool set_bit);
#endif /* CONFIG_PANTECH_USB_EXTERNAL_ID_PULLUP */

/* ADC threshold values */
static int adc_low_threshold = 700;
module_param(adc_low_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_low_threshold, "ADC ID Low voltage threshold");

static int adc_high_threshold = 950;
module_param(adc_high_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_high_threshold, "ADC ID High voltage threshold");

static int adc_meas_interval = ADC_MEAS1_INTERVAL_1S;
module_param(adc_meas_interval, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_meas_interval, "ADC ID polling period");

static int override_phy_init;
module_param(override_phy_init, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(override_phy_init, "Override HSPHY Init Seq");

static int ss_phy_override_deemphasis;
module_param(ss_phy_override_deemphasis, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(ss_phy_override_deemphasis, "Override SSPHY demphasis value");

/* Enable Proprietary charger detection */
static bool prop_chg_detect;
module_param(prop_chg_detect, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(prop_chg_detect, "Enable Proprietary charger detection");

/**
 *  USB DBM Hardware registers.
 *
 */
#define DBM_BASE		0x000F8000
#define DBM_EP_CFG(n)		(DBM_BASE + (0x00 + 4 * (n)))
#define DBM_DATA_FIFO(n)	(DBM_BASE + (0x10 + 4 * (n)))
#define DBM_DATA_FIFO_SIZE(n)	(DBM_BASE + (0x20 + 4 * (n)))
#define DBM_DATA_FIFO_EN	(DBM_BASE + (0x30))
#define DBM_GEVNTADR		(DBM_BASE + (0x34))
#define DBM_GEVNTSIZ		(DBM_BASE + (0x38))
#define DBM_DBG_CNFG		(DBM_BASE + (0x3C))
#define DBM_HW_TRB0_EP(n)	(DBM_BASE + (0x40 + 4 * (n)))
#define DBM_HW_TRB1_EP(n)	(DBM_BASE + (0x50 + 4 * (n)))
#define DBM_HW_TRB2_EP(n)	(DBM_BASE + (0x60 + 4 * (n)))
#define DBM_HW_TRB3_EP(n)	(DBM_BASE + (0x70 + 4 * (n)))
#define DBM_PIPE_CFG		(DBM_BASE + (0x80))
#define DBM_SOFT_RESET		(DBM_BASE + (0x84))
#define DBM_GEN_CFG		(DBM_BASE + (0x88))

/**
 *  USB DBM  Hardware registers bitmask.
 *
 */
/* DBM_EP_CFG */
#define DBM_EN_EP		0x00000001
#define USB3_EPNUM		0x0000003E
#define DBM_BAM_PIPE_NUM	0x000000C0
#define DBM_PRODUCER		0x00000100
#define DBM_DISABLE_WB		0x00000200
#define DBM_INT_RAM_ACC		0x00000400

/* DBM_DATA_FIFO_SIZE */
#define DBM_DATA_FIFO_SIZE_MASK	0x0000ffff

/* DBM_GEVNTSIZ */
#define DBM_GEVNTSIZ_MASK	0x0000ffff

/* DBM_DBG_CNFG */
#define DBM_ENABLE_IOC_MASK	0x0000000f

/* DBM_SOFT_RESET */
#define DBM_SFT_RST_EP0		0x00000001
#define DBM_SFT_RST_EP1		0x00000002
#define DBM_SFT_RST_EP2		0x00000004
#define DBM_SFT_RST_EP3		0x00000008
#define DBM_SFT_RST_EPS_MASK	0x0000000F
#define DBM_SFT_RST_MASK	0x80000000
#define DBM_EN_MASK		0x00000002

#define DBM_MAX_EPS		4

/* DBM TRB configurations */
#define DBM_TRB_BIT		0x80000000
#define DBM_TRB_DATA_SRC	0x40000000
#define DBM_TRB_DMA		0x20000000
#define DBM_TRB_EP_NUM(ep)	(ep<<24)

#define USB3_PORTSC		(0x430)
#define PORT_PE			(0x1 << 1)
/**
 *  USB QSCRATCH Hardware registers
 *
 */
#define QSCRATCH_REG_OFFSET	(0x000F8800)
#define QSCRATCH_CTRL_REG      (QSCRATCH_REG_OFFSET + 0x04)
#define QSCRATCH_GENERAL_CFG	(QSCRATCH_REG_OFFSET + 0x08)
#define QSCRATCH_RAM1_REG	(QSCRATCH_REG_OFFSET + 0x0C)
#define HS_PHY_CTRL_REG		(QSCRATCH_REG_OFFSET + 0x10)
#define PARAMETER_OVERRIDE_X_REG (QSCRATCH_REG_OFFSET + 0x14)
#define CHARGING_DET_CTRL_REG	(QSCRATCH_REG_OFFSET + 0x18)
#define CHARGING_DET_OUTPUT_REG	(QSCRATCH_REG_OFFSET + 0x1C)
#define ALT_INTERRUPT_EN_REG	(QSCRATCH_REG_OFFSET + 0x20)
#define HS_PHY_IRQ_STAT_REG	(QSCRATCH_REG_OFFSET + 0x24)
#define CGCTL_REG		(QSCRATCH_REG_OFFSET + 0x28)
#define SS_PHY_CTRL_REG		(QSCRATCH_REG_OFFSET + 0x30)
#define SS_PHY_PARAM_CTRL_1	(QSCRATCH_REG_OFFSET + 0x34)
#define SS_PHY_PARAM_CTRL_2	(QSCRATCH_REG_OFFSET + 0x38)
#define SS_CR_PROTOCOL_DATA_IN_REG  (QSCRATCH_REG_OFFSET + 0x3C)
#define SS_CR_PROTOCOL_DATA_OUT_REG (QSCRATCH_REG_OFFSET + 0x40)
#define SS_CR_PROTOCOL_CAP_ADDR_REG (QSCRATCH_REG_OFFSET + 0x44)
#define SS_CR_PROTOCOL_CAP_DATA_REG (QSCRATCH_REG_OFFSET + 0x48)
#define SS_CR_PROTOCOL_READ_REG     (QSCRATCH_REG_OFFSET + 0x4C)
#define SS_CR_PROTOCOL_WRITE_REG    (QSCRATCH_REG_OFFSET + 0x50)
#define PWR_EVNT_IRQ_STAT_REG    (QSCRATCH_REG_OFFSET + 0x58)
#define PWR_EVNT_IRQ_MASK_REG    (QSCRATCH_REG_OFFSET + 0x5C)

struct dwc3_msm_req_complete {
	struct list_head list_item;
	struct usb_request *req;
	void (*orig_complete)(struct usb_ep *ep,
			      struct usb_request *req);
};

struct dwc3_msm {
	struct device *dev;
	void __iomem *base;
	struct resource *io_res;
	struct platform_device	*dwc3;
	int dbm_num_eps;
	u8 ep_num_mapping[DBM_MAX_EPS];
	const struct usb_ep_ops *original_ep_ops[DWC3_ENDPOINTS_NUM];
	struct list_head req_complete_list;
	struct clk		*xo_clk;
	struct clk		*ref_clk;
	struct clk		*core_clk;
	struct clk		*iface_clk;
	struct clk		*sleep_clk;
	struct clk		*hsphy_sleep_clk;
	struct clk		*utmi_clk;
	unsigned int		utmi_clk_rate;
	struct clk		*utmi_clk_src;
	struct regulator	*hsusb_3p3;
	struct regulator	*hsusb_1p8;
	struct regulator	*hsusb_vddcx;
	struct regulator	*ssusb_1p8;
	struct regulator	*ssusb_vddcx;
	struct regulator	*dwc3_gdsc;

	/* VBUS regulator if no OTG and running in host only mode */
	struct regulator	*vbus_otg;
	struct dwc3_ext_xceiv	ext_xceiv;
	bool			resume_pending;
	atomic_t                pm_suspended;
	atomic_t		in_lpm;
	int			hs_phy_irq;
	int			hsphy_init_seq;
	int			deemphasis_val;
	bool			lpm_irq_seen;
	struct delayed_work	resume_work;
	struct work_struct	restart_usb_work;
	bool			in_restart;
	struct dwc3_charger	charger;
	struct usb_phy		*otg_xceiv;
	struct delayed_work	chg_work;
	enum usb_chg_state	chg_state;
	int			pmic_id_irq;
	struct work_struct	id_work;
	struct qpnp_adc_tm_btm_param	adc_param;
	struct qpnp_adc_tm_chip *adc_tm_dev;
	struct delayed_work	init_adc_work;
	bool			id_adc_detect;
	struct qpnp_vadc_chip	*vadc_dev;
	u8			dcd_retries;
	u32			bus_perf_client;
	struct msm_bus_scale_pdata	*bus_scale_table;
	struct power_supply	usb_psy;
	struct power_supply	*ext_vbus_psy;
	unsigned int		online;
	unsigned int		host_mode;
	unsigned int		voltage_max;
	unsigned int		current_max;
	unsigned int		vdd_no_vol_level;
	unsigned int		vdd_low_vol_level;
	unsigned int		vdd_high_vol_level;
	unsigned int		tx_fifo_size;
	unsigned int		qdss_tx_fifo_size;
	bool			vbus_active;
	bool			ext_inuse;
	enum dwc3_id_state	id_state;
	unsigned long		lpm_flags;
#define MDWC3_PHY_REF_AND_CORECLK_OFF	BIT(0)
#define MDWC3_TCXO_SHUTDOWN		BIT(1)
#define MDWC3_ASYNC_IRQ_WAKE_CAPABILITY	BIT(2)

	u32 qscratch_ctl_val;
	dev_t ext_chg_dev;
	struct cdev ext_chg_cdev;
	struct class *ext_chg_class;
	struct device *ext_chg_device;
	bool ext_chg_opened;
	bool ext_chg_active;
	struct completion ext_chg_wait;
#ifdef CONFIG_ANDROID_PANTECH_USB_MANAGER
	struct delayed_work connect_work;
#endif
};

#define USB_HSPHY_3P3_VOL_MIN		3050000 /* uV */
#define USB_HSPHY_3P3_VOL_MAX		3300000 /* uV */
#define USB_HSPHY_3P3_HPM_LOAD		16000	/* uA */

#define USB_HSPHY_1P8_VOL_MIN		1800000 /* uV */
#define USB_HSPHY_1P8_VOL_MAX		1800000 /* uV */
#define USB_HSPHY_1P8_HPM_LOAD		19000	/* uA */

#define USB_SSPHY_1P8_VOL_MIN		1800000 /* uV */
#define USB_SSPHY_1P8_VOL_MAX		1800000 /* uV */
#define USB_SSPHY_1P8_HPM_LOAD		23000	/* uA */

static struct usb_ext_notification *usb_ext;

/**
 *
 * Read register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 *
 * @return u32
 */
static inline u32 dwc3_msm_read_reg(void *base, u32 offset)
{
	u32 val = ioread32(base + offset);
	return val;
}

/**
 * Read register masked field with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask.
 *
 * @return u32
 */
static inline u32 dwc3_msm_read_reg_field(void *base,
					  u32 offset,
					  const u32 mask)
{
	u32 shift = find_first_bit((void *)&mask, 32);
	u32 val = ioread32(base + offset);
	val &= mask;		/* clear other bits */
	val >>= shift;
	return val;
}

/**
 *
 * Write register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_reg(void *base, u32 offset, u32 val)
{
	iowrite32(val, base + offset);
}

/**
 * Write register masked field with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask.
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_reg_field(void *base, u32 offset,
					    const u32 mask, u32 val)
{
	u32 shift = find_first_bit((void *)&mask, 32);
	u32 tmp = ioread32(base + offset);

	tmp &= ~mask;		/* clear written bits */
	val = tmp | (val << shift);
	iowrite32(val, base + offset);
}

/**
 * Write register and read back masked value to confirm it is written
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask specifying what should be updated
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_readback(void *base, u32 offset,
					    const u32 mask, u32 val)
{
	u32 write_val, tmp = ioread32(base + offset);

	tmp &= ~mask;		/* retain other bits */
	write_val = tmp | val;

	iowrite32(write_val, base + offset);

	/* Read back to see if val was written */
	tmp = ioread32(base + offset);
	tmp &= mask;		/* clear other bits */

	if (tmp != val)
		pr_err("%s: write: %x to QSCRATCH: %x FAILED\n",
			__func__, val, offset);
}

/**
 *
 * Write SSPHY register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @addr - SSPHY address to write.
 * @val - value to write.
 *
 */
static void dwc3_msm_ssusb_write_phycreg(void *base, u32 addr, u32 val)
{
	iowrite32(addr, base + SS_CR_PROTOCOL_DATA_IN_REG);
	iowrite32(0x1, base + SS_CR_PROTOCOL_CAP_ADDR_REG);
	while (ioread32(base + SS_CR_PROTOCOL_CAP_ADDR_REG))
		cpu_relax();

	iowrite32(val, base + SS_CR_PROTOCOL_DATA_IN_REG);
	iowrite32(0x1, base + SS_CR_PROTOCOL_CAP_DATA_REG);
	while (ioread32(base + SS_CR_PROTOCOL_CAP_DATA_REG))
		cpu_relax();

	iowrite32(0x1, base + SS_CR_PROTOCOL_WRITE_REG);
	while (ioread32(base + SS_CR_PROTOCOL_WRITE_REG))
		cpu_relax();
}

/**
 *
 * Read SSPHY register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @addr - SSPHY address to read.
 *
 */
static u32 dwc3_msm_ssusb_read_phycreg(void *base, u32 addr)
{
	bool first_read = true;

	iowrite32(addr, base + SS_CR_PROTOCOL_DATA_IN_REG);
	iowrite32(0x1, base + SS_CR_PROTOCOL_CAP_ADDR_REG);
	while (ioread32(base + SS_CR_PROTOCOL_CAP_ADDR_REG))
		cpu_relax();

	/*
	 * Due to hardware bug, first read of SSPHY register might be
	 * incorrect. Hence as workaround, SW should perform SSPHY register
	 * read twice, but use only second read and ignore first read.
	 */
retry:
	iowrite32(0x1, base + SS_CR_PROTOCOL_READ_REG);
	while (ioread32(base + SS_CR_PROTOCOL_READ_REG))
		cpu_relax();

	if (first_read) {
		ioread32(base + SS_CR_PROTOCOL_DATA_OUT_REG);
		first_read = false;
		goto retry;
	}

	return ioread32(base + SS_CR_PROTOCOL_DATA_OUT_REG);
}

/**
 * Dump all QSCRATCH registers.
 *
 */
static void dwc3_msm_dump_phy_info(struct dwc3_msm *mdwc)
{

	dbg_print_reg("SSPHY_CTRL_REG", dwc3_msm_read_reg(mdwc->base,
						SS_PHY_CTRL_REG));
	dbg_print_reg("HSPHY_CTRL_REG", dwc3_msm_read_reg(mdwc->base,
						HS_PHY_CTRL_REG));
	dbg_print_reg("QSCRATCH_CTRL_REG", dwc3_msm_read_reg(mdwc->base,
						QSCRATCH_CTRL_REG));
	dbg_print_reg("QSCRATCH_GENERAL_CFG", dwc3_msm_read_reg(mdwc->base,
						QSCRATCH_GENERAL_CFG));
	dbg_print_reg("PARAMETER_OVERRIDE_X_REG", dwc3_msm_read_reg(mdwc->base,
						PARAMETER_OVERRIDE_X_REG));
	dbg_print_reg("HS_PHY_IRQ_STAT_REG", dwc3_msm_read_reg(mdwc->base,
						HS_PHY_IRQ_STAT_REG));
	dbg_print_reg("SS_PHY_PARAM_CTRL_1", dwc3_msm_read_reg(mdwc->base,
						SS_PHY_PARAM_CTRL_1));
	dbg_print_reg("SS_PHY_PARAM_CTRL_2", dwc3_msm_read_reg(mdwc->base,
						SS_PHY_PARAM_CTRL_2));
	dbg_print_reg("QSCRATCH_RAM1_REG", dwc3_msm_read_reg(mdwc->base,
						QSCRATCH_RAM1_REG));
	dbg_print_reg("PWR_EVNT_IRQ_STAT_REG", dwc3_msm_read_reg(mdwc->base,
						PWR_EVNT_IRQ_STAT_REG));
	dbg_print_reg("PWR_EVNT_IRQ_MASK_REG", dwc3_msm_read_reg(mdwc->base,
						PWR_EVNT_IRQ_MASK_REG));
}

/**
 * Return DBM EP number according to usb endpoint number.
 *
 */
static int dwc3_msm_find_matching_dbm_ep(struct dwc3_msm *mdwc, u8 usb_ep)
{
	int i;

	for (i = 0; i < mdwc->dbm_num_eps; i++)
		if (mdwc->ep_num_mapping[i] == usb_ep)
			return i;

	return -ENODEV; /* Not found */
}

/**
 * Return number of configured DBM endpoints.
 *
 */
static int dwc3_msm_configured_dbm_ep_num(struct dwc3_msm *mdwc)
{
	int i;
	int count = 0;

	for (i = 0; i < mdwc->dbm_num_eps; i++)
		if (mdwc->ep_num_mapping[i])
			count++;

	return count;
}

/**
 * Configure the DBM with the USB3 core event buffer.
 * This function is called by the SNPS UDC upon initialization.
 *
 * @addr - address of the event buffer.
 * @size - size of the event buffer.
 *
 */
static int dwc3_msm_event_buffer_config(struct dwc3_msm *mdwc,
					u32 addr, u16 size)
{
	dev_dbg(mdwc->dev, "%s\n", __func__);

	dwc3_msm_write_reg(mdwc->base, DBM_GEVNTADR, addr);
	dwc3_msm_write_reg_field(mdwc->base, DBM_GEVNTSIZ,
		DBM_GEVNTSIZ_MASK, size);

	return 0;
}

/**
 * Reset the DBM registers upon initialization.
 *
 */
static int dwc3_msm_dbm_soft_reset(struct dwc3_msm *mdwc, int enter_reset)
{
	dev_dbg(mdwc->dev, "%s\n", __func__);
	if (enter_reset) {
		dev_dbg(mdwc->dev, "enter DBM reset\n");
		dwc3_msm_write_reg_field(mdwc->base, DBM_SOFT_RESET,
			DBM_SFT_RST_MASK, 1);
	} else {
		dev_dbg(mdwc->dev, "exit DBM reset\n");
		dwc3_msm_write_reg_field(mdwc->base, DBM_SOFT_RESET,
			DBM_SFT_RST_MASK, 0);
		/*enable DBM*/
		dwc3_msm_write_reg_field(mdwc->base, QSCRATCH_GENERAL_CFG,
			DBM_EN_MASK, 0x1);
	}

	return 0;
}

/**
 * Soft reset specific DBM ep.
 * This function is called by the function driver upon events
 * such as transfer aborting, USB re-enumeration and USB
 * disconnection.
 *
 * @dbm_ep - DBM ep number.
 * @enter_reset - should we enter a reset state or get out of it.
 *
 */
static int dwc3_msm_dbm_ep_soft_reset(struct dwc3_msm *mdwc,
					u8 dbm_ep, bool enter_reset)
{
	dev_dbg(mdwc->dev, "%s\n", __func__);

	if (dbm_ep >= mdwc->dbm_num_eps) {
		dev_err(mdwc->dev, "%s: Invalid DBM ep index\n", __func__);
		return -ENODEV;
	}

	if (enter_reset) {
		dwc3_msm_write_reg_field(mdwc->base, DBM_SOFT_RESET,
			DBM_SFT_RST_EPS_MASK & 1 << dbm_ep, 1);
	} else {
		dwc3_msm_write_reg_field(mdwc->base, DBM_SOFT_RESET,
			DBM_SFT_RST_EPS_MASK & 1 << dbm_ep, 0);
	}

	return 0;
}

/**
 * Configure a USB DBM ep to work in BAM mode.
 *
 *
 * @usb_ep - USB physical EP number.
 * @producer - producer/consumer.
 * @disable_wb - disable write back to system memory.
 * @internal_mem - use internal USB memory for data fifo.
 * @ioc - enable interrupt on completion.
 *
 * @return int - DBM ep number.
 */
static int dwc3_msm_dbm_ep_config(struct dwc3_msm *mdwc, u8 usb_ep, u8 bam_pipe,
				  bool producer, bool disable_wb,
				  bool internal_mem, bool ioc)
{
	u8 dbm_ep;
	u32 ep_cfg;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	dbm_ep = dwc3_msm_find_matching_dbm_ep(mdwc, usb_ep);

	if (dbm_ep < 0) {
		dev_err(mdwc->dev,
				"%s: Invalid usb ep index\n", __func__);
		return -ENODEV;
	}
	/* First, reset the dbm endpoint */
	dwc3_msm_dbm_ep_soft_reset(mdwc, dbm_ep, 0);

	/* Set ioc bit for dbm_ep if needed */
	dwc3_msm_write_reg_field(mdwc->base, DBM_DBG_CNFG,
		DBM_ENABLE_IOC_MASK & 1 << dbm_ep, ioc ? 1 : 0);

	ep_cfg = (producer ? DBM_PRODUCER : 0) |
		(disable_wb ? DBM_DISABLE_WB : 0) |
		(internal_mem ? DBM_INT_RAM_ACC : 0);

	dwc3_msm_write_reg_field(mdwc->base, DBM_EP_CFG(dbm_ep),
		DBM_PRODUCER | DBM_DISABLE_WB | DBM_INT_RAM_ACC, ep_cfg >> 8);

	dwc3_msm_write_reg_field(mdwc->base, DBM_EP_CFG(dbm_ep), USB3_EPNUM,
		usb_ep);
	dwc3_msm_write_reg_field(mdwc->base, DBM_EP_CFG(dbm_ep),
		DBM_BAM_PIPE_NUM, bam_pipe);
	dwc3_msm_write_reg_field(mdwc->base, DBM_PIPE_CFG, 0x000000ff,
		0xe4);
	dwc3_msm_write_reg_field(mdwc->base, DBM_EP_CFG(dbm_ep), DBM_EN_EP,
		1);

	return dbm_ep;
}

/**
 * Configure a USB DBM ep to work in normal mode.
 *
 * @usb_ep - USB ep number.
 *
 */
static int dwc3_msm_dbm_ep_unconfig(struct dwc3_msm *mdwc, u8 usb_ep)
{
	u8 dbm_ep;
	u32 data;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	dbm_ep = dwc3_msm_find_matching_dbm_ep(mdwc, usb_ep);

	if (dbm_ep < 0) {
		dev_err(mdwc->dev, "%s: Invalid usb ep index\n", __func__);
		return -ENODEV;
	}

	mdwc->ep_num_mapping[dbm_ep] = 0;

	data = dwc3_msm_read_reg(mdwc->base, DBM_EP_CFG(dbm_ep));
	data &= (~0x1);
	dwc3_msm_write_reg(mdwc->base, DBM_EP_CFG(dbm_ep), data);

	/* Reset the dbm endpoint */
	dwc3_msm_dbm_ep_soft_reset(mdwc, dbm_ep, true);
	/*
	 * 10 usec delay is required before deasserting DBM endpoint reset
	 * according to hardware programming guide.
	 */
	udelay(10);
	dwc3_msm_dbm_ep_soft_reset(mdwc, dbm_ep, false);

	return 0;
}

/**
 * Configure the DBM with the BAM's data fifo.
 * This function is called by the USB BAM Driver
 * upon initialization.
 *
 * @ep - pointer to usb endpoint.
 * @addr - address of data fifo.
 * @size - size of data fifo.
 *
 */
int msm_data_fifo_config(struct usb_ep *ep, u32 addr, u32 size, u8 dst_pipe_idx)
{
	u8 dbm_ep;
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	u8 bam_pipe = dst_pipe_idx;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	dbm_ep = bam_pipe;
	mdwc->ep_num_mapping[dbm_ep] = dep->number;

	dwc3_msm_write_reg(mdwc->base, DBM_DATA_FIFO(dbm_ep), addr);
	dwc3_msm_write_reg_field(mdwc->base, DBM_DATA_FIFO_SIZE(dbm_ep),
		DBM_DATA_FIFO_SIZE_MASK, size);

	return 0;
}

/**
* Cleanups for msm endpoint on request complete.
*
* Also call original request complete.
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to usb_request instance.
*
* @return int - 0 on success, negetive on error.
*/
static void dwc3_msm_req_complete_func(struct usb_ep *ep,
				       struct usb_request *request)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_msm_req_complete *req_complete = NULL;

	/* Find original request complete function and remove it from list */
	list_for_each_entry(req_complete, &mdwc->req_complete_list, list_item) {
		if (req_complete->req == request)
			break;
	}
	if (!req_complete || req_complete->req != request) {
		dev_err(dep->dwc->dev, "%s: could not find the request\n",
					__func__);
		return;
	}
	list_del(&req_complete->list_item);

	/*
	 * Release another one TRB to the pool since DBM queue took 2 TRBs
	 * (normal and link), and the dwc3/gadget.c :: dwc3_gadget_giveback
	 * released only one.
	 */
	dep->busy_slot++;

	/* Unconfigure dbm ep */
	dwc3_msm_dbm_ep_unconfig(mdwc, dep->number);

	/*
	 * If this is the last endpoint we unconfigured, than reset also
	 * the event buffers.
	 */
	if (0 == dwc3_msm_configured_dbm_ep_num(mdwc))
		dwc3_msm_event_buffer_config(mdwc, 0, 0);

	/*
	 * Call original complete function, notice that dwc->lock is already
	 * taken by the caller of this function (dwc3_gadget_giveback()).
	 */
	request->complete = req_complete->orig_complete;
	if (request->complete)
		request->complete(ep, request);

	kfree(req_complete);
}

/**
* Helper function.
* See the header of the dwc3_msm_ep_queue function.
*
* @dwc3_ep - pointer to dwc3_ep instance.
* @req - pointer to dwc3_request instance.
*
* @return int - 0 on success, negetive on error.
*/
static int __dwc3_msm_ep_queue(struct dwc3_ep *dep, struct dwc3_request *req)
{
	struct dwc3_trb *trb;
	struct dwc3_trb *trb_link;
	struct dwc3_gadget_ep_cmd_params params;
	u32 cmd;
	int ret = 0;

	/* We push the request to the dep->req_queued list to indicate that
	 * this request is issued with start transfer. The request will be out
	 * from this list in 2 cases. The first is that the transfer will be
	 * completed (not if the transfer is endless using a circular TRBs with
	 * with link TRB). The second case is an option to do stop stransfer,
	 * this can be initiated by the function driver when calling dequeue.
	 */
	req->queued = true;
	list_add_tail(&req->list, &dep->req_queued);

	/* First, prepare a normal TRB, point to the fake buffer */
	trb = &dep->trb_pool[dep->free_slot & DWC3_TRB_MASK];
	dep->free_slot++;
	memset(trb, 0, sizeof(*trb));

	req->trb = trb;
	trb->bph = DBM_TRB_BIT | DBM_TRB_DMA | DBM_TRB_EP_NUM(dep->number);
	trb->size = DWC3_TRB_SIZE_LENGTH(req->request.length);
	trb->ctrl = DWC3_TRBCTL_NORMAL | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_CHN;
	req->trb_dma = dwc3_trb_dma_offset(dep, trb);

	/* Second, prepare a Link TRB that points to the first TRB*/
	trb_link = &dep->trb_pool[dep->free_slot & DWC3_TRB_MASK];
	dep->free_slot++;
	memset(trb_link, 0, sizeof *trb_link);

	trb_link->bpl = lower_32_bits(req->trb_dma);
	trb_link->bph = DBM_TRB_BIT |
			DBM_TRB_DMA | DBM_TRB_EP_NUM(dep->number);
	trb_link->size = 0;
	trb_link->ctrl = DWC3_TRBCTL_LINK_TRB | DWC3_TRB_CTRL_HWO;

	/*
	 * Now start the transfer
	 */
	memset(&params, 0, sizeof(params));
	params.param0 = 0; /* TDAddr High */
	params.param1 = lower_32_bits(req->trb_dma); /* DAddr Low */

	/* DBM requires IOC to be set */
	cmd = DWC3_DEPCMD_STARTTRANSFER | DWC3_DEPCMD_CMDIOC;
	ret = dwc3_send_gadget_ep_cmd(dep->dwc, dep->number, cmd, &params);
	if (ret < 0) {
		dev_dbg(dep->dwc->dev,
			"%s: failed to send STARTTRANSFER command\n",
			__func__);

		list_del(&req->list);
		return ret;
	}
	dep->flags |= DWC3_EP_BUSY;

	return ret;
}

/**
* Queue a usb request to the DBM endpoint.
* This function should be called after the endpoint
* was enabled by the ep_enable.
*
* This function prepares special structure of TRBs which
* is familier with the DBM HW, so it will possible to use
* this endpoint in DBM mode.
*
* The TRBs prepared by this function, is one normal TRB
* which point to a fake buffer, followed by a link TRB
* that points to the first TRB.
*
* The API of this function follow the regular API of
* usb_ep_queue (see usb_ep_ops in include/linuk/usb/gadget.h).
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to usb_request instance.
* @gfp_flags - possible flags.
*
* @return int - 0 on success, negetive on error.
*/
static int dwc3_msm_ep_queue(struct usb_ep *ep,
			     struct usb_request *request, gfp_t gfp_flags)
{
	struct dwc3_request *req = to_dwc3_request(request);
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_msm_req_complete *req_complete;
	unsigned long flags;
	int ret = 0;
	u8 bam_pipe;
	bool producer;
	bool disable_wb;
	bool internal_mem;
	bool ioc;
	u8 speed;

	if (!(request->udc_priv & MSM_SPS_MODE)) {
		/* Not SPS mode, call original queue */
		dev_vdbg(mdwc->dev, "%s: not sps mode, use regular queue\n",
					__func__);

		return (mdwc->original_ep_ops[dep->number])->queue(ep,
								request,
								gfp_flags);
	}

	if (!dep->endpoint.desc) {
		dev_err(mdwc->dev,
			"%s: trying to queue request %p to disabled ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	}

	if (dep->number == 0 || dep->number == 1) {
		dev_err(mdwc->dev,
			"%s: trying to queue dbm request %p to control ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	}


	if (dep->busy_slot != dep->free_slot || !list_empty(&dep->request_list)
					 || !list_empty(&dep->req_queued)) {
		dev_err(mdwc->dev,
			"%s: trying to queue dbm request %p tp ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	} else {
		dep->busy_slot = 0;
		dep->free_slot = 0;
	}

	/*
	 * Override req->complete function, but before doing that,
	 * store it's original pointer in the req_complete_list.
	 */
	req_complete = kzalloc(sizeof(*req_complete), GFP_KERNEL);
	if (!req_complete) {
		dev_err(mdwc->dev, "%s: not enough memory\n", __func__);
		return -ENOMEM;
	}
	req_complete->req = request;
	req_complete->orig_complete = request->complete;
	list_add_tail(&req_complete->list_item, &mdwc->req_complete_list);
	request->complete = dwc3_msm_req_complete_func;

	/*
	 * Configure the DBM endpoint
	 */
	bam_pipe = request->udc_priv & MSM_PIPE_ID_MASK;
	producer = ((request->udc_priv & MSM_PRODUCER) ? true : false);
	disable_wb = ((request->udc_priv & MSM_DISABLE_WB) ? true : false);
	internal_mem = ((request->udc_priv & MSM_INTERNAL_MEM) ? true : false);
	ioc = ((request->udc_priv & MSM_ETD_IOC) ? true : false);

	ret = dwc3_msm_dbm_ep_config(mdwc, dep->number,
					bam_pipe, producer,
					disable_wb, internal_mem, ioc);
	if (ret < 0) {
		dev_err(mdwc->dev,
			"error %d after calling dwc3_msm_dbm_ep_config\n",
			ret);
		return ret;
	}

	dev_vdbg(dwc->dev, "%s: queing request %p to ep %s length %d\n",
			__func__, request, ep->name, request->length);

	/*
	 * We must obtain the lock of the dwc3 core driver,
	 * including disabling interrupts, so we will be sure
	 * that we are the only ones that configure the HW device
	 * core and ensure that we queuing the request will finish
	 * as soon as possible so we will release back the lock.
	 */
	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_msm_ep_queue(dep, req);
	spin_unlock_irqrestore(&dwc->lock, flags);
	if (ret < 0) {
		dev_err(mdwc->dev,
			"error %d after calling __dwc3_msm_ep_queue\n", ret);
		return ret;
	}

	speed = dwc3_readl(dwc->regs, DWC3_DSTS) & DWC3_DSTS_CONNECTSPD;
	dwc3_msm_write_reg(mdwc->base, DBM_GEN_CFG, speed >> 2);

	return 0;
}

/**
 * Configure MSM endpoint.
 * This function do specific configurations
 * to an endpoint which need specific implementaion
 * in the MSM architecture.
 *
 * This function should be called by usb function/class
 * layer which need a support from the specific MSM HW
 * which wrap the USB3 core. (like DBM specific endpoints)
 *
 * @ep - a pointer to some usb_ep instance
 *
 * @return int - 0 on success, negetive on error.
 */
int msm_ep_config(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_ep_ops *new_ep_ops;

	dwc3_msm_event_buffer_config(mdwc,
			dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTADRLO(0)),
			dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTSIZ(0)));

	/* Save original ep ops for future restore*/
	if (mdwc->original_ep_ops[dep->number]) {
		dev_err(mdwc->dev,
			"ep [%s,%d] already configured as msm endpoint\n",
			ep->name, dep->number);
		return -EPERM;
	}
	mdwc->original_ep_ops[dep->number] = ep->ops;

	/* Set new usb ops as we like */
	new_ep_ops = kzalloc(sizeof(struct usb_ep_ops), GFP_KERNEL);
	if (!new_ep_ops) {
		dev_err(mdwc->dev,
			"%s: unable to allocate mem for new usb ep ops\n",
			__func__);
		return -ENOMEM;
	}
	(*new_ep_ops) = (*ep->ops);
	new_ep_ops->queue = dwc3_msm_ep_queue;
	new_ep_ops->disable = ep->ops->disable;

	ep->ops = new_ep_ops;

	/*
	 * Do HERE more usb endpoint configurations
	 * which are specific to MSM.
	 */

	return 0;
}
EXPORT_SYMBOL(msm_ep_config);

/**
 * Un-configure MSM endpoint.
 * Tear down configurations done in the
 * dwc3_msm_ep_config function.
 *
 * @ep - a pointer to some usb_ep instance
 *
 * @return int - 0 on success, negetive on error.
 */
int msm_ep_unconfig(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_ep_ops *old_ep_ops;

	/* Restore original ep ops */
	if (!mdwc->original_ep_ops[dep->number]) {
		dev_err(mdwc->dev,
			"ep [%s,%d] was not configured as msm endpoint\n",
			ep->name, dep->number);
		return -EINVAL;
	}
	old_ep_ops = (struct usb_ep_ops	*)ep->ops;
	ep->ops = mdwc->original_ep_ops[dep->number];
	mdwc->original_ep_ops[dep->number] = NULL;
	kfree(old_ep_ops);

	/*
	 * Do HERE more usb endpoint un-configurations
	 * which are specific to MSM.
	 */

	return 0;
}
EXPORT_SYMBOL(msm_ep_unconfig);

void dwc3_tx_fifo_resize_request(struct usb_ep *ep, bool qdss_enabled)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	if (qdss_enabled) {
		dwc->tx_fifo_reduced = true;
		dwc->tx_fifo_size = mdwc->qdss_tx_fifo_size;
	} else {
		dwc->tx_fifo_reduced = false;
		dwc->tx_fifo_size = mdwc->tx_fifo_size;
	}
}
EXPORT_SYMBOL(dwc3_tx_fifo_resize_request);

static int dwc3_msm_link_clk_reset(struct dwc3_msm *mdwc, bool assert);
static void dwc3_resume_work(struct work_struct *w);
static void dwc3_msm_block_reset(struct dwc3_ext_xceiv *xceiv, bool core_reset);

static void dwc3_restart_usb_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
						restart_usb_work);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	enum dwc3_chg_type chg_type;
	unsigned timeout = 50;
	int ret = 0;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	if (atomic_read(&mdwc->in_lpm) || !mdwc->otg_xceiv) {
		dev_err(mdwc->dev, "%s failed!!!\n", __func__);
		return;
	}

	/* guard against concurrent VBUS handling */
	mdwc->in_restart = true;

	if (!mdwc->ext_xceiv.bsv) {
		dev_dbg(mdwc->dev, "%s bailing out in disconnect\n", __func__);
		dwc->err_evt_seen = false;
		mdwc->in_restart = false;
		return;
	}

	dbg_event(0xFF, "RestartUSB", 0);
	chg_type = mdwc->charger.chg_type;

	/* Reset active USB connection */
	mdwc->ext_xceiv.bsv = false;
	dwc3_resume_work(&mdwc->resume_work.work);

	/* Make sure disconnect is processed before sending connect */
	while (--timeout && !pm_runtime_suspended(mdwc->dev))
		msleep(20);

	if (!timeout) {
		dev_warn(mdwc->dev, "Not in LPM after disconnect, forcing suspend...\n");
		pm_runtime_suspend(mdwc->dev);
	}

	/* perform block reset after ERROR events */
	if (dwc->err_evt_seen) {
		dev_dbg(mdwc->dev, "%s initiating block reset\n", __func__);
		ret = dwc3_msm_link_clk_reset(mdwc, 1);
		if (ret)
			dev_err(mdwc->dev, "%s: clk assert failed\n", __func__);

		usleep_range(1000, 1200);
		ret = dwc3_msm_link_clk_reset(mdwc, 0);
		if (ret)
			dev_err(mdwc->dev, "%s:clk deassrt failed\n", __func__);
		usleep_range(10000, 12000);
	}
	dwc->err_evt_seen = false;
	/* Force reconnect only if cable is still connected */
	if (mdwc->vbus_active) {
		mdwc->ext_xceiv.bsv = true;
		mdwc->charger.chg_type = chg_type;
		dwc3_resume_work(&mdwc->resume_work.work);
	}

	mdwc->in_restart = false;
}

/**
 * Reset USB peripheral connection
 * Inform OTG for Vbus LOW followed by Vbus HIGH notification.
 * This performs full hardware reset and re-initialization which
 * might be required by some DBM client driver during uninit/cleanup.
 */
void msm_dwc3_restart_usb_session(struct usb_gadget *gadget)
{
	struct dwc3 *dwc = container_of(gadget, struct dwc3, gadget);
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	if (!mdwc)
		return;

	dev_dbg(mdwc->dev, "%s\n", __func__);
	queue_work(system_nrt_wq, &mdwc->restart_usb_work);
}
EXPORT_SYMBOL(msm_dwc3_restart_usb_session);

/**
 * msm_register_usb_ext_notification: register for event notification
 * @info: pointer to client usb_ext_notification structure. May be NULL.
 *
 * @return int - 0 on success, negative on error
 */
int msm_register_usb_ext_notification(struct usb_ext_notification *info)
{
	pr_debug("%s usb_ext: %p\n", __func__, info);

	if (info) {
		if (usb_ext) {
			pr_err("%s: already registered\n", __func__);
			return -EEXIST;
		}

		if (!info->notify) {
			pr_err("%s: notify is NULL\n", __func__);
			return -EINVAL;
		}
	}

	usb_ext = info;
	return 0;
}
EXPORT_SYMBOL(msm_register_usb_ext_notification);

/* HSPHY */
static int dwc3_hsusb_config_vddcx(struct dwc3_msm *dwc, int high)
{
	int min_vol, max_vol, ret;

	max_vol = dwc->vdd_high_vol_level;
	min_vol = high ? dwc->vdd_low_vol_level : dwc->vdd_no_vol_level;
	ret = regulator_set_voltage(dwc->hsusb_vddcx, min_vol, max_vol);
	if (ret) {
		dev_err(dwc->dev, "unable to set voltage for HSUSB_VDDCX\n");
		return ret;
	}

	dev_dbg(dwc->dev, "%s: min_vol:%d max_vol:%d\n", __func__,
							min_vol, max_vol);

	return ret;
}

static int dwc3_hsusb_ldo_init(struct dwc3_msm *dwc, int init)
{
	int rc = 0;

	if (!init) {
		regulator_set_voltage(dwc->hsusb_1p8, 0, USB_HSPHY_1P8_VOL_MAX);
		regulator_set_voltage(dwc->hsusb_3p3, 0, USB_HSPHY_3P3_VOL_MAX);
		return 0;
	}

	dwc->hsusb_3p3 = devm_regulator_get(dwc->dev, "HSUSB_3p3");
	if (IS_ERR(dwc->hsusb_3p3)) {
		dev_err(dwc->dev, "unable to get hsusb 3p3\n");
		return PTR_ERR(dwc->hsusb_3p3);
	}

	rc = regulator_set_voltage(dwc->hsusb_3p3,
			USB_HSPHY_3P3_VOL_MIN, USB_HSPHY_3P3_VOL_MAX);
	if (rc) {
		dev_err(dwc->dev, "unable to set voltage for hsusb 3p3\n");
		return rc;
	}
	dwc->hsusb_1p8 = devm_regulator_get(dwc->dev, "HSUSB_1p8");
	if (IS_ERR(dwc->hsusb_1p8)) {
		dev_err(dwc->dev, "unable to get hsusb 1p8\n");
		rc = PTR_ERR(dwc->hsusb_1p8);
		goto devote_3p3;
	}
	rc = regulator_set_voltage(dwc->hsusb_1p8,
			USB_HSPHY_1P8_VOL_MIN, USB_HSPHY_1P8_VOL_MAX);
	if (rc) {
		dev_err(dwc->dev, "unable to set voltage for hsusb 1p8\n");
		goto devote_3p3;
	}

	return 0;

devote_3p3:
	regulator_set_voltage(dwc->hsusb_3p3, 0, USB_HSPHY_3P3_VOL_MAX);

	return rc;
}

static int dwc3_hsusb_ldo_enable(struct dwc3_msm *dwc, int on)
{
	int rc = 0;

	dev_dbg(dwc->dev, "reg (%s)\n", on ? "HPM" : "LPM");

	if (!on)
		goto disable_regulators;


	rc = regulator_set_optimum_mode(dwc->hsusb_1p8, USB_HSPHY_1P8_HPM_LOAD);
	if (rc < 0) {
		dev_err(dwc->dev, "Unable to set HPM of regulator HSUSB_1p8\n");
		return rc;
	}

	rc = regulator_enable(dwc->hsusb_1p8);
	if (rc) {
		dev_err(dwc->dev, "Unable to enable HSUSB_1p8\n");
		goto put_1p8_lpm;
	}

	rc = regulator_set_optimum_mode(dwc->hsusb_3p3,	USB_HSPHY_3P3_HPM_LOAD);
	if (rc < 0) {
		dev_err(dwc->dev, "Unable to set HPM of regulator HSUSB_3p3\n");
		goto disable_1p8;
	}

	rc = regulator_enable(dwc->hsusb_3p3);
	if (rc) {
		dev_err(dwc->dev, "Unable to enable HSUSB_3p3\n");
		goto put_3p3_lpm;
	}

	return 0;

disable_regulators:
	rc = regulator_disable(dwc->hsusb_3p3);
	if (rc)
		dev_err(dwc->dev, "Unable to disable HSUSB_3p3\n");

put_3p3_lpm:
	rc = regulator_set_optimum_mode(dwc->hsusb_3p3, 0);
	if (rc < 0)
		dev_err(dwc->dev, "Unable to set LPM of regulator HSUSB_3p3\n");

disable_1p8:
	rc = regulator_disable(dwc->hsusb_1p8);
	if (rc)
		dev_err(dwc->dev, "Unable to disable HSUSB_1p8\n");

put_1p8_lpm:
	rc = regulator_set_optimum_mode(dwc->hsusb_1p8, 0);
	if (rc < 0)
		dev_err(dwc->dev, "Unable to set LPM of regulator HSUSB_1p8\n");

	return rc < 0 ? rc : 0;
}

/* SSPHY */
static int dwc3_ssusb_config_vddcx(struct dwc3_msm *dwc, int high)
{
	int min_vol, max_vol, ret;

	max_vol = dwc->vdd_high_vol_level;
	min_vol = high ? dwc->vdd_low_vol_level : dwc->vdd_no_vol_level;
	ret = regulator_set_voltage(dwc->ssusb_vddcx, min_vol, max_vol);
	if (ret) {
		dev_err(dwc->dev, "unable to set voltage for SSUSB_VDDCX\n");
		return ret;
	}

	dev_dbg(dwc->dev, "%s: min_vol:%d max_vol:%d\n", __func__,
							min_vol, max_vol);
	return ret;
}

/* 3.3v supply not needed for SS PHY */
static int dwc3_ssusb_ldo_init(struct dwc3_msm *dwc, int init)
{
	int rc = 0;

	if (!init) {
		regulator_set_voltage(dwc->ssusb_1p8, 0, USB_SSPHY_1P8_VOL_MAX);
		return 0;
	}

	dwc->ssusb_1p8 = devm_regulator_get(dwc->dev, "SSUSB_1p8");
	if (IS_ERR(dwc->ssusb_1p8)) {
		dev_err(dwc->dev, "unable to get ssusb 1p8\n");
		return PTR_ERR(dwc->ssusb_1p8);
	}
	rc = regulator_set_voltage(dwc->ssusb_1p8,
			USB_SSPHY_1P8_VOL_MIN, USB_SSPHY_1P8_VOL_MAX);
	if (rc)
		dev_err(dwc->dev, "unable to set voltage for ssusb 1p8\n");

	return rc;
}

static int dwc3_ssusb_ldo_enable(struct dwc3_msm *dwc, int on)
{
	int rc = 0;

	dev_dbg(dwc->dev, "reg (%s)\n", on ? "HPM" : "LPM");

	if (!on)
		goto disable_regulators;


	rc = regulator_set_optimum_mode(dwc->ssusb_1p8, USB_SSPHY_1P8_HPM_LOAD);
	if (rc < 0) {
		dev_err(dwc->dev, "Unable to set HPM of SSUSB_1p8\n");
		return rc;
	}

	rc = regulator_enable(dwc->ssusb_1p8);
	if (rc) {
		dev_err(dwc->dev, "Unable to enable SSUSB_1p8\n");
		goto put_1p8_lpm;
	}

	return 0;

disable_regulators:
	rc = regulator_disable(dwc->ssusb_1p8);
	if (rc)
		dev_err(dwc->dev, "Unable to disable SSUSB_1p8\n");

put_1p8_lpm:
	rc = regulator_set_optimum_mode(dwc->ssusb_1p8, 0);
	if (rc < 0)
		dev_err(dwc->dev, "Unable to set LPM of SSUSB_1p8\n");

	return rc < 0 ? rc : 0;
}

/*
 * Config Global Distributed Switch Controller (GDSC)
 * to support controller power collapse
 */
static int dwc3_msm_config_gdsc(struct dwc3_msm *mdwc, int on)
{
	int ret = 0;

	if (IS_ERR(mdwc->dwc3_gdsc))
		return 0;

	if (!mdwc->dwc3_gdsc) {
		mdwc->dwc3_gdsc = devm_regulator_get(mdwc->dev,
			"USB3_GDSC");
		if (IS_ERR(mdwc->dwc3_gdsc))
			return 0;
	}

	if (on) {
		ret = regulator_enable(mdwc->dwc3_gdsc);
		if (ret) {
			dev_err(mdwc->dev, "unable to enable usb3 gdsc\n");
			return ret;
		}
	} else {
		regulator_disable(mdwc->dwc3_gdsc);
	}

	return 0;
}

static int dwc3_msm_link_clk_reset(struct dwc3_msm *mdwc, bool assert)
{
	int ret = 0;

	if (assert) {
		/* Using asynchronous block reset to the hardware */
		dev_dbg(mdwc->dev, "block_reset ASSERT\n");
		if (!atomic_read(&mdwc->in_lpm)) {
			clk_disable_unprepare(mdwc->ref_clk);
			clk_disable_unprepare(mdwc->iface_clk);
			clk_disable_unprepare(mdwc->core_clk);
		}
		ret = clk_reset(mdwc->core_clk, CLK_RESET_ASSERT);
		if (ret)
			dev_err(mdwc->dev, "dwc3 core_clk assert failed\n");
	} else {
		dev_dbg(mdwc->dev, "block_reset DEASSERT\n");
		ret = clk_reset(mdwc->core_clk, CLK_RESET_DEASSERT);
		ndelay(200);
		if (!atomic_read(&mdwc->in_lpm)) {
			clk_prepare_enable(mdwc->core_clk);
			clk_prepare_enable(mdwc->ref_clk);
			clk_prepare_enable(mdwc->iface_clk);
		}
		if (ret)
			dev_err(mdwc->dev, "dwc3 core_clk deassert failed\n");
	}

	return ret;
}

#ifdef CONFIG_PANTECH_USB_TUNE_SIGNALING_PARAM
void pantech_set_otg_signaling_param(int on)		
{
	struct dwc3_msm *mdwc = context;

	printk("[%s] Enter on=%d\n", __func__, on);
	
	if (on) {
#if defined(CONFIG_MACH_MSM8974_EF59S) || defined(CONFIG_MACH_MSM8974_EF59K) || defined(CONFIG_MACH_MSM8974_EF59L) \
		|| defined(CONFIG_MACH_MSM8974_EF63S) || defined(CONFIG_MACH_MSM8974_EF63K) || defined(CONFIG_MACH_MSM8974_EF63L)
		//Set OTG on, HSUSB Signaling set hs amplitude 14% + source-impedance 1 ohm
		dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x00301E00, (10 << 9) | (1 << 20)); 
#else // others model(ef56, ef60)
		//Set OTG on, HSUSB Signaling set source-impedance 1 ohm
		dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x00300000, 1 << 20); 
#endif /* defined(CONFIG_MACH_MSM8974_EF59S) || defined(CONFIG_MACH_MSM8974_EF59K) || defined(CONFIG_MACH_MSM8974_EF59L) */
	} else {		
		if (mdwc->hsphy_init_seq)
			dwc3_msm_write_readback(mdwc->base,
				PARAMETER_OVERRIDE_X_REG, 0x03FFFFFF,
				mdwc->hsphy_init_seq & 0x03FFFFFF);
	}

}
#endif

#ifdef CONFIG_PANTECH_USB_DEBUG
/* changing log_mask */
static ssize_t dwc3_logmask_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", dwc3_logmask_value);
}

static ssize_t dwc3_logmask_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	sscanf(buf, "%d", &temp);
	
	dwc3_logmask_value = temp;

	return size;
}
static DEVICE_ATTR(dwc3_logmask, S_IRUGO | S_IWUSR, dwc3_logmask_show, dwc3_logmask_store);

// tarial link_state change history test
static ssize_t usb3_link_state_change_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%x:%x:%x:%x\n", link_state_array[0], link_state_array[1], link_state_array[2], link_state_array[3]);
}
static DEVICE_ATTR(usb3_link_state_change, S_IRUGO, usb3_link_state_change_show, NULL);

static ssize_t usb3_link_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{

	unsigned long		flags;
	struct dwc3		*dwc = temp_dwc3;
	enum dwc3_link_state	state;
	u32			reg;

	spin_lock_irqsave(&dwc->lock, flags);
	reg = dwc3_readl(dwc->regs, DWC3_DSTS);
	state = DWC3_DSTS_USBLNKST(reg);
	spin_unlock_irqrestore(&dwc->lock, flags);
	if(dwc->gadget.speed == USB_SPEED_SUPER) {
		switch (state) {
			case DWC3_LINK_STATE_U0:
				return sprintf(buf, "U0\n");
				
			case DWC3_LINK_STATE_U1:
				return sprintf(buf, "U1\n");
				
			case DWC3_LINK_STATE_U2:
				return sprintf(buf, "U2\n");
				
			case DWC3_LINK_STATE_U3:
				return sprintf(buf, "U3\n");
				
			case DWC3_LINK_STATE_SS_DIS:
				return sprintf(buf, "SS.Disabled\n");
				
			case DWC3_LINK_STATE_RX_DET:
				return sprintf(buf, "Rx.Detect\n");
				
			case DWC3_LINK_STATE_SS_INACT:
				return sprintf(buf, "SS.Inactive\n");
				
			case DWC3_LINK_STATE_POLL:
				return sprintf(buf, "Poll\n");
				
			case DWC3_LINK_STATE_RECOV:
				return sprintf(buf, "Recovery\n");
				
			case DWC3_LINK_STATE_HRESET:
				return sprintf(buf, "HRESET\n");
				
			case DWC3_LINK_STATE_CMPLY:
				return sprintf(buf, "Compliance\n");
				
			case DWC3_LINK_STATE_LPBK:
				return sprintf(buf, "Loopback\n");
				
			default:
				return sprintf(buf, "UNKNOWN %d\n", reg);
		}
	} else
		return  sprintf(buf, "USB_SPEED_HIGH\n");
}
static DEVICE_ATTR(usb3_link_state_show, S_IRUGO, usb3_link_state_show, NULL);

// for stability test Y-CABLE process
static int otg_irq_disable = 0; //default enable

//+++ 20130806, P14787, djjeon, charging setting stability test for pmic
void otg_detect_control_test(int on)
{
	struct dwc3_msm *mdwc = context;
	
	if(otg_irq_disable != on) {
		otg_irq_disable = on;

		if(otg_irq_disable == 0)
			enable_irq(mdwc->pmic_id_irq);
		else
			disable_irq(mdwc->pmic_id_irq);
	} else
		printk(KERN_ERR "%s : tarial current state is same!\n", __func__);
}
EXPORT_SYMBOL(otg_detect_control_test);
//--- 20130806, P14787, djjeon, charging setting stability test for pmic

static ssize_t otg_detect_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", otg_irq_disable ? "disable" : "enable");
}

static ssize_t otg_detect_control_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if(otg_irq_disable != temp) {
		otg_irq_disable = temp;

		if(otg_irq_disable == 0)
			enable_irq(mdwc->pmic_id_irq);
		else
			disable_irq(mdwc->pmic_id_irq);
	} else
		printk(KERN_ERR "%s : tarial current state is same!\n", __func__);

	return size;
}
static DEVICE_ATTR(otg_detect_control, S_IRUGO | S_IWUSR, otg_detect_control_show, otg_detect_control_store);

#endif

#ifdef CONFIG_PANTECH_USB_VER_SWITCH
/* changing usb verion 3.0 <-> 2.0 */
static ssize_t dwc3_usb_ver_swi_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int usb_ver = 0;
	struct dwc3		*dwc = temp_dwc3;
	if(dwc && (dwc->gadget.max_speed == USB_SPEED_SUPER))
	{
		usb_ver = 1;	
	}else{
		usb_ver = 0;
	}
	return sprintf(buf, "%d\n", usb_ver);
}

static ssize_t dwc3_usb_ver_swi_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	int usb_ver;
	struct dwc3		*dwc = temp_dwc3;

	sscanf(buf, "%d", &temp);
	if((temp<0) || (temp>1)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 or 1.\n", __func__);
		return size;
	}

	if(!dwc){
		printk(KERN_ERR "%s : dwc is null!\n", __func__);
		return size;
	}

	usb_ver = (dwc->gadget.max_speed == USB_SPEED_HIGH)? 0 : 1;

	if(temp == usb_ver){
		printk(KERN_ERR "%s : usb_ver(%d) is same!\n", __func__, usb_ver);
		return size;
	}

	if(temp == 0){
		dwc->maximum_speed = DWC3_DCFG_HIGHSPEED;	
		dwc->gadget.max_speed = USB_SPEED_HIGH;
	}else{
		dwc->maximum_speed = DWC3_DCFG_SUPERSPEED;	
		dwc->gadget.max_speed = USB_SPEED_SUPER;
	}

	usb_reconnect_cb(0);
	return size;
}
static DEVICE_ATTR(dwc3_usb_ver_swi, S_IRUGO | S_IWUSR, dwc3_usb_ver_swi_show, dwc3_usb_ver_swi_store);
#endif

#ifdef CONFIG_PANTECH_USB_TUNE_SIGNALING_PARAM
/* 
 * SSUSB tune
 */
static ssize_t usb3_tx_swing_full_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, SS_PHY_PARAM_CTRL_1);
	reg &= 0x07f00000;
	read_value = reg >> 20;	
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb3_tx_swing_full_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);
	
	if((temp<0) || (temp>127)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 127.\n", __func__);
		return size;
	}
	write_value = temp;
  pantech_ssphy_init &= ~0x07F00000;
  pantech_ssphy_init |= (write_value << 20);

	dwc3_msm_write_readback(mdwc->base, SS_PHY_PARAM_CTRL_1, 0x07f00000, write_value << 20);

	return size;
}
static DEVICE_ATTR(usb3_tx_swing_full, S_IRUGO | S_IWUSR, usb3_tx_swing_full_show, usb3_tx_swing_full_store);

static ssize_t usb3_tx_deemph_3_5db_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, SS_PHY_PARAM_CTRL_1);
	reg &= 0x00003f00;
	read_value = reg >> 8;
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb3_tx_deemph_3_5db_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>63)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 63.\n", __func__);
		return size;
	}

	write_value = temp;
  pantech_ssphy_init &= ~0x00003F00;
  pantech_ssphy_init |= (write_value << 8);

	dwc3_msm_write_readback(mdwc->base, SS_PHY_PARAM_CTRL_1, 0x00003f00, write_value << 8);

	return size;
}
static DEVICE_ATTR(usb3_tx_deemph_3_5db, S_IRUGO | S_IWUSR, usb3_tx_deemph_3_5db_show, usb3_tx_deemph_3_5db_store);

static ssize_t usb3_rx_equalization_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	/*
	 * Fix RX Equalization setting as follows
	 * LANE0.RX_OVRD_IN_HI. RX_EQ_EN set to 0
	 * LANE0.RX_OVRD_IN_HI.RX_EQ_EN_OVRD set to 1
	 * LANE0.RX_OVRD_IN_HI.RX_EQ set to 3
	 * LANE0.RX_OVRD_IN_HI.RX_EQ_OVRD set to 1
	 */
	reg = dwc3_msm_ssusb_read_phycreg(mdwc->base, 0x1006);
	reg &= ~0xFFFFF8FF; // set value 000 ~ 111;

	read_value = reg >> 8;
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb3_rx_equalization_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	//unsigned long write_value;
	//struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>7)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 7.\n", __func__);
		return size;
	}

	pantech_rx_equal_data = temp;

	return size;
}
static DEVICE_ATTR(usb3_rx_equalization, S_IRUGO | S_IWUSR, usb3_rx_equalization_show, usb3_rx_equalization_store);

static unsigned long rx_elastic_buffer = 0x00; //1032A base default value
static ssize_t usb3_rx_elastic_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned long reg;
	struct dwc3_msm *mdwc = context;
	
	reg = dwc3_msm_read_reg(mdwc->base, DWC3_GUSB3PIPECTL(0));
	reg &= 0x00000001;

	return sprintf(buf, "%d\n", (int)reg);
}

static ssize_t usb3_rx_elastic_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int write_value;
	
	sscanf(buf, "%d", &write_value);

	if((write_value<0) || (write_value>1)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 1.\n", __func__);
		return size;
	}

	rx_elastic_buffer = write_value;

	return size;
}
static DEVICE_ATTR(usb3_rx_elastic, S_IRUGO | S_IWUSR, usb3_rx_elastic_show, usb3_rx_elastic_store);

/* 
 * LS, FS, HSUSB tune
 */
// output impedance for ls/fs
static ssize_t usb2_txfsls_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, PARAMETER_OVERRIDE_X_REG);
	reg &= 0x03C00000;
	read_value = reg >> 22;
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb2_txfsls_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>15)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 15.\n", __func__);
		return size;
	}

	write_value = temp;

	dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x03C00000, write_value << 22);

	return size;
}
static DEVICE_ATTR(usb2_txfsls, S_IRUGO | S_IWUSR, usb2_txfsls_show, usb2_txfsls_store);

// output impedance for hs
static ssize_t usb2_txres_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, PARAMETER_OVERRIDE_X_REG);
	reg &= 0x00300000;
	read_value = reg >> 20;
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb2_txres_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>4)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 4.\n", __func__);
		return size;
	}

	write_value = temp;

	dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x00300000, write_value << 20);

	return size;
}
static DEVICE_ATTR(usb2_txres, S_IRUGO | S_IWUSR, usb2_txres_show, usb2_txres_store);

// hs cross over voltage
static ssize_t usb2_txhsxv_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, PARAMETER_OVERRIDE_X_REG);
	reg &= 0x000C0000;
	read_value = reg >> 18;
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb2_txhsxv_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>4)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 4.\n", __func__);
		return size;
	}

	write_value = temp;

	dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x000C0000, write_value << 18);

	return size;
}
static DEVICE_ATTR(usb2_txhsxv, S_IRUGO | S_IWUSR, usb2_txhsxv_show, usb2_txhsxv_store);

// hs rise/fall time
static ssize_t usb2_txrise_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, PARAMETER_OVERRIDE_X_REG);
	reg &= 0x00030000;
	read_value = reg >> 16;
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb2_txrise_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>4)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 4.\n", __func__);
		return size;
	}

	write_value = temp;

	dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x00030000, write_value << 16);

	return size;
}
static DEVICE_ATTR(usb2_txrise, S_IRUGO | S_IWUSR, usb2_txrise_show, usb2_txrise_store);

// pre-emphasis amplitude
static ssize_t usb2_txpreempamp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, PARAMETER_OVERRIDE_X_REG);
	reg &= 0x0000C000;
	read_value = reg >> 14;
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb2_txpreempamp_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>4)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 4.\n", __func__);
		return size;
	}

	write_value = temp;

	dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x0000C000, write_value << 14);

	return size;
}
static DEVICE_ATTR(usb2_txpreempamp, S_IRUGO | S_IWUSR, usb2_txpreempamp_show, usb2_txpreempamp_store);

// pre-emphasis duration
static ssize_t usb2_txpreemppulse_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, PARAMETER_OVERRIDE_X_REG);
	reg &= 0x00002000;
	read_value = reg >> 13;
	
	return sprintf(buf, "%d\n", read_value);
}

static ssize_t usb2_txpreemppulse_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>1)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 1.\n", __func__);
		return size;
	}

	write_value = temp;

	dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x00002000, write_value << 13);

	return size;
}
static DEVICE_ATTR(usb2_txpreemppulse, S_IRUGO | S_IWUSR, usb2_txpreemppulse_show, usb2_txpreemppulse_store);

// hs amplitude
static ssize_t usb2_txvref_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int read_value;
	unsigned long reg;
	struct dwc3_msm *mdwc = context;

	reg = dwc3_msm_read_reg(mdwc->base, PARAMETER_OVERRIDE_X_REG);
	reg &= 0x00001E00;
	read_value = reg >> 9;
	
	return sprintf(buf, "%d\n", read_value);

}

static ssize_t usb2_txvref_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int temp;
	unsigned long write_value;
	struct dwc3_msm *mdwc = context;

	sscanf(buf, "%d", &temp);

	if((temp<0) || (temp>15)) {
		printk(KERN_ERR "%s : Invalid param range! allowed 0 to 15.\n", __func__);
		return size;
	}

	write_value = temp;

	dwc3_msm_write_readback(mdwc->base, PARAMETER_OVERRIDE_X_REG, 0x00001E00, write_value << 9);

	return size;

}
static DEVICE_ATTR(usb2_txvref, S_IRUGO | S_IWUSR, usb2_txvref_show, usb2_txvref_store);

static struct attribute *dwc3_pantech_phy_control_attrs[] = {
#ifdef CONFIG_PANTECH_USB_DEBUG
	&dev_attr_dwc3_logmask.attr,
// tarial link_state change history test
	&dev_attr_usb3_link_state_change.attr,
	&dev_attr_usb3_link_state_show.attr,
// for stability test Y-CABLE process
	&dev_attr_otg_detect_control.attr,
#endif
#ifdef CONFIG_PANTECH_USB_VER_SWITCH
	&dev_attr_dwc3_usb_ver_swi.attr,
#endif
	// ssusb tune param
	&dev_attr_usb3_tx_swing_full.attr,
	&dev_attr_usb3_tx_deemph_3_5db.attr,
	&dev_attr_usb3_rx_elastic.attr,
	&dev_attr_usb3_rx_equalization.attr,
	// hsusb tune param
	&dev_attr_usb2_txfsls.attr,
	&dev_attr_usb2_txres.attr,
	&dev_attr_usb2_txhsxv.attr,
	&dev_attr_usb2_txrise.attr,
	&dev_attr_usb2_txpreempamp.attr,
	&dev_attr_usb2_txpreemppulse.attr,
	&dev_attr_usb2_txvref.attr,
	NULL,
};

static struct attribute_group dwc3_pantech_phy_control_attr_grp = {
	.attrs = dwc3_pantech_phy_control_attrs,
};
#endif

/* Reinitialize SSPHY parameters by overriding using QSCRATCH CR interface */
static void dwc3_msm_ss_phy_reg_init(struct dwc3_msm *mdwc)
{
	u32 data = 0;

	/*
	 * WORKAROUND: There is SSPHY suspend bug due to which USB enumerates
	 * in HS mode instead of SS mode. Workaround it by asserting
	 * LANE0.TX_ALT_BLOCK.EN_ALT_BUS to enable TX to use alt bus mode
	 */
	data = dwc3_msm_ssusb_read_phycreg(mdwc->base, 0x102D);
	data |= (1 << 7);
	dwc3_msm_ssusb_write_phycreg(mdwc->base, 0x102D, data);

	data = dwc3_msm_ssusb_read_phycreg(mdwc->base, 0x1010);
	data &= ~0xFF0;
	data |= 0x20;
	dwc3_msm_ssusb_write_phycreg(mdwc->base, 0x1010, data);

	/*
	 * Fix RX Equalization setting as follows
	 * LANE0.RX_OVRD_IN_HI. RX_EQ_EN set to 0
	 * LANE0.RX_OVRD_IN_HI.RX_EQ_EN_OVRD set to 1
	 * LANE0.RX_OVRD_IN_HI.RX_EQ set to 3
	 * LANE0.RX_OVRD_IN_HI.RX_EQ_OVRD set to 1
	 */
	data = dwc3_msm_ssusb_read_phycreg(mdwc->base, 0x1006);
	data &= ~(1 << 6);
	data |= (1 << 7);
	data &= ~(0x7 << 8);
#ifdef CONFIG_PANTECH_USB_TUNE_SIGNALING_PARAM
	data |= (pantech_rx_equal_data << 8); //set value 000 ~ 111
#else
	data |= (0x3 << 8);
#endif
	data |= (0x1 << 11);
	dwc3_msm_ssusb_write_phycreg(mdwc->base, 0x1006, data);

	/*
	 * Set EQ and TX launch amplitudes as follows
	 * LANE0.TX_OVRD_DRV_LO.PREEMPH set to 22
	 * LANE0.TX_OVRD_DRV_LO.AMPLITUDE set to 127
	 * LANE0.TX_OVRD_DRV_LO.EN set to 1.
	 */
	data = dwc3_msm_ssusb_read_phycreg(mdwc->base, 0x1002);
	data &= ~0x3F80;
	if (ss_phy_override_deemphasis)
		mdwc->deemphasis_val = ss_phy_override_deemphasis;
	if (mdwc->deemphasis_val)
		data |= (mdwc->deemphasis_val << 7);
	else
		data |= (0x16 << 7);
	data &= ~0x7F;
	data |= (0x7F | (1 << 14));
	dwc3_msm_ssusb_write_phycreg(mdwc->base, 0x1002, data);

	/*
	 * Set the QSCRATCH SS_PHY_PARAM_CTRL1 parameters as follows
	 * TX_FULL_SWING [26:20] amplitude to 127
	 * TX_DEEMPH_3_5DB [13:8] to 22
	 * LOS_BIAS [2:0] to 0x5
	 */
#ifdef CONFIG_PANTECH_USB_TUNE_SIGNALING_PARAM
	dwc3_msm_write_readback(mdwc->base, SS_PHY_PARAM_CTRL_1,
				0x07f03f07, pantech_ssphy_init);
#else
	dwc3_msm_write_readback(mdwc->base, SS_PHY_PARAM_CTRL_1,
				0x07f03f07, 0x07f01605);
#endif
}

static void dwc3_msm_update_ref_clk(struct dwc3_msm *mdwc)
{
	u32 guctl, gfladj = 0;

	guctl = dwc3_msm_read_reg(mdwc->base, DWC3_GUCTL);
	guctl &= ~DWC3_GUCTL_REFCLKPER;

	/* GFLADJ register is used starting with revision 2.50a */
	if (dwc3_msm_read_reg(mdwc->base, DWC3_GSNPSID) >= DWC3_REVISION_250A) {
		gfladj = dwc3_msm_read_reg(mdwc->base, DWC3_GFLADJ);
		gfladj &= ~DWC3_GFLADJ_REFCLK_240MHZDECR_PLS1;
		gfladj &= ~DWC3_GFLADJ_REFCLK_240MHZ_DECR;
		gfladj &= ~DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj &= ~DWC3_GFLADJ_REFCLK_FLADJ;
	}

	/* Refer to SNPS Databook Table 6-55 for calculations used */
	switch (mdwc->utmi_clk_rate) {
	case 19200000:
		guctl |= 52 << __ffs(DWC3_GUCTL_REFCLKPER);
		gfladj |= 12 << __ffs(DWC3_GFLADJ_REFCLK_240MHZ_DECR);
		gfladj |= DWC3_GFLADJ_REFCLK_240MHZDECR_PLS1;
		gfladj |= DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj |= 200 << __ffs(DWC3_GFLADJ_REFCLK_FLADJ);
		break;
	case 24000000:
		guctl |= 41 << __ffs(DWC3_GUCTL_REFCLKPER);
		gfladj |= 10 << __ffs(DWC3_GFLADJ_REFCLK_240MHZ_DECR);
		gfladj |= DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj |= 2032 << __ffs(DWC3_GFLADJ_REFCLK_FLADJ);
		break;
	default:
		dev_warn(mdwc->dev, "Unsupported utmi_clk_rate: %u\n",
				mdwc->utmi_clk_rate);
		break;
	}

	dwc3_msm_write_reg(mdwc->base, DWC3_GUCTL, guctl);
	if (gfladj)
		dwc3_msm_write_reg(mdwc->base, DWC3_GFLADJ, gfladj);
}

/* Initialize QSCRATCH registers for HSPHY and SSPHY operation */
static void dwc3_msm_qscratch_reg_init(struct dwc3_msm *mdwc,
						unsigned event_status)
{
	if (event_status == DWC3_CONTROLLER_POST_RESET_EVENT) {
		dwc3_msm_ss_phy_reg_init(mdwc);
		return;
	}

	/* SSPHY Initialization: Use ref_clk from pads and set its parameters */
	dwc3_msm_write_reg(mdwc->base, SS_PHY_CTRL_REG, 0x10210002);
	msleep(30);
	/* Assert SSPHY reset */
	dwc3_msm_write_reg(mdwc->base, SS_PHY_CTRL_REG, 0x10210082);
	usleep_range(2000, 2200);
	/* De-assert SSPHY reset - power and ref_clock must be ON */
	dwc3_msm_write_reg(mdwc->base, SS_PHY_CTRL_REG, 0x10210002);
	usleep_range(2000, 2200);
	/* Ref clock must be stable now, enable ref clock for HS mode */
	dwc3_msm_write_reg(mdwc->base, SS_PHY_CTRL_REG, 0x11210102);
	usleep_range(2000, 2200);
	/*
	 * HSPHY Initialization: Enable UTMI clock and clamp enable HVINTs,
	 * and disable RETENTION (power-on default is ENABLED)
	 */
	dwc3_msm_write_reg(mdwc->base, HS_PHY_CTRL_REG, 0x5220bb2);
	usleep_range(2000, 2200);
	/* Set XHCI_REV bit (2) to 1 - XHCI version 1.0 */
	dwc3_msm_write_reg(mdwc->base, QSCRATCH_GENERAL_CFG, 0x4);
	/*
	 * write HSPHY init value to QSCRATCH reg to set HSPHY parameters like
	 * VBUS valid threshold, disconnect valid threshold, DC voltage level,
	 * preempasis and rise/fall time.
	 */
	if (override_phy_init)
		mdwc->hsphy_init_seq = override_phy_init;
	if (mdwc->hsphy_init_seq)
		dwc3_msm_write_readback(mdwc->base,
					PARAMETER_OVERRIDE_X_REG, 0x03FFFFFF,
					mdwc->hsphy_init_seq & 0x03FFFFFF);

	/*
	 * Enable master clock for RAMs to allow BAM to access RAMs when
	 * RAM clock gating is enabled via DWC3's GCTL. Otherwise issues
	 * are seen where RAM clocks get turned OFF in SS mode
	 */
	dwc3_msm_write_reg(mdwc->base, CGCTL_REG,
		dwc3_msm_read_reg(mdwc->base, CGCTL_REG) | 0x18);

	/*
	 * This is required to restore the POR value after userspace
	 * is done with charger detection.
	 */
	mdwc->qscratch_ctl_val =
		dwc3_msm_read_reg(mdwc->base, QSCRATCH_CTRL_REG);
}

static void dwc3_msm_notify_event(struct dwc3 *dwc, unsigned event)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	u32 reg;

	if (dwc->revision < DWC3_REVISION_230A)
		return;

	switch (event) {
	case DWC3_CONTROLLER_ERROR_EVENT:
		dev_info(mdwc->dev,
			"DWC3_CONTROLLER_ERROR_EVENT received, irq cnt %lu\n",
			dwc->irq_cnt);

		dwc3_msm_dump_phy_info(mdwc);
		dwc3_msm_write_reg(mdwc->base, DWC3_DEVTEN, 0);

		/* prevent core from generating interrupts until recovery */
		reg = dwc3_msm_read_reg(mdwc->base, DWC3_GCTL);
		reg |= DWC3_GCTL_CORESOFTRESET;
		dwc3_msm_write_reg(mdwc->base, DWC3_GCTL, reg);

		/* restart USB which performs full reset and reconnect */
		queue_work(system_nrt_wq, &mdwc->restart_usb_work);
		break;
	case DWC3_CONTROLLER_RESET_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_RESET_EVENT received\n");
		dwc3_msm_qscratch_reg_init(mdwc, DWC3_CONTROLLER_RESET_EVENT);
		break;
	case DWC3_CONTROLLER_POST_RESET_EVENT:
		dev_dbg(mdwc->dev,
				"DWC3_CONTROLLER_POST_RESET_EVENT received\n");
		dwc3_msm_qscratch_reg_init(mdwc,
					DWC3_CONTROLLER_POST_RESET_EVENT);
		dwc3_msm_update_ref_clk(mdwc);
		dwc->tx_fifo_size = mdwc->tx_fifo_size;
		break;
	case DWC3_CONTROLLER_POST_INITIALIZATION_EVENT:
		/* clear LANE0_PWR_PRESENT bit after initialization is done */
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 24),
									0x0);
	default:
		dev_dbg(mdwc->dev, "unknown dwc3 event\n");
		break;
	}
}

static void dwc3_msm_block_reset(struct dwc3_ext_xceiv *xceiv, bool core_reset)
{
	struct dwc3_msm *mdwc = container_of(xceiv, struct dwc3_msm, ext_xceiv);
	int ret  = 0;

	if (core_reset) {
		ret = dwc3_msm_link_clk_reset(mdwc, 1);
		if (ret)
			return;

		usleep_range(1000, 1200);
		ret = dwc3_msm_link_clk_reset(mdwc, 0);
		if (ret)
			return;

		usleep_range(10000, 12000);
	}

	/* Reset the DBM */
	dwc3_msm_dbm_soft_reset(mdwc, 1);
	usleep_range(1000, 1200);
	dwc3_msm_dbm_soft_reset(mdwc, 0);
}

static void dwc3_chg_enable_secondary_det(struct dwc3_msm *mdwc)
{
	u32 chg_ctrl;

	/* Turn off VDP_SRC */
	dwc3_msm_write_reg(mdwc->base, CHARGING_DET_CTRL_REG, 0x0);
	msleep(20);

	/* Before proceeding make sure VDP_SRC is OFF */
	chg_ctrl = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_CTRL_REG);
	if (chg_ctrl & 0x3F)
		dev_err(mdwc->dev, "%s Unable to reset chg_det block: %x\n",
						 __func__, chg_ctrl);
	/*
	 * Configure DM as current source, DP as current sink
	 * and enable battery charging comparators.
	 */
	dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG, 0x3F, 0x34);
}

static bool dwc3_chg_det_check_linestate(struct dwc3_msm *mdwc)
{
	u32 chg_det;

	if (!prop_chg_detect)
		return false;

	chg_det = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_OUTPUT_REG);
	return chg_det & (3 << 8);
}

static bool dwc3_chg_det_check_output(struct dwc3_msm *mdwc)
{
	u32 chg_det;
	bool ret = false;

	chg_det = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_OUTPUT_REG);
	ret = chg_det & 1;

	return ret;
}

static void dwc3_chg_enable_primary_det(struct dwc3_msm *mdwc)
{
	/*
	 * Configure DP as current source, DM as current sink
	 * and enable battery charging comparators.
	 */
	dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG, 0x3F, 0x30);
}

static inline bool dwc3_chg_check_dcd(struct dwc3_msm *mdwc)
{
	u32 chg_state;
	bool ret = false;

	chg_state = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_OUTPUT_REG);
	ret = chg_state & 2;

	return ret;
}

static inline void dwc3_chg_disable_dcd(struct dwc3_msm *mdwc)
{
	dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG, 0x3F, 0x0);
}

static inline void dwc3_chg_enable_dcd(struct dwc3_msm *mdwc)
{
	/* Data contact detection enable, DCDENB */
	dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG, 0x3F, 0x2);
}

static void dwc3_chg_block_reset(struct dwc3_msm *mdwc)
{
	u32 chg_ctrl;

	dwc3_msm_write_reg(mdwc->base, QSCRATCH_CTRL_REG,
			mdwc->qscratch_ctl_val);
	/* Clear charger detecting control bits */
	dwc3_msm_write_reg(mdwc->base, CHARGING_DET_CTRL_REG, 0x0);

	/* Clear alt interrupt latch and enable bits */
	dwc3_msm_write_reg(mdwc->base, HS_PHY_IRQ_STAT_REG, 0xFFF);
	dwc3_msm_write_reg(mdwc->base, ALT_INTERRUPT_EN_REG, 0x0);

	udelay(100);

	/* Before proceeding make sure charger block is RESET */
	chg_ctrl = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_CTRL_REG);
	if (chg_ctrl & 0x3F)
		dev_err(mdwc->dev, "%s Unable to reset chg_det block: %x\n",
						 __func__, chg_ctrl);
}

static const char *chg_to_string(enum dwc3_chg_type chg_type)
{
	switch (chg_type) {
	case DWC3_SDP_CHARGER:		return "USB_SDP_CHARGER";
	case DWC3_DCP_CHARGER:		return "USB_DCP_CHARGER";
	case DWC3_CDP_CHARGER:		return "USB_CDP_CHARGER";
	case DWC3_PROPRIETARY_CHARGER:	return "USB_PROPRIETARY_CHARGER";
	case DWC3_FLOATED_CHARGER:	return "USB_FLOATED_CHARGER";
	default:			return "UNKNOWN_CHARGER";
	}
}

#define DWC3_CHG_DCD_POLL_TIME		(100 * HZ/1000) /* 100 msec */
#define DWC3_CHG_DCD_MAX_RETRIES	6 /* Tdcd_tmout = 6 * 100 msec */
#define DWC3_CHG_PRIMARY_DET_TIME	(50 * HZ/1000) /* TVDPSRC_ON */
#define DWC3_CHG_SECONDARY_DET_TIME	(50 * HZ/1000) /* TVDMSRC_ON */

static void dwc3_chg_detect_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, chg_work.work);
	bool is_dcd = false, tmout, vout;
	static bool dcd;
	unsigned long delay;

	dev_dbg(mdwc->dev, "chg detection work\n");
	switch (mdwc->chg_state) {
	case USB_CHG_STATE_UNDEFINED:
		dwc3_chg_block_reset(mdwc);
		dwc3_chg_enable_dcd(mdwc);
		mdwc->chg_state = USB_CHG_STATE_WAIT_FOR_DCD;
		mdwc->dcd_retries = 0;
		delay = DWC3_CHG_DCD_POLL_TIME;
		break;
	case USB_CHG_STATE_WAIT_FOR_DCD:
		is_dcd = dwc3_chg_check_dcd(mdwc);
		tmout = ++mdwc->dcd_retries == DWC3_CHG_DCD_MAX_RETRIES;
		if (is_dcd || tmout) {
			if (is_dcd)
				dcd = true;
			else
				dcd = false;
			dwc3_chg_disable_dcd(mdwc);
			usleep_range(1000, 1200);
			if (dwc3_chg_det_check_linestate(mdwc)) {
				mdwc->charger.chg_type =
						DWC3_PROPRIETARY_CHARGER;
				mdwc->chg_state = USB_CHG_STATE_DETECTED;
				delay = 0;
				break;
			}
			dwc3_chg_enable_primary_det(mdwc);
			delay = DWC3_CHG_PRIMARY_DET_TIME;
			mdwc->chg_state = USB_CHG_STATE_DCD_DONE;
		} else {
			delay = DWC3_CHG_DCD_POLL_TIME;
		}
		break;
	case USB_CHG_STATE_DCD_DONE:
		vout = dwc3_chg_det_check_output(mdwc);
		if (vout) {
			dwc3_chg_enable_secondary_det(mdwc);
			delay = DWC3_CHG_SECONDARY_DET_TIME;
			mdwc->chg_state = USB_CHG_STATE_PRIMARY_DONE;
		} else {
			/*
			 * Detect floating charger only if propreitary
			 * charger detection is enabled.
			 */
			if (!dcd && prop_chg_detect)
				mdwc->charger.chg_type =
						DWC3_FLOATED_CHARGER;
			else
				mdwc->charger.chg_type = DWC3_SDP_CHARGER;
			mdwc->chg_state = USB_CHG_STATE_DETECTED;
			delay = 0;
		}
		break;
	case USB_CHG_STATE_PRIMARY_DONE:
		vout = dwc3_chg_det_check_output(mdwc);
		if (vout)
			mdwc->charger.chg_type = DWC3_DCP_CHARGER;
		else
			mdwc->charger.chg_type = DWC3_CDP_CHARGER;
		mdwc->chg_state = USB_CHG_STATE_SECONDARY_DONE;
		/* fall through */
	case USB_CHG_STATE_SECONDARY_DONE:
		mdwc->chg_state = USB_CHG_STATE_DETECTED;
		/* fall through */
	case USB_CHG_STATE_DETECTED:
		dwc3_chg_block_reset(mdwc);
		/* Enable VDP_SRC */
		if (mdwc->charger.chg_type == DWC3_DCP_CHARGER) {
			dwc3_msm_write_readback(mdwc->base,
					CHARGING_DET_CTRL_REG, 0x1F, 0x10);
			if (mdwc->ext_chg_opened) {
				init_completion(&mdwc->ext_chg_wait);
				mdwc->ext_chg_active = true;
			}
		}
		dev_dbg(mdwc->dev, "chg_type = %s\n",
			chg_to_string(mdwc->charger.chg_type));
		mdwc->charger.notify_detection_complete(mdwc->otg_xceiv->otg,
								&mdwc->charger);
		return;
	default:
		return;
	}

	queue_delayed_work(system_nrt_wq, &mdwc->chg_work, delay);
}

static void dwc3_start_chg_det(struct dwc3_charger *charger, bool start)
{
	struct dwc3_msm *mdwc = container_of(charger, struct dwc3_msm, charger);

	if (start == false) {
		dev_dbg(mdwc->dev, "canceling charging detection work\n");
		cancel_delayed_work_sync(&mdwc->chg_work);
		mdwc->chg_state = USB_CHG_STATE_UNDEFINED;
		charger->chg_type = DWC3_INVALID_CHARGER;
		return;
	}

	/* Skip if charger type was already detected externally */
	if (mdwc->chg_state == USB_CHG_STATE_DETECTED &&
		charger->chg_type != DWC3_INVALID_CHARGER)
		return;

	mdwc->chg_state = USB_CHG_STATE_UNDEFINED;
	charger->chg_type = DWC3_INVALID_CHARGER;
	queue_delayed_work(system_nrt_wq, &mdwc->chg_work, 0);
}

static int dwc3_msm_suspend(struct dwc3_msm *mdwc)
{
	int ret;
	bool dcp;
	bool host_bus_suspend;
	bool host_ss_active;
	bool host_ss_suspend;
	bool device_bus_suspend;

	dev_dbg(mdwc->dev, "%s: entering lpm\n", __func__);

	if (atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: Already suspended\n", __func__);
		return 0;
	}

	host_ss_active = dwc3_msm_read_reg(mdwc->base, USB3_PORTSC) & PORT_PE;
	if (mdwc->hs_phy_irq)
		disable_irq(mdwc->hs_phy_irq);

	if (cancel_delayed_work_sync(&mdwc->chg_work))
		dev_dbg(mdwc->dev, "%s: chg_work was pending\n", __func__);
	if (mdwc->chg_state != USB_CHG_STATE_DETECTED) {
		/* charger detection wasn't complete; re-init flags */
		mdwc->chg_state = USB_CHG_STATE_UNDEFINED;
		mdwc->charger.chg_type = DWC3_INVALID_CHARGER;
		dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG,
								0x37, 0x0);
	}

	dcp = ((mdwc->charger.chg_type == DWC3_DCP_CHARGER) ||
	      (mdwc->charger.chg_type == DWC3_PROPRIETARY_CHARGER) ||
	      (mdwc->charger.chg_type == DWC3_FLOATED_CHARGER));
	host_bus_suspend = mdwc->host_mode == 1;
	host_ss_suspend = host_bus_suspend && host_ss_active;
	device_bus_suspend = ((mdwc->charger.chg_type == DWC3_SDP_CHARGER) ||
				 (mdwc->charger.chg_type == DWC3_CDP_CHARGER));

	if (!dcp && !host_bus_suspend)
		dwc3_msm_write_reg(mdwc->base, QSCRATCH_CTRL_REG,
			mdwc->qscratch_ctl_val);

	/* Sequence to put SSPHY in low power state:
	 * 1. Clear REF_SS_PHY_EN in SS_PHY_CTRL_REG
	 * 2. Clear REF_USE_PAD in SS_PHY_CTRL_REG
	 * 3. Set TEST_POWERED_DOWN in SS_PHY_CTRL_REG to enable PHY retention
	 * 4. Disable SSPHY ref clk
	 */
	if (!host_ss_suspend) {
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 8),
									0x0);
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 28),
									0x0);
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 26),
								(1 << 26));
	}
	usleep_range(1000, 1200);
	if (!host_ss_suspend)
		clk_disable_unprepare(mdwc->ref_clk);

	if (host_bus_suspend) {
		/* Sequence for host bus suspend case:
		 * 1. Set suspend and sleep bits in GUSB2PHYCONFIG reg
		 * 2. Clear interrupt latch register and enable BSV, ID HV intr
		 * 3. Enable DP and DM HV interrupts in ALT_INTERRUPT_EN_REG
		 */
		dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
			dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0)) |
								0x00000140);
		dwc3_msm_write_reg(mdwc->base, HS_PHY_IRQ_STAT_REG, 0xFFF);
		if (mdwc->otg_xceiv && (!mdwc->ext_xceiv.otg_capability))
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
							 0x18000, 0x18000);
		dwc3_msm_write_reg(mdwc->base, ALT_INTERRUPT_EN_REG, 0xFC0);
		udelay(5);
	} else {
		/* Sequence to put hardware in low power state:
		 * 1. Set OTGDISABLE to disable OTG block in HSPHY (saves power)
		 * 2. Clear charger detection control fields (performed above)
		 * 3. SUSPEND PHY and turn OFF core clock after some delay
		 * 4. Clear interrupt latch register and enable BSV, ID HV intr
		 * 5. Enable PHY retention
		 */
		dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG, 0x1000,
									0x1000);
		dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
							0xC00000, 0x800000);
		dwc3_msm_write_reg(mdwc->base, HS_PHY_IRQ_STAT_REG, 0xFFF);
		if (mdwc->otg_xceiv && (!mdwc->ext_xceiv.otg_capability))
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
							0x18000, 0x18000);
		if (!dcp)
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
								0x2, 0x0);
	}

	/* make sure above writes are completed before turning off clocks */
	wmb();

	/* remove vote for controller power collapse */
	if (!host_bus_suspend)
		dwc3_msm_config_gdsc(mdwc, 0);

	if (!host_ss_suspend) {
		clk_disable_unprepare(mdwc->core_clk);
		mdwc->lpm_flags |= MDWC3_PHY_REF_AND_CORECLK_OFF;
	}
	clk_disable_unprepare(mdwc->iface_clk);

	if (!host_bus_suspend)
		clk_disable_unprepare(mdwc->utmi_clk);

	if (!host_bus_suspend) {
		/* USB PHY no more requires TCXO */
		clk_disable_unprepare(mdwc->xo_clk);
		mdwc->lpm_flags |= MDWC3_TCXO_SHUTDOWN;
	}

	if (mdwc->bus_perf_client) {
		ret = msm_bus_scale_client_update_request(
						mdwc->bus_perf_client, 0);
		if (ret)
			dev_err(mdwc->dev, "Failed to reset bus bw vote\n");
	}

	if (mdwc->otg_xceiv && mdwc->ext_xceiv.otg_capability && !dcp &&
							!host_bus_suspend)
		dwc3_hsusb_ldo_enable(mdwc, 0);

	dwc3_ssusb_ldo_enable(mdwc, 0);
	dwc3_ssusb_config_vddcx(mdwc, 0);
	if (!host_bus_suspend && !dcp)
		dwc3_hsusb_config_vddcx(mdwc, 0);
	pm_relax(mdwc->dev);
	atomic_set(&mdwc->in_lpm, 1);

	dev_info(mdwc->dev, "DWC3 in low power mode\n");

	if (mdwc->hs_phy_irq) {
		/*
		 * with DCP or during cable disconnect, we dont require wakeup
		 * using HS_PHY_IRQ. Hence enable wakeup only in case of host
		 * bus suspend and device bus suspend.
		 */
		if (host_bus_suspend || device_bus_suspend) {
			enable_irq_wake(mdwc->hs_phy_irq);
			mdwc->lpm_flags |= MDWC3_ASYNC_IRQ_WAKE_CAPABILITY;
		}
		enable_irq(mdwc->hs_phy_irq);
	}

	return 0;
}

static int dwc3_msm_resume(struct dwc3_msm *mdwc)
{
	int ret;
	bool dcp;
	bool host_bus_suspend;
	bool resume_from_core_clk_off = false;

	dev_dbg(mdwc->dev, "%s: exiting lpm\n", __func__);

	if (!atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: Already resumed\n", __func__);
		return 0;
	}

	pm_stay_awake(mdwc->dev);

	if (mdwc->lpm_flags & MDWC3_PHY_REF_AND_CORECLK_OFF)
		resume_from_core_clk_off = true;

	if (mdwc->bus_perf_client) {
		ret = msm_bus_scale_client_update_request(
						mdwc->bus_perf_client, 1);
		if (ret)
			dev_err(mdwc->dev, "Failed to vote for bus scaling\n");
	}

	dcp = ((mdwc->charger.chg_type == DWC3_DCP_CHARGER) ||
	      (mdwc->charger.chg_type == DWC3_PROPRIETARY_CHARGER) ||
	      (mdwc->charger.chg_type == DWC3_FLOATED_CHARGER));
	host_bus_suspend = mdwc->host_mode == 1;

	if (mdwc->lpm_flags & MDWC3_TCXO_SHUTDOWN) {
		/* Vote for TCXO while waking up USB HSPHY */
		ret = clk_prepare_enable(mdwc->xo_clk);
		if (ret)
			dev_err(mdwc->dev, "%s failed to vote TCXO buffer%d\n",
						__func__, ret);
		mdwc->lpm_flags &= ~MDWC3_TCXO_SHUTDOWN;
	}

	/* add vote for controller power collapse */
	if (!host_bus_suspend)
		dwc3_msm_config_gdsc(mdwc, 1);

	if (!host_bus_suspend)
		clk_prepare_enable(mdwc->utmi_clk);

	if (mdwc->otg_xceiv && mdwc->ext_xceiv.otg_capability && !dcp &&
							!host_bus_suspend)
		dwc3_hsusb_ldo_enable(mdwc, 1);

	dwc3_ssusb_ldo_enable(mdwc, 1);
	dwc3_ssusb_config_vddcx(mdwc, 1);

	if (!host_bus_suspend && !dcp)
		dwc3_hsusb_config_vddcx(mdwc, 1);

	if (mdwc->lpm_flags & MDWC3_PHY_REF_AND_CORECLK_OFF)
		clk_prepare_enable(mdwc->ref_clk);
	usleep_range(1000, 1200);

	clk_prepare_enable(mdwc->iface_clk);
	if (mdwc->lpm_flags & MDWC3_PHY_REF_AND_CORECLK_OFF) {
		clk_prepare_enable(mdwc->core_clk);
		mdwc->lpm_flags &= ~MDWC3_PHY_REF_AND_CORECLK_OFF;
	}

	if (host_bus_suspend) {
		/* Disable HV interrupt */
		if (mdwc->otg_xceiv && (!mdwc->ext_xceiv.otg_capability))
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
							0x18000, 0x0);
		/* Clear interrupt latch register */
		dwc3_msm_write_reg(mdwc->base, HS_PHY_IRQ_STAT_REG, 0x000);

		/* Disable DP and DM HV interrupt */
		dwc3_msm_write_reg(mdwc->base, ALT_INTERRUPT_EN_REG, 0x000);
	} else {
		/* Disable HV interrupt */
		if (mdwc->otg_xceiv && (!mdwc->ext_xceiv.otg_capability))
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
								0x18000, 0x0);
		/* Disable Retention */
		dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG, 0x2, 0x2);

		dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
			dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0)) |
								 0xF0000000);
		/* 10usec delay required before de-asserting PHY RESET */
		udelay(10);
		dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
		      dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0)) &
								0x7FFFFFFF);

		/* Bring PHY out of suspend */
		dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG, 0xC00000,
									0x0);

	}

	if (resume_from_core_clk_off) {
		/* Assert SS PHY RESET */
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 7),
								(1 << 7));
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 28),
								(1 << 28));
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 8),
								(1 << 8));
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 26),
									0x0);
		/* 10usec delay required before de-asserting SS PHY RESET */
		udelay(10);
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 7),
									0x0);

		/*
		 * Reinitilize SSPHY parameters as SS_PHY RESET will reset
		 * the internal registers to default values.
		 */
		dwc3_msm_ss_phy_reg_init(mdwc);
	}
	atomic_set(&mdwc->in_lpm, 0);

	/* match disable_irq call from isr */
	if (mdwc->lpm_irq_seen && mdwc->hs_phy_irq) {
		enable_irq(mdwc->hs_phy_irq);
		mdwc->lpm_irq_seen = false;
	}
	/* Disable wakeup capable for HS_PHY IRQ, if enabled */
	if (mdwc->hs_phy_irq &&
			(mdwc->lpm_flags & MDWC3_ASYNC_IRQ_WAKE_CAPABILITY)) {
			disable_irq_wake(mdwc->hs_phy_irq);
			mdwc->lpm_flags &= ~MDWC3_ASYNC_IRQ_WAKE_CAPABILITY;
	}

	dev_info(mdwc->dev, "DWC3 exited from low power mode\n");

	return 0;
}

static void dwc3_wait_for_ext_chg_done(struct dwc3_msm *mdwc)
{
	unsigned long t;

	/*
	 * Defer next cable connect event till external charger
	 * detection is completed.
	 */

	if (mdwc->ext_chg_active && (mdwc->ext_xceiv.bsv ||
				!mdwc->ext_xceiv.id)) {

		dev_dbg(mdwc->dev, "before ext chg wait\n");

		t = wait_for_completion_timeout(&mdwc->ext_chg_wait,
				msecs_to_jiffies(3000));
		if (!t)
			dev_err(mdwc->dev, "ext chg wait timeout\n");
		else
			dev_dbg(mdwc->dev, "ext chg wait done\n");
	}

}

static void dwc3_resume_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
							resume_work.work);

	dev_dbg(mdwc->dev, "%s: dwc3 resume work\n", __func__);
	/* handle any event that was queued while work was already running */
	if (!atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: notifying xceiv event\n", __func__);
		if (mdwc->otg_xceiv) {
			dwc3_wait_for_ext_chg_done(mdwc);
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_XCEIV_STATE);
		}
		return;
	}

	/* bail out if system resume in process, else initiate RESUME */
	if (atomic_read(&mdwc->pm_suspended)) {
		mdwc->resume_pending = true;
	} else {
		pm_runtime_get_sync(mdwc->dev);
		if (mdwc->otg_xceiv)
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_PHY_RESUME);
		pm_runtime_put_noidle(mdwc->dev);
		if (mdwc->otg_xceiv && (mdwc->ext_xceiv.otg_capability)) {
			dwc3_wait_for_ext_chg_done(mdwc);
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_XCEIV_STATE);
		}
	}
}

static u32 debug_id = true, debug_bsv, debug_connect;

static int dwc3_connect_show(struct seq_file *s, void *unused)
{
	if (debug_connect)
		seq_printf(s, "true\n");
	else
		seq_printf(s, "false\n");

	return 0;
}

static int dwc3_connect_open(struct inode *inode, struct file *file)
{
	return single_open(file, dwc3_connect_show, inode->i_private);
}

static ssize_t dwc3_connect_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct dwc3_msm *mdwc = s->private;
	char buf[8];

	memset(buf, 0x00, sizeof(buf));

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count)))
		return -EFAULT;

	if (!strncmp(buf, "enable", 6) || !strncmp(buf, "true", 4)) {
		debug_connect = true;
	} else {
		debug_connect = debug_bsv = false;
		debug_id = true;
	}

	mdwc->ext_xceiv.bsv = debug_bsv;
	mdwc->ext_xceiv.id = debug_id ? DWC3_ID_FLOAT : DWC3_ID_GROUND;

	if (atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: calling resume_work\n", __func__);
		dwc3_resume_work(&mdwc->resume_work.work);
	} else {
		dev_dbg(mdwc->dev, "%s: notifying xceiv event\n", __func__);
		if (mdwc->otg_xceiv)
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_XCEIV_STATE);
	}

	return count;
}

const struct file_operations dwc3_connect_fops = {
	.open = dwc3_connect_open,
	.read = seq_read,
	.write = dwc3_connect_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct dentry *dwc3_debugfs_root;

static void dwc3_msm_debugfs_init(struct dwc3_msm *mdwc)
{
	dwc3_debugfs_root = debugfs_create_dir("msm_dwc3", NULL);

	if (!dwc3_debugfs_root || IS_ERR(dwc3_debugfs_root))
		return;

	if (!debugfs_create_bool("id", S_IRUGO | S_IWUSR, dwc3_debugfs_root,
				 &debug_id))
		goto error;

	if (!debugfs_create_bool("bsv", S_IRUGO | S_IWUSR, dwc3_debugfs_root,
				 &debug_bsv))
		goto error;

	if (!debugfs_create_file("connect", S_IRUGO | S_IWUSR,
				dwc3_debugfs_root, mdwc, &dwc3_connect_fops))
		goto error;

	return;

error:
	debugfs_remove_recursive(dwc3_debugfs_root);
}

static irqreturn_t msm_dwc3_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;

	if (atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s received in LPM\n", __func__);
		mdwc->lpm_irq_seen = true;
		disable_irq_nosync(irq);
		queue_delayed_work(system_nrt_wq, &mdwc->resume_work, 0);
	} else {
		pr_info_ratelimited("%s: IRQ outside LPM\n", __func__);
	}

	return IRQ_HANDLED;
}

static int
get_prop_usbin_voltage_now(struct dwc3_msm *mdwc)
{
	int rc = 0;
	struct qpnp_vadc_result results;

	if (IS_ERR_OR_NULL(mdwc->vadc_dev)) {
		mdwc->vadc_dev = qpnp_get_vadc(mdwc->dev, "usbin");
		if (IS_ERR(mdwc->vadc_dev))
			return PTR_ERR(mdwc->vadc_dev);
	}

	rc = qpnp_vadc_read(mdwc->vadc_dev, USBIN, &results);
	if (rc) {
		pr_err("Unable to read usbin rc=%d\n", rc);
		return 0;
	} else {
		return results.physical;
	}
}

static int dwc3_msm_power_get_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);
#ifdef CONFIG_PANTECH_PMIC_ABNORMAL
    int nonstandard_working_state = qpnp_chg_get_nonstandard_state() == NONSTANDARD_WORKING ? 1 : 0;
#endif /* CONFIG_PANTECH_PMIC_ABNORMAL */

	switch (psp) {
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = mdwc->host_mode;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = mdwc->voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
#ifdef CONFIG_PANTECH_PMIC_ABNORMAL
        if((nonstandard_working_state)
           && (mdwc->current_max <= 2 && mdwc->charger.chg_type == DWC3_SDP_CHARGER))
            val->intval = 500000;  /* 500 mA */
        else
		val->intval = mdwc->current_max;
#else
		val->intval = mdwc->current_max;
#endif /* CONFIG_PANTECH_PMIC_ABNORMAL */
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = mdwc->vbus_active;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
#ifdef CONFIG_PANTECH_QUALCOMM_OTG_MODE_OVP_BUG
        switch(mdwc->charger.chg_type) {
        case DWC3_SDP_CHARGER:
        case DWC3_PROPRIETARY_CHARGER:
            val->intval = 1;
            break;
        default:
            val->intval = 0;
            break;
        }
#else
		val->intval = mdwc->online;
#endif /* CONFIG_PANTECH_QUALCOMM_OTG_MODE_OVP_BUG */
		break;
	case POWER_SUPPLY_PROP_TYPE:
#ifdef CONFIG_PANTECH_PMIC_ABNORMAL
        if(nonstandard_working_state)
            val->intval = POWER_SUPPLY_TYPE_USB_DCP;
        else
            val->intval = psy->type;
#else
		val->intval = psy->type;
#endif /* CONFIG_PANTECH_PMIC_ABNORMAL */
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = get_prop_usbin_voltage_now(mdwc);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

#ifdef CONFIG_ANDROID_PANTECH_USB_MANAGER
static void dwc3_connect_work(struct work_struct *data)
{
	struct dwc3_msm *mdwc = container_of(data, struct dwc3_msm,
								connect_work.work);
	char *disconnect[2] = { "USB_CABLE=DISCONNECT", NULL };
	char *connect[2] = { "USB_CABLE=CONNECT", NULL };
	char **uevent_envp = NULL;
	
	if(mdwc->vbus_active)
		uevent_envp = connect;
	else
		uevent_envp = disconnect;
	if(uevent_envp) {
		kobject_uevent_env(&mdwc->dev->kobj, KOBJ_CHANGE, uevent_envp);
		printk(KERN_ERR "%s: send uevent %s\n", __func__, uevent_envp[0]);
	}
}		
#endif

static int dwc3_msm_power_set_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  const union power_supply_propval *val)
{
	static bool init;
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_SCOPE:
		mdwc->host_mode = val->intval;
		break;
	/* Process PMIC notification in PRESENT prop */
	case POWER_SUPPLY_PROP_PRESENT:
		dev_dbg(mdwc->dev, "%s: notify xceiv event\n", __func__);
		mdwc->vbus_active = val->intval;
		if (mdwc->otg_xceiv && !mdwc->ext_inuse && !mdwc->in_restart &&
		    (mdwc->ext_xceiv.otg_capability || !init)) {
			if (mdwc->ext_xceiv.bsv == val->intval)
				break;

			mdwc->ext_xceiv.bsv = val->intval;
			/*
			 * set debouncing delay to 120msec. Otherwise battery
			 * charging CDP complaince test fails if delay > 120ms.
			 */
			queue_delayed_work(system_nrt_wq,
							&mdwc->resume_work, 12);

			if (!init)
				init = true;
		}
#ifdef CONFIG_ANDROID_PANTECH_USB_MANAGER
		printk(KERN_ERR "%s:USB VBUS state from PMIC [%d]\n", __func__, val->intval);
		queue_delayed_work(system_nrt_wq, &mdwc->connect_work, 0);
#endif
#if defined(CONFIG_PANTECH_PMIC_CHARGER_BQ2419X)
		/* FIXME : In case of abnormal charger state, 
		 * We are fixed to match charger variable between power_supply_type and charger type.
		 * This code is only valid on EF63 series.
		 * LS4-USB tarial
		 */
		if((val->intval==0) && (psy->type==5))
			mdwc->charger.chg_type = DWC3_DCP_CHARGER;
#endif
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		mdwc->online = val->intval;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		mdwc->voltage_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		mdwc->current_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		psy->type = val->intval;
#if defined(CONFIG_PANTECH_PMIC_ABNORMAL)
		if((psy->type == 5) && (mdwc->charger.chg_type == DWC3_SDP_CHARGER)) //for External DCP
			mdwc->charger.chg_type = DWC3_DCP_CHARGER;
#endif
		break;
	default:
		return -EINVAL;
	}

	power_supply_changed(&mdwc->usb_psy);
	return 0;
}

static void dwc3_msm_external_power_changed(struct power_supply *psy)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm, usb_psy);
	union power_supply_propval ret = {0,};

	if (!mdwc->ext_vbus_psy)
		mdwc->ext_vbus_psy = power_supply_get_by_name("ext-vbus");

	if (!mdwc->ext_vbus_psy) {
		pr_err("%s: Unable to get ext_vbus power_supply\n", __func__);
		return;
	}

	mdwc->ext_vbus_psy->get_property(mdwc->ext_vbus_psy,
					POWER_SUPPLY_PROP_ONLINE, &ret);
	if (ret.intval) {
		dwc3_start_chg_det(&mdwc->charger, false);
		mdwc->ext_vbus_psy->get_property(mdwc->ext_vbus_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &ret);
		power_supply_set_current_limit(&mdwc->usb_psy, ret.intval);
	}

	power_supply_set_online(&mdwc->usb_psy, ret.intval);
	power_supply_changed(&mdwc->usb_psy);
}

static int
dwc3_msm_property_is_writeable(struct power_supply *psy,
				enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return 1;
	default:
		break;
	}

	return 0;
}


static char *dwc3_msm_pm_power_supplied_to[] = {
	"battery",
};

static enum power_supply_property dwc3_msm_pm_power_props_usb[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static void dwc3_init_adc_work(struct work_struct *w);

static void dwc3_ext_notify_online(void *ctx, int on)
{
	struct dwc3_msm *mdwc = ctx;
	bool notify_otg = false;

	if (!mdwc) {
		pr_err("%s: DWC3 driver already removed\n", __func__);
		return;
	}

	dev_dbg(mdwc->dev, "notify %s%s\n", on ? "" : "dis", "connected");

	if (!mdwc->ext_vbus_psy)
		mdwc->ext_vbus_psy = power_supply_get_by_name("ext-vbus");

	mdwc->ext_inuse = on;
	if (on) {
		/* force OTG to exit B-peripheral state */
		mdwc->ext_xceiv.bsv = false;
		notify_otg = true;
		dwc3_start_chg_det(&mdwc->charger, false);
	} else {
		/* external client offline; tell OTG about cached ID/BSV */
		if (mdwc->ext_xceiv.id != mdwc->id_state) {
			mdwc->ext_xceiv.id = mdwc->id_state;
			notify_otg = true;
		}

		mdwc->ext_xceiv.bsv = mdwc->vbus_active;
		notify_otg |= mdwc->vbus_active;
	}

	if (mdwc->ext_vbus_psy)
		power_supply_set_present(mdwc->ext_vbus_psy, on);

	if (notify_otg)
		queue_delayed_work(system_nrt_wq, &mdwc->resume_work, 0);
}

static void dwc3_id_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, id_work);
	int ret;

#ifdef CONFIG_PANTECH_USB_BLOCKING_MDMSTATE
    {
        int value = get_pantech_mdm_state();

        printk(KERN_ERR "[PUSB][%s]:mdm_state[%d], id_state[%d]\n",__func__, value,mdwc->id_state);
        if(value < 0){
            register_mdm_callback(MDM_REG_TYPE_RETRY);
            return;
        }else if(value == 0){
            register_mdm_callback(MDM_REG_TYPE_CONTROL);
        }else{
            if(mdwc->id_state == DWC3_ID_GROUND){
                if(mdwc->id_adc_detect){
                    qpnp_adc_tm_usbid_end(mdwc->adc_tm_dev);
                    mdwc->id_adc_detect = false;
                }
                mdwc->id_state = DWC3_ID_FLOAT;
                return;
            }
        }
    }
#endif

	/* Give external client a chance to handle */
	if (!mdwc->ext_inuse && usb_ext) {
		if (mdwc->pmic_id_irq)
			disable_irq(mdwc->pmic_id_irq);

		ret = usb_ext->notify(usb_ext->ctxt, mdwc->id_state,
				      dwc3_ext_notify_online, mdwc);
		dev_dbg(mdwc->dev, "%s: external handler returned %d\n",
			__func__, ret);

		if (mdwc->pmic_id_irq) {
			unsigned long flags;
			local_irq_save(flags);
			/* ID may have changed while IRQ disabled; update it */
			mdwc->id_state = !!irq_read_line(mdwc->pmic_id_irq);
			local_irq_restore(flags);
			enable_irq(mdwc->pmic_id_irq);
		}

		mdwc->ext_inuse = (ret == 0);
	}

	if (!mdwc->ext_inuse) { /* notify OTG */
#ifndef CONFIG_PANTECH_SIO_BUG_FIX //remove otg id detect state during operating peripheral mode
		mdwc->ext_xceiv.id = mdwc->id_state;
		dwc3_resume_work(&mdwc->resume_work.work);
#else
		if(mdwc->ext_xceiv.bsv && !mdwc->id_state) {
			printk(KERN_ERR "%s : tarial bsv already detect!!! cancel otg id notify.\n", __func__);
		} else {
			mdwc->ext_xceiv.id = mdwc->id_state;
			dwc3_resume_work(&mdwc->resume_work.work);		
		}
#endif
	}
}

#ifdef CONFIG_PANTECH_USB_BLOCKING_MDMSTATE
static void control_otg_mdm_state(bool mdm_enabled)
{
    struct dwc3_msm *mdwc = context;

    printk(KERN_ERR "[PUSB][%s]:mdm_reg_value=%d, mdm_enabled=%d, id_state[%d]\n", __func__, (int)mdm_reg_value, mdm_enabled, mdwc->id_state);
    switch(mdm_reg_value){
    case MDM_REG_TYPE_RETRY:
        {
            if(mdm_enabled){
                qpnp_adc_tm_usbid_end(mdwc->adc_tm_dev);
                mdwc->id_adc_detect = false;
                mdwc->id_state = DWC3_ID_FLOAT;
            }else{
	            dwc3_id_work(&mdwc->id_work);
            }
            mdm_reg_value = MDM_REG_TYPE_CONTROL;
            break;
        }
    case MDM_REG_TYPE_CONTROL:
        {
            if(mdm_enabled){
                if(mdwc->id_adc_detect){
                    qpnp_adc_tm_usbid_end(mdwc->adc_tm_dev);
                    mdwc->id_adc_detect = false;
                }
                if(mdwc->id_state != DWC3_ID_FLOAT){
                    mdwc->id_state = DWC3_ID_FLOAT;
                    dwc3_id_work(&mdwc->id_work); 
                }
            }else{
			    if(!mdwc->id_adc_detect){
                    //maybe regenerate interrupt.     
                    mdwc->id_state = DWC3_ID_FLOAT;
                    dwc3_init_adc_work(&mdwc->init_adc_work.work);
                }
            }
            break;
        }
    }
}

static void register_mdm_callback(enum mdm_reg_type value)
{
    printk(KERN_ERR "[PUSB][%s]:mdm_reg_value=%d\n", __func__,  (int)value);
    mdm_reg_value = value;
    set_pantech_mdm_callback((void*)control_otg_mdm_state);
}
#endif

static irqreturn_t dwc3_pmic_id_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;
	enum dwc3_id_state id;

	/* If we can't read ID line state for some reason, treat it as float */
	id = !!irq_read_line(irq);
	if (mdwc->id_state != id) {
		mdwc->id_state = id;
		queue_work(system_nrt_wq, &mdwc->id_work);
	}

	return IRQ_HANDLED;
}

static void dwc3_adc_notification(enum qpnp_tm_state state, void *ctx)
{
	struct dwc3_msm *mdwc = ctx;

	if (state >= ADC_TM_STATE_NUM) {
		pr_err("%s: invalid notification %d\n", __func__, state);
		return;
	}

	dev_dbg(mdwc->dev, "%s: state = %s\n", __func__,
			state == ADC_TM_HIGH_STATE ? "high" : "low");

	/* save ID state, but don't necessarily notify OTG */
	if (state == ADC_TM_HIGH_STATE) {
		mdwc->id_state = DWC3_ID_FLOAT;
		mdwc->adc_param.state_request = ADC_TM_LOW_THR_ENABLE;
	} else {
		mdwc->id_state = DWC3_ID_GROUND;
		mdwc->adc_param.state_request = ADC_TM_HIGH_THR_ENABLE;
	}

	dwc3_id_work(&mdwc->id_work);

	/* re-arm ADC interrupt */
	qpnp_adc_tm_usbid_configure(mdwc->adc_tm_dev, &mdwc->adc_param);
}

static void dwc3_init_adc_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
							init_adc_work.work);
	int ret;

	mdwc->adc_tm_dev = qpnp_get_adc_tm(mdwc->dev, "dwc_usb3-adc_tm");
	if (IS_ERR(mdwc->adc_tm_dev)) {
		if (PTR_ERR(mdwc->adc_tm_dev) == -EPROBE_DEFER)
			queue_delayed_work(system_nrt_wq, to_delayed_work(w),
					msecs_to_jiffies(100));
		else
			mdwc->adc_tm_dev = NULL;

		return;
	}

	mdwc->adc_param.low_thr = adc_low_threshold;
	mdwc->adc_param.high_thr = adc_high_threshold;
	mdwc->adc_param.timer_interval = adc_meas_interval;
	mdwc->adc_param.state_request = ADC_TM_HIGH_LOW_THR_ENABLE;
	mdwc->adc_param.btm_ctx = mdwc;
	mdwc->adc_param.threshold_notification = dwc3_adc_notification;

	ret = qpnp_adc_tm_usbid_configure(mdwc->adc_tm_dev, &mdwc->adc_param);
	if (ret) {
		dev_err(mdwc->dev, "%s: request ADC error %d\n", __func__, ret);
		return;
	}

	mdwc->id_adc_detect = true;
}

static ssize_t adc_enable_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	if (!mdwc)
		return -EINVAL;

	return snprintf(buf, PAGE_SIZE, "%s\n", mdwc->id_adc_detect ?
						"enabled" : "disabled");
}

static ssize_t adc_enable_store(struct device *dev,
		struct device_attribute *attr, const char
		*buf, size_t size)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	if (!mdwc)
		return -EINVAL;

	if (!strnicmp(buf, "enable", 6)) {
		if (!mdwc->id_adc_detect)
			dwc3_init_adc_work(&mdwc->init_adc_work.work);
		return size;
	} else if (!strnicmp(buf, "disable", 7)) {
		qpnp_adc_tm_usbid_end(mdwc->adc_tm_dev);
		mdwc->id_adc_detect = false;
		return size;
	}

	return -EINVAL;
}

static DEVICE_ATTR(adc_enable, S_IRUGO | S_IWUSR, adc_enable_show,
		adc_enable_store);

static int dwc3_msm_ext_chg_open(struct inode *inode, struct file *file)
{
	struct dwc3_msm *mdwc =
		container_of(inode->i_cdev, struct dwc3_msm, ext_chg_cdev);

	pr_debug("dwc3-msm ext chg open\n");
	file->private_data = mdwc;
	mdwc->ext_chg_opened = true;

	return 0;
}

static long
dwc3_msm_ext_chg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dwc3_msm *mdwc = file->private_data;
	struct msm_usb_chg_info info = {0};
	int ret = 0, val;

	switch (cmd) {
	case MSM_USB_EXT_CHG_INFO:
		info.chg_block_type = USB_CHG_BLOCK_QSCRATCH;
		info.page_offset = (mdwc->io_res->start +
				QSCRATCH_REG_OFFSET) & ~PAGE_MASK;
		/*
		 * The charger block register address space is only
		 * 512 bytes.  But mmap() works on PAGE granularity.
		 */
		info.length = PAGE_SIZE;

		if (copy_to_user((void __user *)arg, &info, sizeof(info))) {
			pr_err("%s: copy to user failed\n\n", __func__);
			ret = -EFAULT;
		}
		break;
	case MSM_USB_EXT_CHG_BLOCK_LPM:
		if (get_user(val, (int __user *)arg)) {
			pr_err("%s: get_user failed\n\n", __func__);
			ret = -EFAULT;
			break;
		}
		pr_debug("%s: LPM block request %d\n", __func__, val);
		if (val) { /* block LPM */
			if (mdwc->charger.chg_type == DWC3_DCP_CHARGER) {
				pm_runtime_get_sync(mdwc->dev);
			} else {
				mdwc->ext_chg_active = false;
				complete(&mdwc->ext_chg_wait);
				ret = -ENODEV;
			}
		} else {
			mdwc->ext_chg_active = false;
			complete(&mdwc->ext_chg_wait);
			pm_runtime_put(mdwc->dev);
		}
		break;
	case MSM_USB_EXT_CHG_VOLTAGE_INFO:
		if (get_user(val, (int __user *)arg)) {
			pr_err("%s: get_user failed\n\n", __func__);
			ret = -EFAULT;
			break;
		}

		if (val == USB_REQUEST_5V)
			pr_debug("%s:voting 5V voltage request\n", __func__);
		else if (val == USB_REQUEST_9V)
			pr_debug("%s:voting 9V voltage request\n", __func__);
		break;
	case MSM_USB_EXT_CHG_RESULT:
		if (get_user(val, (int __user *)arg)) {
			pr_err("%s: get_user failed\n\n", __func__);
			ret = -EFAULT;
			break;
		}

		if (!val)
			pr_debug("%s:voltage request successful\n", __func__);
		else
			pr_debug("%s:voltage request failed\n", __func__);
		break;
	case MSM_USB_EXT_CHG_TYPE:
		if (get_user(val, (int __user *)arg)) {
			pr_err("%s: get_user failed\n\n", __func__);
			ret = -EFAULT;
			break;
		}

		if (val)
			pr_debug("%s:charger is external charger\n", __func__);
		else
			pr_debug("%s:charger is not ext charger\n", __func__);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int dwc3_msm_ext_chg_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct dwc3_msm *mdwc = file->private_data;
	unsigned long vsize = vma->vm_end - vma->vm_start;
	int ret;

	if (vma->vm_pgoff != 0 || vsize > PAGE_SIZE)
		return -EINVAL;

	vma->vm_pgoff = __phys_to_pfn(mdwc->io_res->start +
				QSCRATCH_REG_OFFSET);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	ret = io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				 vsize, vma->vm_page_prot);
	if (ret < 0)
		pr_err("%s: failed with return val %d\n", __func__, ret);

	return ret;
}

static int dwc3_msm_ext_chg_release(struct inode *inode, struct file *file)
{
	struct dwc3_msm *mdwc = file->private_data;

	pr_debug("dwc3-msm ext chg release\n");

	mdwc->ext_chg_opened = false;

	return 0;
}

static const struct file_operations dwc3_msm_ext_chg_fops = {
	.owner = THIS_MODULE,
	.open = dwc3_msm_ext_chg_open,
	.unlocked_ioctl = dwc3_msm_ext_chg_ioctl,
	.mmap = dwc3_msm_ext_chg_mmap,
	.release = dwc3_msm_ext_chg_release,
};

static int dwc3_msm_setup_cdev(struct dwc3_msm *mdwc)
{
	int ret;

	ret = alloc_chrdev_region(&mdwc->ext_chg_dev, 0, 1, "usb_ext_chg");
	if (ret < 0) {
		pr_err("Fail to allocate usb ext char dev region\n");
		return ret;
	}
	mdwc->ext_chg_class = class_create(THIS_MODULE, "dwc_ext_chg");
	if (ret < 0) {
		pr_err("Fail to create usb ext chg class\n");
		goto unreg_chrdev;
	}
	cdev_init(&mdwc->ext_chg_cdev, &dwc3_msm_ext_chg_fops);
	mdwc->ext_chg_cdev.owner = THIS_MODULE;

	ret = cdev_add(&mdwc->ext_chg_cdev, mdwc->ext_chg_dev, 1);
	if (ret < 0) {
		pr_err("Fail to add usb ext chg cdev\n");
		goto destroy_class;
	}
	mdwc->ext_chg_device = device_create(mdwc->ext_chg_class,
					NULL, mdwc->ext_chg_dev, NULL,
					"usb_ext_chg");
	if (IS_ERR(mdwc->ext_chg_device)) {
		pr_err("Fail to create usb ext chg device\n");
		ret = PTR_ERR(mdwc->ext_chg_device);
		mdwc->ext_chg_device = NULL;
		goto del_cdev;
	}

	pr_debug("dwc3 msm ext chg cdev setup success\n");
	return 0;

del_cdev:
	cdev_del(&mdwc->ext_chg_cdev);
destroy_class:
	class_destroy(mdwc->ext_chg_class);
unreg_chrdev:
	unregister_chrdev_region(mdwc->ext_chg_dev, 1);

	return ret;
}

static int __devinit dwc3_msm_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node, *dwc3_node;
	struct dwc3_msm *mdwc;
	struct resource *res;
	void __iomem *tcsr;
	unsigned long flags;
	int ret = 0;
	int len = 0;
	u32 tmp[3];

	mdwc = devm_kzalloc(&pdev->dev, sizeof(*mdwc), GFP_KERNEL);
	if (!mdwc) {
		dev_err(&pdev->dev, "not enough memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, mdwc);
#ifdef CONFIG_PANTECH_USB_TUNE_SIGNALING_PARAM
	context = mdwc;
#endif
	mdwc->dev = &pdev->dev;

	INIT_LIST_HEAD(&mdwc->req_complete_list);
	INIT_DELAYED_WORK(&mdwc->chg_work, dwc3_chg_detect_work);
	INIT_DELAYED_WORK(&mdwc->resume_work, dwc3_resume_work);
	INIT_WORK(&mdwc->restart_usb_work, dwc3_restart_usb_work);
	INIT_WORK(&mdwc->id_work, dwc3_id_work);
	INIT_DELAYED_WORK(&mdwc->init_adc_work, dwc3_init_adc_work);
#ifdef CONFIG_ANDROID_PANTECH_USB_MANAGER
	INIT_DELAYED_WORK(&mdwc->connect_work, dwc3_connect_work);
#endif
	init_completion(&mdwc->ext_chg_wait);

	ret = dwc3_msm_config_gdsc(mdwc, 1);
	if (ret) {
		dev_err(&pdev->dev, "unable to configure usb3 gdsc\n");
		return ret;
	}

	mdwc->xo_clk = clk_get(&pdev->dev, "xo");
	if (IS_ERR(mdwc->xo_clk)) {
		dev_err(&pdev->dev, "%s unable to get TCXO buffer handle\n",
								__func__);
		ret = PTR_ERR(mdwc->xo_clk);
		goto disable_dwc3_gdsc;
	}

	ret = clk_prepare_enable(mdwc->xo_clk);
	if (ret) {
		dev_err(&pdev->dev, "%s failed to vote for TCXO buffer%d\n",
						__func__, ret);
		goto put_xo;
	}

	/*
	 * DWC3 Core requires its CORE CLK (aka master / bus clk) to
	 * run at 125Mhz in SSUSB mode and >60MHZ for HSUSB mode.
	 */
	mdwc->core_clk = devm_clk_get(&pdev->dev, "core_clk");
	if (IS_ERR(mdwc->core_clk)) {
		dev_err(&pdev->dev, "failed to get core_clk\n");
		ret = PTR_ERR(mdwc->core_clk);
		goto disable_xo;
	}
	clk_set_rate(mdwc->core_clk, 125000000);
	clk_prepare_enable(mdwc->core_clk);

	mdwc->iface_clk = devm_clk_get(&pdev->dev, "iface_clk");
	if (IS_ERR(mdwc->iface_clk)) {
		dev_err(&pdev->dev, "failed to get iface_clk\n");
		ret = PTR_ERR(mdwc->iface_clk);
		goto disable_core_clk;
	}
	clk_prepare_enable(mdwc->iface_clk);

	mdwc->sleep_clk = devm_clk_get(&pdev->dev, "sleep_clk");
	if (IS_ERR(mdwc->sleep_clk)) {
		dev_err(&pdev->dev, "failed to get sleep_clk\n");
		ret = PTR_ERR(mdwc->sleep_clk);
		goto disable_iface_clk;
	}
	clk_prepare_enable(mdwc->sleep_clk);

	mdwc->hsphy_sleep_clk = devm_clk_get(&pdev->dev, "sleep_a_clk");
	if (IS_ERR(mdwc->hsphy_sleep_clk)) {
		dev_err(&pdev->dev, "failed to get sleep_a_clk\n");
		ret = PTR_ERR(mdwc->hsphy_sleep_clk);
		goto disable_sleep_clk;
	}
	clk_prepare_enable(mdwc->hsphy_sleep_clk);

	ret = of_property_read_u32(node, "qcom,utmi-clk-rate",
				   (u32 *)&mdwc->utmi_clk_rate);
	if (ret)
		mdwc->utmi_clk_rate = 60000000;

	mdwc->utmi_clk = devm_clk_get(&pdev->dev, "utmi_clk");
	if (IS_ERR(mdwc->utmi_clk)) {
		dev_err(&pdev->dev, "failed to get utmi_clk\n");
		ret = PTR_ERR(mdwc->utmi_clk);
		goto disable_sleep_a_clk;
	}

	if (mdwc->utmi_clk_rate == 24000000) {
		/*
		 * For setting utmi clock to 24MHz, first set 48MHz on parent
		 * clock "utmi_clk_src" and then set divider 2 on child branch
		 * "utmi_clk".
		 */
		mdwc->utmi_clk_src = devm_clk_get(&pdev->dev, "utmi_clk_src");
		if (IS_ERR(mdwc->utmi_clk_src)) {
			dev_err(&pdev->dev, "failed to get utmi_clk_src\n");
			ret = PTR_ERR(mdwc->utmi_clk_src);
			goto disable_sleep_a_clk;
		}
		clk_set_rate(mdwc->utmi_clk_src, 48000000);
		/* 1 means divide utmi_clk_src by 2 */
		clk_set_rate(mdwc->utmi_clk, 1);
	} else {
		clk_set_rate(mdwc->utmi_clk, mdwc->utmi_clk_rate);
	}
	clk_prepare_enable(mdwc->utmi_clk);

	mdwc->ref_clk = devm_clk_get(&pdev->dev, "ref_clk");
	if (IS_ERR(mdwc->ref_clk)) {
		dev_err(&pdev->dev, "failed to get ref_clk\n");
		ret = PTR_ERR(mdwc->ref_clk);
		goto disable_utmi_clk;
	}
	clk_prepare_enable(mdwc->ref_clk);

	of_get_property(node, "qcom,vdd-voltage-level", &len);
	if (len == sizeof(tmp)) {
		of_property_read_u32_array(node, "qcom,vdd-voltage-level",
							tmp, len/sizeof(*tmp));
		mdwc->vdd_no_vol_level = tmp[0];
		mdwc->vdd_low_vol_level = tmp[1];
		mdwc->vdd_high_vol_level = tmp[2];
	} else {
		dev_err(&pdev->dev, "no qcom,vdd-voltage-level property\n");
		ret = -EINVAL;
		goto disable_ref_clk;
	}

	/* SS PHY */
	mdwc->ssusb_vddcx = devm_regulator_get(&pdev->dev, "ssusb_vdd_dig");
	if (IS_ERR(mdwc->ssusb_vddcx)) {
		dev_err(&pdev->dev, "unable to get ssusb vddcx\n");
		ret = PTR_ERR(mdwc->ssusb_vddcx);
		goto disable_ref_clk;
	}

	ret = dwc3_ssusb_config_vddcx(mdwc, 1);
	if (ret) {
		dev_err(&pdev->dev, "ssusb vddcx configuration failed\n");
		goto disable_ref_clk;
	}

	ret = regulator_enable(mdwc->ssusb_vddcx);
	if (ret) {
		dev_err(&pdev->dev, "unable to enable the ssusb vddcx\n");
		goto unconfig_ss_vddcx;
	}

	ret = dwc3_ssusb_ldo_init(mdwc, 1);
	if (ret) {
		dev_err(&pdev->dev, "ssusb vreg configuration failed\n");
		goto disable_ss_vddcx;
	}

	ret = dwc3_ssusb_ldo_enable(mdwc, 1);
	if (ret) {
		dev_err(&pdev->dev, "ssusb vreg enable failed\n");
		goto free_ss_ldo_init;
	}

	/* HS PHY */
	mdwc->hsusb_vddcx = devm_regulator_get(&pdev->dev, "hsusb_vdd_dig");
	if (IS_ERR(mdwc->hsusb_vddcx)) {
		dev_err(&pdev->dev, "unable to get hsusb vddcx\n");
		ret = PTR_ERR(mdwc->hsusb_vddcx);
		goto disable_ss_ldo;
	}

	ret = dwc3_hsusb_config_vddcx(mdwc, 1);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vddcx configuration failed\n");
		goto disable_ss_ldo;
	}

	ret = regulator_enable(mdwc->hsusb_vddcx);
	if (ret) {
		dev_err(&pdev->dev, "unable to enable the hsusb vddcx\n");
		goto unconfig_hs_vddcx;
	}

	ret = dwc3_hsusb_ldo_init(mdwc, 1);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vreg configuration failed\n");
		goto disable_hs_vddcx;
	}

	ret = dwc3_hsusb_ldo_enable(mdwc, 1);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vreg enable failed\n");
		goto free_hs_ldo_init;
	}

	mdwc->id_state = mdwc->ext_xceiv.id = DWC3_ID_FLOAT;
	mdwc->ext_xceiv.otg_capability = of_property_read_bool(node,
				"qcom,otg-capability");
	mdwc->charger.charging_disabled = of_property_read_bool(node,
				"qcom,charging-disabled");

	mdwc->charger.skip_chg_detect = of_property_read_bool(node,
				"qcom,skip-charger-detection");
	/*
	 * DWC3 has separate IRQ line for OTG events (ID/BSV) and for
	 * DP and DM linestate transitions during low power mode.
	 */
	mdwc->hs_phy_irq = platform_get_irq_byname(pdev, "hs_phy_irq");
	if (mdwc->hs_phy_irq < 0) {
		dev_dbg(&pdev->dev, "pget_irq for hs_phy_irq failed\n");
		mdwc->hs_phy_irq = 0;
	} else {
		ret = devm_request_irq(&pdev->dev, mdwc->hs_phy_irq,
				msm_dwc3_irq, IRQF_TRIGGER_RISING,
			       "msm_dwc3", mdwc);
		if (ret) {
			dev_err(&pdev->dev, "irqreq HSPHYINT failed\n");
			goto disable_hs_ldo;
		}
	}

	if (mdwc->ext_xceiv.otg_capability) {
		mdwc->pmic_id_irq =
			platform_get_irq_byname(pdev, "pmic_id_irq");
		if (mdwc->pmic_id_irq > 0) {
			/* check if PMIC ID IRQ is supported */
			ret = qpnp_misc_irqs_available(&pdev->dev);

			if (ret == -EPROBE_DEFER) {
				/* qpnp hasn't probed yet; defer dwc probe */
				goto disable_hs_ldo;
			} else if (ret == 0) {
				mdwc->pmic_id_irq = 0;
			} else {
#ifdef CONFIG_PANTECH_USB_EXTERNAL_ID_PULLUP
#ifdef CONFIG_PANTECH_PMIC_SHARED_DATA
#if defined(CONFIG_MACH_MSM8974_EF59S) || defined(CONFIG_MACH_MSM8974_EF59K) || defined(CONFIG_MACH_MSM8974_EF59L)
				if(get_oem_hw_rev() >= 4) // in case of EF59 series, usb external id set after TP20
#elif defined(CONFIG_MACH_MSM8974_EF60S) || defined(CONFIG_MACH_MSM8974_EF61K) || defined(CONFIG_MACH_MSM8974_EF62L)
				if(get_oem_hw_rev() >= 1) // in case of EF60 series, usb external id set after WS20
#elif defined(CONFIG_MACH_MSM8974_EF56S)
				if(0) // other model(ex, ef56s)
#else
				if(1) // other model(ex, ef63 series, ef65s)
#endif
#else
#if defined(CONFIG_MACH_MSM8974_EF63S) || defined(CONFIG_MACH_MSM8974_EF63K) || defined(CONFIG_MACH_MSM8974_EF63L) \
					|| defined(CONFIG_MACH_MSM8974_EF60S) || defined(CONFIG_MACH_MSM8974_EF65S) || defined(CONFIG_MACH_MSM8974_EF61K) \
					|| defined(CONFIG_MACH_MSM8974_EF62L) || defined(CONFIG_MACH_MSM8974_EF59S) \
					|| defined(CONFIG_MACH_MSM8974_EF59K) || defined(CONFIG_MACH_MSM8974_EF59L)
				if(1)
#else
				if(0) // other model(ex. ef56s) 
#endif
#endif /* CONFIG_PANTECH_PMIC_SHARED_DATA */
				{
				if (!qpnp_misc_usb_id_pull_up_set(&pdev->dev, 0))
					dev_err(&pdev->dev, "external usb_id set fail!\n");
				}
#endif /* CONFIG_PANTECH_USB_EXTERNAL_ID_PULLUP */

				ret = devm_request_irq(&pdev->dev,
						       mdwc->pmic_id_irq,
						       dwc3_pmic_id_irq,
						       IRQF_TRIGGER_RISING |
						       IRQF_TRIGGER_FALLING,
						       "dwc3_msm_pmic_id",
						       mdwc);
				if (ret) {
					dev_err(&pdev->dev, "irqreq IDINT failed\n");
					goto disable_hs_ldo;
				}

				local_irq_save(flags);
				/* Update initial ID state */
				mdwc->id_state =
					!!irq_read_line(mdwc->pmic_id_irq);
				printk(KERN_ERR "%s: Update initial ID state value = [%d]\n", __func__, mdwc->id_state); //LS4_USB
				if (mdwc->id_state == DWC3_ID_GROUND)
					queue_work(system_nrt_wq,
							&mdwc->id_work);
				local_irq_restore(flags);
				enable_irq_wake(mdwc->pmic_id_irq);
			}
		}

		if (mdwc->pmic_id_irq <= 0) {
			/* If no PMIC ID IRQ, use ADC for ID pin detection */
			queue_work(system_nrt_wq, &mdwc->init_adc_work.work);
			device_create_file(&pdev->dev, &dev_attr_adc_enable);
			mdwc->pmic_id_irq = 0;
		}
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_dbg(&pdev->dev, "missing TCSR memory resource\n");
	} else {
		tcsr = devm_ioremap_nocache(&pdev->dev, res->start,
			resource_size(res));
		if (!tcsr) {
			dev_dbg(&pdev->dev, "tcsr ioremap failed\n");
		} else {
			/* Enable USB3 on the primary USB port. */
			writel_relaxed(0x1, tcsr);
			/*
			 * Ensure that TCSR write is completed before
			 * USB registers initialization.
			 */
			mb();
		}
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing memory base resource\n");
		ret = -ENODEV;
		goto disable_hs_ldo;
	}

	mdwc->base = devm_ioremap_nocache(&pdev->dev, res->start,
		resource_size(res));
	if (!mdwc->base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENODEV;
		goto disable_hs_ldo;
	}

	mdwc->io_res = res; /* used to calculate chg block offset */

	if (of_property_read_u32(node, "qcom,dwc-hsphy-init",
						&mdwc->hsphy_init_seq))
		dev_dbg(&pdev->dev, "unable to read hsphy init seq\n");
	else if (!mdwc->hsphy_init_seq)
		dev_warn(&pdev->dev, "incorrect hsphyinitseq.Using PORvalue\n");

#ifdef CONFIG_PANTECH_USB_TUNE_SIGNALING_PARAM
	if (of_property_read_u32(node, "qcom,dwc-ssphy-init",
						&pantech_ssphy_init))
  {
    pantech_ssphy_init = 0x07F01605;//qualcomm default
  }
  
	if (of_property_read_u32(node, "qcom,dwc-ssphy-rx-equal-data",
						&pantech_rx_equal_data))
  {

    pantech_rx_equal_data = 0x3;//qualcomm default
  }
#endif

	if (of_property_read_u32(node, "qcom,dwc-ssphy-deemphasis-value",
						&mdwc->deemphasis_val))
		dev_dbg(&pdev->dev, "unable to read ssphy deemphasis value\n");

	pm_runtime_set_active(mdwc->dev);
	pm_runtime_enable(mdwc->dev);

	if (of_property_read_u32(node, "qcom,dwc-usb3-msm-dbm-eps",
				 &mdwc->dbm_num_eps)) {
		dev_err(&pdev->dev,
			"unable to read platform data num of dbm eps\n");
		mdwc->dbm_num_eps = DBM_MAX_EPS;
	}

	if (mdwc->dbm_num_eps > DBM_MAX_EPS) {
		dev_err(&pdev->dev,
			"Driver doesn't support number of DBM EPs. "
			"max: %d, dbm_num_eps: %d\n",
			DBM_MAX_EPS, mdwc->dbm_num_eps);
		ret = -ENODEV;
		goto disable_hs_ldo;
	}

	if (of_property_read_u32(node, "qcom,dwc-usb3-msm-tx-fifo-size",
				 &mdwc->tx_fifo_size))
		dev_err(&pdev->dev,
			"unable to read platform data tx fifo size\n");

	if (of_property_read_u32(node, "qcom,dwc-usb3-msm-qdss-tx-fifo-size",
				 &mdwc->qdss_tx_fifo_size))
		dev_err(&pdev->dev,
			"unable to read platform data qdss tx fifo size\n");

	dwc3_set_notifier(&dwc3_msm_notify_event);

	/* Assumes dwc3 is the only DT child of dwc3-msm */
	dwc3_node = of_get_next_available_child(node, NULL);
	if (!dwc3_node) {
		dev_err(&pdev->dev, "failed to find dwc3 child\n");
		goto disable_hs_ldo;
	}

	/* usb_psy required only for vbus_notifications or charging support */
	if (mdwc->ext_xceiv.otg_capability ||
			!mdwc->charger.charging_disabled) {
		mdwc->usb_psy.name = "usb";
		mdwc->usb_psy.type = POWER_SUPPLY_TYPE_USB;
		mdwc->usb_psy.supplied_to = dwc3_msm_pm_power_supplied_to;
		mdwc->usb_psy.num_supplicants = ARRAY_SIZE(
						dwc3_msm_pm_power_supplied_to);
		mdwc->usb_psy.properties = dwc3_msm_pm_power_props_usb;
		mdwc->usb_psy.num_properties =
					ARRAY_SIZE(dwc3_msm_pm_power_props_usb);
		mdwc->usb_psy.get_property = dwc3_msm_power_get_property_usb;
		mdwc->usb_psy.set_property = dwc3_msm_power_set_property_usb;
		mdwc->usb_psy.external_power_changed =
					dwc3_msm_external_power_changed;
		mdwc->usb_psy.property_is_writeable =
				dwc3_msm_property_is_writeable;

		ret = power_supply_register(&pdev->dev, &mdwc->usb_psy);
		if (ret < 0) {
			dev_err(&pdev->dev,
					"%s:power_supply_register usb failed\n",
						__func__);
			of_node_put(dwc3_node);
			goto disable_hs_ldo;
		}
	}

	ret = of_platform_populate(node, NULL, NULL, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to add create dwc3 core\n");
		of_node_put(dwc3_node);
		goto put_psupply;
	}

	mdwc->dwc3 = of_find_device_by_node(dwc3_node);
	of_node_put(dwc3_node);
	if (!mdwc->dwc3) {
		dev_err(&pdev->dev, "failed to get dwc3 platform device\n");
		goto put_psupply;
	}

	mdwc->bus_scale_table = msm_bus_cl_get_pdata(pdev);
	if (!mdwc->bus_scale_table) {
		dev_err(&pdev->dev, "bus scaling is disabled\n");
	} else {
		mdwc->bus_perf_client =
			msm_bus_scale_register_client(mdwc->bus_scale_table);
		ret = msm_bus_scale_client_update_request(
						mdwc->bus_perf_client, 1);
		if (ret)
			dev_err(&pdev->dev, "Failed to vote for bus scaling\n");
	}

	mdwc->otg_xceiv = usb_get_transceiver();
	/* Register with OTG if present, ignore USB2 OTG using other PHY */
	if (mdwc->otg_xceiv &&
			!(mdwc->otg_xceiv->flags & ENABLE_SECONDARY_PHY)) {
		/* Skip charger detection for simulator targets */
		if (!mdwc->charger.skip_chg_detect) {
			mdwc->charger.start_detection = dwc3_start_chg_det;
			ret = dwc3_set_charger(mdwc->otg_xceiv->otg,
					&mdwc->charger);
			if (ret || !mdwc->charger.notify_detection_complete) {
				dev_err(&pdev->dev,
					"failed to register charger: %d\n",
					ret);
				goto put_xcvr;
			}
		}

		if (mdwc->ext_xceiv.otg_capability)
			mdwc->ext_xceiv.ext_block_reset = dwc3_msm_block_reset;
		ret = dwc3_set_ext_xceiv(mdwc->otg_xceiv->otg,
						&mdwc->ext_xceiv);
		if (ret || !mdwc->ext_xceiv.notify_ext_events) {
			dev_err(&pdev->dev, "failed to register xceiver: %d\n",
									ret);
			goto put_xcvr;
		}
	} else {
		dev_dbg(&pdev->dev, "No OTG, DWC3 running in host only mode\n");
		mdwc->host_mode = 1;
		mdwc->vbus_otg = devm_regulator_get(&pdev->dev, "vbus_dwc3");
		if (IS_ERR(mdwc->vbus_otg)) {
			dev_dbg(&pdev->dev, "Failed to get vbus regulator\n");
			mdwc->vbus_otg = 0;
		} else {
			ret = regulator_enable(mdwc->vbus_otg);
			if (ret) {
				mdwc->vbus_otg = 0;
				dev_err(&pdev->dev, "Failed to enable vbus_otg\n");
			}
		}
		mdwc->otg_xceiv = NULL;
	}
	if (mdwc->ext_xceiv.otg_capability && mdwc->charger.start_detection) {
		ret = dwc3_msm_setup_cdev(mdwc);
		if (ret)
			dev_err(&pdev->dev, "Fail to setup dwc3 setup cdev\n");
	}

	device_init_wakeup(mdwc->dev, 1);
	pm_stay_awake(mdwc->dev);
	dwc3_msm_debugfs_init(mdwc);

#ifdef CONFIG_PANTECH_USB_TUNE_SIGNALING_PARAM
	//LS2_USB tarial ssphy tune
	ret = sysfs_create_group(&context->dev->kobj, &dwc3_pantech_phy_control_attr_grp);
#endif
#ifdef CONFIG_PANTECH_USB_DEBUG
	dwc3_logmask_value = 0;
#endif

#ifdef CONFIG_PANTECH_USB_REDRIVER_EN_CONTROL
	gpio_request(REDRIVER_EN, "redriver_en");
#endif

	return 0;

put_xcvr:
	platform_device_put(mdwc->dwc3);
	usb_put_transceiver(mdwc->otg_xceiv);
put_psupply:
	if (mdwc->usb_psy.dev)
		power_supply_unregister(&mdwc->usb_psy);
disable_hs_ldo:
	dwc3_hsusb_ldo_enable(mdwc, 0);
free_hs_ldo_init:
	dwc3_hsusb_ldo_init(mdwc, 0);
disable_hs_vddcx:
	regulator_disable(mdwc->hsusb_vddcx);
unconfig_hs_vddcx:
	dwc3_hsusb_config_vddcx(mdwc, 0);
disable_ss_ldo:
	dwc3_ssusb_ldo_enable(mdwc, 0);
free_ss_ldo_init:
	dwc3_ssusb_ldo_init(mdwc, 0);
disable_ss_vddcx:
	regulator_disable(mdwc->ssusb_vddcx);
unconfig_ss_vddcx:
	dwc3_ssusb_config_vddcx(mdwc, 0);
disable_ref_clk:
	clk_disable_unprepare(mdwc->ref_clk);
disable_utmi_clk:
	clk_disable_unprepare(mdwc->utmi_clk);
disable_sleep_a_clk:
	clk_disable_unprepare(mdwc->hsphy_sleep_clk);
disable_sleep_clk:
	clk_disable_unprepare(mdwc->sleep_clk);
disable_iface_clk:
	clk_disable_unprepare(mdwc->iface_clk);
disable_core_clk:
	clk_disable_unprepare(mdwc->core_clk);
disable_xo:
	clk_disable_unprepare(mdwc->xo_clk);
put_xo:
	clk_put(mdwc->xo_clk);
disable_dwc3_gdsc:
	dwc3_msm_config_gdsc(mdwc, 0);

	return ret;
}

static int __devexit dwc3_msm_remove(struct platform_device *pdev)
{
	struct dwc3_msm	*mdwc = platform_get_drvdata(pdev);

	if (!mdwc->ext_chg_device) {
		device_destroy(mdwc->ext_chg_class, mdwc->ext_chg_dev);
		cdev_del(&mdwc->ext_chg_cdev);
		class_destroy(mdwc->ext_chg_class);
		unregister_chrdev_region(mdwc->ext_chg_dev, 1);
	}

	if (mdwc->id_adc_detect)
		qpnp_adc_tm_usbid_end(mdwc->adc_tm_dev);
	if (dwc3_debugfs_root)
		debugfs_remove_recursive(dwc3_debugfs_root);
	if (mdwc->otg_xceiv) {
		dwc3_start_chg_det(&mdwc->charger, false);
		usb_put_transceiver(mdwc->otg_xceiv);
	}
	if (mdwc->usb_psy.dev)
		power_supply_unregister(&mdwc->usb_psy);
	if (mdwc->vbus_otg)
		regulator_disable(mdwc->vbus_otg);
	platform_device_put(mdwc->dwc3);

	pm_runtime_disable(mdwc->dev);
	device_init_wakeup(mdwc->dev, 0);

	dwc3_hsusb_ldo_enable(mdwc, 0);
	dwc3_hsusb_ldo_init(mdwc, 0);
	regulator_disable(mdwc->hsusb_vddcx);
	dwc3_hsusb_config_vddcx(mdwc, 0);
	dwc3_ssusb_ldo_enable(mdwc, 0);
	dwc3_ssusb_ldo_init(mdwc, 0);
	regulator_disable(mdwc->ssusb_vddcx);
	dwc3_ssusb_config_vddcx(mdwc, 0);
	clk_disable_unprepare(mdwc->core_clk);
	clk_disable_unprepare(mdwc->iface_clk);
	clk_disable_unprepare(mdwc->sleep_clk);
	clk_disable_unprepare(mdwc->hsphy_sleep_clk);
	clk_disable_unprepare(mdwc->ref_clk);
	clk_disable_unprepare(mdwc->xo_clk);
	clk_put(mdwc->xo_clk);

	dwc3_msm_config_gdsc(mdwc, 0);

	return 0;
}

static int dwc3_msm_pm_suspend(struct device *dev)
{
	int ret = 0;
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "dwc3-msm PM suspend\n");

	flush_delayed_work_sync(&mdwc->resume_work);
	if (!atomic_read(&mdwc->in_lpm)) {
		dev_err(mdwc->dev, "Abort PM suspend!! (USB is outside LPM)\n");
		return -EBUSY;
	}

	ret = dwc3_msm_suspend(mdwc);
	if (!ret)
		atomic_set(&mdwc->pm_suspended, 1);

	return ret;
}

static int dwc3_msm_pm_resume(struct device *dev)
{
	int ret = 0;
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "dwc3-msm PM resume\n");

	atomic_set(&mdwc->pm_suspended, 0);
	if (mdwc->resume_pending) {
		mdwc->resume_pending = false;

		ret = dwc3_msm_resume(mdwc);
		/* Update runtime PM status */
		pm_runtime_disable(dev);
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);

		/* Let OTG know about resume event and update pm_count */
		if (mdwc->otg_xceiv) {
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_PHY_RESUME);
			if (mdwc->ext_xceiv.otg_capability)
				mdwc->ext_xceiv.notify_ext_events(
							mdwc->otg_xceiv->otg,
							DWC3_EVENT_XCEIV_STATE);
		}
	}

	return ret;
}

static int dwc3_msm_runtime_idle(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "DWC3-msm runtime idle\n");

	if (mdwc->ext_chg_active) {
		dev_dbg(dev, "Deferring LPM\n");
		/*
		 * Charger detection may happen in user space.
		 * Delay entering LPM by 3 sec.  Otherwise we
		 * have to exit LPM when user space begins
		 * charger detection.
		 *
		 * This timer will be canceled when user space
		 * votes against LPM by incrementing PM usage
		 * counter.  We enter low power mode when
		 * PM usage counter is decremented.
		 */
		pm_schedule_suspend(dev, 3000);
		return -EAGAIN;
	}

	return 0;
}

static int dwc3_msm_runtime_suspend(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "DWC3-msm runtime suspend\n");

	return dwc3_msm_suspend(mdwc);
}

static int dwc3_msm_runtime_resume(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "DWC3-msm runtime resume\n");

	return dwc3_msm_resume(mdwc);
}

static const struct dev_pm_ops dwc3_msm_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_msm_pm_suspend, dwc3_msm_pm_resume)
	SET_RUNTIME_PM_OPS(dwc3_msm_runtime_suspend, dwc3_msm_runtime_resume,
				dwc3_msm_runtime_idle)
};

static const struct of_device_id of_dwc3_matach[] = {
	{
		.compatible = "qcom,dwc-usb3-msm",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_dwc3_matach);

static struct platform_driver dwc3_msm_driver = {
	.probe		= dwc3_msm_probe,
	.remove		= __devexit_p(dwc3_msm_remove),
	.driver		= {
		.name	= "msm-dwc3",
		.pm	= &dwc3_msm_dev_pm_ops,
		.of_match_table	= of_dwc3_matach,
	},
};

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 MSM Glue Layer");

static int __devinit dwc3_msm_init(void)
{
	return platform_driver_register(&dwc3_msm_driver);
}
module_init(dwc3_msm_init);

static void __exit dwc3_msm_exit(void)
{
	platform_driver_unregister(&dwc3_msm_driver);
}
module_exit(dwc3_msm_exit);

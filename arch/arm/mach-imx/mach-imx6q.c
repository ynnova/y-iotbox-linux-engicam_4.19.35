/*
 * Copyright 2011-2015 Freescale Semiconductor, Inc.
 * Copyright 2011 Linaro Ltd.
 * Copyright 2017 NXP.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/pci.h>
#include <linux/phy.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/micrel_phy.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>
#include <linux/of_net.h>
#include <linux/of_gpio.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/system_misc.h>

#include "common.h"
#include "cpuidle.h"
#include "hardware.h"

static int edimm_ver = 10;

/* For imx6q sabrelite board: set KSZ9021RN RGMII pad skew */
static int ksz9021rn_phy_fixup(struct phy_device *phydev)
{
	if (IS_BUILTIN(CONFIG_PHYLIB)) {
		/* min rx data delay */
		phy_write(phydev, MICREL_KSZ9021_EXTREG_CTRL,
			0x8000 | MICREL_KSZ9021_RGMII_RX_DATA_PAD_SCEW);
		phy_write(phydev, MICREL_KSZ9021_EXTREG_DATA_WRITE, 0x0000);

		/* max rx/tx clock delay, min rx/tx control delay */
		phy_write(phydev, MICREL_KSZ9021_EXTREG_CTRL,
			0x8000 | MICREL_KSZ9021_RGMII_CLK_CTRL_PAD_SCEW);
		phy_write(phydev, MICREL_KSZ9021_EXTREG_DATA_WRITE, 0xf0f0);
		phy_write(phydev, MICREL_KSZ9021_EXTREG_CTRL,
			MICREL_KSZ9021_RGMII_CLK_CTRL_PAD_SCEW);
	}

	return 0;
}

static void mmd_write_reg(struct phy_device *dev, int device, int reg, int val)
{
	phy_write(dev, 0x0d, device);
	phy_write(dev, 0x0e, reg);
	phy_write(dev, 0x0d, (1 << 14) | device);
	phy_write(dev, 0x0e, val);
}

static int ksz9031rn_phy_fixup(struct phy_device *dev)
{
	printk("Init ksz9031rn PHY\n");

	//write register 6 addr 2 TXD[0:3] skew
	mmd_write_reg(dev, 2, 6, 0x4111);

	//write register 5 addr 2 RXD[0:3] skew
	mmd_write_reg(dev, 2, 5, 0x47a7);

	//write register 4 addr 2 RX_DV TX_EN skew
	mmd_write_reg(dev, 2, 4, 0x004A);

	//write register 8 addr 2 RX_CLK GTX_CLK skew
	mmd_write_reg(dev, 2, 8, 0x0273);

	return 0;
}

/*
 * fixup for PLX PEX8909 bridge to configure GPIO1-7 as output High
 * as they are used for slots1-7 PERST#
 */
static void ventana_pciesw_early_fixup(struct pci_dev *dev)
{
	u32 dw;

	if (!of_machine_is_compatible("gw,ventana"))
		return;

	if (dev->devfn != 0)
		return;

	pci_read_config_dword(dev, 0x62c, &dw);
	dw |= 0xaaa8; // GPIO1-7 outputs
	pci_write_config_dword(dev, 0x62c, dw);

	pci_read_config_dword(dev, 0x644, &dw);
	dw |= 0xfe;   // GPIO1-7 output high
	pci_write_config_dword(dev, 0x644, dw);

	msleep(100);
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_PLX, 0x8609, ventana_pciesw_early_fixup);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_PLX, 0x8606, ventana_pciesw_early_fixup);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_PLX, 0x8604, ventana_pciesw_early_fixup);

static int ar8031_phy_fixup(struct phy_device *dev)
{
	u16 val;

	/* Set RGMII IO voltage to 1.8V */
	phy_write(dev, 0x1d, 0x1f);
	phy_write(dev, 0x1e, 0x8);

	/* disable phy AR8031 SmartEEE function. */
	phy_write(dev, 0xd, 0x3);
	phy_write(dev, 0xe, 0x805d);
	phy_write(dev, 0xd, 0x4003);
	val = phy_read(dev, 0xe);
	val &= ~(0x1 << 8);
	phy_write(dev, 0xe, val);

	/* To enable AR8031 output a 125MHz clk from CLK_25M */
	phy_write(dev, 0xd, 0x7);
	phy_write(dev, 0xe, 0x8016);
	phy_write(dev, 0xd, 0x4007);

	val = phy_read(dev, 0xe);
	val &= 0xffe3;
	val |= 0x18;
	phy_write(dev, 0xe, val);

	/* introduce tx clock delay */
	phy_write(dev, 0x1d, 0x5);
	val = phy_read(dev, 0x1e);
	val |= 0x0100;
	phy_write(dev, 0x1e, val);

	return 0;
}

#define PHY_ID_AR8031	0x004dd074

static int ar8035_phy_fixup(struct phy_device *dev)
{
	u16 val;

	/* Ar803x phy SmartEEE feature cause link status generates glitch,
	 * which cause ethernet link down/up issue, so disable SmartEEE
	 */
	phy_write(dev, 0xd, 0x3);
	phy_write(dev, 0xe, 0x805d);
	phy_write(dev, 0xd, 0x4003);

	val = phy_read(dev, 0xe);
	phy_write(dev, 0xe, val & ~(1 << 8));

	/*
	 * Enable 125MHz clock from CLK_25M on the AR8031.  This
	 * is fed in to the IMX6 on the ENET_REF_CLK (V22) pad.
	 * Also, introduce a tx clock delay.
	 *
	 * This is the same as is the AR8031 fixup.
	 */
	ar8031_phy_fixup(dev);

	/*check phy power*/
	val = phy_read(dev, 0x0);
	if (val & BMCR_PDOWN)
		phy_write(dev, 0x0, val & ~BMCR_PDOWN);

	return 0;
}

#define PHY_ID_AR8035 0x004dd072

static void __init imx6q_enet_phy_init(void)
{
	if (IS_BUILTIN(CONFIG_PHYLIB)) {
		phy_register_fixup_for_uid(PHY_ID_KSZ9021, MICREL_PHY_ID_MASK,
				ksz9021rn_phy_fixup);
		phy_register_fixup_for_uid(PHY_ID_KSZ9031, MICREL_PHY_ID_MASK,
				ksz9031rn_phy_fixup);
		phy_register_fixup_for_uid(PHY_ID_AR8031, 0xffffffef,
				ar8031_phy_fixup);
		phy_register_fixup_for_uid(PHY_ID_AR8035, 0xffffffef,
				ar8035_phy_fixup);
	}
}

#define ICORE_GPIO_EDIMM_VER 191 /* GPIO6_31 */

static void __init icore_set_enet_clock(void)
{
	int icore_ver_gpio;
	struct regmap *gpr;
	
	gpr = syscon_regmap_lookup_by_compatible("fsl,imx6q-iomuxc-gpr");
	
	icore_ver_gpio = ICORE_GPIO_EDIMM_VER;
	
	if (gpio_is_valid(icore_ver_gpio) &&
	    !gpio_request_one(icore_ver_gpio, GPIOF_DIR_IN, "icore_ver_gpio"))
	{
		gpio_direction_input(icore_ver_gpio);
	
		if(!gpio_get_value(icore_ver_gpio))
		{
			edimm_ver = 15;
			printk("i.Core M6 1.5 version\n");
			regmap_update_bits(gpr, IOMUXC_GPR1,
					IMX6Q_GPR1_ENET_CLK_SEL_MASK,
					IMX6Q_GPR1_ENET_CLK_SEL_ANATOP);
		}		
		else
		{
			edimm_ver = 10;
			printk("i.Core M6 standard 1.0 version\n");
			regmap_update_bits(gpr, IOMUXC_GPR1,
					IMX6Q_GPR1_ENET_CLK_SEL_MASK,
					IMX6Q_GPR1_ENET_CLK_SEL_PAD);
		}
		gpio_free(icore_ver_gpio);
	}
}


static void __init imx6q_csi_mux_init(void)
{
	/*
	 * MX6Q SabreSD board:
	 * IPU1 CSI0 connects to parallel interface.
	 * Set GPR1 bit 19 to 0x1.
	 *
	 * MX6DL SabreSD board:
	 * IPU1 CSI0 connects to parallel interface.
	 * Set GPR13 bit 0-2 to 0x4.
	 * IPU1 CSI1 connects to MIPI CSI2 virtual channel 1.
	 * Set GPR13 bit 3-5 to 0x1.
	 */
	struct regmap *gpr;

	gpr = syscon_regmap_lookup_by_compatible("fsl,imx6q-iomuxc-gpr");
	if (!IS_ERR(gpr)) {

		/*
		 * MX6Q i.Core board:
		 * IPU1 CSI0 connects to parallel interface.
		 * IPU2 CSI1 connects to parallel interface.
		 * Set GPR1 bit 19 and 20 to 0x1.
		 *
		 * MX6DL SabreSD board:
		 * IPU1 CSI0 connects to parallel interface.
		 * Set GPR13 bit 0-2 to 0x4.
		 * IPU1 CSI1 connects to parallel interface.
		 * Set GPR13 bit 3-5 to 0x4.
		 */
		if (of_machine_is_compatible("fsl,imx6-icore"))
		{
			if(cpu_is_imx6q())
				regmap_update_bits(gpr, IOMUXC_GPR1, 3 << 19, 3 << 19);
			else
				regmap_update_bits(gpr, IOMUXC_GPR13, 0x3F, 0x24);
		}

		if (of_machine_is_compatible("fsl,imx6q-sabresd") ||
			of_machine_is_compatible("fsl,imx6q-sabreauto") ||
			of_machine_is_compatible("fsl,imx6qp-sabresd") ||
			of_machine_is_compatible("fsl,imx6qp-sabreauto"))
			regmap_update_bits(gpr, IOMUXC_GPR1, 1 << 19, 1 << 19);
		else if (of_machine_is_compatible("fsl,imx6dl-sabresd") ||
			 of_machine_is_compatible("fsl,imx6dl-sabreauto"))
			regmap_update_bits(gpr, IOMUXC_GPR13, 0x3F, 0x0C);
	} else {
		pr_err("%s(): failed to find fsl,imx6q-iomux-gpr regmap\n",
		       __func__);
	}
}

static void __init imx6q_axi_init(void)
{
	struct regmap *gpr;
	unsigned int mask;

	gpr = syscon_regmap_lookup_by_compatible("fsl,imx6q-iomuxc-gpr");
	if (!IS_ERR(gpr)) {
		/*
		 * Enable the cacheable attribute of VPU and IPU
		 * AXI transactions.
		 */
		mask = IMX6Q_GPR4_VPU_WR_CACHE_SEL |
			IMX6Q_GPR4_VPU_RD_CACHE_SEL |
			IMX6Q_GPR4_VPU_P_WR_CACHE_VAL |
			IMX6Q_GPR4_VPU_P_RD_CACHE_VAL_MASK |
			IMX6Q_GPR4_IPU_WR_CACHE_CTL |
			IMX6Q_GPR4_IPU_RD_CACHE_CTL;
		regmap_update_bits(gpr, IOMUXC_GPR4, mask, mask);

		/* Increase IPU read QoS priority */
		regmap_update_bits(gpr, IOMUXC_GPR6,
				IMX6Q_GPR6_IPU1_ID00_RD_QOS_MASK |
				IMX6Q_GPR6_IPU1_ID01_RD_QOS_MASK,
				(0xf << 16) | (0x7 << 20));
		regmap_update_bits(gpr, IOMUXC_GPR7,
				IMX6Q_GPR7_IPU2_ID00_RD_QOS_MASK |
				IMX6Q_GPR7_IPU2_ID01_RD_QOS_MASK,
				(0xf << 16) | (0x7 << 20));
	} else {
		pr_warn("failed to find fsl,imx6q-iomuxc-gpr regmap\n");
	}
}

static void __init imx6q_enet_clk_sel(void)
{
	struct regmap *gpr;

	gpr = syscon_regmap_lookup_by_compatible("fsl,imx6q-iomuxc-gpr");
	if (!IS_ERR(gpr))
		regmap_update_bits(gpr, IOMUXC_GPR5,
				   IMX6Q_GPR5_ENET_TX_CLK_SEL, IMX6Q_GPR5_ENET_TX_CLK_SEL);
	else
		pr_err("failed to find fsl,imx6q-iomux-gpr regmap\n");
}

static inline void imx6q_enet_init(void)
{
	imx6_enet_mac_init("fsl,imx6q-fec", "fsl,imx6q-ocotp");
	imx6q_enet_phy_init();

	if (cpu_is_imx6q() && imx_get_soc_revision() >= IMX_CHIP_REVISION_2_0)
		imx6q_enet_clk_sel();
}

static void __init imx6q_init_machine(void)
{
	struct device *parent;

	if (cpu_is_imx6q() && imx_get_soc_revision() >= IMX_CHIP_REVISION_2_0)
		imx_print_silicon_rev("i.MX6QP", IMX_CHIP_REVISION_1_0);
	else
		imx_print_silicon_rev(cpu_is_imx6dl() ? "i.MX6DL" : "i.MX6Q",
				imx_get_soc_revision());

	parent = imx_soc_device_init();
	if (parent == NULL)
		pr_warn("failed to initialize soc device\n");

	of_platform_default_populate(NULL, NULL, parent);

	imx6q_enet_init();
	imx_anatop_init();
	imx6q_csi_mux_init();
	cpu_is_imx6q() ?  imx6q_pm_init() : imx6dl_pm_init();
	imx6q_axi_init();
}

static void __init icore_late_init(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct pinctrl *pctl;
	int icore_ver_gpio;

	icore_set_enet_clock();

	if(edimm_ver == 15) /* EDIMM 1.5 */
		return;

	np = of_find_node_by_path("/soc/aips-bus@02100000/usb@02184000");
//	np = of_find_compatible_node(NULL, NULL, "fsl,imx6q-usb");
	pdev = of_find_device_by_node(np);
	if (!pdev) {
		pr_err("%s: can't find usb otg device\n", __func__);
		goto put_node;
	}


	icore_ver_gpio = of_get_named_gpio(np, "ver-gpios", 0);

	if (gpio_is_valid(icore_ver_gpio) &&
		!gpio_request_one(icore_ver_gpio, GPIOF_DIR_IN, "icore_ver_gpio")) {
		if(gpio_get_value(icore_ver_gpio))
		{
			printk("i.Core revision C or older\n");
			pctl = pinctrl_get_select(&pdev->dev, "rev_c"); 
			if (IS_ERR(pctl)) {
				pr_err("%s: can't get pinctrl state\n", __func__);
				goto put_node;
			}
		}		
		else
			printk("i.Core revision D or higher\n");
	}
	else
		goto put_node;	

put_node:
	of_node_put(np);

}


static void __init icore_rqs_late_init(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct clk *lvds2_sel, *osc, *lvds2_out;

	printk("uQseven i.Core-rqs module\n");

	printk("Init clock for USB HUB on RQS....");
	lvds2_sel = clk_get_sys(NULL, "lvds2_sel");
	osc = clk_get_sys(NULL, "osc");
	lvds2_out = clk_get_sys(NULL, "lvds2_out");
	if (IS_ERR(osc) || IS_ERR(lvds2_sel) ||
	    IS_ERR(lvds2_out))
	{
		printk("*** Error getting clock\n");
		return;
	}
	clk_set_parent(lvds2_sel, osc);
	clk_set_rate(lvds2_out, 24000000);
	clk_prepare_enable(lvds2_out);
	printk("Done\n");

	np = of_find_node_by_path("/soc/aips-bus@02100000/usb@02184000");
//	np = of_find_compatible_node(NULL, NULL, "fsl,imx6q-usb");
	pdev = of_find_device_by_node(np);
	if (!pdev) {
		pr_err("%s: can't find usb otg device\n", __func__);
		goto put_node;
	}

put_node:
	of_node_put(np);

}

static void __init imx6q_init_late(void)
{
	/*
	 * WAIT mode is broken on imx6 Dual/Quad revision 1.0 and 1.1 so
	 * there is no point to run cpuidle on them.
	 *
	 * It does work on imx6 Solo/DualLite starting from 1.1
	 */
	if ((cpu_is_imx6q() && imx_get_soc_revision() > IMX_CHIP_REVISION_1_1) ||
	    (cpu_is_imx6dl() && imx_get_soc_revision() > IMX_CHIP_REVISION_1_0))
		imx6q_cpuidle_init();

	if (IS_ENABLED(CONFIG_ARM_IMX6Q_CPUFREQ))
		platform_device_register_simple("imx6q-cpufreq", -1, NULL, 0);

	if (of_machine_is_compatible("fsl,imx6-icore")) {
				icore_late_init();
		}

	if (of_machine_is_compatible("fsl,imx6-icore-rqs")) {
		icore_rqs_late_init();
	}


}

static void __init imx6q_map_io(void)
{
	debug_ll_io_init();
	imx_scu_map_io();
	imx6_pm_map_io();
	imx_busfreq_map_io();
}

static void __init imx6q_init_irq(void)
{
	imx_gpc_check_dt();
	imx_init_revision_from_anatop();
	imx_init_l2cache();
	imx_src_init();
	irqchip_init();
	imx6_pm_ccm_init("fsl,imx6q-ccm");
}

static const char * const imx6q_dt_compat[] __initconst = {
	"fsl,imx6dl",
	"fsl,imx6q",
	"fsl,imx6qp",
	NULL,
};

DT_MACHINE_START(IMX6Q, "Freescale i.MX6 Quad/DualLite (Device Tree)")
	.l2c_aux_val 	= 0,
	.l2c_aux_mask	= ~0,
	.smp		= smp_ops(imx_smp_ops),
	.map_io		= imx6q_map_io,
	.init_irq	= imx6q_init_irq,
	.init_machine	= imx6q_init_machine,
	.init_late      = imx6q_init_late,
	.dt_compat	= imx6q_dt_compat,
MACHINE_END

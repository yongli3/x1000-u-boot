/*
 * Ingenic grus setup code
 *
 * Copyright (c) 2013 Ingenic Semiconductor Co.,Ltd
 * Author: Justin <ptkang@ingenic.cn>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <nand.h>
#include <net.h>
#include <netdev.h>
#include <asm/gpio.h>
#include <asm/arch/cpm.h>
#include <asm/arch/nand.h>
#include <asm/arch/mmc.h>

extern int act8600_regulator_init(void);
#ifdef CONFIG_BOOT_ANDROID
extern void boot_mode_select(void);
#endif

#if defined(CONFIG_CMD_BATTERYDET) && defined(CONFIG_BATTERY_INIT_GPIO)
static void battery_init_gpio(void)
{
}
#endif

int board_early_init_f(void)
{
	/* Power on TF-card */
	gpio_direction_output(GPIO_PF(19), 1);
	act8600_regulator_init();

	return 0;
}

int misc_init_r(void)
{
#if 0 /* TO DO */
	uint8_t mac[6] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc };

	/* set MAC address */
	eth_setenv_enetaddr("ethaddr", mac);
#endif
#ifdef CONFIG_BOOT_ANDROID
	boot_mode_select();
#endif

#if defined(CONFIG_CMD_BATTERYDET) && defined(CONFIG_BATTERY_INIT_GPIO)
	battery_init_gpio();
#endif
	return 0;
}

int board_nand_init(struct nand_chip *nand)
{
	return 0;
}


#ifdef CONFIG_MMC
int board_mmc_init(bd_t *bd)
{
	jz_mmc_init();
	return 0;
}
#endif

int board_eth_init(bd_t *bis)
{
	/* reset grus DM9000 */
	gpio_direction_output(CONFIG_GPIO_DM9000_RESET, CONFIG_GPIO_DM9000_RESET_ENLEVEL);
	mdelay(10);
	gpio_set_value(CONFIG_GPIO_DM9000_RESET, !CONFIG_GPIO_DM9000_RESET_ENLEVEL);
	mdelay(10);

	/* init grus gpio */
	gpio_set_func(GPIO_PORT_A, GPIO_FUNC_0, 0x040300ff);
	gpio_set_func(GPIO_PORT_B, GPIO_FUNC_0, 0x00000002);

	return dm9000_initialize(bis);
}

/* U-Boot common routines */
int checkboard(void)
{
	puts("Board: grus (Ingenic XBurst JZ4780 SoC)\n");
	return 0;
}

#ifdef CONFIG_SPL_BUILD

void spl_board_init(void)
{
}

#endif /* CONFIG_SPL_BUILD */
/*
 * JZ4775 pll calculation
 *
 * Copyright (c) 2013 Ingenic Semiconductor Co.,Ltd
 * Based on: JZ4770 PLL header file
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

#ifndef __PLL_H__
#define __PLL_H__

#define PLL_OUT_MAX 1400		/* 1400MHz. */


#define __CFG_EXTAL     (CONFIG_SYS_EXTAL / 1000000)

#if (CONFIG_SYS_APLL_FREQ > 0)
#define __CFG_APLL_OUT  ((CONFIG_SYS_APLL_FREQ) / 1000000)
#if (__CFG_APLL_OUT > PLL_OUT_MAX)
	#error "PLL output can NOT more than 1000MHZ"
#elif (__CFG_APLL_OUT > 600)
	#define __APLL_BS          1
	#define __APLL_OD          0
#elif (__CFG_APLL_OUT > 300)
	#define __APLL_BS          0
	#define __APLL_OD          0
#elif (__CFG_APLL_OUT > 155)
	#define __APLL_BS          0
	#define __APLL_OD          1
#elif (__CFG_APLL_OUT > 76)
	#define __APLL_BS          0
	#define __APLL_OD          2
#elif (__CFG_APLL_OUT > 47)
	#define __APLL_BS          0
	#define __APLL_OD          3
#else
	#error "APLL ouptput can NOT less than 48"
#endif

#define __APLL_NO		0
#define APLL_NR 			(__APLL_NO + 1)
#define APLL_NO 			(0x1 << __APLL_OD)
#define __APLL_MO		(((__CFG_APLL_OUT / __CFG_EXTAL) * APLL_NR * APLL_NO) - 1)
#define APLL_NF 			(__APLL_MO + 1)
#define APLL_FOUT			(__CFG_EXTAL * APLL_NF / APLL_NR / APLL_NO)

#if ((__CFG_EXTAL / APLL_NR > 50) || (__CFG_EXTAL / APLL_NR < 10))
	#error "Can NOT set the value to APLL_N"
#endif

#if ((__APLL_MO > 127) || (__APLL_MO < 1))
	#error "Can NOT set the value to APLL_M"
#endif

#if (__APLL_BS == 1)
	#if (((APLL_FOUT * APLL_NO) > PLL_OUT_MAX) || ((APLL_FOUT * APLL_NO) < 500))
		#error "FVCO check failed : APLL_BS = 1"
	#endif
#elif (__APLL_BS == 0)
	#if (((APLL_FOUT * APLL_NO) > 600) || ((APLL_FOUT * APLL_NO) < 300))
		#error "FVCO check failed : APLL_BS = 0"
	#endif
#endif

#define APLL_VALUE	((__APLL_MO << 24) | (__APLL_NO << 18) | (__APLL_OD << 16) | (__APLL_BS << 31))
#endif /* CONFIG_SYS_APLL_FREQ > 0 */

#if (CONFIG_SYS_MPLL_FREQ > 0)
#define __CFG_MPLL_OUT  ((CONFIG_SYS_MPLL_FREQ) / 1000000)
#if (__CFG_MPLL_OUT > PLL_OUT_MAX)
	#error "MPLL output can NO1T more than 1000MHZ"
#elif (__CFG_MPLL_OUT > 600)
	#define __MPLL_BS          1
	#define __MPLL_OD          0
#elif (__CFG_MPLL_OUT > 300)
	#define __MPLL_BS          0
	#define __MPLL_OD          0
#elif (__CFG_MPLL_OUT > 155)
	#define __MPLL_BS          0
	#define __MPLL_OD          1
#elif (__CFG_MPLL_OUT > 76)
	#define __MPLL_BS          0
	#define __MPLL_OD          2
#elif (__CFG_MPLL_OUT > 47)
	#define __MPLL_BS          0
	#define __MPLL_OD          3
#else
	#error "MPLL ouptput can NOT less than 48"
#endif

#define __MPLL_NO		0
#define MPLL_NR			(__MPLL_NO + 1)
#define MPLL_NO			(0x1 << __MPLL_OD)
#define __MPLL_MO		(((__CFG_MPLL_OUT / __CFG_EXTAL) * MPLL_NR * MPLL_NO) - 1)
#define MPLL_NF			(__MPLL_MO + 1)
#define MPLL_FOUT		(__CFG_EXTAL * MPLL_NF / MPLL_NR / MPLL_NO)

#if ((__CFG_EXTAL / MPLL_NR > 50) || (__CFG_EXTAL / MPLL_NR < 10))
	#error "Can NOT set the value to MPLL_N"
#endif

#if ((__MPLL_MO > 127) || (__MPLL_MO < 1))
	#error "Can NOT set the value to MPLL_M"
#endif

#if (__MPLL_BS == 1)
	#if (((MPLL_FOUT * MPLL_NO) > 1000) || ((MPLL_FOUT * MPLL_NO) < 500))
		#error "FVCO1 check failed : MPLL_BS1 = 1"
	#endif
#elif (__MPLL_BS == 0)
	#if (((MPLL_FOUT * MPLL_NO) > 600) || ((MPLL_FOUT * MPLL_NO) < 300))
		#error "FVCO1 check failed : MPLL_BS1 = 0"
	#endif
#endif

#define MPLL_VALUE	((__MPLL_MO << 24) | (__MPLL_NO << 18) | (__MPLL_OD << 16) | (__MPLL_BS << 31))
#endif /* CONFIG_SYS_MPLL_FREQ > 0 */

#endif /* __PLL_H__ */
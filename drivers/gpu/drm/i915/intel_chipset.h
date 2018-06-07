/* SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#ifndef _INTEL_CHIPSET_H_
#define _INTEL_CHIPSET_H_

#define INTEL_GEN(dev_priv)	((dev_priv)->info.gen)
#define INTEL_DEVID(dev_priv)	((dev_priv)->info.device_id)

#define REVID_FOREVER		0xff
#define INTEL_REVID(dev_priv)	((dev_priv)->drm.pdev->revision)

#define GEN_FOREVER (0)

#define INTEL_GEN_MASK(s, e) ( \
	BUILD_BUG_ON_ZERO(!__builtin_constant_p(s)) + \
	BUILD_BUG_ON_ZERO(!__builtin_constant_p(e)) + \
	GENMASK((e) != GEN_FOREVER ? (e) - 1 : BITS_PER_LONG - 1, \
		(s) != GEN_FOREVER ? (s) - 1 : 0) \
)

/*
 * Returns true if Gen is in inclusive range [Start, End].
 *
 * Use GEN_FOREVER for unbound start and or end.
 */
#define IS_GEN(dev_priv, s, e) \
	(!!((dev_priv)->info.gen_mask & INTEL_GEN_MASK((s), (e))))

/*
 * Return true if revision is in range [since,until] inclusive.
 *
 * Use 0 for open-ended since, and REVID_FOREVER for open-ended until.
 */
#define IS_REVID(p, since, until) \
	(INTEL_REVID(p) >= (since) && INTEL_REVID(p) <= (until))

#define IS_PLATFORM(dev_priv, p) ((dev_priv)->info.platform_mask & BIT(p))

#define IS_I830(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I830)
#define IS_I845G(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I845G)
#define IS_I85X(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I85X)
#define IS_I865G(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I865G)
#define IS_I915G(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I915G)
#define IS_I915GM(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I915GM)
#define IS_I945G(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I945G)
#define IS_I945GM(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I945GM)
#define IS_I965G(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I965G)
#define IS_I965GM(dev_priv)	IS_PLATFORM(dev_priv, INTEL_I965GM)
#define IS_G45(dev_priv)	IS_PLATFORM(dev_priv, INTEL_G45)
#define IS_GM45(dev_priv)	IS_PLATFORM(dev_priv, INTEL_GM45)
#define IS_G4X(dev_priv)	(IS_G45(dev_priv) || IS_GM45(dev_priv))
#define IS_PINEVIEW_G(dev_priv)	(INTEL_DEVID(dev_priv) == 0xa001)
#define IS_PINEVIEW_M(dev_priv)	(INTEL_DEVID(dev_priv) == 0xa011)
#define IS_PINEVIEW(dev_priv)	IS_PLATFORM(dev_priv, INTEL_PINEVIEW)
#define IS_G33(dev_priv)	IS_PLATFORM(dev_priv, INTEL_G33)
#define IS_IRONLAKE_M(dev_priv)	(INTEL_DEVID(dev_priv) == 0x0046)
#define IS_IVYBRIDGE(dev_priv)	IS_PLATFORM(dev_priv, INTEL_IVYBRIDGE)
#define IS_IVB_GT1(dev_priv)	(IS_IVYBRIDGE(dev_priv) && \
				 (dev_priv)->info.gt == 1)
#define IS_VALLEYVIEW(dev_priv)	IS_PLATFORM(dev_priv, INTEL_VALLEYVIEW)
#define IS_CHERRYVIEW(dev_priv)	IS_PLATFORM(dev_priv, INTEL_CHERRYVIEW)
#define IS_HASWELL(dev_priv)	IS_PLATFORM(dev_priv, INTEL_HASWELL)
#define IS_BROADWELL(dev_priv)	IS_PLATFORM(dev_priv, INTEL_BROADWELL)
#define IS_SKYLAKE(dev_priv)	IS_PLATFORM(dev_priv, INTEL_SKYLAKE)
#define IS_BROXTON(dev_priv)	IS_PLATFORM(dev_priv, INTEL_BROXTON)
#define IS_KABYLAKE(dev_priv)	IS_PLATFORM(dev_priv, INTEL_KABYLAKE)
#define IS_GEMINILAKE(dev_priv)	IS_PLATFORM(dev_priv, INTEL_GEMINILAKE)
#define IS_COFFEELAKE(dev_priv)	IS_PLATFORM(dev_priv, INTEL_COFFEELAKE)
#define IS_CANNONLAKE(dev_priv)	IS_PLATFORM(dev_priv, INTEL_CANNONLAKE)
#define IS_ICELAKE(dev_priv)	IS_PLATFORM(dev_priv, INTEL_ICELAKE)
#define IS_MOBILE(dev_priv)	((dev_priv)->info.is_mobile)
#define IS_HSW_EARLY_SDV(dev_priv) (IS_HASWELL(dev_priv) && \
				    (INTEL_DEVID(dev_priv) & 0xFF00) == 0x0C00)
#define IS_BDW_ULT(dev_priv)	(IS_BROADWELL(dev_priv) && \
				 ((INTEL_DEVID(dev_priv) & 0xf) == 0x6 ||	\
				 (INTEL_DEVID(dev_priv) & 0xf) == 0xb ||	\
				 (INTEL_DEVID(dev_priv) & 0xf) == 0xe))
/* ULX machines are also considered ULT. */
#define IS_BDW_ULX(dev_priv)	(IS_BROADWELL(dev_priv) && \
				 (INTEL_DEVID(dev_priv) & 0xf) == 0xe)
#define IS_BDW_GT3(dev_priv)	(IS_BROADWELL(dev_priv) && \
				 (dev_priv)->info.gt == 3)
#define IS_HSW_ULT(dev_priv)	(IS_HASWELL(dev_priv) && \
				 (INTEL_DEVID(dev_priv) & 0xFF00) == 0x0A00)
#define IS_HSW_GT3(dev_priv)	(IS_HASWELL(dev_priv) && \
				 (dev_priv)->info.gt == 3)
/* ULX machines are also considered ULT. */
#define IS_HSW_ULX(dev_priv)	(INTEL_DEVID(dev_priv) == 0x0A0E || \
				 INTEL_DEVID(dev_priv) == 0x0A1E)
#define IS_SKL_ULT(dev_priv)	(INTEL_DEVID(dev_priv) == 0x1906 || \
				 INTEL_DEVID(dev_priv) == 0x1913 || \
				 INTEL_DEVID(dev_priv) == 0x1916 || \
				 INTEL_DEVID(dev_priv) == 0x1921 || \
				 INTEL_DEVID(dev_priv) == 0x1926)
#define IS_SKL_ULX(dev_priv)	(INTEL_DEVID(dev_priv) == 0x190E || \
				 INTEL_DEVID(dev_priv) == 0x1915 || \
				 INTEL_DEVID(dev_priv) == 0x191E)
#define IS_KBL_ULT(dev_priv)	(INTEL_DEVID(dev_priv) == 0x5906 || \
				 INTEL_DEVID(dev_priv) == 0x5913 || \
				 INTEL_DEVID(dev_priv) == 0x5916 || \
				 INTEL_DEVID(dev_priv) == 0x5921 || \
				 INTEL_DEVID(dev_priv) == 0x5926)
#define IS_KBL_ULX(dev_priv)	(INTEL_DEVID(dev_priv) == 0x590E || \
				 INTEL_DEVID(dev_priv) == 0x5915 || \
				 INTEL_DEVID(dev_priv) == 0x591E)
#define IS_SKL_GT2(dev_priv)	(IS_SKYLAKE(dev_priv) && \
				 (dev_priv)->info.gt == 2)
#define IS_SKL_GT3(dev_priv)	(IS_SKYLAKE(dev_priv) && \
				 (dev_priv)->info.gt == 3)
#define IS_SKL_GT4(dev_priv)	(IS_SKYLAKE(dev_priv) && \
				 (dev_priv)->info.gt == 4)
#define IS_KBL_GT2(dev_priv)	(IS_KABYLAKE(dev_priv) && \
				 (dev_priv)->info.gt == 2)
#define IS_KBL_GT3(dev_priv)	(IS_KABYLAKE(dev_priv) && \
				 (dev_priv)->info.gt == 3)
#define IS_CFL_ULT(dev_priv)	(IS_COFFEELAKE(dev_priv) && \
				 (INTEL_DEVID(dev_priv) & 0x00F0) == 0x00A0)
#define IS_CFL_GT2(dev_priv)	(IS_COFFEELAKE(dev_priv) && \
				 (dev_priv)->info.gt == 2)
#define IS_CFL_GT3(dev_priv)	(IS_COFFEELAKE(dev_priv) && \
				 (dev_priv)->info.gt == 3)
#define IS_CNL_WITH_PORT_F(dev_priv)   (IS_CANNONLAKE(dev_priv) && \
					(INTEL_DEVID(dev_priv) & 0x0004) == 0x0004)

#define PRODUCT_REVID_UNKNOWN	REVID_FOREVER
#define FIRST_PRODUCT_REVID(intel_info) ((intel_info)->first_product_revid)
#define IS_PREPRODUCTION_HW(dev_priv)   (INTEL_REVID(dev_priv) < FIRST_PRODUCT_REVID(INTEL_INFO(dev_priv)))
#define IS_ALPHA_SUPPORT(intel_info)    (FIRST_PRODUCT_REVID(intel_info) == PRODUCT_REVID_UNKNOWN)

#define SKL_REVID_A0		0x0
#define SKL_REVID_B0		0x1
#define SKL_REVID_C0		0x2
#define SKL_REVID_D0		0x3
#define SKL_REVID_E0		0x4
#define SKL_REVID_F0		0x5
#define SKL_REVID_G0		0x6
#define SKL_REVID_H0		0x7

#define IS_SKL_REVID(p, since, until) (IS_SKYLAKE(p) && IS_REVID(p, since, until))

#define BXT_REVID_A0		0x0
#define BXT_REVID_A1		0x1
#define BXT_REVID_B0		0x3
#define BXT_REVID_B_LAST	0x8
#define BXT_REVID_C0		0x9

#define IS_BXT_REVID(dev_priv, since, until) \
	(IS_BROXTON(dev_priv) && IS_REVID(dev_priv, since, until))

#define KBL_REVID_A0		0x0
#define KBL_REVID_B0		0x1
#define KBL_REVID_C0		0x2
#define KBL_REVID_D0		0x3
#define KBL_REVID_E0		0x4

#define IS_KBL_REVID(dev_priv, since, until) \
	(IS_KABYLAKE(dev_priv) && IS_REVID(dev_priv, since, until))

#define GLK_REVID_A0		0x0
#define GLK_REVID_A1		0x1
#define GLK_REVID_B0		0x3

#define IS_GLK_REVID(dev_priv, since, until) \
	(IS_GEMINILAKE(dev_priv) && IS_REVID(dev_priv, since, until))

#define CNL_REVID_A0		0x0
#define CNL_REVID_B0		0x1
#define CNL_REVID_C0		0x2
#define CNL_REVID_D0		0x4
#define CNL_REVID_G0		0x5

#define IS_CNL_REVID(p, since, until) \
	(IS_CANNONLAKE(p) && IS_REVID(p, since, until))

#define ICL_REVID_A0		0x0
#define ICL_REVID_A2		0x1
#define ICL_REVID_B0		0x3
#define ICL_REVID_B2		0x4
#define ICL_REVID_C0		0x5

#define IS_ICL_REVID(p, since, until) \
	(IS_ICELAKE(p) && IS_REVID(p, since, until))

/*
 * The genX designation typically refers to the render engine, so render
 * capability related checks should use IS_GEN, while display and other checks
 * have their own (e.g. HAS_PCH_SPLIT for ILK+ display, IS_foo for particular
 * chips, etc.).
 */
#define IS_GEN2(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(1)))
#define IS_GEN3(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(2)))
#define IS_GEN4(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(3)))
#define IS_GEN5(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(4)))
#define IS_GEN6(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(5)))
#define IS_GEN7(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(6)))
#define IS_GEN8(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(7)))
#define IS_GEN9(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(8)))
#define IS_GEN10(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(9)))
#define IS_GEN11(dev_priv)	(!!((dev_priv)->info.gen_mask & BIT(10)))

#define IS_LP(dev_priv)	(INTEL_INFO(dev_priv)->is_lp)
#define IS_GEN9_LP(dev_priv)	(IS_GEN9(dev_priv) && IS_LP(dev_priv))
#define IS_GEN9_BC(dev_priv)	(IS_GEN9(dev_priv) && !IS_LP(dev_priv))

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __QCOM_PM660L_LCDB_INTERNAL_H__
#define __QCOM_PM660L_LCDB_INTERNAL_H__

#define PM660L_LCDB_MIN_UV              4000000U
#define PM660L_LCDB_MAX_UV              6000000U
#define PM660L_LCDB_N_VOLTAGES          32U
#define PM660L_LCDB_COARSE_MAX_SEL      9U
#define PM660L_LCDB_COARSE_STEP_UV      100000U
#define PM660L_LCDB_FINE_MIN_SEL        10U
#define PM660L_LCDB_FINE_MIN_UV         4950000U
#define PM660L_LCDB_FINE_STEP_UV        50000U

#define PM660L_LCDB_BST_MIN_UV          4700000U
#define PM660L_LCDB_BST_MAX_UV          6250000U
#define PM660L_LCDB_BST_STEP_UV         50000U
#define PM660L_LCDB_BST_N_VOLTAGES      32U
#define PM660L_LCDB_DEFAULT_HEADROOM_UV 200000U

/* Return 0 for an invalid selector, otherwise the exact supported voltage. */
static inline unsigned int
pm660l_lcdb_list_voltage(unsigned int selector)
{
	if (selector >= PM660L_LCDB_N_VOLTAGES)
		return 0;

	if (selector <= PM660L_LCDB_COARSE_MAX_SEL)
		return PM660L_LCDB_MIN_UV +
			selector * PM660L_LCDB_COARSE_STEP_UV;

	return PM660L_LCDB_FINE_MIN_UV +
		(selector - PM660L_LCDB_FINE_MIN_SEL) *
		PM660L_LCDB_FINE_STEP_UV;
}

/* Choose the lowest supported voltage inside the regulator-core interval. */
static inline int
pm660l_lcdb_map_voltage(unsigned int min_uv, unsigned int max_uv,
			unsigned int *selector)
{
	unsigned int sel, uv;

	if (!selector || min_uv > max_uv || min_uv < PM660L_LCDB_MIN_UV ||
	    max_uv > PM660L_LCDB_MAX_UV)
		return -1;

	for (sel = 0; sel < PM660L_LCDB_N_VOLTAGES; sel++) {
		uv = pm660l_lcdb_list_voltage(sel);
		if (uv >= min_uv && uv <= max_uv) {
			*selector = sel;
			return 0;
		}
	}

	return -1;
}

/* PM660L boost selector is a 5-bit, 50 mV linear field from 4.7 V. */
static inline int
pm660l_lcdb_bst_map_voltage(unsigned int min_uv, unsigned int max_uv,
			    unsigned int *selector)
{
	unsigned int delta, sel, uv;

	if (!selector || min_uv > max_uv || min_uv < PM660L_LCDB_BST_MIN_UV ||
	    max_uv > PM660L_LCDB_BST_MAX_UV)
		return -1;

	delta = min_uv - PM660L_LCDB_BST_MIN_UV;
	sel = delta / PM660L_LCDB_BST_STEP_UV;
	if (delta % PM660L_LCDB_BST_STEP_UV)
		sel++;
	if (sel >= PM660L_LCDB_BST_N_VOLTAGES)
		return -1;

	uv = PM660L_LCDB_BST_MIN_UV + sel * PM660L_LCDB_BST_STEP_UV;
	if (uv > max_uv)
		return -1;

	*selector = sel;
	return 0;
}

/* Both output voltages are supplied as positive magnitudes. */
static inline int
pm660l_lcdb_calc_boost(unsigned int ldo_uv, unsigned int ncp_abs_uv,
		       unsigned int headroom_uv, unsigned int *boost_uv)
{
	unsigned int target;

	if (!boost_uv || ldo_uv < PM660L_LCDB_MIN_UV ||
	    ldo_uv > PM660L_LCDB_MAX_UV ||
	    ncp_abs_uv < PM660L_LCDB_MIN_UV ||
	    ncp_abs_uv > PM660L_LCDB_MAX_UV)
		return -1;

	target = (ldo_uv > ncp_abs_uv ? ldo_uv : ncp_abs_uv) + headroom_uv;
	if (target < PM660L_LCDB_BST_MIN_UV)
		target = PM660L_LCDB_BST_MIN_UV;
	if (target > PM660L_LCDB_BST_MAX_UV)
		return -1;

	*boost_uv = target;
	return 0;
}

#define PM660L_LCDB_REQ_LDO  (1U << 0)
#define PM660L_LCDB_REQ_NCP  (1U << 1)
#define PM660L_LCDB_REQ_MASK (PM660L_LCDB_REQ_LDO | PM660L_LCDB_REQ_NCP)

/*
 * Calculate the next consumer-request mask without changing physical state.
 * The caller serializes this decision with the hardware transition.
 */
static inline int
pm660l_lcdb_next_enable_mask(unsigned int old_mask, unsigned int rail,
			     unsigned int *new_mask, int *enable_module)
{
	if (!new_mask || !enable_module ||
	    (rail != PM660L_LCDB_REQ_LDO && rail != PM660L_LCDB_REQ_NCP) ||
	    (old_mask & ~PM660L_LCDB_REQ_MASK))
		return -1;

	*new_mask = old_mask | rail;
	*enable_module = old_mask == 0;
	return 0;
}

/* The shared module may be shut down only after the final request is removed. */
static inline int
pm660l_lcdb_next_disable_mask(unsigned int old_mask, unsigned int rail,
			      unsigned int *new_mask, int *disable_module)
{
	if (!new_mask || !disable_module ||
	    (rail != PM660L_LCDB_REQ_LDO && rail != PM660L_LCDB_REQ_NCP) ||
	    (old_mask & ~PM660L_LCDB_REQ_MASK) || !(old_mask & rail))
		return -1;

	*new_mask = old_mask & ~rail;
	*disable_module = *new_mask == 0;
	return 0;
}

/* PM660L LCDB register offsets and status fields (relative to base 0xec00). */
#define PM660L_LCDB_STS3_REG       0x0a
#define PM660L_LCDB_STS4_REG       0x0b
#define PM660L_LCDB_INT_RT_REG     0x10
#define PM660L_LCDB_BST_VOLT_REG   0x41
#define PM660L_LCDB_MODULE_RDY_REG 0x45
#define PM660L_LCDB_ENABLE_REG     0x46
#define PM660L_LCDB_MISC_CTL_REG   0x60
#define PM660L_LCDB_LDO_VOLT_REG   0x71
#define PM660L_LCDB_NCP_VOLT_REG   0x81
#define PM660L_LCDB_SEC_ACCESS_REG 0xd0

#define PM660L_LCDB_VREG_OK_BIT    (1U << 7)
#define PM660L_LCDB_MODULE_EN_BIT  (1U << 7)
#define PM660L_LCDB_OVERALL_OK_BIT (1U << 0)
#define PM660L_LCDB_SC_ERROR_BIT   (1U << 1)
#define PM660L_LCDB_SCP_DISABLE_BIT (1U << 0)
#define PM660L_LCDB_SEC_ACCESS_KEY 0xa5U
#define PM660L_LCDB_V2P0_REV4      0x02U

static inline int pm660l_lcdb_needs_ncp_scp_wa(unsigned int pmic_rev4)
{
	return pmic_rev4 < PM660L_LCDB_V2P0_REV4;
}

/* Decode the per-output VREG_OK bits after the required STS3/STS4 pre-write. */
static inline unsigned int
pm660l_lcdb_decode_output_status(unsigned int sts3, unsigned int sts4)
{
	return ((sts3 & PM660L_LCDB_VREG_OK_BIT) ? PM660L_LCDB_REQ_LDO : 0) |
	       ((sts4 & PM660L_LCDB_VREG_OK_BIT) ? PM660L_LCDB_REQ_NCP : 0);
}

#endif /* __QCOM_PM660L_LCDB_INTERNAL_H__ */

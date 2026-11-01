/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>

#include "../drivers/regulator/qcom-pm660l-lcdb-internal.h"

int main(void)
{
	unsigned int sel, uv, i;

	assert(pm660l_lcdb_list_voltage(0) == 4000000U);
	assert(pm660l_lcdb_list_voltage(9) == 4900000U);
	assert(pm660l_lcdb_list_voltage(10) == 4950000U);
	assert(pm660l_lcdb_list_voltage(31) == 6000000U);
	assert(pm660l_lcdb_list_voltage(32) == 0U);

	for (i = 0; i < PM660L_LCDB_N_VOLTAGES; i++) {
		uv = pm660l_lcdb_list_voltage(i);
		assert(pm660l_lcdb_map_voltage(uv, uv, &sel) == 0);
		assert(sel == i);
	}

	assert(pm660l_lcdb_map_voltage(4900001U, 4949999U, &sel) == -1);
	assert(pm660l_lcdb_map_voltage(4950001U, 5000000U, &sel) == 0);
	assert(pm660l_lcdb_list_voltage(sel) == 5000000U);
	assert(pm660l_lcdb_map_voltage(3999999U, 5000000U, &sel) == -1);
	assert(pm660l_lcdb_map_voltage(5000000U, 4000000U, &sel) == -1);

	assert(pm660l_lcdb_bst_map_voltage(4700000U, 4700000U, &sel) == 0);
	assert(sel == 0);
	assert(pm660l_lcdb_bst_map_voltage(4725000U, 4750000U, &sel) == 0);
	assert(sel == 1);
	assert(pm660l_lcdb_bst_map_voltage(6250000U, 6250000U, &sel) == 0);
	assert(sel == 31);
	assert(pm660l_lcdb_bst_map_voltage(6250001U, 6300000U, &sel) == -1);
	assert(pm660l_lcdb_bst_map_voltage(4725000U, 4725000U, &sel) == -1);

	assert(pm660l_lcdb_calc_boost(4000000U, 4500000U, 200000U, &uv) == 0);
	assert(uv == 4700000U);
	assert(pm660l_lcdb_calc_boost(6000000U, 5800000U, 200000U, &uv) == 0);
	assert(uv == 6200000U);
	assert(pm660l_lcdb_calc_boost(6000000U, 5800000U, 300000U, &uv) == -1);


	{
		unsigned int mask;
		int action;

		assert(pm660l_lcdb_next_enable_mask(0, PM660L_LCDB_REQ_LDO,
						    &mask, &action) == 0);
		assert(mask == PM660L_LCDB_REQ_LDO && action == 1);
		assert(pm660l_lcdb_next_enable_mask(mask, PM660L_LCDB_REQ_NCP,
						    &mask, &action) == 0);
		assert(mask == PM660L_LCDB_REQ_MASK && action == 0);
		assert(pm660l_lcdb_next_disable_mask(mask, PM660L_LCDB_REQ_LDO,
						     &mask, &action) == 0);
		assert(mask == PM660L_LCDB_REQ_NCP && action == 0);
		assert(pm660l_lcdb_next_disable_mask(mask, PM660L_LCDB_REQ_NCP,
						     &mask, &action) == 0);
		assert(mask == 0 && action == 1);
		assert(pm660l_lcdb_next_disable_mask(0, PM660L_LCDB_REQ_LDO,
						     &mask, &action) == -1);
		assert(pm660l_lcdb_next_enable_mask(0, 4, &mask, &action) == -1);
	}

	puts("PM660L LCDB selector and shared-request tests: PASS");
	return 0;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * PM660L LCDB regulator driver.
 *
 * The LDO and NCP outputs share one boost converter and module-enable bit.
 * Keep all shared-state transitions serialized by lcdb->lock.
 */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <soc/qcom/qcom-spmi-pmic.h>

#include "qcom-pm660l-lcdb-internal.h"

#define PM660L_LCDB_TOUCH_WAKE_BIT BIT(3)
#define PM660L_LCDB_IRQ_NAME		"sc-irq"
#define PM660L_LCDB_STATUS_POLL_US	1000
#define PM660L_LCDB_STATUS_POLLS	100
#define PM660L_LCDB_SC_RETRY_WINDOW_US	1000000
#define PM660L_LCDB_SC_RETRY_MAX	10

struct pm660l_lcdb;

struct pm660l_lcdb_rail {
	struct pm660l_lcdb *lcdb;
	struct regulator_desc desc;
	struct regulator_dev *rdev;
	unsigned int request_bit;
	unsigned int voltage_reg;
	const char *name;
};

struct pm660l_lcdb {
	struct device *dev;
	struct regmap *regmap;
	const struct qcom_spmi_pmic *pmic;
	struct mutex lock;
	u32 base;
	unsigned int request_mask;
	unsigned int output_good_mask;
	bool module_enabled;
	bool fault_latched;
	unsigned int sc_count;
	ktime_t sc_window_start;
	struct pm660l_lcdb_rail ldo;
	struct pm660l_lcdb_rail ncp;
};

static int pm660l_lcdb_read_output_status(struct pm660l_lcdb *lcdb,
					  unsigned int *status)
{
	u8 clear[2] = { 0, 0 };
	u8 sts[2];
	int ret;

	/*
	 * PM660L requires a write to STS3/STS4 before the status pair is read.
	 * Preserve the downstream driver's two-byte transaction.
	 */
	ret = regmap_bulk_write(lcdb->regmap,
				lcdb->base + PM660L_LCDB_STS3_REG,
				clear, ARRAY_SIZE(clear));
	if (ret)
		return ret;

	ret = regmap_bulk_read(lcdb->regmap,
			       lcdb->base + PM660L_LCDB_STS3_REG,
			       sts, ARRAY_SIZE(sts));
	if (ret)
		return ret;

	*status = pm660l_lcdb_decode_output_status(sts[0], sts[1]);
	return 0;
}

static int pm660l_lcdb_poll_module_ready(struct pm660l_lcdb *lcdb)
{
	unsigned int val, status;
	int i, ret;

	for (i = 0; i < PM660L_LCDB_STATUS_POLLS; i++) {
		ret = regmap_read(lcdb->regmap,
				  lcdb->base + PM660L_LCDB_INT_RT_REG, &val);
		if (ret)
			return ret;

		if (val & PM660L_LCDB_OVERALL_OK_BIT) {
			ret = pm660l_lcdb_read_output_status(lcdb, &status);
			if (ret)
				return ret;

			lcdb->output_good_mask = status;
			if ((status & lcdb->request_mask) != lcdb->request_mask)
				return -EIO;
			return 0;
		}

		usleep_range(PM660L_LCDB_STATUS_POLL_US,
			     PM660L_LCDB_STATUS_POLL_US + 100);
	}

	return -ETIMEDOUT;
}

static int pm660l_lcdb_old_revision_wa(struct pm660l_lcdb *lcdb)
{
	int ret;

	if (!pm660l_lcdb_needs_ncp_scp_wa(lcdb->pmic->major))
		return 0;

	/* The PM660L v1.x workaround requires a controlled module discharge. */
	ret = regmap_update_bits(lcdb->regmap,
				 lcdb->base + PM660L_LCDB_ENABLE_REG,
				 PM660L_LCDB_MODULE_EN_BIT,
				 PM660L_LCDB_MODULE_EN_BIT);
	if (ret)
		return ret;
	lcdb->module_enabled = true;

	ret = regmap_update_bits(lcdb->regmap,
				 lcdb->base + PM660L_LCDB_ENABLE_REG,
				 PM660L_LCDB_MODULE_EN_BIT, 0);
	if (ret) {
		lcdb->fault_latched = true;
		return ret;
	}

	lcdb->module_enabled = false;
	usleep_range(10000, 10100);

	ret = regmap_update_bits(lcdb->regmap,
				 lcdb->base + PM660L_LCDB_MISC_CTL_REG,
				 PM660L_LCDB_SCP_DISABLE_BIT,
				 PM660L_LCDB_SCP_DISABLE_BIT);
	if (ret) {
		lcdb->fault_latched = true;
		return ret;
	}

	usleep_range(1000, 1100);

	ret = regmap_update_bits(lcdb->regmap,
				 lcdb->base + PM660L_LCDB_MISC_CTL_REG,
				 PM660L_LCDB_SCP_DISABLE_BIT, 0);
	if (ret) {
		lcdb->fault_latched = true;
		return ret;
	}

	usleep_range(1000, 1100);
	return 0;
}

static int pm660l_lcdb_module_disable(struct pm660l_lcdb *lcdb);

static int pm660l_lcdb_module_enable(struct pm660l_lcdb *lcdb,
				     bool run_revision_wa)
{
	int ret;

	if (lcdb->module_enabled)
		return pm660l_lcdb_poll_module_ready(lcdb);

	if (run_revision_wa) {
		ret = pm660l_lcdb_old_revision_wa(lcdb);
		if (ret) {
			int disable_ret = pm660l_lcdb_module_disable(lcdb);

			lcdb->fault_latched = true;
			if (disable_ret)
				dev_err(lcdb->dev,
					"revision workaround failed (%d), shutdown failed (%d)\n",
					ret, disable_ret);
			return ret;
		}
	}

	ret = regmap_update_bits(lcdb->regmap,
				 lcdb->base + PM660L_LCDB_ENABLE_REG,
				 PM660L_LCDB_MODULE_EN_BIT,
				 PM660L_LCDB_MODULE_EN_BIT);
	if (ret) {
		int disable_ret = pm660l_lcdb_module_disable(lcdb);

		if (disable_ret)
			lcdb->fault_latched = true;
		return ret;
	}

	lcdb->module_enabled = true;
	ret = pm660l_lcdb_poll_module_ready(lcdb);
	if (ret) {
		int disable_ret;

		disable_ret = regmap_update_bits(lcdb->regmap,
						 lcdb->base + PM660L_LCDB_ENABLE_REG,
						 PM660L_LCDB_MODULE_EN_BIT, 0);
		if (!disable_ret) {
			lcdb->module_enabled = false;
			lcdb->output_good_mask = 0;
		} else {
			lcdb->fault_latched = true;
		}
		return ret;
	}

	return 0;
}

static int pm660l_lcdb_module_disable(struct pm660l_lcdb *lcdb)
{
	int ret;

	ret = regmap_update_bits(lcdb->regmap,
				 lcdb->base + PM660L_LCDB_ENABLE_REG,
				 PM660L_LCDB_MODULE_EN_BIT, 0);
	if (ret) {
		unsigned int val;
		int read_ret;

		read_ret = regmap_read(lcdb->regmap,
				       lcdb->base + PM660L_LCDB_ENABLE_REG, &val);
		if (read_ret || !(val & PM660L_LCDB_MODULE_EN_BIT)) {
			lcdb->module_enabled = false;
			lcdb->output_good_mask = 0;
			lcdb->fault_latched = true;
		} else {
			lcdb->module_enabled = true;
		}
		return ret;
	}

	lcdb->module_enabled = false;
	lcdb->output_good_mask = 0;
	return 0;
}

static int pm660l_lcdb_enable(struct regulator_dev *rdev)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	struct pm660l_lcdb *lcdb = rail->lcdb;
	unsigned int status, next_mask;
	int enable_module, ret;

	mutex_lock(&lcdb->lock);
	if (lcdb->fault_latched) {
		ret = -EIO;
		goto out;
	}

	if (lcdb->request_mask & rail->request_bit) {
		ret = 0;
		goto out;
	}

	ret = pm660l_lcdb_next_enable_mask(lcdb->request_mask,
					    rail->request_bit,
					    &next_mask, &enable_module);
	if (ret) {
		ret = -EINVAL;
		goto out;
	}

	if (!lcdb->module_enabled) {
		if (!enable_module) {
			lcdb->fault_latched = true;
			ret = -EIO;
			goto out;
		}
		ret = pm660l_lcdb_module_enable(lcdb, true);
		if (ret)
			goto out;
	} else {
		ret = pm660l_lcdb_read_output_status(lcdb, &status);
		if (ret)
			goto out;
		lcdb->output_good_mask = status;
	}

	if (!(lcdb->output_good_mask & rail->request_bit)) {
		ret = -EIO;
		goto out;
	}

	lcdb->request_mask = next_mask;
	ret = 0;
out:
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int pm660l_lcdb_disable(struct regulator_dev *rdev)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	struct pm660l_lcdb *lcdb = rail->lcdb;
	unsigned int next_mask;
	int disable_module, ret = 0;

	mutex_lock(&lcdb->lock);
	if (!(lcdb->request_mask & rail->request_bit))
		goto out;

	ret = pm660l_lcdb_next_disable_mask(lcdb->request_mask,
					     rail->request_bit,
					     &next_mask, &disable_module);
	if (ret) {
		ret = -EINVAL;
		goto out;
	}

	/* Do not commit the request change unless the shared shutdown succeeds. */
	if (disable_module) {
		ret = pm660l_lcdb_module_disable(lcdb);
		if (ret)
			goto out;
	}

	lcdb->request_mask = next_mask;
out:
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int pm660l_lcdb_is_enabled(struct regulator_dev *rdev)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	struct pm660l_lcdb *lcdb = rail->lcdb;
	unsigned int status;
	int ret;

	mutex_lock(&lcdb->lock);
	ret = pm660l_lcdb_read_output_status(lcdb, &status);
	if (!ret) {
		lcdb->output_good_mask = status;
		ret = !!(status & rail->request_bit);
	}
	mutex_unlock(&lcdb->lock);

	return ret;
}

static int pm660l_lcdb_get_voltage_sel(struct regulator_dev *rdev)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	struct pm660l_lcdb *lcdb = rail->lcdb;
	unsigned int val;
	int ret;

	mutex_lock(&lcdb->lock);
	ret = regmap_read(lcdb->regmap, lcdb->base + rail->voltage_reg, &val);
	mutex_unlock(&lcdb->lock);
	if (ret)
		return ret;

	return val & GENMASK(4, 0);
}

static int pm660l_lcdb_set_voltage_sel(struct regulator_dev *rdev,
				       unsigned int selector)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	struct pm660l_lcdb *lcdb = rail->lcdb;
	unsigned int old_sel, other_sel, old_bst_sel, new_bst_sel;
	unsigned int other_uv, new_uv, old_bst_uv, new_bst_uv;
	unsigned int ldo_uv, ncp_uv, boost_uv;
	unsigned int other_reg;
	bool boost_raised;
	int ret, rollback_ret;

	if (selector >= PM660L_LCDB_N_VOLTAGES)
		return -EINVAL;

	mutex_lock(&lcdb->lock);
	if (lcdb->fault_latched) {
		ret = -EIO;
		goto out;
	}

	other_reg = rail->request_bit == PM660L_LCDB_REQ_LDO ?
		    PM660L_LCDB_NCP_VOLT_REG : PM660L_LCDB_LDO_VOLT_REG;

	ret = regmap_read(lcdb->regmap, lcdb->base + rail->voltage_reg,
			  &old_sel);
	if (ret)
		goto out;
	ret = regmap_read(lcdb->regmap, lcdb->base + other_reg, &other_sel);
	if (ret)
		goto out;
	ret = regmap_read(lcdb->regmap,
			  lcdb->base + PM660L_LCDB_BST_VOLT_REG, &old_bst_sel);
	if (ret)
		goto out;

	old_sel &= GENMASK(4, 0);
	other_sel &= GENMASK(4, 0);
	old_bst_sel &= GENMASK(4, 0);
	other_uv = pm660l_lcdb_list_voltage(other_sel);
	new_uv = pm660l_lcdb_list_voltage(selector);
	old_bst_uv = PM660L_LCDB_BST_MIN_UV +
		     old_bst_sel * PM660L_LCDB_BST_STEP_UV;

	ldo_uv = rail->request_bit == PM660L_LCDB_REQ_LDO ?
		 new_uv : other_uv;
	ncp_uv = rail->request_bit == PM660L_LCDB_REQ_NCP ?
		 new_uv : other_uv;

	ret = pm660l_lcdb_calc_boost(ldo_uv, ncp_uv,
				     PM660L_LCDB_DEFAULT_HEADROOM_UV,
				     &boost_uv);
	if (ret) {
		ret = -EINVAL;
		goto out;
	}
	ret = pm660l_lcdb_bst_map_voltage(boost_uv,
					  PM660L_LCDB_BST_MAX_UV,
					  &new_bst_sel);
	if (ret) {
		ret = -EINVAL;
		goto out;
	}
	new_bst_uv = PM660L_LCDB_BST_MIN_UV +
		     new_bst_sel * PM660L_LCDB_BST_STEP_UV;

	boost_raised = new_bst_uv > old_bst_uv;
	if (boost_raised) {
		ret = regmap_update_bits(lcdb->regmap,
					 lcdb->base + PM660L_LCDB_BST_VOLT_REG,
					 GENMASK(4, 0), new_bst_sel);
		if (ret) {
			rollback_ret = regmap_update_bits(lcdb->regmap,
						  lcdb->base + PM660L_LCDB_BST_VOLT_REG,
						  GENMASK(4, 0), old_bst_sel);
			if (rollback_ret) {
				lcdb->fault_latched = true;
				dev_err(lcdb->dev,
					"boost write failed (%d), rollback failed (%d)\n",
					ret, rollback_ret);
			}
			goto out;
		}
	}

	ret = regmap_update_bits(lcdb->regmap,
				 lcdb->base + rail->voltage_reg,
				 GENMASK(4, 0), selector);
	if (ret) {
		rollback_ret = regmap_update_bits(lcdb->regmap,
						  lcdb->base + rail->voltage_reg,
						  GENMASK(4, 0), old_sel);
		if (rollback_ret) {
			lcdb->fault_latched = true;
			dev_err(lcdb->dev,
				"output write failed (%d), output rollback failed (%d); keeping boost conservative\n",
				ret, rollback_ret);
			goto out;
		}

		if (boost_raised) {
			rollback_ret = regmap_update_bits(lcdb->regmap,
						  lcdb->base + PM660L_LCDB_BST_VOLT_REG,
						  GENMASK(4, 0), old_bst_sel);
			if (rollback_ret) {
				lcdb->fault_latched = true;
				dev_err(lcdb->dev,
					"output write failed (%d), boost rollback failed (%d)\n",
					ret, rollback_ret);
			}
		}
		goto out;
	}

	/* Lower boost only after the output has safely reached its new value. */
	if (!boost_raised && new_bst_uv < old_bst_uv) {
		ret = regmap_update_bits(lcdb->regmap,
					 lcdb->base + PM660L_LCDB_BST_VOLT_REG,
					 GENMASK(4, 0), new_bst_sel);
		if (ret)
			dev_err(lcdb->dev,
				"output voltage changed but boost reduction failed: %d\n",
				ret);
	}

out:
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int pm660l_lcdb_regulator_map_voltage(struct regulator_dev *rdev,
				   int min_uv, int max_uv)
{
	unsigned int selector;
	int ret;

	if (min_uv < 0 || max_uv < 0)
		return -EINVAL;

	ret = pm660l_lcdb_map_voltage((unsigned int)min_uv,
				      (unsigned int)max_uv, &selector);
	if (ret)
		return -EINVAL;

	return selector;
}

static int pm660l_lcdb_regulator_list_voltage(struct regulator_dev *rdev,
				    unsigned int selector)
{
	return pm660l_lcdb_list_voltage(selector);
}

static const struct regulator_ops pm660l_lcdb_ops = {
	.enable = pm660l_lcdb_enable,
	.disable = pm660l_lcdb_disable,
	.is_enabled = pm660l_lcdb_is_enabled,
	.get_voltage_sel = pm660l_lcdb_get_voltage_sel,
	.set_voltage_sel = pm660l_lcdb_set_voltage_sel,
	.map_voltage = pm660l_lcdb_regulator_map_voltage,
	.list_voltage = pm660l_lcdb_regulator_list_voltage,
};

static irqreturn_t pm660l_lcdb_sc_irq_thread(int irq, void *data)
{
	struct pm660l_lcdb *lcdb = data;
	unsigned int status, misc, output_status;
	s64 elapsed;
	int ret;

	/* First sample the interrupt and wake mode without holding the lock
	 * across the panel blanking delay.
	 */
	mutex_lock(&lcdb->lock);
	ret = regmap_read(lcdb->regmap,
			  lcdb->base + PM660L_LCDB_INT_RT_REG, &status);
	if (!ret && (status & PM660L_LCDB_SC_ERROR_BIT))
		ret = regmap_read(lcdb->regmap,
				  lcdb->base + PM660L_LCDB_MISC_CTL_REG, &misc);
	mutex_unlock(&lcdb->lock);
	if (ret)
		goto fault_unlocked;
	if (!(status & PM660L_LCDB_SC_ERROR_BIT))
		return IRQ_HANDLED;

	if (misc & PM660L_LCDB_TOUCH_WAKE_BIT) {
		/* Touch-wake mode needs the short display-blanking interval. */
		usleep_range(300, 310);
		mutex_lock(&lcdb->lock);
		ret = pm660l_lcdb_read_output_status(lcdb, &output_status);
		mutex_unlock(&lcdb->lock);
		if (ret)
			goto fault_unlocked;

		/* The downstream handler treats either missing output-good bit
		 * as a confirmed short-circuit event in touch-wake mode.
		 */
		if (output_status == PM660L_LCDB_REQ_MASK)
			return IRQ_HANDLED;
	} else {
		/* Outside touch-wake mode, confirm the latched event after blanking. */
		usleep_range(2000, 2100);
		mutex_lock(&lcdb->lock);
		ret = regmap_read(lcdb->regmap,
				  lcdb->base + PM660L_LCDB_INT_RT_REG, &status);
		mutex_unlock(&lcdb->lock);
		if (ret)
			goto fault_unlocked;
		if (!(status & PM660L_LCDB_SC_ERROR_BIT))
			return IRQ_HANDLED;
	}

	mutex_lock(&lcdb->lock);
	if (lcdb->fault_latched)
		goto handled;

	/* Ignore a stale SC indication after the block was already shut down. */
	if (!lcdb->request_mask && !lcdb->module_enabled)
		goto handled;

	/* A bootloader-enabled block can fault before any consumer requests it. */
	if (!lcdb->request_mask) {
		ret = pm660l_lcdb_module_disable(lcdb);
		if (ret)
			goto fault_locked;
		lcdb->fault_latched = true;
		dev_err(lcdb->dev,
			"short circuit with no consumer request; LCDB shut down\n");
		goto handled;
	}

	elapsed = ktime_us_delta(ktime_get(), lcdb->sc_window_start);
	if (!lcdb->sc_count || elapsed > PM660L_LCDB_SC_RETRY_WINDOW_US) {
		lcdb->sc_count = 0;
		lcdb->sc_window_start = ktime_get();
	}
	if (lcdb->sc_count >= PM660L_LCDB_SC_RETRY_MAX) {
		ret = -ELOOP;
		goto fault_locked;
	}
	lcdb->sc_count++;

	ret = pm660l_lcdb_module_disable(lcdb);
	if (ret)
		goto fault_locked;

	/* Match the PM660L discharge interval before restarting the converter. */
	usleep_range(10000, 10100);
	ret = pm660l_lcdb_module_enable(lcdb, false);
	if (ret)
		goto fault_locked;

	/* Do not report recovery until both outputs and the SC latch are clear. */
	ret = pm660l_lcdb_read_output_status(lcdb, &output_status);
	if (ret)
		goto fault_locked;
	if (output_status != PM660L_LCDB_REQ_MASK) {
		ret = -EIO;
		goto fault_locked;
	}
	ret = regmap_read(lcdb->regmap,
			  lcdb->base + PM660L_LCDB_INT_RT_REG, &status);
	if (ret || (status & PM660L_LCDB_SC_ERROR_BIT)) {
		if (!ret)
			ret = -EIO;
		goto fault_locked;
	}

	dev_warn(lcdb->dev, "LCDB short-circuit event recovered (%u/%u)\n",
		 lcdb->sc_count, PM660L_LCDB_SC_RETRY_MAX);
	goto handled;

fault_locked:
	lcdb->fault_latched = true;
	dev_err(lcdb->dev,
		"LCDB short-circuit recovery failed; outputs locked off (error %d)\n",
		ret);
handled:
	mutex_unlock(&lcdb->lock);
	return IRQ_HANDLED;

fault_unlocked:
	mutex_lock(&lcdb->lock);
	{
		int disable_ret = pm660l_lcdb_module_disable(lcdb);

		lcdb->fault_latched = true;
		dev_err(lcdb->dev,
			"LCDB short-circuit status read failed (%d), shutdown result %d\n",
			ret, disable_ret);
	}
	mutex_unlock(&lcdb->lock);
	return IRQ_HANDLED;
}

static int pm660l_lcdb_register_rail(struct pm660l_lcdb *lcdb,
				     struct pm660l_lcdb_rail *rail,
				     struct device_node *node,
				     const char *name,
				     unsigned int id,
				     unsigned int request_bit,
				     unsigned int voltage_reg)
{
	struct regulator_config config = { };
	struct regulator_dev *rdev;

	rail->lcdb = lcdb;
	rail->name = name;
	rail->request_bit = request_bit;
	rail->voltage_reg = voltage_reg;
	rail->desc.name = name;
	rail->desc.of_match = request_bit == PM660L_LCDB_REQ_LDO ? "ldo" : "ncp";
	rail->desc.id = id;
	rail->desc.owner = THIS_MODULE;
	rail->desc.type = REGULATOR_VOLTAGE;
	rail->desc.ops = &pm660l_lcdb_ops;
	rail->desc.n_voltages = PM660L_LCDB_N_VOLTAGES;

	config.dev = lcdb->dev;
	config.driver_data = rail;
	config.of_node = node;

	rdev = devm_regulator_register(lcdb->dev, &rail->desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(lcdb->dev, PTR_ERR(rdev),
				     "failed to register %s regulator\n", name);

	rail->rdev = rdev;
	return 0;
}

static int pm660l_lcdb_probe(struct platform_device *pdev)
{
	struct device_node *ldo_node, *ncp_node;
	struct pm660l_lcdb *lcdb;
	unsigned int val;
	int irq, ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	lcdb = devm_kzalloc(&pdev->dev, sizeof(*lcdb), GFP_KERNEL);
	if (!lcdb)
		return -ENOMEM;

	lcdb->dev = &pdev->dev;
	mutex_init(&lcdb->lock);

	ret = of_property_read_u32(pdev->dev.of_node, "reg", &lcdb->base);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "missing LCDB reg base\n");

	lcdb->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!lcdb->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "parent SPMI regmap unavailable\n");

	lcdb->pmic = qcom_pmic_get(&pdev->dev);
	if (IS_ERR(lcdb->pmic))
		return dev_err_probe(&pdev->dev, PTR_ERR(lcdb->pmic),
				     "failed to obtain PMIC revision\n");
	if (lcdb->pmic->subtype != PM660L_SUBTYPE)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "parent PMIC is not PM660L (subtype 0x%x)\n",
				     lcdb->pmic->subtype);

	/* Read the live module state before touching MODULE_RDY. */
	ret = regmap_read(lcdb->regmap,
			  lcdb->base + PM660L_LCDB_ENABLE_REG, &val);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to read module enable state\n");
	lcdb->module_enabled = !!(val & PM660L_LCDB_MODULE_EN_BIT);

	/* Match the vendor handoff: set MODULE_RDY only while the block is off. */
	if (!lcdb->module_enabled) {
		ret = regmap_read(lcdb->regmap,
				  lcdb->base + PM660L_LCDB_MODULE_RDY_REG, &val);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to read MODULE_RDY\n");
		if (!(val & BIT(7))) {
			ret = regmap_update_bits(lcdb->regmap,
						 lcdb->base + PM660L_LCDB_MODULE_RDY_REG,
						 BIT(7), BIT(7));
			if (ret)
				return dev_err_probe(&pdev->dev, ret,
						     "failed to set MODULE_RDY\n");
		}
	}

	/* Initial request ownership is empty; physical boot state is preserved. */
	ret = pm660l_lcdb_read_output_status(lcdb, &lcdb->output_good_mask);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to read output-good status\n");

	ldo_node = of_get_child_by_name(pdev->dev.of_node, "ldo");
	if (!ldo_node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing ldo regulator child\n");
	ncp_node = of_get_child_by_name(pdev->dev.of_node, "ncp");
	if (!ncp_node) {
		of_node_put(ldo_node);
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing ncp regulator child\n");
	}

	ret = pm660l_lcdb_register_rail(lcdb, &lcdb->ldo, ldo_node,
					"pm660l-lcdb-ldo", 0,
					PM660L_LCDB_REQ_LDO,
					PM660L_LCDB_LDO_VOLT_REG);
	if (!ret)
		ret = pm660l_lcdb_register_rail(lcdb, &lcdb->ncp, ncp_node,
						"pm660l-lcdb-ncp", 1,
						PM660L_LCDB_REQ_NCP,
						PM660L_LCDB_NCP_VOLT_REG);
	of_node_put(ncp_node);
	of_node_put(ldo_node);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, lcdb);

	/*
	 * PM660L revisions using IRQ-driven short-circuit recovery must have
	 * the verified USID 3 "sc-irq" route. Older revisions use the
	 * hardware-specific SCP workaround instead.
	 */
	if (!pm660l_lcdb_needs_ncp_scp_wa(lcdb->pmic->major)) {
		irq = platform_get_irq_byname(pdev, PM660L_LCDB_IRQ_NAME);
		if (irq < 0)
			return dev_err_probe(&pdev->dev, irq,
					     "required SC IRQ is unavailable\\n");

		ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
						pm660l_lcdb_sc_irq_thread,
						IRQF_ONESHOT,
						"pm660l-lcdb-sc", lcdb);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to request SC IRQ\\n");
	}

	dev_info(&pdev->dev,
		 "PM660L LCDB registered (PMIC v%u.%u, module %s, outputs 0x%x)\n",
		 lcdb->pmic->major, lcdb->pmic->minor,
		 lcdb->module_enabled ? "enabled" : "disabled",
		 lcdb->output_good_mask);
	return 0;
}

static const struct of_device_id pm660l_lcdb_of_match[] = {
	{ .compatible = "qcom,pm660l-lcdb-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, pm660l_lcdb_of_match);

static struct platform_driver pm660l_lcdb_driver = {
	.probe = pm660l_lcdb_probe,
	.driver = {
		.name = "qcom-pm660l-lcdb-regulator",
		.of_match_table = pm660l_lcdb_of_match,
	},
};
module_platform_driver(pm660l_lcdb_driver);

MODULE_DESCRIPTION("Qualcomm PM660L LCDB regulator driver");
MODULE_LICENSE("GPL");

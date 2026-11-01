// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm PM660L LCDB regulator driver.
 *
 * The PM660L LCDB has two voltage outputs sharing one boost converter and
 * one module-enable bit.  Keep this implementation PM660L-specific: the
 * register layout and boost encoding differ from other Qualcomm LCDB blocks.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/mutex.h>
#include <linux/bitops.h>

#include <soc/qcom/qcom-spmi-pmic.h>

#include "qcom-pm660l-lcdb-internal.h"

#define PM660L_LCDB_DRV_NAME          "qcom-pm660l-lcdb"
#define PM660L_LCDB_MODULE_RDY_BIT    BIT(7)
#define PM660L_LCDB_EN_TOUCH_WAKE_BIT  BIT(3)
#define PM660L_LCDB_MAX_STARTUP_US    100000
#define PM660L_LCDB_POLL_US           1000
#define PM660L_LCDB_SC_CONFIRM_US     2000
#define PM660L_LCDB_SC_RECOVERY_US    10000
#define PM660L_LCDB_SC_WINDOW_US      1000000
#define PM660L_LCDB_SC_MAX_RETRIES    10

struct pm660l_lcdb;

struct pm660l_lcdb_rail {
	struct regulator_desc desc;
	struct regulator_dev *rdev;
	struct pm660l_lcdb *lcdb;
	struct device_node *of_node;
	unsigned int request_bit;
	unsigned int voltage_reg;
	const char *name;
};

struct pm660l_lcdb {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
	unsigned int base;
	unsigned int request_mask;
	unsigned int output_good_mask;
	unsigned int boost_uv;
	unsigned int headroom_uv;
	unsigned int pmic_rev4;
	int sc_irq;
	bool module_enabled;
	bool fault_latched;
	bool hw_state_unknown;
	unsigned int sc_count;
	ktime_t last_sc_time;
	struct pm660l_lcdb_rail ldo;
	struct pm660l_lcdb_rail ncp;
};

static int pm660l_lcdb_read(struct pm660l_lcdb *lcdb, unsigned int reg,
			    unsigned int *value)
{
	return regmap_read(lcdb->regmap, lcdb->base + reg, value);
}

static int pm660l_lcdb_update(struct pm660l_lcdb *lcdb, unsigned int reg,
			      unsigned int mask, unsigned int value)
{
	return regmap_update_bits(lcdb->regmap, lcdb->base + reg, mask, value);
}

/*
 * PM660L requires a write to STS3/STS4 before their status bytes are read.
 * Keep the two-byte transaction together; do not replace it with read-only
 * accesses without hardware evidence that the status semantics changed.
 */
static int pm660l_lcdb_read_output_status(struct pm660l_lcdb *lcdb,
					  unsigned int *status)
{
	u8 sts[2] = { 0, 0 };
	u8 clear[2] = { 0, 0 };
	int ret;

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

static int pm660l_lcdb_read_module_enabled(struct pm660l_lcdb *lcdb,
					   bool *enabled)
{
	unsigned int value;
	int ret;

	ret = pm660l_lcdb_read(lcdb, PM660L_LCDB_ENABLE_REG, &value);
	if (ret)
		return ret;

	*enabled = !!(value & PM660L_LCDB_MODULE_EN_BIT);
	return 0;
}

static int pm660l_lcdb_wait_output(struct pm660l_lcdb *lcdb,
				    unsigned int rail_bit)
{
	unsigned int status;
	unsigned int elapsed;
	int ret;

	for (elapsed = 0; elapsed < PM660L_LCDB_MAX_STARTUP_US;
	     elapsed += PM660L_LCDB_POLL_US) {
		ret = pm660l_lcdb_read_output_status(lcdb, &status);
		if (ret)
			return ret;

		if ((status & rail_bit) == rail_bit) {
			lcdb->output_good_mask = status;
			return 0;
		}

		usleep_range(PM660L_LCDB_POLL_US,
			     PM660L_LCDB_POLL_US + 200);
	}

	return -ETIMEDOUT;
}

static int pm660l_lcdb_revision_workaround(struct pm660l_lcdb *lcdb)
{
	int ret;

	if (!pm660l_lcdb_needs_ncp_scp_wa(lcdb->pmic_rev4))
		return 0;

	/* The PM660L NCP SCP workaround requires a complete boost discharge. */
	ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_ENABLE_REG,
				 PM660L_LCDB_MODULE_EN_BIT,
				 PM660L_LCDB_MODULE_EN_BIT);
	if (ret)
		return ret;

	ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_ENABLE_REG,
				 PM660L_LCDB_MODULE_EN_BIT, 0);
	if (ret)
		return ret;

	usleep_range(10000, 10100);

	ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_MISC_CTL_REG,
				 PM660L_LCDB_SCP_DISABLE_BIT,
				 PM660L_LCDB_SCP_DISABLE_BIT);
	if (ret)
		return ret;

	usleep_range(1000, 1100);

	ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_MISC_CTL_REG,
				 PM660L_LCDB_SCP_DISABLE_BIT, 0);
	if (ret)
		return ret;

	usleep_range(1000, 1100);
	return 0;
}

static int pm660l_lcdb_module_enable(struct pm660l_lcdb *lcdb,
				      unsigned int rail_bit)
{
	bool enabled;
	int ret;

	if (lcdb->fault_latched || lcdb->hw_state_unknown)
		return -EIO;

	ret = pm660l_lcdb_read_module_enabled(lcdb, &enabled);
	if (ret)
		return ret;

	if (!enabled) {
		ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_MODULE_RDY_REG,
					 PM660L_LCDB_MODULE_RDY_BIT,
					 PM660L_LCDB_MODULE_RDY_BIT);
		if (ret)
			return ret;

		ret = pm660l_lcdb_revision_workaround(lcdb);
		if (ret)
			return ret;

		ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_ENABLE_REG,
					 PM660L_LCDB_MODULE_EN_BIT,
					 PM660L_LCDB_MODULE_EN_BIT);
		if (ret)
			return ret;
	}

	lcdb->module_enabled = true;
	ret = pm660l_lcdb_wait_output(lcdb, rail_bit);
	if (ret) {
		/*
		 * Do not drop the module if another consumer already owns a
		 * rail.  If this was the first request, the caller will unwind
		 * by disabling the shared module.
		 */
		return ret;
	}

	return 0;
}

static int pm660l_lcdb_module_disable(struct pm660l_lcdb *lcdb)
{
	int ret;

	ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_ENABLE_REG,
				 PM660L_LCDB_MODULE_EN_BIT, 0);
	if (ret)
		return ret;

	lcdb->module_enabled = false;
	lcdb->output_good_mask = 0;
	return 0;
}

static int pm660l_lcdb_enable(struct regulator_dev *rdev)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	struct pm660l_lcdb *lcdb = rail->lcdb;
	unsigned int new_mask;
	int enable_module;
	int ret;

	mutex_lock(&lcdb->lock);

	if (lcdb->fault_latched || lcdb->hw_state_unknown) {
		ret = -EIO;
		goto out;
	}

	ret = pm660l_lcdb_next_enable_mask(lcdb->request_mask,
					   rail->request_bit, &new_mask,
					   &enable_module);
	if (ret) {
		ret = -EINVAL;
		goto out;
	}
	(void)enable_module;

	ret = pm660l_lcdb_module_enable(lcdb, rail->request_bit);
	if (ret) {
		/*
		 * A failed first enable must not leave the shared boost running.
		 * Preserve the original error if the best-effort shutdown works;
		 * mark state unknown if shutdown itself fails.
		 */
		if (!lcdb->request_mask) {
			int shutdown_ret = pm660l_lcdb_module_disable(lcdb);

			if (shutdown_ret)
				lcdb->hw_state_unknown = true;
		}
		goto out;
	}

	lcdb->request_mask = new_mask;
out:
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int pm660l_lcdb_disable(struct regulator_dev *rdev)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	struct pm660l_lcdb *lcdb = rail->lcdb;
	unsigned int new_mask;
	int disable_module;
	int ret;

	mutex_lock(&lcdb->lock);

	ret = pm660l_lcdb_next_disable_mask(lcdb->request_mask,
					    rail->request_bit, &new_mask,
					    &disable_module);
	if (ret) {
		/* A bootloader-enabled rail may not have a Linux request bit. */
		ret = 0;
		goto out;
	}

	if (disable_module) {
		ret = pm660l_lcdb_module_disable(lcdb);
		if (ret)
			goto out;
	}

	lcdb->request_mask = new_mask;
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
	if (!ret)
		lcdb->output_good_mask = status;
	mutex_unlock(&lcdb->lock);

	if (ret)
		return ret;

	return !!(status & rail->request_bit);
}

static int pm660l_lcdb_get_voltage_sel(struct regulator_dev *rdev)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	unsigned int value;
	int ret;

	ret = pm660l_lcdb_read(rail->lcdb, rail->voltage_reg, &value);
	if (ret)
		return ret;

	return value & GENMASK(4, 0);
}

static int pm660l_lcdb_set_voltage_sel(struct regulator_dev *rdev,
				       unsigned int selector)
{
	struct pm660l_lcdb_rail *rail = rdev_get_drvdata(rdev);
	struct pm660l_lcdb *lcdb = rail->lcdb;
	unsigned int old_ldo_sel, old_ncp_sel, old_bst_sel;
	unsigned int new_ldo_sel, new_ncp_sel, new_bst_sel;
	unsigned int new_ldo_uv, new_ncp_uv;
	unsigned int old_bst_uv, new_bst_uv, boost_value;
	bool boost_raised = false;
	int ret, rollback_ret;

	if (selector >= PM660L_LCDB_N_VOLTAGES)
		return -EINVAL;

	mutex_lock(&lcdb->lock);
	if (lcdb->fault_latched || lcdb->hw_state_unknown) {
		ret = -EIO;
		goto out;
	}

	ret = pm660l_lcdb_read(lcdb, PM660L_LCDB_LDO_VOLT_REG, &boost_value);
	if (ret)
		goto out;
	old_ldo_sel = boost_value & GENMASK(4, 0);

	ret = pm660l_lcdb_read(lcdb, PM660L_LCDB_NCP_VOLT_REG, &boost_value);
	if (ret)
		goto out;
	old_ncp_sel = boost_value & GENMASK(4, 0);

	if (rail->request_bit == PM660L_LCDB_REQ_LDO)
		new_ldo_sel = selector;
	else
		new_ldo_sel = old_ldo_sel;
	if (rail->request_bit == PM660L_LCDB_REQ_NCP)
		new_ncp_sel = selector;
	else
		new_ncp_sel = old_ncp_sel;

	new_ldo_uv = pm660l_lcdb_list_voltage(new_ldo_sel);
	new_ncp_uv = pm660l_lcdb_list_voltage(new_ncp_sel);

	ret = pm660l_lcdb_read(lcdb, PM660L_LCDB_BST_VOLT_REG, &boost_value);
	if (ret)
		goto out;
	old_bst_sel = boost_value & GENMASK(4, 0);
	old_bst_uv = PM660L_LCDB_BST_MIN_UV +
		     old_bst_sel * PM660L_LCDB_BST_STEP_UV;

	ret = pm660l_lcdb_calc_boost(new_ldo_uv, new_ncp_uv,
				      lcdb->headroom_uv, &new_bst_uv);
	if (ret)
		goto out;
	ret = pm660l_lcdb_bst_map_voltage(new_bst_uv,
					  PM660L_LCDB_BST_MAX_UV,
					  &new_bst_sel);
	if (ret)
		goto out;
	new_bst_uv = PM660L_LCDB_BST_MIN_UV +
		     new_bst_sel * PM660L_LCDB_BST_STEP_UV;

	/* Raise boost before raising an output; lower it only afterwards. */
	if (new_bst_uv > old_bst_uv) {
		ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_BST_VOLT_REG,
					 GENMASK(4, 0), new_bst_sel);
		if (ret)
			goto out;
		boost_raised = true;
	}

	ret = pm660l_lcdb_update(lcdb, rail->voltage_reg, GENMASK(4, 0),
				 selector);
	if (ret) {
		if (boost_raised) {
			rollback_ret = pm660l_lcdb_update(lcdb,
					PM660L_LCDB_BST_VOLT_REG,
					GENMASK(4, 0), old_bst_sel);
			if (rollback_ret)
				lcdb->hw_state_unknown = true;
		}
		goto out;
	}

	if (new_bst_uv < old_bst_uv) {
		ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_BST_VOLT_REG,
					 GENMASK(4, 0), new_bst_sel);
		if (ret)
			/* Output is at its new value; old boost remains safe. */
			goto out;
	}

	lcdb->boost_uv = new_bst_uv;
	ret = 0;
out:
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int pm660l_lcdb_regulator_map_voltage(struct regulator_dev *rdev,
				   int min_uv, int max_uv)
{
	unsigned int selector;
	int ret;

	ret = pm660l_lcdb_map_voltage(min_uv, max_uv, &selector);
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
	unsigned int status, misc, outputs;
	s64 elapsed;
	int ret;
	irqreturn_t irqret = IRQ_HANDLED;
	bool confirmed = false;

	mutex_lock(&lcdb->lock);

	ret = pm660l_lcdb_read(lcdb, PM660L_LCDB_INT_RT_REG, &status);
	if (ret)
		goto out;

	if (!(status & PM660L_LCDB_SC_ERROR_BIT)) {
		irqret = IRQ_NONE;
		goto out;
	}

	ret = pm660l_lcdb_read(lcdb, PM660L_LCDB_MISC_CTL_REG, &misc);
	if (ret)
		goto out;

	if (misc & PM660L_LCDB_EN_TOUCH_WAKE_BIT) {
		/*
		 * Touch-wake mode uses the two per-output VREG_OK status
		 * bytes after the LCDB blanking interval.  The helper
		 * preserves the vendor-required STS3/STS4 pre-write.
		 */
		usleep_range(300, 310);
		ret = pm660l_lcdb_read_output_status(lcdb, &outputs);
		if (ret)
			goto out;
		confirmed = outputs != PM660L_LCDB_REQ_MASK;
	} else {
		/* Confirm a persistent short-circuit after blanking time. */
		usleep_range(PM660L_LCDB_SC_CONFIRM_US,
			     PM660L_LCDB_SC_CONFIRM_US + 100);
		ret = pm660l_lcdb_read(lcdb, PM660L_LCDB_INT_RT_REG,
				       &status);
		if (ret)
			goto out;
		confirmed = !!(status & PM660L_LCDB_SC_ERROR_BIT);
	}

	if (!confirmed)
		goto out;

	/* Never restart after an unsuccessful shutdown. */
	ret = pm660l_lcdb_module_disable(lcdb);
	if (ret) {
		lcdb->hw_state_unknown = true;
		goto out;
	}

	elapsed = ktime_us_delta(ktime_get(), lcdb->last_sc_time);
	if (!ktime_to_ns(lcdb->last_sc_time) ||
	    elapsed > PM660L_LCDB_SC_WINDOW_US)
		lcdb->sc_count = 0;
	lcdb->sc_count++;
	lcdb->last_sc_time = ktime_get();

	if (lcdb->sc_count > PM660L_LCDB_SC_MAX_RETRIES) {
		lcdb->fault_latched = true;
		goto out;
	}

	if (!lcdb->request_mask)
		goto out;

	usleep_range(PM660L_LCDB_SC_RECOVERY_US,
		     PM660L_LCDB_SC_RECOVERY_US + 100);
	ret = pm660l_lcdb_module_enable(lcdb, lcdb->request_mask);
	if (ret)
		lcdb->hw_state_unknown = true;

out:
	mutex_unlock(&lcdb->lock);
	return irqret;
}

static int pm660l_lcdb_register_rail(struct pm660l_lcdb *lcdb,
				     struct pm660l_lcdb_rail *rail,
				     struct device_node *node,
				     const char *name,
				     unsigned int request_bit,
				     unsigned int voltage_reg)
{
	struct regulator_config config = { };
	struct regulator_dev *rdev;

	rail->lcdb = lcdb;
	rail->of_node = node;
	rail->name = name;
	rail->request_bit = request_bit;
	rail->voltage_reg = voltage_reg;
	rail->desc.name = name;
	rail->desc.of_match = name;
	rail->desc.id = -1;
	rail->desc.owner = THIS_MODULE;
	rail->desc.type = REGULATOR_VOLTAGE;
	rail->desc.ops = &pm660l_lcdb_ops;
	rail->desc.n_voltages = PM660L_LCDB_N_VOLTAGES;
	rail->desc.enable_time = 10000;

	config.dev = lcdb->dev;
	config.driver_data = rail;
	config.of_node = node;
	config.regmap = lcdb->regmap;

	rdev = devm_regulator_register(lcdb->dev, &rail->desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(lcdb->dev, PTR_ERR(rdev),
				     "failed to register %s regulator\n",
				     name);

	rail->rdev = rdev;
	return 0;
}

static int pm660l_lcdb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *ldo_node, *ncp_node;
	const struct qcom_spmi_pmic *pmic;
	struct pm660l_lcdb *lcdb;
	unsigned int value, status;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	lcdb = devm_kzalloc(dev, sizeof(*lcdb), GFP_KERNEL);
	if (!lcdb)
		return -ENOMEM;

	ret = of_property_read_u32(dev->of_node, "reg", &lcdb->base);
	if (ret)
		return dev_err_probe(dev, ret, "missing LCDB register base\n");

	lcdb->regmap = dev_get_regmap(dev->parent, NULL);
	if (!lcdb->regmap)
		return dev_err_probe(dev, -ENODEV, "parent regmap unavailable\n");

	pmic = qcom_pmic_get(dev);
	if (IS_ERR(pmic))
		return dev_err_probe(dev, PTR_ERR(pmic), "PMIC revision unavailable\n");
	if (pmic->subtype != PM660L_SUBTYPE)
		return dev_err_probe(dev, -ENODEV, "not a PM660L PMIC\n");

	lcdb->dev = dev;
	lcdb->pmic_rev4 = pmic->major;
	lcdb->headroom_uv = PM660L_LCDB_DEFAULT_HEADROOM_UV;
	lcdb->sc_irq = platform_get_irq_byname(pdev, "sc-irq");
	if (lcdb->sc_irq == -EPROBE_DEFER)
		return lcdb->sc_irq;
	if (lcdb->sc_irq < 0)
		dev_warn(dev, "short-circuit IRQ unavailable; fault recovery disabled\n");

	mutex_init(&lcdb->lock);
	platform_set_drvdata(pdev, lcdb);

	ldo_node = of_get_child_by_name(dev->of_node, "ldo");
	ncp_node = of_get_child_by_name(dev->of_node, "ncp");
	if (!ldo_node || !ncp_node) {
		ret = -EINVAL;
		dev_err(dev, "both ldo and ncp child nodes are required\n");
		goto err_put_nodes;
	}

	ret = pm660l_lcdb_read_module_enabled(lcdb, &lcdb->module_enabled);
	if (ret)
		goto err_put_nodes;

	ret = pm660l_lcdb_read_output_status(lcdb, &status);
	if (ret)
		goto err_put_nodes;
	lcdb->output_good_mask = status;
	/*
	 * Preserve bootloader-enabled outputs as active until their consumers
	 * release them.  Do not infer the shared module state from VREG_OK.
	 */
	lcdb->request_mask = lcdb->module_enabled ? status : 0;

	ret = pm660l_lcdb_read(lcdb, PM660L_LCDB_BST_VOLT_REG, &value);
	if (ret)
		goto err_put_nodes;
	lcdb->boost_uv = PM660L_LCDB_BST_MIN_UV +
			 (value & GENMASK(4, 0)) * PM660L_LCDB_BST_STEP_UV;

	if (!lcdb->module_enabled) {
		ret = pm660l_lcdb_update(lcdb, PM660L_LCDB_MODULE_RDY_REG,
					 PM660L_LCDB_MODULE_RDY_BIT,
					 PM660L_LCDB_MODULE_RDY_BIT);
		if (ret)
			goto err_put_nodes;
	}

	ret = pm660l_lcdb_register_rail(lcdb, &lcdb->ldo, ldo_node,
					"ldo", PM660L_LCDB_REQ_LDO,
					PM660L_LCDB_LDO_VOLT_REG);
	if (ret)
		goto err_put_nodes;

	ret = pm660l_lcdb_register_rail(lcdb, &lcdb->ncp, ncp_node,
					"ncp", PM660L_LCDB_REQ_NCP,
					PM660L_LCDB_NCP_VOLT_REG);
	if (ret)
		goto err_put_nodes;

	if (lcdb->sc_irq >= 0 &&
	    !pm660l_lcdb_needs_ncp_scp_wa(lcdb->pmic_rev4)) {
		ret = devm_request_threaded_irq(dev, lcdb->sc_irq, NULL,
						pm660l_lcdb_sc_irq_thread,
						IRQF_ONESHOT,
						"pm660l-lcdb-sc", lcdb);
		if (ret)
			goto err_put_nodes;
	}

	dev_info(dev, "PM660L LCDB registered (rev4=%u, module=%s, outputs=0x%x)\n",
		 lcdb->pmic_rev4, lcdb->module_enabled ? "on" : "off",
		 lcdb->output_good_mask);

	of_node_put(ldo_node);
	of_node_put(ncp_node);
	return 0;

err_put_nodes:
	of_node_put(ldo_node);
	of_node_put(ncp_node);
	return ret;
}

static const struct of_device_id pm660l_lcdb_of_match[] = {
	{ .compatible = "qcom,pm660l-lcdb" },
	{ }
};
MODULE_DEVICE_TABLE(of, pm660l_lcdb_of_match);

static struct platform_driver pm660l_lcdb_driver = {
	.probe = pm660l_lcdb_probe,
	.driver = {
		.name = PM660L_LCDB_DRV_NAME,
		.of_match_table = pm660l_lcdb_of_match,
	},
};
module_platform_driver(pm660l_lcdb_driver);

MODULE_DESCRIPTION("Qualcomm PM660L LCDB regulator driver");
MODULE_LICENSE("GPL");

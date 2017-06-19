/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/ratelimit.h>

unsigned int temp_threshold = 38;
module_param(temp_threshold, int, 0644);

static struct thermal_info {
        uint32_t cpuinfo_max_freq;
        uint32_t limited_max_freq;
        unsigned int safe_diff;
        bool throttling;
        bool pending_change;
} info = {
        .cpuinfo_max_freq = LONG_MAX,
        .limited_max_freq = LONG_MAX,
        .safe_diff = 5,
        .throttling = false,
        .pending_change = false,
};

enum thermal_freqs {
        FREQ_NOTE_7             = 729600,
        FREQ_HELL               = 960000,
        FREQ_BBQ                = 1036800,
        FREQ_MICROWAVE          = 1190400,
        FREQ_VERY_HOT           = 1267200,
        FREQ_HOT                = 1497600,
        FREQ_WARM               = 1574400,
        FREQ_ZIPPY              = 1728000,
        FREQ_MAX                = 1958400,
};

enum threshold_levels {
        LEVEL_NOTE_7            = 18,
        LEVEL_HELL              = 14,
        LEVEL_BBQ               = 12,
        LEVEL_MICROWAVE         = 10,
        LEVEL_VERY_HOT          = 8,
        LEVEL_HOT               = 5,
        LEVEL_WARM              = 2,
};

static struct qpnp_vadc_chip *vadc_dev;
static enum qpnp_vadc_channels adc_chan;

static struct delayed_work check_temp_work;
static struct workqueue_struct *thermal_wq;

static void cpu_offline_wrapper(int cpu)
{
        if (cpu_online(cpu))
                cpu_down(cpu);
}

static void __ref cpu_online_wrapper(int cpu)
{
        if (!cpu_online(cpu))
                cpu_up(cpu);
}

static int msm_thermal_cpufreq_callback(struct notifier_block *nfb,
                unsigned long event, void *data)
{
        struct cpufreq_policy *policy = data;

        if (event != CPUFREQ_ADJUST && !info.pending_change)
                return 0;

        cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
                info.limited_max_freq);

        return 0;
}

static struct notifier_block msm_thermal_cpufreq_notifier = {
        .notifier_call = msm_thermal_cpufreq_callback,
};

static void limit_cpu_freqs(uint32_t max_freq)
{
        unsigned int cpu;

        if (info.limited_max_freq == max_freq)
                return;

        info.limited_max_freq = max_freq;
        info.pending_change = true;

        pr_info_ratelimited("%s: Setting cpu max frequency to %u\n",
                KBUILD_MODNAME, max_freq);

        if (num_online_cpus() < NR_CPUS) {
                if (max_freq > FREQ_BBQ)
                        cpu_online_wrapper(1);
                if (max_freq > FREQ_MICROWAVE)
                        cpu_online_wrapper(2);
                if (max_freq > FREQ_VERY_HOT)
                        cpu_online_wrapper(3);
        }

        get_online_cpus();
        for_each_online_cpu(cpu)
                cpufreq_update_policy(cpu);
        put_online_cpus();

        if (max_freq == FREQ_VERY_HOT)
                cpu_offline_wrapper(3);
        else if (max_freq == FREQ_MICROWAVE)
                cpu_offline_wrapper(2);
        else if (max_freq == FREQ_BBQ)
                cpu_offline_wrapper(1);

        info.pending_change = false;
}

static void check_temp(struct work_struct *work)
{
        uint32_t freq = 0;
        int64_t temp;
        struct qpnp_vadc_result result;

        qpnp_vadc_read(vadc_dev, adc_chan, &result);
        temp = result.physical;

        if (info.throttling) {
                if (temp < (temp_threshold - info.safe_diff)) {
                        limit_cpu_freqs(info.cpuinfo_max_freq);
                        info.throttling = false;
                        goto reschedule;
                }
        }

        if (temp >= temp_threshold + LEVEL_NOTE_7)
                freq = FREQ_NOTE_7;
        else if (temp >= temp_threshold + LEVEL_HELL)
                freq = FREQ_HELL;
        else if (temp >= temp_threshold + LEVEL_BBQ)
                freq = FREQ_BBQ;
        else if (temp >= temp_threshold + LEVEL_MICROWAVE)
                freq = FREQ_MICROWAVE;
        else if (temp >= temp_threshold + LEVEL_VERY_HOT)
                freq = FREQ_VERY_HOT;
        else if (temp >= temp_threshold + LEVEL_HOT)
                freq = FREQ_HOT;
        else if (temp >= temp_threshold + LEVEL_WARM)
                freq = FREQ_WARM;
        else if (temp >= temp_threshold)
                freq = FREQ_ZIPPY;
        else if (temp < temp_threshold)
                freq = FREQ_MAX;

        if (freq) {
                limit_cpu_freqs(freq);

                if (!info.throttling)
                        info.throttling = true;
        }

reschedule:
        queue_delayed_work(thermal_wq, &check_temp_work, msecs_to_jiffies(250));
}

static int __devinit msm_thermal_dev_probe(struct platform_device *pdev)
{
        int ret = 0;
        struct device_node *node = pdev->dev.of_node;

        vadc_dev = qpnp_get_vadc(&pdev->dev, "thermal");

        ret = of_property_read_u32(node, "qcom,adc-channel", &adc_chan);
        if (ret)
                return ret;

        ret = cpufreq_register_notifier(&msm_thermal_cpufreq_notifier,
                CPUFREQ_POLICY_NOTIFIER);

        thermal_wq = alloc_workqueue("thermal_wq", WQ_HIGHPRI, 0);
        if (!thermal_wq) {
                goto err;
        }

        INIT_DELAYED_WORK(&check_temp_work, check_temp);
        queue_delayed_work(thermal_wq, &check_temp_work, 5);

err:
        return ret;
}

static int msm_thermal_dev_remove(struct platform_device *pdev)
{
        cancel_delayed_work_sync(&check_temp_work);
        destroy_workqueue(thermal_wq);
        cpufreq_unregister_notifier(&msm_thermal_cpufreq_notifier,
                        CPUFREQ_POLICY_NOTIFIER);
        return 0;
}

static struct of_device_id msm_thermal_match_table[] = {
        {.compatible = "qcom,msm-thermal-simple"},
        {},
};

static struct platform_driver msm_thermal_device_driver = {
        .probe = msm_thermal_dev_probe,
        .remove = msm_thermal_dev_remove,
        .driver = {
                .name = "msm-thermal-simple",
                .owner = THIS_MODULE,
                .of_match_table = msm_thermal_match_table,
        },
};

static int __init msm_thermal_device_init(void)
{
        return platform_driver_register(&msm_thermal_device_driver);
}

static void __exit msm_thermal_device_exit(void)
{
        platform_driver_unregister(&msm_thermal_device_driver);
}

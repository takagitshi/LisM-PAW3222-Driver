/*
 * Copyright 2024 Google LLC
 * Modifications Copyright 2025 sekigon-gonnoc
 *
 * Original source code:
 * https://github.com/zephyrproject-rtos/zephyr/blob/19c6240b6865bcb28e1d786d4dcadfb3a02067a0/drivers/input/input_paw32xx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
#include <zmk/keymap.h>
#endif

#include "../include/paw3222.h"
#include "../include/paw3222_delta.h"
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
#include "../include/paw3222_output.h"
#include "../include/paw3222_pointer_acceleration.h"
#endif

LOG_MODULE_REGISTER(paw32xx, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT pixart_paw3222

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define PAW32XX_PRODUCT_ID1 0x00
#define PAW32XX_PRODUCT_ID2 0x01
#define PAW32XX_MOTION 0x02
#define PAW32XX_DELTA_X 0x03
#define PAW32XX_DELTA_Y 0x04
#define PAW32XX_OPERATION_MODE 0x05
#define PAW32XX_CONFIGURATION 0x06
#define PAW32XX_WRITE_PROTECT 0x09
#define PAW32XX_SLEEP1 0x0a
#define PAW32XX_SLEEP2 0x0b
#define PAW32XX_SLEEP3 0x0c
#define PAW32XX_CPI_X 0x0d
#define PAW32XX_CPI_Y 0x0e
#define PAW32XX_DELTA_XY_HI 0x12
#define PAW32XX_MOUSE_OPTION 0x19

#define PRODUCT_ID_PAW32XX 0x30
#define SPI_WRITE BIT(7)

#define MOTION_STATUS_MOTION BIT(7)
#define OPERATION_MODE_SLP_ENH BIT(4)
#define OPERATION_MODE_SLP2_ENH BIT(3)
#define OPERATION_MODE_SLP_MASK (OPERATION_MODE_SLP_ENH | OPERATION_MODE_SLP2_ENH)
#define CONFIGURATION_PD_ENH BIT(3)
#define CONFIGURATION_RESET BIT(7)
#define WRITE_PROTECT_ENABLE 0x00
#define WRITE_PROTECT_DISABLE 0x5a
#define MOUSE_OPTION_MOVX_INV_BIT 3
#define MOUSE_OPTION_MOVY_INV_BIT 4

#define RESET_DELAY_MS 2
#define MOTION_RETRY_MIN_DELAY_MS 1
#define MOTION_RETRY_MAX_DELAY_MS 64

#define RES_STEP 38
#define RES_MIN (16 * RES_STEP)
#define RES_MAX (127 * RES_STEP)

struct paw32xx_config {
    struct spi_dt_spec spi;
    struct gpio_dt_spec irq_gpio;
    struct gpio_dt_spec power_gpio;
    int16_t res_cpi;
    bool force_awake;
    uint32_t report_interval_ms;
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
    bool pointer_acceleration_enabled;
    uint8_t pointer_acceleration_scroll_layer;
    uint8_t pointer_acceleration_gesture_layer;
    uint8_t pointer_acceleration_gesture_layer_2;
    struct paw3222_pointer_accel_curve pointer_acceleration_curve;
#endif
};

struct paw32xx_data {
    const struct device *dev;
    struct k_work motion_work;
    struct k_work_delayable report_work;
    struct gpio_callback motion_cb;
    struct k_timer motion_retry_timer;
    atomic_t suspended;
    uint8_t retry_delay_ms;
    int32_t pending_x;
    int32_t pending_y;
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
    int64_t pending_start_time_ms;
    bool have_pending_start;
    struct paw3222_pointer_accel_state pointer_acceleration;
    struct paw3222_output_state output;
#endif
};

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
static int paw32xx_force_cs(const struct device *dev, bool force_low) {
    const struct paw32xx_config *cfg = dev->config;
    const struct gpio_dt_spec *cs = NULL;
    int ret;

    if (cfg->spi.config.cs.gpio.port != NULL) {
        cs = &cfg->spi.config.cs.gpio;
    }

    if (cs == NULL || cs->port == NULL || !device_is_ready(cs->port)) {
        LOG_ERR("CS GPIO not defined or not ready");
        return ENODEV;
    }

    ret = gpio_pin_set_dt(cs, force_low ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Failed to drive CS pin: %d", ret);
        return ret;
    }

    return 0;
}
#endif

static int paw32xx_read_reg(const struct device *dev, uint8_t addr, uint8_t *value) {
    const struct paw32xx_config *cfg = dev->config;
    int ret;

    const struct spi_buf tx_buf = {
        .buf = &addr,
        .len = sizeof(addr),
    };
    const struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };

    struct spi_buf rx_buf[] = {
        {
            .buf = NULL,
            .len = sizeof(addr),
        },
        {
            .buf = value,
            .len = 1,
        },
    };
    const struct spi_buf_set rx = {
        .buffers = rx_buf,
        .count = ARRAY_SIZE(rx_buf),
    };

    ret = spi_transceive_dt(&cfg->spi, &tx, &rx);

    return ret;
}

static int paw32xx_write_reg(const struct device *dev, uint8_t addr, uint8_t value) {
    const struct paw32xx_config *cfg = dev->config;

    uint8_t write_buf[] = {addr | SPI_WRITE, value};
    const struct spi_buf tx_buf = {
        .buf = write_buf,
        .len = sizeof(write_buf),
    };
    const struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };

    return spi_write_dt(&cfg->spi, &tx);
}

static int paw32xx_update_reg(const struct device *dev, uint8_t addr, uint8_t mask, uint8_t value) {
    uint8_t val;
    int ret;

    ret = paw32xx_read_reg(dev, addr, &val);
    if (ret < 0) {
        return ret;
    }

    val = (val & ~mask) | (value & mask);

    ret = paw32xx_write_reg(dev, addr, val);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static int paw32xx_read_xy(const struct device *dev, int16_t *x, int16_t *y) {
    const struct paw32xx_config *cfg = dev->config;
    int ret;

    uint8_t tx_data[] = {
        PAW32XX_DELTA_X,
        0xff,
        PAW32XX_DELTA_Y,
        0xff,
        PAW32XX_DELTA_XY_HI,
        0xff,
    };
    uint8_t rx_data[sizeof(tx_data)];

    const struct spi_buf tx_buf = {
        .buf = tx_data,
        .len = sizeof(tx_data),
    };
    const struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };

    struct spi_buf rx_buf = {
        .buf = rx_data,
        .len = sizeof(rx_data),
    };
    const struct spi_buf_set rx = {
        .buffers = &rx_buf,
        .count = 1,
    };

    ret = spi_transceive_dt(&cfg->spi, &tx, &rx);
    if (ret < 0) {
        return ret;
    }

    *x = paw3222_decode_delta(rx_data[1], rx_data[5] >> 4);
    *y = paw3222_decode_delta(rx_data[3], rx_data[5]);

    return 0;
}

static int paw32xx_interrupt_configure(const struct device *dev, gpio_flags_t flags) {
    const struct paw32xx_config *cfg = dev->config;

    if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
        return -ENODEV;
    }

    return gpio_pin_interrupt_configure_dt(&cfg->irq_gpio, flags);
}

static int paw32xx_interrupt_enable(const struct device *dev) {
    return paw32xx_interrupt_configure(dev, GPIO_INT_EDGE_TO_ACTIVE);
}

static int paw32xx_interrupt_disable(const struct device *dev) {
    return paw32xx_interrupt_configure(dev, GPIO_INT_DISABLE);
}

static void paw32xx_motion_retry_timer_handler(struct k_timer *timer) {
    struct paw32xx_data *data = CONTAINER_OF(timer, struct paw32xx_data, motion_retry_timer);

    if (!atomic_get(&data->suspended)) {
        k_work_submit(&data->motion_work);
    }
}

static int paw32xx_motion_pin_get(struct paw32xx_data *data) {
    const struct paw32xx_config *cfg = data->dev->config;
    int motion = gpio_pin_get_dt(&cfg->irq_gpio);

    if (motion < 0) {
        LOG_ERR("Failed to read motion GPIO: %d", motion);
    }

    return motion;
}

static void paw32xx_retry_if_motion(struct paw32xx_data *data) {
    int motion;
    uint8_t delay_ms;

    if (atomic_get(&data->suspended)) {
        return;
    }

    motion = paw32xx_motion_pin_get(data);
    if (motion <= 0) {
        data->retry_delay_ms = MOTION_RETRY_MIN_DELAY_MS;
        return;
    }

    delay_ms = data->retry_delay_ms;
    k_timer_start(&data->motion_retry_timer, K_MSEC(delay_ms), K_NO_WAIT);
    data->retry_delay_ms = MIN(delay_ms * 2U, MOTION_RETRY_MAX_DELAY_MS);
}

static void paw32xx_resubmit_if_motion(struct paw32xx_data *data) {
    data->retry_delay_ms = MOTION_RETRY_MIN_DELAY_MS;

    if (!atomic_get(&data->suspended) && paw32xx_motion_pin_get(data) > 0) {
        k_work_submit(&data->motion_work);
    }
}

static void paw32xx_report_pending(struct paw32xx_data *data) {
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
    const struct paw32xx_config *cfg = data->dev->config;
    const int64_t now_ms = k_uptime_get();
    if (cfg->pointer_acceleration_enabled) {
        const bool draining_output = paw3222_output_has_pending(&data->output);

        if (!draining_output) {
            int32_t x = data->pending_x;
            int32_t y = data->pending_y;
            const int64_t collection_elapsed_ms =
                data->have_pending_start ? now_ms - data->pending_start_time_ms : 0;

            if (x == 0 && y == 0) {
                return;
            }

            data->pending_x = 0;
            data->pending_y = 0;
            data->pending_start_time_ms = 0;
            data->have_pending_start = false;

            bool bypass = true;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            bypass = zmk_keymap_layer_active(cfg->pointer_acceleration_scroll_layer) ||
                     zmk_keymap_layer_active(cfg->pointer_acceleration_gesture_layer) ||
                     zmk_keymap_layer_active(cfg->pointer_acceleration_gesture_layer_2);
#endif
            if (bypass) {
                paw3222_pointer_accel_reset(&data->pointer_acceleration);
            } else {
                paw3222_pointer_accel_apply_frame(
                    &cfg->pointer_acceleration_curve, &data->pointer_acceleration, x, y,
                    now_ms, collection_elapsed_ms, &x, &y);
            }
            paw3222_output_queue(&data->output, x, y, true);
        }

        const struct paw3222_output_frame frame =
            paw3222_output_take_next(&data->output);
        const bool have_x = frame.x != 0;
        const bool have_y = frame.y != 0;
        int x_err = 0;
        int y_err = 0;

        if (!have_x && !have_y && frame.force_sync) {
            x_err = input_report_rel(data->dev, INPUT_REL_X, 0, true, K_NO_WAIT);
        } else if (have_x) {
            x_err = input_report_rel(data->dev, INPUT_REL_X, frame.x, !have_y, K_NO_WAIT);
        }

        struct paw3222_frame_retry retry =
            paw3222_frame_retry_result(frame.x, frame.y, x_err, 0);
        if (retry.send_y) {
            y_err = input_report_rel(data->dev, INPUT_REL_Y, frame.y, true, K_NO_WAIT);
            retry = paw3222_frame_retry_result(frame.x, frame.y, x_err, y_err);
        }

        const bool zero_sync_failed =
            frame.force_sync && !have_x && !have_y && x_err < 0;
        paw3222_output_complete(&data->output, retry, zero_sync_failed);
        const bool have_output = paw3222_output_has_pending(&data->output);

        if (retry.x != 0 || retry.y != 0 || zero_sync_failed) {
            LOG_WRN("PAW3222 input report failed; retaining delta (%d, %d)", x_err,
                    y_err);
        }

        if (have_output || data->pending_x != 0 || data->pending_y != 0) {
            int ret = k_work_reschedule(&data->report_work,
                                        K_MSEC(cfg->report_interval_ms));
            if (ret < 0) {
                LOG_ERR("Failed to reschedule PAW3222 report: %d", ret);
            }
        }
        return;
    }
#endif

    int32_t x = data->pending_x;
    int32_t y = data->pending_y;

    data->pending_x = 0;
    data->pending_y = 0;
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
    data->pending_start_time_ms = 0;
    data->have_pending_start = false;
#endif

    input_report_rel(data->dev, INPUT_REL_X, x, false, K_FOREVER);
    input_report_rel(data->dev, INPUT_REL_Y, y, true, K_FOREVER);
}

static void paw32xx_report_work_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct paw32xx_data *data = CONTAINER_OF(delayable, struct paw32xx_data, report_work);

    if (!atomic_get(&data->suspended)) {
        paw32xx_report_pending(data);
    }
}

static void paw32xx_report_motion(struct paw32xx_data *data, int16_t x, int16_t y) {
    const struct paw32xx_config *cfg = data->dev->config;

    if (cfg->report_interval_ms == 0U) {
        input_report_rel(data->dev, INPUT_REL_X, x, false, K_FOREVER);
        input_report_rel(data->dev, INPUT_REL_Y, y, true, K_FOREVER);
        return;
    }

#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
    if (!data->have_pending_start && (x != 0 || y != 0)) {
        data->pending_start_time_ms = k_uptime_get();
        data->have_pending_start = true;
    }
#endif
    data->pending_x += x;
    data->pending_y += y;
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
    if (data->pending_x == 0 && data->pending_y == 0) {
        data->pending_start_time_ms = 0;
        data->have_pending_start = false;
    }
#endif

    /* Keep the first deadline so continuous motion cannot postpone reporting. */
    k_work_schedule(&data->report_work, K_MSEC(cfg->report_interval_ms));
}

static void paw32xx_motion_work_handler(struct k_work *work) {
    struct paw32xx_data *data = CONTAINER_OF(work, struct paw32xx_data, motion_work);
    const struct device *dev = data->dev;
    uint8_t val;
    int16_t x, y;
    int ret;

    if (atomic_get(&data->suspended)) {
        return;
    }

    ret = paw32xx_read_reg(dev, PAW32XX_MOTION, &val);
    if (ret < 0) {
        if (data->retry_delay_ms == MOTION_RETRY_MIN_DELAY_MS) {
            LOG_ERR("Failed to read motion status: %d", ret);
        }
        paw32xx_retry_if_motion(data);
        return;
    }

    if ((val & MOTION_STATUS_MOTION) == 0x00) {
        /* Retry a GPIO/register race without slowing the normal motion path. */
        paw32xx_retry_if_motion(data);
        return;
    }

    ret = paw32xx_read_xy(dev, &x, &y);
    if (ret < 0) {
        if (data->retry_delay_ms == MOTION_RETRY_MIN_DELAY_MS) {
            LOG_ERR("Failed to read motion delta: %d", ret);
        }
        paw32xx_retry_if_motion(data);
        return;
    }

    LOG_DBG("x=%4d y=%4d", x, y);

    paw32xx_report_motion(data, x, y);

    /* Drain all pending samples without the former fixed 15 ms delay. */
    paw32xx_resubmit_if_motion(data);
}

static void paw32xx_motion_handler(const struct device *gpio_dev, struct gpio_callback *cb,
                                   uint32_t pins) {
    struct paw32xx_data *data = CONTAINER_OF(cb, struct paw32xx_data, motion_cb);

    ARG_UNUSED(gpio_dev);
    ARG_UNUSED(pins);

    if (atomic_get(&data->suspended)) {
        return;
    }

    k_timer_stop(&data->motion_retry_timer);
    data->retry_delay_ms = MOTION_RETRY_MIN_DELAY_MS;
    k_work_submit(&data->motion_work);
}

int paw32xx_set_resolution(const struct device *dev, uint16_t res_cpi) {
    uint8_t val;
    int ret;

    if (!IN_RANGE(res_cpi, RES_MIN, RES_MAX)) {
        LOG_ERR("res_cpi out of range: %d", res_cpi);
        return -EINVAL;
    }

    val = res_cpi / RES_STEP;

    ret = paw32xx_write_reg(dev, PAW32XX_WRITE_PROTECT, WRITE_PROTECT_DISABLE);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_write_reg(dev, PAW32XX_CPI_X, val);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_write_reg(dev, PAW32XX_CPI_Y, val);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_write_reg(dev, PAW32XX_WRITE_PROTECT, WRITE_PROTECT_ENABLE);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

int paw32xx_force_awake(const struct device *dev, bool enable) {
    uint8_t val = enable ? 0 : OPERATION_MODE_SLP_MASK;
    int ret;

    ret = paw32xx_write_reg(dev, PAW32XX_WRITE_PROTECT, WRITE_PROTECT_DISABLE);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_update_reg(dev, PAW32XX_OPERATION_MODE, OPERATION_MODE_SLP_MASK, val);
    if (ret < 0) {
        return ret;
    }

    ret = paw32xx_write_reg(dev, PAW32XX_WRITE_PROTECT, WRITE_PROTECT_ENABLE);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static int paw32xx_configure(const struct device *dev) {
    const struct paw32xx_config *cfg = dev->config;
    uint8_t val;
    int ret;
    int retry_count = 10;

    // Check if the device is ready
    while (retry_count--) {
        ret = paw32xx_read_reg(dev, PAW32XX_PRODUCT_ID1, &val);
        if (ret < 0) {
            if (retry_count == 0) {
                return ret;
            }
            k_sleep(K_MSEC(100)); // Wait before retrying
            continue;
        }

        if (val != PRODUCT_ID_PAW32XX) {
            LOG_ERR("Invalid product id: %02x", val);

            if (retry_count == 0) {
                return -ENODEV; // Device not ready after retries
            }
#if DT_INST_NODE_HAS_PROP(0, power_gpios)
            // reboot
            ret = paw32xx_force_cs(dev, true);
            if (ret < 0) {
                return ret;
            }

            gpio_pin_set_dt(&cfg->power_gpio, 0);
            k_sleep(K_MSEC(50)); // Wait before retrying
            gpio_pin_set_dt(&cfg->power_gpio, 1);

            ret = paw32xx_force_cs(dev, false);
            if (ret < 0) {
                return ret;
            }
#endif
            k_sleep(K_MSEC(100)); // Wait before retrying
            continue;
        }
        else {
            break; // Device is ready
        }
    }

    ret = paw32xx_update_reg(dev, PAW32XX_CONFIGURATION, CONFIGURATION_RESET, CONFIGURATION_RESET);
    if (ret < 0) {
        return ret;
    }

    k_sleep(K_MSEC(RESET_DELAY_MS));

    if (cfg->res_cpi > 0) {
        paw32xx_set_resolution(dev, cfg->res_cpi);
    }

    paw32xx_force_awake(dev, cfg->force_awake);

    // Dummy reads to clear any residual data
    paw32xx_read_reg(dev, PAW32XX_MOTION, &val);
    paw32xx_read_reg(dev, PAW32XX_DELTA_X, &val);
    paw32xx_read_reg(dev, PAW32XX_DELTA_Y, &val);
    paw32xx_read_reg(dev, PAW32XX_DELTA_XY_HI, &val);

    return 0;
}

static int paw32xx_init(const struct device *dev) {
    const struct paw32xx_config *cfg = dev->config;
    struct paw32xx_data *data = dev->data;
    int ret;

    if (!spi_is_ready_dt(&cfg->spi)) {
        LOG_ERR("%s is not ready", cfg->spi.bus->name);
        return -ENODEV;
    }

    data->dev = dev;
    atomic_clear(&data->suspended);
    data->retry_delay_ms = MOTION_RETRY_MIN_DELAY_MS;
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
    paw3222_pointer_accel_reset(&data->pointer_acceleration);
    paw3222_output_init(&data->output);
#endif

    k_work_init(&data->motion_work, paw32xx_motion_work_handler);
    k_work_init_delayable(&data->report_work, paw32xx_report_work_handler);
    k_timer_init(&data->motion_retry_timer, paw32xx_motion_retry_timer_handler, NULL);

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    // Initialize power GPIO if defined
    if (gpio_is_ready_dt(&cfg->power_gpio)) {
        ret = paw32xx_force_cs(dev, true);
        if (ret != 0) {
            return ret;
        }

        // Configure as output but start with power OFF
        ret = gpio_pin_configure_dt(&cfg->power_gpio, GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            LOG_ERR("Power pin configuration failed: %d", ret);
            return ret;
        }

        // Wait 0.01 seconds before turning on power
        k_sleep(K_MSEC(10));

        // Now turn on power
        ret = gpio_pin_set_dt(&cfg->power_gpio, 1);
        if (ret != 0) {
            LOG_ERR("Power pin set failed: %d", ret);
            return ret;
        }

        // Wait for power stabilization
        k_sleep(K_MSEC(500));

        ret = paw32xx_force_cs(dev, false);
        if (ret != 0) {
            return ret;
        }

        // Wait for power stabilization
        k_sleep(K_MSEC(50));
    }
#endif

    if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
        LOG_ERR("%s is not ready", cfg->irq_gpio.port->name);
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
    if (ret != 0) {
        LOG_ERR("Motion pin configuration failed: %d", ret);
        return ret;
    }

    gpio_init_callback(&data->motion_cb, paw32xx_motion_handler, BIT(cfg->irq_gpio.pin));

    ret = gpio_add_callback_dt(&cfg->irq_gpio, &data->motion_cb);
    if (ret < 0) {
        LOG_ERR("Could not set motion callback: %d", ret);
        return ret;
    }

    ret = paw32xx_configure(dev);
    if (ret != 0) {
        LOG_ERR("Device configuration failed: %d", ret);
        return ret;
    }

    ret = paw32xx_interrupt_enable(dev);
    if (ret != 0) {
        LOG_ERR("Motion interrupt configuration failed: %d", ret);
        return ret;
    }

    ret = pm_device_runtime_enable(dev);
    if (ret < 0) {
        LOG_ERR("Failed to enable runtime power management: %d", ret);
        return ret;
    }

    return 0;
}

#ifdef CONFIG_PM_DEVICE
static int paw32xx_pm_action(const struct device *dev, enum pm_device_action action) {
    const struct paw32xx_config *cfg = dev->config;
    struct paw32xx_data *data = dev->data;
    int ret;
    uint8_t val;

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        atomic_set(&data->suspended, 1);
        k_timer_stop(&data->motion_retry_timer);
        k_work_cancel_delayable(&data->report_work);
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
        paw3222_pointer_accel_reset(&data->pointer_acceleration);
#endif

        // Disable IRQ interrupt
        ret = paw32xx_interrupt_disable(dev);
        if (ret < 0) {
            LOG_ERR("Failed to disable IRQ interrupt: %d", ret);
            atomic_clear(&data->suspended);
            return ret;
        }

        // Disconnect IRQ GPIO
        ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_DISCONNECTED);
        if (ret < 0) {
            LOG_ERR("Failed to disconnect IRQ GPIO: %d", ret);
            atomic_clear(&data->suspended);
            return ret;
        }

        val = CONFIGURATION_PD_ENH;
        ret = paw32xx_update_reg(dev, PAW32XX_CONFIGURATION, CONFIGURATION_PD_ENH, val);
        if (ret < 0) {
            atomic_clear(&data->suspended);
            return ret;
        }

        break;

    case PM_DEVICE_ACTION_RESUME:

        val = 0;
        ret = paw32xx_update_reg(dev, PAW32XX_CONFIGURATION, CONFIGURATION_PD_ENH, val);
        if (ret < 0) {
            return ret;
        }

        // Reconfigure IRQ GPIO as input
        ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
        if (ret < 0) {
            LOG_ERR("Failed to configure IRQ GPIO: %d", ret);
            return ret;
        }

        // Re-enable IRQ interrupt
        ret = paw32xx_interrupt_enable(dev);
        if (ret < 0) {
            LOG_ERR("Failed to enable IRQ interrupt: %d", ret);
            return ret;
        }

        /* An asserted level during resume may not produce a new edge. */
        atomic_clear(&data->suspended);
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
        if (data->pending_x != 0 || data->pending_y != 0) {
            data->pending_start_time_ms = k_uptime_get();
            data->have_pending_start = true;
        }
#endif
        if (data->pending_x != 0 || data->pending_y != 0
#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
            || paw3222_output_has_pending(&data->output)
#endif
        ) {
            k_work_schedule(&data->report_work, K_NO_WAIT);
        }
        paw32xx_resubmit_if_motion(data);
        break;

    default:
        return -ENOTSUP;
    }

    return 0;
}
#endif

#define PAW32XX_SPI_MODE                                                                           \
    (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_TRANSFER_MSB)

#if IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION)
#define PAW32XX_GESTURE_LAYER_2(n)                                                               \
    DT_INST_PROP_OR(n, pointer_acceleration_gesture_layer_2,                                     \
                    DT_INST_PROP(n, pointer_acceleration_gesture_layer))

#define PAW32XX_POINTER_ACCEL_ASSERTS(n)                                                           \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, report_interval_ms) > 0,                                      \
                 "pointer acceleration requires a non-zero report interval");                    \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_base_gain_milli) > 0,                    \
                 "pointer acceleration base gain must be positive");                             \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_base_gain_milli) <= 4000,               \
                 "pointer acceleration base gain must not exceed 4.0x");                         \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_base_gain_milli) <=                      \
                         DT_INST_PROP(n, pointer_acceleration_max_gain_milli),                     \
                 "pointer acceleration base gain exceeds max gain");                             \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_max_gain_milli) <= 4000,                \
                 "pointer acceleration max gain must not exceed 4.0x");                          \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_takeoff_speed) >= 0,                    \
                 "pointer acceleration takeoff speed must not be negative");                     \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_takeoff_speed) <                        \
                         DT_INST_PROP(n, pointer_acceleration_full_speed),                         \
                 "pointer acceleration takeoff must be below full speed");                       \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_full_speed) <= UINT16_MAX,              \
                 "pointer acceleration speeds must fit in 16 bits");                             \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_reference_interval_ms) > 0,             \
                 "pointer acceleration reference interval must be positive");                    \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_reference_interval_ms) <= UINT16_MAX,   \
                 "pointer acceleration reference interval must fit in 16 bits");                 \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_idle_reset_ms) >                        \
                         DT_INST_PROP(n, pointer_acceleration_reference_interval_ms),             \
                 "pointer acceleration idle reset must exceed the reference interval");          \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_idle_reset_ms) <= UINT16_MAX,           \
                 "pointer acceleration idle reset must fit in 16 bits");                         \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_scroll_layer) >= 0,                     \
                 "pointer acceleration Scroll layer must not be negative");                      \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_scroll_layer) <= UINT8_MAX,             \
                 "pointer acceleration Scroll layer must fit in 8 bits");                        \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_scroll_layer) < 32,                     \
                 "pointer acceleration Scroll layer must fit the active-layer mask");           \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_scroll_layer) <                         \
                         ZMK_KEYMAP_LAYERS_LEN,                                                   \
                 "pointer acceleration Scroll layer must exist");                               \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_gesture_layer) >= 0,                    \
                 "pointer acceleration Gesture layer must not be negative");                     \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_gesture_layer) <= UINT8_MAX,            \
                 "pointer acceleration Gesture layer must fit in 8 bits");                       \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_gesture_layer) < 32,                    \
                 "pointer acceleration Gesture layer must fit the active-layer mask");          \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     DT_INST_PROP(n, pointer_acceleration_gesture_layer) <                        \
                         ZMK_KEYMAP_LAYERS_LEN,                                                   \
                 "pointer acceleration Gesture layer must exist");                              \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     PAW32XX_GESTURE_LAYER_2(n) >= 0,                                             \
                 "pointer acceleration Gesture layer 2 must not be negative");                   \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     PAW32XX_GESTURE_LAYER_2(n) <= UINT8_MAX,                                     \
                 "pointer acceleration Gesture layer 2 must fit in 8 bits");                     \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     PAW32XX_GESTURE_LAYER_2(n) < 32,                                             \
                 "pointer acceleration Gesture layer 2 must fit the active-layer mask");         \
    BUILD_ASSERT(!DT_INST_PROP(n, pointer_acceleration) ||                                         \
                     PAW32XX_GESTURE_LAYER_2(n) < ZMK_KEYMAP_LAYERS_LEN,                           \
                 "pointer acceleration Gesture layer 2 must exist")

#define PAW32XX_POINTER_ACCEL_CONFIG(n)                                                            \
    .pointer_acceleration_enabled = DT_INST_PROP(n, pointer_acceleration),                         \
    .pointer_acceleration_scroll_layer =                                                           \
        DT_INST_PROP(n, pointer_acceleration_scroll_layer),                                        \
    .pointer_acceleration_gesture_layer =                                                          \
        DT_INST_PROP(n, pointer_acceleration_gesture_layer),                                       \
    .pointer_acceleration_gesture_layer_2 = PAW32XX_GESTURE_LAYER_2(n),                            \
    .pointer_acceleration_curve =                                                                  \
        {                                                                                          \
            .base_gain_milli = DT_INST_PROP(n, pointer_acceleration_base_gain_milli),              \
            .takeoff_speed = DT_INST_PROP(n, pointer_acceleration_takeoff_speed),                  \
            .full_speed = DT_INST_PROP(n, pointer_acceleration_full_speed),                        \
            .max_gain_milli = DT_INST_PROP(n, pointer_acceleration_max_gain_milli),                \
            .reference_interval_ms =                                                              \
                DT_INST_PROP(n, pointer_acceleration_reference_interval_ms),                       \
            .idle_reset_ms = DT_INST_PROP(n, pointer_acceleration_idle_reset_ms),                  \
        },
#else
#define PAW32XX_POINTER_ACCEL_ASSERTS(n)
#define PAW32XX_POINTER_ACCEL_CONFIG(n)
#endif

#define PAW32XX_INIT(n)                                                                            \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP_OR(n, res_cpi, RES_MIN), RES_MIN, RES_MAX),                 \
                 "invalid res-cpi");                                                               \
    BUILD_ASSERT(IS_ENABLED(CONFIG_PAW3222_POINTER_ACCELERATION) ||                                \
                     !DT_INST_PROP(n, pointer_acceleration),                                      \
                 "pointer-acceleration requires CONFIG_PAW3222_POINTER_ACCELERATION");            \
    PAW32XX_POINTER_ACCEL_ASSERTS(n);                                                              \
                                                                                                   \
    static const struct paw32xx_config paw32xx_cfg_##n = {                                         \
        .spi = SPI_DT_SPEC_INST_GET(n, PAW32XX_SPI_MODE, 0),                                       \
        .irq_gpio = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                           \
        .power_gpio = GPIO_DT_SPEC_INST_GET_OR(n, power_gpios, {0}),                               \
        .res_cpi = DT_INST_PROP_OR(n, res_cpi, -1),                                                \
        .force_awake = DT_INST_PROP(n, force_awake),                                               \
        .report_interval_ms = DT_INST_PROP(n, report_interval_ms),                                 \
        PAW32XX_POINTER_ACCEL_CONFIG(n)                                                            \
    };                                                                                             \
                                                                                                   \
    static struct paw32xx_data paw32xx_data_##n;                                                   \
                                                                                                   \
    PM_DEVICE_DT_INST_DEFINE(n, paw32xx_pm_action);                                                \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, paw32xx_init, PM_DEVICE_DT_INST_GET(n), &paw32xx_data_##n,            \
                          &paw32xx_cfg_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PAW32XX_INIT)

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

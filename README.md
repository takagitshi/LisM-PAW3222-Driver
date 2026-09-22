# LisM PAW3222 Driver

> [!IMPORTANT]
> **このリポジトリはLisMの内部依存ドライバで、通常は直接編集しません。**
> 普段のキーマップ、レイヤー、ビルド設定、ファームウェア変更は
> [`LisM-ZMK-Firmware`](https://github.com/takagitshi/LisM-ZMK-Firmware)で行います。

This driver enables the use of the PIXART PAW3222 optical sensor with the ZMK framework.

## Overview

The PAW3222 is a low-power optical mouse sensor suitable for tracking applications such as mice and trackballs. This driver communicates with the PAW3222 sensor via SPI interface.

## Installation

1. Add as a ZMK module in your west.yml:

```
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-driver-paw3222
      remote: sekigon-gonnoc
      revision: main
```

## Device Tree Configuration

Configure in your shield or board config file (.overlay or .dtsi):

```dts
&pinctrl {
    spi0_default: spi0_default {
        group1 {
            psels = <NRF_PSEL(SPIM_SCK, 0, 12)>,
                <NRF_PSEL(SPIM_MOSI, 1, 9)>,
                <NRF_PSEL(SPIM_MISO, 1, 9)>;
        };
    };

    spi0_sleep: spi0_sleep {
        group1 {
            psels = <NRF_PSEL(SPIM_SCK, 0, 12)>,
                <NRF_PSEL(SPIM_MOSI, 1, 9)>,
                <NRF_PSEL(SPIM_MISO, 1, 9)>;
            low-power-enable;
        };
    };
};

&spi0 {
    status = "okay";
    compatible = "nordic,nrf-spim";
    pinctrl-0 = <&spi0_default>;
    pinctrl-1 = <&spi0_sleep>;
    pinctrl-names = "default", "sleep";
    cs-gpios = <&gpio0 13 GPIO_ACTIVE_LOW>;

    trackball: trackball@0 {
        status = "okay";
        compatible = "pixart,paw3222";
        reg = <0>;
        spi-max-frequency = <2000000>;
        irq-gpios = <&gpio0 15 GPIO_ACTIVE_LOW>;
    };
};
```

## Enable the module in your keyboard's Kconfig file

Add the following to your keyboard's `Kconfig.defconfig`:

```kconfig
if ZMK_KEYBOARD_YOUR_KEYBOARD

config ZMK_POINTING
    default y

config PAW3222
    default y

endif
```

## Properties

- `irq-gpios`: GPIO connected to the motion pin (required)
- `res-cpi`: CPI resolution for the sensor (optional)
- `force-awake`: Initialize the sensor in "force awake" mode (optional, boolean)
- `report-interval-ms`: Aggregate deltas and report at most once per interval
  (optional, defaults to immediate reporting)
- `pointer-acceleration`: Apply an optional same-frame vector gain curve at the
  report aggregation point. The tuning properties use the
  `pointer-acceleration-*` prefix.

## Motion sampling

The driver reads both low-byte delta registers and `DELTA_XY_HI`, then reports
signed 12-bit X/Y deltas. While the MOTION pin remains active it drains pending
samples immediately instead of imposing a fixed polling delay. A capped 1--64 ms
exponential retry is used only for a transient SPI failure or a GPIO/register race.

For a split peripheral, set `report-interval-ms` to a small bounded value such
as 8 ms. The sensor is still drained immediately, but accumulated deltas are
sent as one X/Y frame per interval so a bounded split transport is not flooded.

`force-awake` remains disabled by default because it trades lower wake latency
for higher battery use. Enable it only when comparing first-motion wake latency
on the target hardware.

Pointer acceleration is opt-in per sensor node. It scales a complete X/Y frame
with one shared gain, retains fractional motion, and normalizes only genuine
collection backlog. The configured Scroll and Gesture layers bypass the curve.
For a split keyboard, enable it on a central sensor only; a peripheral cannot
observe the central keymap layer state and therefore keeps the raw path.

---

# ZMK PAW3222 ドライバ

このドライバは、PIXART PAW3222光学センサーをZMKフレームワークで使用できるようにします。

## 概要

PAW3222は、マウスやトラックボールなどのトラッキングアプリケーションに適した低消費電力の光学マウスセンサーです。このドライバはSPIインターフェースを介してPAW3222センサーと通信します。

## インストール

1. ZMKモジュールとして追加：

```
# west.yml に追加
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-driver-paw3222
      remote: sekigon-gonnoc
      revision: main
```

## デバイスツリー設定

シールドまたはボード設定ファイル（.overlayまたは.dtsi）で設定：

```dts
&pinctrl {
    spi0_default: spi0_default {
        group1 {
            psels = <NRF_PSEL(SPIM_SCK, 0, 12)>,
                <NRF_PSEL(SPIM_MOSI, 1, 9)>,
                <NRF_PSEL(SPIM_MISO, 1, 9)>;
        };
    };

    spi0_sleep: spi0_sleep {
        group1 {
            psels = <NRF_PSEL(SPIM_SCK, 0, 12)>,
                <NRF_PSEL(SPIM_MOSI, 1, 9)>,
                <NRF_PSEL(SPIM_MISO, 1, 9)>;
            low-power-enable;
        };
    };
};

&spi0 {
    status = "okay";
    compatible = "nordic,nrf-spim";
    pinctrl-0 = <&spi0_default>;
    pinctrl-1 = <&spi0_sleep>;
    pinctrl-names = "default", "sleep";
    cs-gpios = <&gpio0 13 GPIO_ACTIVE_LOW>;

    trackball: trackball@0 {
        status = "okay";
        compatible = "pixart,paw3222";
        reg = <0>;
        spi-max-frequency = <2000000>;
        irq-gpios = <&gpio0 15 GPIO_ACTIVE_LOW>;
    };
};
```

## キーボードのKconfigファイルでモジュールを有効化

キーボードの `Kconfig.defconfig` に以下を追加：

```kconfig
if ZMK_KEYBOARD_YOUR_KEYBOARD

config ZMK_POINTING
    default y

config PAW3222
    default y

endif
```

## プロパティ

- `irq-gpios`: モーションピンに接続されたGPIO（必須）
- `res-cpi`: センサーのCPI解像度（任意）
- `force-awake`: センサーを「強制起動」モードで初期化（任意、ブール値）

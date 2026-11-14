# SLNeko-XNU — 红米 Note 11T Pro（xaga，MT6895）上的 iOS 16.5

[English](README.md) · **简体中文**

这是 HakiOS 的 **xaga** 移植源码树：把 Apple 开源的 Darwin/XNU
（`xnu-8796.121.2`，iOS 16.5 内核）跑在小米红米 Note 11T Pro / POCO X5 Pro
（MT6895）上。iOS 16.5（20F66）用户空间保持原样：launchd → backboardd →
SpringBoard。

本树派生自 HakiOS 的 `mtk` 分支。上游完整文档保留在
[`README.hakios.md`](README.hakios.md) 和 [`docs/`](docs/)；本文件只说明 xaga
移植部分。

## 当前状态（2026 年 11 月 14 日）

| 模块 | 状态 |
|---|---|
| 启动 | XNU 8796.121.2 经 `mtkloader` 从 `boot_a` 启动；all-root/App 的 launchd 配置可用。 |
| 显示 | 图形模式（`v_display=1`）下显示正常，SpringBoard 能绘制 Hello/OOBE 界面。 |
| 触控 | Novatek **NT36672E**，SPI2（GPIO 109–112，IRQ GPIO135），固件使用 **`novatek_nt36672e_l16_fw01.bin`**；多点触控报点正常。此面板不要用 fw02：它能下载，但 baseline 停在 `a1`，只报 `0x77` 填充。 |
| OOBE | Hello → 上滑 → SpringBoard 主屏；工作用 root 已关闭 `bcatch` 的自动上滑。 |
| 日志 | loader 把 HLOG logbook 写到 `iosroot` 尾部预留区最后 8 MiB（整个预留区为 200 MiB）。 |
| 电池/充电 | 第一版：MT6375 在 I2C5（GPIO33/34 bit-bang），读取 VBAT、PWR_RDY、CHG_STAT，用电压估算 SOC 并交给 `RedwoodBattery`。属于 bring-up 质量，真正的 MT6375 库仑计尚未实现。 |
| 音频/扬声器 | AFE DL1/I2S3 环路和两颗 TFA9873（I2C6 `0x34`/`0x35`，revision `0x0b73`）已工作；iOS `mediaserverd` 能通过 `/dev/redwood-audio` 输出。已去掉 AFE sink 原先固定的 /64 衰减，音量改由 iOS 软件音量控制；并增加了 TFA9873 `0x52` 增益日志。仍属 bring-up 质量。 |
| 按键 | 电源和音量加使用 MT6363 PSC 中断状态 `0x091b`（按下/松开分别有独立 bit）；音量减使用 SoC keypad `KP_MEM1` bit 0。三个键都已工作。 |
| 显示缩放 | Preferences/Settings 使用 IOMFB selector 53，稳定 960×2079 画布、左右裁切和触控补偿；原生面板仍为 1080×2460。 |
| Wi-Fi/蓝牙 | 进行中：MT6895 CONNSYS + MT6637/CONNAC20；Android 侧调查确认 `soc7_0` 固件系列和约 `0x18024000` 的 WFDMA。尚未有可用的 Wi-Fi/蓝牙驱动。 |
| 非目标 | 蜂窝、相机采集、Apple 在线服务/激活。 |

## 硬件参考

- **SoC**：MediaTek MT6895。
- **触控**：Novatek NT36672E，SPI2 `0x11012000`，MI/CS/MO/CLK =
  GPIO **109/110/111/112**，中断 = GPIO **135**；驱动在
  `bsd/dev/redwood_nvt.c`，用 GPIO 手工 bit-bang。
- **充电/电量计**：MT6375 在 **I2C5**（PMU 地址 `0x34`，电池监测
  `0x1A`）；SCL/SDA = GPIO **33/34**。
- **显示**：1080×2460，使用 `v_display=1` 的图形 loader。
- **音频**：AFE DL1 → I2S3 → 两颗 TFA9873（I2C6 `0x34`/`0x35`，revision
  `0x0b73`）；`/dev/redwood-audio` 是内核 sink。
- **按键**：电源和音量加走 MT6363 PMIC PSC 中断（`0x091b`）；音量减走
  SoC keypad `KP_MEM1` bit 0。

## 最近改动

- `17e8bb0 fix(audio): restore full-scale AFE speaker output` — 去掉
  `redwood_backlight.c` 中 `AFE_SINK_SHIFT 6`（-36 dB）的固定衰减；音量由 iOS
  软件音量控制，并记录 TFA9873 增益寄存器。

## 构建

内核由外部 Apple XNU 源码树加 `xnu/redwood.patch`、`xnu/files/` 构建：

```sh
xnu/apply.sh                 # 拉取 xnu-8796.121.2 并应用移植
xnu-deps/setup.sh            # 主机工具、MIG、tcsh、SDK shim
./xnu-build.sh -j8 -k        # -> xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837
userland/build.sh            # root.apfs / 系统镜像
```

iOS 系统镜像由你自己的 IPSW 生成；本仓库不包含 Apple 文件，也不包含手机
固件。NVT 固件头 `xnu/files/bsd/dev/redwood.nvt_fw.h` 由你自己的手机里的
vendor 文件生成：

```sh
# /vendor/firmware/novatek_nt36672e_l16_fw01.bin
python3 tools/mknvtfw.py --output xnu/files/bsd/dev/redwood.nvt_fw.h \
        novatek_nt36672e_l16_fw01.bin
```

`userland/build.sh`、loader 和刷机脚本需要 Theos 的 cctools/SDK，路径为
`$THEOS`。完整工具链说明见 [`README.hakios.md`](README.hakios.md) 和
[`xnu/README.md`](xnu/README.md)。

## 刷机

启动镜像只写入 **`boot_a`**。**绝不要刷写或覆盖 `boot_b`**；它是 Android
回退，必须保持不动。`mtkloader` 位于 `boot_a` 中 Android 内核之前，
`iosroot` 保存 iOS 系统镜像。

在 Android 且有 root 的环境下读取 logbook：

```sh
fbdiag/pull-iosroot-logbook.sh
# 或者：dd if=/dev/block/by-name/iosroot of=logbook.bin bs=4096 \
#           skip=$(( (size/4096) - 2048 )) count=1025
```

## 致谢

- **ShuiLan114514** — xaga 移植维护、NVT 触控（fw01）、显示/OOBE bring-up、
  logbook 流程、MT6375 电池 bring-up。
- **mytiantian001** — 共同作者 / 贡献者。
- **HakiOS / oosexgang** — 上游 `hakios-xnu-kernel` 的 `mtk` 分支、loader、
  XNU 兼容工作和文档。
- **Apple** — XNU 源码和 iOS 用户空间（本仓库不重新分发）。

## 许可

XNU 补丁及由 XNU 派生的文件遵循 Apple Public Source License 2.0；其余文件
除文件头另有说明外遵循 MIT。本项目是独立研究项目，与 Apple、小米或联发科
无隶属关系，也未获其背书。

<p align="left"><img alt="OSPTEK" src="./images/logo.png" width="200" /></p>

<h1 align="center">OSPTEK 1.1″ AMOLED 126×294（ICNA3306 · SPI）</h1>

<p align="center"><b>条形 AMOLED · SPI · ICNA3306</b></p>

<p align="center"><a href="./README_EN.md">English</a> | 简体中文 · <a href="../../README.md">规格族索引</a></p>

<p align="center">
  <img alt="Size: 1.1 inch" src="https://img.shields.io/badge/Size-1.1%22-3498DB?style=flat-square" />
  <img alt="Resolution: 126x294" src="https://img.shields.io/badge/Resolution-126%C3%97294-8E44AD?style=flat-square" />
  <img alt="Interface: SPI" src="https://img.shields.io/badge/Interface-SPI-27AE60?style=flat-square" />
  <img alt="Driver: ICNA3306" src="https://img.shields.io/badge/Driver-ICNA3306-E7352C?style=flat-square" />
</p>

<p align="center"><img alt="OSPTEK 1.1 寸 126×294 AMOLED SPI 模组（ICNA3306）宣传图" src="./images/product.png" width="640" /></p>

## 目录

- [产品简介](#产品简介)
- [规格参数](#规格参数)
- [示例工程](#示例工程)
- [仓库结构](#仓库结构)
- [相关资料](#相关资料)
- [购买链接](#购买链接)
- [技术支持](#技术支持)

---

## 产品简介

OSPTEK **1.1 寸 126×294 AMOLED** 是一款 **SPI** 接口彩色显示模组，驱动芯片为 **ICNA3306**。细长分辨率适合条形 HMI、侧边状态条与紧凑信息面板等场景。

规格标识（仓库名）：`1.1-amoled-126x294-spi-icna3306`

当前模组版本：**AM110Q126294LK1**。电气与外形细节以 [`docs/AM110Q126294LK1.pdf`](./docs/AM110Q126294LK1.pdf) 为准。

## 规格参数

| 项目 | 规格 |
| ---- | ---- |
| 尺寸 | 1.1 英寸 |
| 类型 | AMOLED（彩色） |
| 分辨率 | 126×294 |
| 接口 | SPI |
| 驱动 IC | ICNA3306 |

> 完整外形尺寸、FPC 定义、供电与时序以产品规格书 / 驱动手册为准。

## 示例工程

| 说明 | 路径 |
| ---- | ---- |
| ESP32-S3 · ICNA3306 SPI + CHSC6417 触摸 + LVGL（Widgets Demo） | [`examples/1.1AMOLED/`](./examples/1.1AMOLED/) |

## 仓库结构

```text
1.1-amoled-126x294-spi-icna3306/                                # 仓库根（导航见 ../../README.md）
└── versions/
    └── AM110Q126294LK1/                                # 本料号完整资料
        ├── README.md
        ├── README_EN.md
        ├── images/
        ├── docs/
        └── examples/
```

## 相关资料

| 资料 | 链接 |
| ---- | ---- |
| 产品规格书（AM110Q126294LK1） | [`docs/AM110Q126294LK1.pdf`](./docs/AM110Q126294LK1.pdf) |

### 示例工程

- [ESP32-S3 ICNA3306 SPI + CHSC6417 + LVGL](./examples/1.1AMOLED/)

## 购买链接

<p align="center">
  <a href="https://shop110742373.taobao.com/"><img alt="淘宝官方店铺" src="https://img.shields.io/badge/淘宝-官方店铺-FF6A00?style=for-the-badge" /></a>
  &nbsp;&nbsp;
  <a href="https://www.aliexpress.com/store/1105701619"><img alt="速卖通官方店铺" src="https://img.shields.io/badge/速卖通-官方店铺-E62E04?style=for-the-badge&logo=aliexpress&logoColor=white" /></a>
</p>

**国内（淘宝）**

- 店铺：[鱼鹰光电工厂店](https://shop110742373.taobao.com/)

**海外（AliExpress）**

- 店铺：[OSPTEK Official Store](https://www.aliexpress.com/store/1105701619)

## 技术支持

- 技术支持 / 产品咨询：<luyu@osptek.com>
- QQ 技术交流群：**985881096**
- 公司官网：<https://osptek.com/>
- 有任何问题，都可以在本仓库 Issues 中提问

---

<p align="center"><sub>© 2026 OSPTEK 鱼鹰光电 · 本仓库资料采用 CC BY 4.0 许可</sub></p>

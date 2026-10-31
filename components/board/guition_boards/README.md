# Guition Boards

[English](README_EN.md) | 简体中文

部分开发板配置可能不完善，会在之后逐渐完善。

如果发现某个开发板配置存在问题，欢迎在 [Guition 仓库](https://github.com/Guition-JC/Guition)中[提交 Issue](https://github.com/Guition-JC/Guition/issues)。

Guition 开发板的 ESP Board Manager 板级支持组件，组件名为 `guition/guition_boards`。

本组件提供开发板信息、外设与设备配置、板级默认 sdkconfig，以及显示、触摸、背光、SD 卡和音频适配。添加组件后，通过 ESP Board Manager 选择板型并生成配置代码，即可在 ESP-IDF 应用中使用对应设备。

每个板型目录可完整复制到其他 ESP-IDF 应用中独立使用，无须安装整个 `guition_boards` 组件。板型目录包含自身所需的初始化源码、触摸支持、Kconfig 选项及以下配置：

- `board_info.yaml`：板型标识、目标芯片和厂商信息。
- `board_peripherals.yaml`：总线、GPIO 等外设配置。
- `board_devices.yaml`：设备、驱动依赖及初始化参数。
- `sdkconfig.defaults.board`：板级默认配置。

板级 YAML 使用两空格缩进，列表项相对所属字段缩进一级，设备和外设条目之间保留空行。标量数组采用行内形式（如 `[FL, FR]`），I2C 地址采用十六进制（如 `0x30`、`[0xBA, 0x28]`）；描述、厂商、挂载路径和需要引号的字符串使用双引号。

ESP Board Manager 的完整用法见[组件文档](https://github.com/espressif/esp-board-manager/blob/main/README.md)。

## 兼容性

当前版本为 `0.2.2`，依赖要求以 [idf_component.yml](idf_component.yml) 为准：

| 依赖 | 版本 |
| --- | --- |
| ESP-IDF | `>=6.1.0` |
| `espressif/esp_board_manager` | `0.7.3~1` |
| `espressif/esp_lcd_touch` | `1.2.1` |

组件清单声明的目标为 ESP32、ESP32-C3、ESP32-S3、ESP32-P4 和 ESP32-S31，共 26 个板型配置。ESP32-S31 板型需要所用 ESP-IDF 环境提供 `esp32s31` 目标支持。

## 支持的开发板

下表描述本组件提供的配置能力，板型标识可直接用于 `idf.py bmgr -b <board>`。触摸列列出可选硬件变体，应按实际硬件选择；`-` 表示未提供对应设备配置。板型配置不代表已完成逐板实机验证。

### ESP32-P4

| 板型标识 | LCD（接口） | 分辨率 | 触摸 | SD 卡 | 音频 |
| --- | --- | --- | --- | --- | --- |
| [jc1060p470](jc1060p470/) | JD9165（MIPI DSI） | 1024×600 | GT911 | SDMMC | ES8311（DAC + ADC） |
| [jc1060p470_i_w_y](jc1060p470_i_w_y/) | JD9165（MIPI DSI） | 1024×600 | GT911 | SDMMC | ES8311（DAC + ADC） |
| [jc4880p443](jc4880p443/) | ST7701（MIPI DSI） | 480×800 | GT911 | SDMMC | ES8311（DAC + ADC） |
| [jc8012p4a1](jc8012p4a1/) | JD9365（MIPI DSI） | 800×1280 | GSL3680 | SDMMC | ES8311（DAC + ADC） |

`jc1060p470` 和 `jc1060p470_i_w_y` 分别对应无外壳与带外壳版本。JC8012P4A1 可在 menuconfig 的 `Guition JC8012P4A1 display` 中选择 LCD V1 / V2，默认 V2。JC4880P443 的触摸列以实际 GT911 工厂函数为准；其 YAML 芯片名与坐标范围仍需结合硬件核对，详见[板级配置](jc4880p443/board_devices.yaml)。

### ESP32-S31

| 板型标识 | LCD（接口） | 分辨率 | 触摸 | SD 卡 | 音频 |
| --- | --- | --- | --- | --- | --- |
| [jc5454f328](jc5454f328/) | AXS15260（三线 SPI + RGB） | 540×540 | AXS15260 | SDMMC | I2S DAC + ES7210 ADC |
| [jc8048f543](jc8048f543/) | 通用 RGB 面板（RGB） | 800×480 | GT911 | SDMMC | ES8389（DAC + ADC） |
| [jc8048f550](jc8048f550/) | 通用 RGB 面板（RGB） | 800×480 | GT911 | SDMMC | ES8389（DAC + ADC） |
| [jc3636f118](jc3636f118/) | ST77916（QSPI） | 360×360 | CST816S / 无 | SDMMC | PDM 输入 + I2S 输出 |
| [jc1060f570c](jc1060f570c/) | 通用 RGB 面板（RGB） | 1024×600 | GT911 | SDMMC | ES8389（DAC + ADC） |


### ESP32-S3

| 板型标识 | LCD（接口） | 分辨率 | 触摸 | SD 卡 | 音频 |
| --- | --- | --- | --- | --- | --- |
| [esp32_4827s043](esp32_4827s043/) | ST7262（RGB） | 480×272 | GT911 / XPT2046 / 无 | SPI | - |
| [esp32_4848s040](esp32_4848s040/) | ST7701（三线 SPI + RGB） | 480×480 | GT911 / 无 | SPI | - |
| [esp32_8048s043](esp32_8048s043/) | ST7262（RGB） | 800×480 | GT911 / 无 | SPI | - |
| [esp32_8048s050](esp32_8048s050/) | ST7262（RGB） | 800×480 | GT911 / 无 | SPI | - |
| [esp32_8048s070](esp32_8048s070/) | ST7262（RGB） | 800×480 | GT911 / 无 | SPI | - |
| [jc3248w535](jc3248w535/) | AXS15231B（QSPI） | 320×480 | AXS15231B / 无 | SPI | - |
| [jc3636k518](jc3636k518/) | ST77916（QSPI） | 360×360 | CST816S / 无 | SDMMC | PDM 输入 |
| [jc3636k718](jc3636k718/) | ST77916（QSPI） | 360×360 | CST816S / 无 | SDMMC | PDM 输入 + I2S 输出 |
| [jc3636w518](jc3636w518/) | ST77916（QSPI） | 360×360 | CST816S / 无 | SDMMC | PDM 输入 + I2S 输出 |
| [jc4827w543](jc4827w543/) | ST77916（QSPI） | 480×272 | GT911 / XPT2046 / 无 | SPI | - |
| [jc8048w550](jc8048w550/) | ST7262（RGB） | 800×480 | GT911 / 无 | SPI | - |

### ESP32

| 板型标识 | LCD（接口） | 分辨率 | 触摸 | SD 卡 |
| --- | --- | --- | --- | --- |
| [esp32_2432s024](esp32_2432s024/) | ST7789（SPI） | 240×320 | CST820 / XPT2046 / 无 | SPI |
| [esp32_2432s028](esp32_2432s028/) | ST7789（SPI） | 240×320 | CST820 / XPT2046 / 无 | SPI |
| [esp32_2432s032](esp32_2432s032/) | ST7796（SPI） | 240×320 | GT911 / XPT2046 / 无 | SPI |
| [esp32_3248s035](esp32_3248s035/) | ST7796（SPI） | 320×480 | GT911 / XPT2046 / 无 | SPI |
| [jc2432w328](jc2432w328/) | ST7789（SPI） | 240×320 | CST820 / XPT2046 / 无 | SPI |

### ESP32-C3

| 板型标识 | LCD（接口） | 分辨率 | 触摸 | SD 卡 |
| --- | --- | --- | --- | --- |
| [esp32_2424s012](esp32_2424s012/) | GC9A01（SPI） | 240×240 | CST816S / 无 | - |

## 快速开始

### 安装与激活 ESP-IDF

推荐使用 [ESP-IDF Installation Manager（EIM）](https://docs.espressif.com/projects/idf-im-ui/en/latest/) 下载并安装 ESP-IDF 6.1 及所需工具链。以下命令适用于 Linux / macOS 的默认安装路径；自定义安装位置时，使用 EIM 生成的实际激活脚本路径。

每次打开终端后，按所用 Shell 选择一种方式激活环境。

Bash / Zsh：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
```

Fish：

```fish
source ~/.espressif/tools/activate_idf_v6.1.fish
```

激活环境后，从乐鑫组件中心下载的 `espressif/esp_board_manager` 扩展属于 ESP-IDF 的受信任来源，本流程无须手动设置 `IDF_EXTENSION_ALLOW_UNTRUSTED` 即可使用 `bmgr` 命令。下列操作均在已激活的终端中执行。

### 从组件中心获取 LVGL 示例

`idf.py add-dependency` 添加组件依赖，不会同时下载独立发布的示例工程。要运行 `display_lv_demo_widgets`，在用于存放工程的目录执行：

```bash
idf.py create-project-from-example "guition/guition_boards=0.2.2:display_lv_demo_widgets"
cd display_lv_demo_widgets
idf.py set-target esp32p4
idf.py bmgr -b jc8012p4a1
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

该流程以 `jc8012p4a1` 为例，将 `PORT` 替换为实际串口。创建示例的命令下载工程源码；首次配置或构建时，组件管理器会自动拉取 `guition_boards` 及其他依赖到 `managed_components/`。示例已声明 `guition_boards` 依赖，默认版本约束为 `"*"`，自动获取满足工程约束的版本，无须再次添加依赖。若需固定组件版本，在配置或构建前，将 `main/idf_component.yml` 中 `guition/guition_boards` 的版本改为 `"==0.2.2"`。

此命令要求组件中心的 `0.2.2` 版本已发布并包含 `display_lv_demo_widgets` 示例。若提示 `Cannot find example`，请检查该版本的 Examples 页面和示例名称；用法见[乐鑫示例工程文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-component-manager.html#creating-a-project-from-an-example)。

### 在现有工程中添加组件

将本组件复制到应用的 `components/guition_boards`。组件发布到组件中心后，也可通过以下命令添加：

```bash
idf.py add-dependency "guition/guition_boards==0.2.2"
```

本组件已声明 ESP Board Manager 依赖；各板型所需的驱动依赖由 ESP Board Manager 按所选配置生成。

### 单独使用板型

单独使用板型时，将完整板型目录复制到应用的 `components/` 下。例如复制 `jc3636k718/` 后，在应用目录执行：

```bash
idf.py add-dependency "espressif/esp_board_manager==0.7.3~1"
idf.py set-target esp32s3
idf.py bmgr -b jc3636k718
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

板型目录内的 `.c` 文件由 BMGR 生成组件统一编译；不要额外注册同一份触摸源码。无触摸模式仍需保留板型声明的基础触摸 API 依赖。总线和触摸参数均在板型自己的 YAML 文件中配置。

### 初始化与五色刷新示例

将 [board_init 示例](examples/board_init/README.md) 目录复制到独立工作目录。在复制后的示例目录执行：

```bash
idf.py set-target esp32p4
idf.py bmgr -b jc8012p4a1
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

示例在配置或构建时从乐鑫组件中心获取 `guition/guition_boards` `0.2.2` 及其他依赖，下载到 `managed_components/`，无需复制板级组件或配置本地路径。初始化 LCD、触摸和背光后，循环全屏显示红、绿、蓝、白、黑五种颜色，每种持续 1 秒。刷新分辨率和像素格式来自所选板型的 BMGR 配置。示例不使用 LVGL，也不初始化音频或 SD 卡。

MIPI DSI / RGB 使用双帧缓冲，在空闲缓冲区完成整帧绘制，并等待驱动的帧切换完成事件后复用旧缓冲区，以避免写入正在扫描的画面。板级配置需提供至少两个帧缓冲；各接口的同步要求见 [board_init 示例](examples/board_init/README.md)。

使用其他开发板时，将目标芯片和板型标识替换为对应值；切换板型后重新生成 BMGR 配置。切换目标芯片时建议使用独立工程目录，避免沿用旧的生成配置。ESP32-S31 是 ESP-IDF 6.1 的预览目标，相关命令需在 `idf.py` 后、动作名前添加 `--preview`，例如 `idf.py --preview bmgr -b jc5454f328` 和 `idf.py --preview build`。

## 配置与使用说明

- **触摸选择**：ESP32、ESP32-C3 和 ESP32-S3 板型在 menuconfig 的 `Guition touch` 中选择电容、电阻或无触摸，选项由板型能力决定。ESP32-P4 使用各板原生触摸配置。
- **触摸参数**：电容触摸使用 BMGR 原生 `lcd_touch` 设备，分辨率、复位与中断 GPIO、方向和 IO 参数在 `board_devices.yaml` 的 `lcd_touch.config` 中配置；`peripherals.i2c_addr` 使用左移一位的 I²C 地址，例如 7 位地址 `0x15` 写为 `[0x2A]`。电阻触摸使用 `lcd_touch_resistive.config`，引脚来自 `board_peripherals.yaml`。ESP32-2432S028 使用独立的软件 SPI；与 LCD 或 SD 卡共线的型号使用 ESP-IDF 硬件 SPI 管理共享总线。修改 YAML 后执行 `idf.py bmgr -b <board>` 和 `idf.py build`；ESP32-S31 在 `idf.py` 后添加 `--preview`。
- **设备初始化**：多触摸变体板型的触摸设备设置 `init_skip: true`，应用根据 `CONFIG_GUITION_TOUCH_CAPACITIVE` / `CONFIG_GUITION_TOUCH_RESISTIVE` 显式初始化 `lcd_touch` / `lcd_touch_resistive`，无触摸时跳过。两者均通过 `dev_lcd_touch_handles_t.touch_handle` 提供句柄。YAML 的依赖 `matches` 根据 menuconfig 选择驱动依赖；设备初始化选择见 [board_init 示例](examples/board_init/main/board_init.c)。不要批量初始化全部外设，以免启用未选择的总线。
- **显示缓冲**：本组件的 ESP32 / ESP32-C3 板型没有 PSRAM，应用需使用内部 RAM 缓冲。JC3248W535 的 AXS15231B 驱动要求整帧提交，应用需配置相应的刷新方式。
- **SD 卡**：提供 SD 选项的板型通过 `GUITION_SD_ENABLED` 启用，默认关闭。JC5454F328 使用原生 `fs_sdcard` 设备，由应用显式初始化，不使用 `GUITION_SD_ENABLED`。通过 ESP Board Manager 的 `fs_sdcard` 设备管理挂载、卸载及卡句柄；参数由板级设备和外设配置提供。共享 SPI 的板型需要应用协调片选状态、LCD / 触摸初始化顺序及挂载期间的触摸轮询。
- **SD 卡**：使用 FAT32 文件系统。
- **P4 配置**：旧版芯片需在 menuconfig 中选择匹配的芯片版本；应用需提供与 Flash 容量匹配的分区表，示例已附带分区表。

板型与触摸变体索引见 [board_catalog.json](board_catalog.json)。

使用 ESP-IDF 推荐通过 [ESP-IDF Installation Manager（EIM）](https://docs.espressif.com/projects/idf-im-ui/en/latest/) 下载和安装。

每次打开终端后，先激活环境,根据所需版本来配置环境，下面以v6.1为例，具体路径与版本根据实际情况进行替换。Bash/Zsh 使用：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
```

Fish 使用：

```fish
source ~/.espressif/tools/activate_idf_v6.1.fish
```

激活环境后，从乐鑫组件中心下载的 `espressif/esp_board_manager` 扩展属于 ESP-IDF 的受信任来源，本流程无需手动设置 `IDF_EXTENSION_ALLOW_UNTRUSTED` 即可使用 `bmgr` 命令。

从乐鑫组件中心获取示例工程：

```shell
idf.py create-project-from-example "guition/guition_boards=0.2.2:display_lv_demo_widgets"
cd display_lv_demo_widgets
```

该命令要求组件中心已发布的 `0.2.2` 包含 `display_lv_demo_widgets` 示例。`create-project-from-example` 创建独立的示例工程，后续配置或构建时自动下载组件依赖；仅执行 `add-dependency` 不会获取示例工程。示例已声明 `guition/guition_boards: "*"`，无须再次添加依赖。若需固定组件版本，在配置或构建前，将 `main/idf_component.yml` 中 `guition/guition_boards` 的版本改为 `"==0.2.2"`。

使用本地仓库中的示例时，进入仓库的 `example/display/display_lv_demo_widgets` 目录即可；其板级支持仍由组件中心提供，首次解析依赖时下载满足工程约束的最新版本，也可按上述方式在清单中固定版本。

以 `jc8012p4a1` 为例，在工程目录执行：

```shell
idf.py set-target esp32p4
idf.py bmgr -b jc8012p4a1
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

将 `PORT` 替换为实际串口。`idf.py bmgr -l` 可列出支持的开发板；使用其他板型时，选择对应的芯片目标和开发板名称。

`jc2432w328` 的触摸类型在 `idf.py menuconfig` 的 `Component config → Guition touch → Touch variant` 中选择：`No touch`（默认）、`Capacitive touch`（CST820）或 `Resistive touch (XPT2046)`。选择须与实际硬件一致；保存配置后执行 `idf.py reconfigure` 和 `idf.py build`，组件管理器会按触摸类型解析对应驱动依赖。

显示旋转在 `idf.py menuconfig → Demo display` 中配置，支持 0°、90°、180°、270°。`Display rotation method` 选择硬件或软件方式，`Display rotation angle` 选择角度；角度相对于板级 LCD 配置，90°/270° 交换逻辑宽高，触摸坐标同步转换。

普通 SPI 屏默认使用 20 行局部绘图缓冲和双缓冲，使绘制下一条带与上一条带传输重叠，减少刷新间隙。`Demo display` 中的 `SPI partial draw buffer lines` 可调整为 4～40 行，`SPI double draw buffers` 可关闭以节省内部 RAM；软件旋转还需要一块同尺寸的 DMA 条带缓冲。这些绘图缓冲与 RGB/MIPI 的板级 `num_fbs` 不同。JC3248W535 保留 GPIO38 TE 同步整帧传输，不受这两个选项影响。未接入 TE 的屏幕写入与扫描不同步，双绘图缓冲不能保证消除撕裂；较大条带的实际效果和可用内存需结合硬件验证。

- ESP32、ESP32-C3 的 240×240、240×320 板型，以及 JC3636 的 360×360 板型，可选择 LCD 控制器硬件旋转或软件旋转。
- 任一边超过 360 像素的屏幕，90°/270° 使用软件旋转。SPI/QSPI 和 MIPI 板型可先选择 `Display rotation angle → 180 degrees`，再选择 `Display rotation method → Hardware (LCD controller)`，通过 LCD 镜像命令实现硬件 180°，触摸坐标同步转换。此路径保持板级默认刷新模式，不需要为软件旋转增加帧缓冲。
- 当前普通 RGB 驱动的镜像操作属于 CPU 像素处理，RGB+3-wire SPI 板型也未启用命令镜像，因此这些板型仍使用软件旋转。RGB/MIPI 软件旋转使用 Adapter 的帧缓冲旋转；普通 SPI 软件旋转使用 Adapter 绘图回调和额外的内部 DMA 条带缓冲。硬件镜像调用失败时显示初始化会返回错误。
- JC3248W535 的 AXS15231B 使用异步 TE 软件旋转，默认 90°；其他板型默认 0°。RGB/MIPI 软件旋转需要板级 LCD 至少提供两个帧缓冲，SPI 软件旋转使用 RGB565。

RGB/MIPI 板型可在 `Demo display → RGB/MIPI single frame buffer (requires board num_fbs = 1)` 选择单帧局部刷新。单 buffer 禁止非零角度的软件旋转，仅支持 0° 无旋转或硬件旋转，显示可能出现撕裂。该选项不会修改板级内存分配；节省 PSRAM 时须将 board 的 `num_fbs` 设为 1 并重新执行 `idf.py bmgr -b <board>`。单帧模式仅在板级实际提供一帧且无需软件旋转时自动启用；板级提供两帧或三帧时，即使保存了单帧选项也会忽略该选项，保留多帧防撕裂模式；软件旋转或其他刷新模式所需帧数不足时，初始化会报告配置错误并停止，不注册不兼容的显示。

RGB/MIPI 板型选择软件旋转及非零角度后，`Demo display → RGB/MIPI software rotation refresh mode` 提供以下模式：

| 刷新模式 | Adapter 模式 | board `num_fbs` 要求 |
| --- | --- | --- |
| 两帧、局部刷新（默认） | `DOUBLE_PARTIAL` | 2 |
| 三帧、局部刷新 | `TRIPLE_PARTIAL` | 3 |
| 两帧、整屏刷新 | `DOUBLE_FULL` | **3**，非零旋转额外需要一帧 |
| 三帧、整屏刷新 | `TRIPLE_FULL` | 3 |

**menuconfig 的刷新模式不会自动修改 board 的 buffer 数量。** 必须在所选板型的 `board_devices.yaml` 中，将 `display_lcd` 设备的 `config.rgb_panel_config.num_fbs`（RGB，包括 RGB+3-wire SPI）或 `config.dpi_config.num_fbs`（MIPI）设为上表要求，再执行 `idf.py bmgr -b <board>`、`idf.py reconfigure` 和 `idf.py build`。帧缓冲不足会导致显示注册失败；更多帧缓冲会增加内存占用，整屏刷新会增加每次重绘的数据量。

`Demo display → Display color format` 可选择 RGB565 或 RGB888。MIPI DSI 默认 RGB888，其他接口默认 RGB565。MIPI 的输入和输出格式随选项设置，所选色深还须受屏幕驱动和厂家初始化命令支持；此选项不会改写厂家命令。其他接口必须与板级原生色深一致，否则初始化会返回错误。

JC4880P443 保留板级 RGB888 配置，不提供 RGB565 选项，其厂家初始化命令包含固定的像素格式设置。

MIPI 三帧整屏刷新复用 PSRAM 中的面板帧缓冲：一帧供 LVGL 绘图，另外两帧用于旋转输出和显示。例如，480×1280 的 RGB565 每帧为 1,228,800 字节，三帧共约 3.52 MiB；RGB888 时三帧共约 5.27 MiB。ESP32-P4 的适配器在格式和内存对齐满足条件时自动使用 PPA 旋转。选择三帧整屏刷新时，须同时在板级 YAML 设置 `num_fbs: 3`。

比较刷新性能时，应保持相同的编译优化等级、绘图任务数、缓存设置、页面操作和旋转角度。RGB888 的每帧数据量是 RGB565 的 1.5 倍；整屏刷新可减少内部绘图缓冲需求，但不能仅根据 PSRAM 占用判断流畅度。

MIPI 软件旋转选择局部刷新时，`MIPI software rotation partial draw buffer lines` 可设为 20～120，默认 80。适配器为局部刷新分配一块内部 SRAM 绘图缓冲，其容量为物理 LCD 宽度 × 配置行数 × 每像素字节数；该行数不等于 90°/270° 旋转后的逻辑绘图行数。480 像素宽、RGB565、80 行占 76,800 字节，RGB888 占 115,200 字节。内部内存不足时可减小行数，或使用复用 PSRAM 帧缓冲的整屏刷新。

`Log display refresh performance` 默认关闭，可在 menuconfig 中启用。启用后每五秒汇总 LVGL 有绘制内容的刷新次数、刷新率、处理耗时均值/P95/最大值及空闲内部 RAM/PSRAM。`flush_avg` 为刷新回调和同步等待的平均耗时，包含该路径中的旋转、缓存同步和等待；重叠区间仅计一次。`render_other_avg` 为其余处理耗时，包括布局和绘制。统计不包含尚未完成的异步面板扫描，刷新率不是屏幕实际显示帧率。对比刷新模式时应使用相同页面、色深、角度、编译优化等级和滑动操作，不能只根据 PSRAM 容量判断性能。

`jc1060f570c` 的 LCD 默认配置为 `num_fbs: 3`。减小 `num_fbs` 可以减少帧缓冲占用，增加可用于其他功能的 PSRAM；调整后仍须满足所选刷新模式的最低缓冲数量。使用两帧局部刷新或默认的双缓冲直接刷新时可设为 2；需要三帧的模式不可减为 2。

应修改实际被 Board Manager 使用的板级组件文件；仅修改外部 board 源码不会自动更新组件中心下载的版本。板级源码发布后，工程需更新依赖并重新生成板级配置。SPI/QSPI 的刷新路径以及 0° 时的默认刷新配置不受此选项影响。

依赖解析后由 `dependencies.lock` 锁定版本。需要更新组件时执行 `idf.py update-dependencies`，再执行 `idf.py bmgr -b <board>` 和 `idf.py build`。

Flash 容量由板级配置或 `idf.py menuconfig` 中的 `Serial flasher config → Flash size` 指定，应与实际硬件一致。分区设置使用自定义表且文件名为默认的 `partitions.csv` 时，构建会按该容量自动选择布局：

| Flash 容量 | 分区表 | 应用分区 | SPIFFS 存储分区 |
| --- | --- | --- | --- |
| 1MB | `partitions_1mb.csv` | 768KB | 128KB |
| 2MB | `partitions_2mb.csv` | 1792KB | 128KB |
| 4MB | `partitions_4mb.csv` | 3MB | 896KB |
| 8MB | `partitions_8mb.csv` | 5MB | 2944KB |
| 16MB 及以上 | `partitions.csv` | 9MB | 4MB |

自动选择依据构建配置，不探测硬件容量。更大 Flash 的剩余空间不分配；需要其他布局时，可在 `Partition Table` 中指定其他自定义 CSV 文件名。应用固件仍须小于对应应用分区容量。

`idf.py set-target` 后、选择开发板前，ESP-IDF 默认 Flash 容量可能为 2MB。此时可以继续执行 `idf.py bmgr -b <board>` 或 `idf.py menuconfig`，按实际硬件配置容量。1MB/2MB 分区可用于配置及容量检查，不保证包含全部 LVGL 功能的固件能够装入；应用超出分区时构建会报错。

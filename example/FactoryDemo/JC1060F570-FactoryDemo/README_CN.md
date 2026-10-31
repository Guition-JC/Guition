# JC1060F570C Factory Demo

中文 | [English](README.md)

本工程是适用于 Guition JC1060F570C 开发板的 ESP-IDF 工厂演示程序，提供触摸桌面、网络、媒体播放、设备设置及硬件工具。当前工程配置使用 ESP32-S31、1024 × 600 显示屏和 16 MB Flash。

## 环境要求

- JC1060F570C 开发板、USB 数据线，以及能够访问设备串口的电脑。
- 支持 ESP32-S31 的 **ESP-IDF 6.2.0 或更新版本**及其工具链。推荐使用 Espressif Installation Manager（EIM）安装 ESP-IDF。组件依赖声明见 `main/idf_component.yml`；ESP-IDF Component Manager 解析依赖时生成 `dependencies.lock`。
- 如果本地尚无所需组件，ESP-IDF Component Manager 需要联网下载依赖。

## 编译

在工程根目录打开终端，启用通过 EIM 安装的 ESP-IDF 环境。Linux 下使用 Bash 时执行：

```bash
source ~/.espressif/tools/activate_idf_master.sh
```

Linux 下使用 Fish 时执行：

```fish
source ~/.espressif/tools/activate_idf_master.fish
```

环境启用脚本的路径取决于 EIM 安装位置及所选的 ESP-IDF 版本。首次检出工程后，配置并编译：

```bash
idf.py --preview set-target esp32s31
idf.py bmgr -b jc1060f570c
idf.py menuconfig
idf.py build
```

ESP32-S31 在 ESP-IDF master 中属于预览目标，因此选择芯片时需要 `--preview`。工程提供 `sdkconfig.defaults`，ESP-IDF 在配置时生成本地 `sdkconfig`。

可通过 `idf.py menuconfig` 查看或修改配置。构建会生成 `build/FactoryDemo.bin`，以及引导程序、分区表、语音模型、资源和字体镜像；资源集根据所选板型确定。

## 烧录与串口监视

连接开发板，将下面的 `/dev/ttyACM0` 替换为实际串口：

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

按 `Ctrl+]` 退出串口监视。首次烧录，以及修改内置资源或字体后，请使用完整的 `flash` 命令：资源和字体位于独立分区，仅烧录应用程序不会更新它们。自定义分区布局见 `partitions.csv`。

## 使用方式

- 通过触摸桌面打开时钟、设置、Wi-Fi、以太网、音乐、视频、相册、频谱和 AI 聊天，也可使用工厂测试、GPIO、I2C、串口及 USB 等工具。
- 在 **Wi-Fi** 页面扫描并选择网络，或启动二维码配网。使用二维码配网时，将手机连接到设备热点并打开配网页面；连接失败后，在设备上重新发起二维码配网会话。
- 设备获得 IP 地址后，可在同一网络中访问 `http://<设备IP>/` 打开网页设置，天气配置页面位于 `/weather`。配网期间配网页占用 80 端口，设置页暂时停止服务。这些页面无需身份验证，请仅在可信网络中使用。
- **设置**页面提供语言、亮度、音量、时间和待机等选项；待机内容可选择时钟、照片或视频。
- 使用 SD 卡媒体时，将文件分别放入卡根目录的 `music/`、`image/` 和 `video/`。视频播放器支持 MP4 容器内的 MJPEG 视频和 AAC 或 MP3 音频；仅有 MP4 扩展名并不代表编码受支持。音乐还支持经典蓝牙音源。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `main/` | 程序入口、桌面、界面和网页设置 |
| `components/` | 开发板服务和应用组件 |
| `managed_components/` | ESP-IDF Component Manager 管理的依赖 |
| `spiffs/` | 按屏幕分辨率区分的内置资源 |
| `fonts/` | 内置字体及其许可信息 |
| `Docs/` | 中英文项目说明文档 |
| `sdkconfig.defaults` | 默认构建配置 |

各功能的详细说明见[文档索引](Docs/README.md)。

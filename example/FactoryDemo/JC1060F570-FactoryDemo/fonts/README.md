# 内置中文字体

桌面使用本目录中的中文字体。内置字体随固件资源烧录到独立的 `fonts` 分区。

| 文件 | 作用 |
| --- | --- |
| `SourceHanSansCN-VF.ttf` | Adobe Source Han Sans CN 可变源字体，不参与固件打包 |
| `desktop_sans_sc_light.ttf` | 派生字体母本 |
| `embedded/desktop_sans_sc_light.ttf` | 打包进 `fonts` 分区的派生字体 |
| `zh-freq.txt` | 按字频降序排列的常用汉字表 |
| `LICENSE.txt` | 上游 SIL Open Font License 1.1 许可全文 |

字体文件的基本名称使用字母、数字和下划线，以便 `esp_mmap_assets` 生成有效的 C 枚举标识符。字体内部的 family 名为 `PadOSSansSC`，full/PostScript 名为 `PadOSSansSC-Light`。

## 运行时读取

`partitions.csv` 为 `fonts` 分区分配 512 KiB。`main/CMakeLists.txt` 使用 `spiffs_create_partition_assets()` 打包 `embedded/` 中的字体。

1. `mmap_assets_new()` 校验分区内容，并通过 `esp_partition_mmap` 映射字体数据。
2. `lv_fs_make_path_from_buffer()` 将映射地址和长度转换为 LVGL 内存文件路径。
3. FreeType 从映射内存读取字体数据，避免逐字形进行文件系统读取。

设备启动前需要烧录字体分区；标准 `idf.py flash` 命令包含该分区镜像。字体映射或加载失败时，桌面使用内置 Montserrat 字体。更新字体后需要重新执行 `idf.py build` 和 `idf.py flash`，使字体镜像与应用中的校验信息一致。

## 字体来源与许可

派生字体来自 Adobe 的 Source Han Sans CN VF，采用 SIL Open Font License 1.1。派生字体为 **wght=300（Light）** 的静态字体，包含固件所需字符及部分常用汉字，文件大小为 **460,556 B**。

`Source` 是上游保留字体名，派生字体使用 `PadOSSansSC` 和 `PadOSSansSC-Light`。字体元数据保留上游版权声明、OFL 许可全文及许可 URL；其中版权声明中的 “Reserved Font Name 'Source'” 属于必要归属信息。

派生字体仍以 OFL 1.1 分发，许可全文见 [LICENSE.txt](LICENSE.txt)。字体不得单独售卖。

常用汉字表 `zh-freq.txt` 来源于采用 MIT 许可的 `jieba` 项目 `dict.txt`，按词频对字符加权统计并降序排列。内置字体仅覆盖部分汉字，未覆盖的字符需要包含相应字形的字体支持。

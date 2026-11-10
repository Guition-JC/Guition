# Guition

## 克隆仓库

克隆时使用 `--recurse-submodules`，同时下载仓库中的子模块：

```bash
git clone --recurse-submodules https://github.com/Guition-JC/Guition.git
cd Guition
```

如果已经克隆仓库，在仓库根目录执行以下命令初始化并下载子模块：

```bash
git submodule update --init --recursive
```

拉取仓库更新后，同步子模块到仓库记录的版本：

```bash
git pull
git submodule update --init --recursive
```

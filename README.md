# PowerScope

专门面向 Windows 11 高性能笔记本的 C++20 终端耗能诊断器。当前配置重点适配：

- Intel Core Ultra 平台
- NVIDIA GeForce RTX Laptop GPU（通过驱动自带 NVML）
- Windows 11 内置屏幕、HDR/高级颜色和现代电源管理

## 它能准确测什么

### 直接测量

- **电池供电时的整机实时功耗**：读取 Windows `SYSTEM_BATTERY_STATE.Rate`，来源是电池控制器，单位为 mW。
- **NVIDIA 独显功耗**：通过 NVIDIA 驱动自带的 NVML 获取，单位为 mW。

### 活动与配置指标

- CPU 总利用率、平均当前频率、频率上限
- 最活跃进程的 CPU 与磁盘 I/O
- 物理网卡实时收发速率（主动避开 TUN/虚拟网卡重复计数）
- 内存占用
- 屏幕分辨率、刷新率、亮度、HDR/高级颜色状态
- 电源来源、电池百分比、节电模式、活动电源方案
- 5 秒、30 秒和 60 秒整机功耗滑动平均值
- 固定区域原地刷新，减少终端闪烁和错位
- 自动异常判断（不生成日志文件）

## 它不会伪造什么

- **插电状态的整机输入功率**：Windows 没有适用于所有电脑的公开统一接口。插电时应使用外置插座功率计。
- **CPU Package Power**：纯用户态公开 Windows API 不提供。要加入该指标，需要 Intel PCM/RAPL 及其底层驱动；本版不把 CPU 利用率伪装成瓦数。
- 屏幕、内存、SSD、主板、风扇的独立瓦数：大多数笔记本没有公开传感器，只报告影响因子。

## 编译

### 需要安装

1. Visual Studio 2022 Build Tools 或完整 Visual Studio 2022。
2. 勾选 **“使用 C++ 的桌面开发”**。
3. 组件中包含：
   - MSVC v143 x64/x86
   - Windows 11 SDK
   - CMake tools for Windows

### 一键构建

在 PowerShell 中进入项目目录后：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

成功后项目根目录会出现：

```text
PowerScope.exe
```

程序静态链接 MSVC 运行库，通常无需另外安装 VC++ Runtime。

## 运行

```powershell
.\PowerScope.exe
```

默认行为：

- 每 1 秒刷新终端；
- 每 5 秒查询一次 NVML；
- 在固定终端区域内原地刷新，不滚屏、不生成日志文件；
- 按 `Ctrl+C` 正常停止。

常用选项：

```powershell
# 禁用 NVIDIA NVML，验证监控本身是否影响独显休眠
.\PowerScope.exe --no-gpu

# 将终端刷新间隔改为 2 秒
.\PowerScope.exe --interval-ms 2000

# 将 NVML 查询间隔改为 10 秒
.\PowerScope.exe --gpu-interval-ms 10000
```

## 推荐测试流程

1. 电池充到 90% 以上，拔掉适配器。
2. 关闭游戏、下载、系统更新和不必要的外接设备。
3. 先运行：

```powershell
.\PowerScope.exe --no-gpu
```

空闲 5 分钟，观察 30 秒和 60 秒平均整机功耗。

4. 再运行默认模式：

```powershell
.\PowerScope.exe
```

比较整机功耗是否因 NVML 查询显著升高，并观察 RTX 5060 是否有持续功耗、频率和温度。

5. 分别测试：
   - 60 Hz 与高刷新率；
   - HDR 开与关；
   - 低亮度与高亮度；
   - 核显模式与独显直连模式。

## 数据解释

以 99.9 Wh 电池为例，不考虑关机保留量和转换差异：

- 12.5 W ≈ 8 小时
- 20 W ≈ 5 小时
- 33 W ≈ 3 小时
- 50 W ≈ 2 小时

请优先看 **30 秒和 60 秒平均值**，不要只看瞬时功耗。程序启动后的前 60 秒会用当前已有样本预热，并在终端显示进度。

## 已知限制

- 某些电池驱动不报告实时 Rate，此时整机功耗显示 N/A。
- NVML 指标是否可用由 NVIDIA 驱动、GPU 电源状态和笔记本固件决定。
- 查询 NVML 在部分混合显卡笔记本上可能唤醒独显，因此程序提供 `--no-gpu` 对照模式。
- WMI 亮度仅适用于支持 `WmiMonitorBrightness` 的内置显示器；外接显示器通常显示 N/A。
- 进程 I/O 是可访问进程的汇总，不等同于存储控制器的物理总吞吐量。

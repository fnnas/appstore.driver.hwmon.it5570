# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 常用命令

本仓库是仓库外维护的 Linux 内核 hwmon 模块，主要构建入口在 `drv/Makefile`。

```sh
make -C drv
```

为指定内核版本构建：

```sh
make -C drv TARGET="$(uname -r)"
```

清理内核模块构建产物：

```sh
make -C drv clean
```

检查构建产物中的 git revision 注入结果：

```sh
modinfo -F revision drv/it5570_hwmon.ko
```

常用加载与卸载命令：

```sh
sudo insmod drv/it5570_hwmon.ko
sudo insmod drv/it5570_hwmon.ko transport=d2ec
sudo rmmod it5570_hwmon
```

查看探测和注册日志：

```sh
dmesg | tail -n 120
```

检查模块元数据与目标内核是否匹配：

```sh
modinfo drv/it5570_hwmon.ko
modinfo -F revision drv/it5570_hwmon.ko
modinfo -F vermagic drv/it5570_hwmon.ko
```

启用 dynamic debug 观察 D2EC 寄存器访问：

```sh
sudo sh -c 'echo "module it5570_hwmon +p" > /sys/kernel/debug/dynamic_debug/control'
sudo dmesg -C
sudo insmod drv/it5570_hwmon.ko
dmesg | tail -n 200
sudo rmmod it5570_hwmon
sudo sh -c 'echo "module it5570_hwmon -p" > /sys/kernel/debug/dynamic_debug/control'
```

仓库当前没有发现独立的自动化测试或单测入口；验证主要依赖内核模块构建、`modinfo`、加载日志和目标机器上的 sysfs 节点读回。

## 运行时观察命令

定位驱动注册出的 hwmon 目录：

```sh
for h in /sys/class/hwmon/hwmon*; do
    [ -r "$h/name" ] || continue
    [ "$(cat "$h/name")" = "it5570_hwmon" ] || continue
    echo "$h"
    break
done
```

读取驱动暴露的全部节点：

```sh
for h in /sys/class/hwmon/hwmon*; do
    name_file="$h/name"
    [ -r "$name_file" ] || continue
    read -r name < "$name_file"
    [ "$name" = "it5570_hwmon" ] || continue

    printf '===== %s =====\n' "$h"
    for f in "$h"/*; do
        [ -e "$f" ] || continue
        printf '%s=' "$f"
        if [ -f "$f" ] && [ -r "$f" ]; then
            read -r value < "$f" 2>/dev/null && printf '%s\n' "$value" || echo "<read error>"
        elif [ -L "$f" ]; then
            readlink -f "$f" 2>/dev/null || echo "<broken symlink>"
        elif [ -d "$f" ]; then
            echo "<directory>"
        else
            echo "<other>"
        fi
    done
done
```

## 架构概览

驱动分为探测/传输选择、底层 SuperIO/D2EC 访问、以及 hwmon/sysfs 暴露三层。`it5570_main.c` 负责模块参数、platform device/driver 生命周期、SuperIO 端口探测、LDN 枚举和传输路径选择。`it5570_core.c` 放共享查找表、日志封装、SuperIO 访问、D2EC 字节读写和可选 ACPI 诊断。`it5570_sensors.c` 负责 hwmon 注册、标准传感器读回、raw debug sysfs 节点、EC 寄存器到电压/RPM/PWM 占空比的换算。`it5570_hwmon.h` 集中保存寄存器定义、枚举、共享状态结构和跨文件函数声明。

当前真实传感器路径以 `d2ec` 为准。`transport=auto` 默认优先选择 D2EC；`smfi`、`pmc*`、`peci` 分支仍用于识别、探测和日志说明，但当前不提供真实 EC RAM 传感器读写能力。不要让不支持的传输路径伪造传感器值；无法提供真实读数时应保持错误/跳过语义。

D2EC 访问通过探测到的 IT5570 SuperIO 配置端口进行。驱动写入 EC RAM 地址到 Depth-2 地址寄存器，再通过 `I2EC_DATA` 读取数据。使用的 EC RAM 窗口主要是 `0x1800` 的 PWM/风扇 tach 寄存器和 `0x1900` 的 ADC 结果/状态寄存器。`struct it5570_hwmon_data` 中的 `io_lock` 用于串行化底层 I/O，避免并发 sysfs 读取交叉污染地址/数据事务。

标准 hwmon 节点包括 `in0_input..in7_input`、`fan1_input..fan3_input`、`pwm1..pwm8`。PWM 相关节点当前是只读观测接口；raw debug 节点也只读，用来把芯片文档寄存器、板级布线和实机行为对应起来。不要重新引入 `allow_pwm_write`、raw store handler 或标准 PWM 写入路径，除非用户明确要求重新设计写入能力。

PWM 读回不是固定读取单一周期寄存器。`it5570_sensors.c` 会根据 `PCSSGL`、`PCSSGH`、`CLK6MSEL.CTRMODE` 和 `CTR0/CTR1/CTR1M/CTR2/CTR3` 解析每个 PWM 通道当前使用的周期配置，再把 DCR 映射到 hwmon 的 `0..255` 标度。边界语义来自 IT5570 文档：`DCR == 0` 和 `DCR > CTR` 视为输出低电平，`DCR == CTR` 视为输出高电平。`PWMPOL` 反相位会影响标准 `pwmN` 的有效占空比读回语义。

风扇 tach 读取使用 LSB/MSB 成对寄存器，并按文档保留 raw 计数为 0 时的停转语义。`fanN` 和 `pwmN` 不保证板级上一一对应，驱动不应伪造这种映射；需要通过 raw 节点和实机观察确认实际布线。

电压输入来自 ADC EC view，按 10 位 ADC 原始值和默认 3.0V ADC 参考电压换算成毫伏。新增寄存器换算时，应优先查 `docs/IT5570_A_V0.3.1_U.md`，并在相关代码附近保留必要的文档公式或寄存器语义说明。

## 重要约束

`drv/README.md` 是当前行为说明的权威文档之一，修改驱动行为时要同步更新其中的构建、模块参数、节点可见性和 PWM/TACH 语义。芯片手册的 Markdown 版本在 `docs/IT5570_A_V0.3.1_U.md`，涉及寄存器语义、PWM 边界条件或 ADC/TACH 换算时应优先查阅它。

构建环境必须有目标内核的 headers。`drv/Makefile` 会优先查找 `/usr/src/linux-headers-$(TARGET)`，其次 `/usr/src/kernels/$(TARGET)`，最后使用 `/lib/modules/$(TARGET)/build`。当前 Claude Code 运行的 Windows 侧环境通常没有 Linux 内核模块工具链，例如 `make` 和目标内核 headers；不要反复尝试在 Windows 侧编译这个 Linux 驱动，也不能声称模块构建已通过。需要构建验证时，请让用户在目标 Linux 机器上运行 `make -C drv` 并回传输出。

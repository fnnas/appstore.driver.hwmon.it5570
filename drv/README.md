# it5570_hwmon

`it5570_hwmon` 是一个面向使用 ITE IT5570 嵌入式控制器机器的、仓库外维护的 Linux hwmon 驱动。当前实现针对通过 IT5570 SuperIO Depth-2 EC 访问路径暴露出来的 EC RAM 传感器寄存器。

驱动会注册标准 Linux hwmon 节点，用于读取电压、风扇转速计以及 PWM 占空比；同时提供一组只读 raw 调试节点，便于把文档寄存器、板级布线和实机行为对应起来。

## 当前状态

- 主传输路径：`d2ec`
- 默认传输策略：优先自动选择 `d2ec`
- 电压输入：8 路标准 `in0_input..in7_input`
- 风扇输入：3 路标准 `fan1_input..fan3_input`
- PWM 占空比视图：8 路标准 `pwm1..pwm8`
- PWM 相关节点：当前均为只读观测接口
- 复杂模式寄存器：只通过 raw 节点暴露，不走标准 hwmon 写接口
- ACPI 传感器探测：仅作为可选调试辅助

`smfi`、`pmc*` 和 `peci` 传输分支目前仍保留用于识别和探测，但当前驱动并不通过这些分支提供真实 EC RAM 读写能力。

## 构建

在仓库根目录执行：

```sh
make -C drv
```

为指定内核版本构建：

```sh
make -C drv TARGET="$(uname -r)"
```

清理生成的模块文件：

```sh
make -C drv clean
```

模块 revision 会在构建时从仓库提交号注入：

```sh
modinfo -F revision drv/it5570_hwmon.ko
```

如果无法读取 git commit，revision 会显示为 `none`。

## 加载

典型加载命令：

```sh
sudo insmod drv/it5570_hwmon.ko
```

当未指定 `transport` 时，驱动会使用自动传输选择路径。若要显式测试 D2EC：

```sh
sudo insmod drv/it5570_hwmon.ko transport=d2ec
```

卸载：

```sh
sudo rmmod it5570_hwmon
```

查看探测日志：

```sh
dmesg | tail -n 120
```

成功选择传输路径时，通常会看到：

```text
stage=transport event=evaluating branch=d2ec forced=0
stage=transport event=selected branch=d2ec
stage=hwmon event=register status=ready transport=d2ec
```

## 模块参数

`transport`

选择传输分支：

```text
auto|d2ec|smfi|pmc1|pmc2|pmc3|pmc4|pmc5|peci|off
```

推荐使用 `auto`。当前它会优先尝试 `d2ec`，因为这是已经验证过的真实传感器访问路径；当未指定 `transport` 时，这也是默认值。

`probe_acpi`

启用可选的 ACPI RPMD/RPRC 风格方法探测，用于诊断；正常的传感器注册流程不会使用它。

`fault_inject`

仅用于测试的负路径控制：

```text
none|id_mismatch|smfi_unstable|reg_access_fail
```

正常运行时不要启用 fault injection。

## 暴露的标准 hwmon 节点

标准节点的具体可见性仍取决于探测阶段是否读到了有效通道，以及 D2EC 传输是否可用。典型会看到：

```text
name
in0_input
...
in7_input
fan1_input
fan2_input
fan3_input
pwm1
pwm2
pwm3
pwm4
pwm5
pwm6
pwm7
pwm8
```

### 重要说明

- `pwm1..pwm8` 是 **PWM 输出通道**，对应芯片的 PWM0..PWM7。
- `fan1..fan3` 是 **tach 采样通道**，对应 Fan1..Fan3 / TACH0..TACH2 的读取路径。
- **`pwmN` 与 `fanN` 不保证一一对应。** 驱动不会伪造板级映射关系。要确认某个 PWM 通道实际控制哪个风扇，请结合 raw 节点和实机观察建立映射。
- `fan3` 可能受 `PWM5TOCTRL` 的特殊模式影响；在某些配置下，TACH2 路径可能更接近 backlight/frequency 语义，而不是普通风扇 RPM。

## 读取全部驱动节点

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
            read -r value < "$f" 2>/dev/null \
                && printf '%s\n' "$value" \
                || echo "<read error>"
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

## 传感器含义

### 电压输入

电压输入以毫伏为单位上报。驱动会从 IT5570 EC ADC 寄存器窗口读取 10 位 ADC 采样值，并按文档中的默认 3.0V ADC 参考电压进行换算。

### 风扇输入

风扇输入以 RPM 为单位上报。原始转速计节点会暴露用于 RPM 换算的 EC 原始 tach 计数值。原始 tach 计数为 `0` 时，会按文档中的风扇停转语义上报为 `0 RPM`。

### PWM 输入/写入

`pwm1..pwm8` 采用标准 hwmon 的 `0..255` 标度上报。IT5570 文档中，常规 PWM 占空比定义为：

```text
DCRi / (CTR + 1)
```

但每个 PWM 通道的 **实际周期寄存器** 并不一定固定是 `CTR0`。驱动会在读取/写入时，根据运行时的：

- `PCSSGL`
- `PCSSGH`
- `CLK6MSEL.CTRMODE`
- `CTR0/CTR1/CTR1M/CTR2/CTR3`

来解析该 PWM 通道当前真正使用的周期配置。

边界规则遵循文档：

- `DCR == 0` -> 输出保持低电平，读值上报 `0`
- `DCR > CTR` -> 输出保持低电平，读值上报 `0`
- `DCR == CTR` -> 输出保持高电平，读值上报 `255`

除这些边界情况外，驱动按当前有效的 `CTR(+1)` 把 duty 映射到 hwmon `0..255`。

## 建议的 PWM/TACH 映射观察流程

若要确认“哪个 `pwmN` 实际对应哪个 `fanN`”，建议：

1. 先观察所有标准节点与 raw 节点的基线值；
2. 一次只改一个 `pwmN`；
3. 同时观察对应的 `pwmN_dcr_raw` 以及全部 `fan*_tach_raw / fan*_input`；
4. 记录哪些风扇 tach 会随某个 PWM 通道变化。

示例：

```sh
h=
for x in /sys/class/hwmon/hwmon*; do
    [ -r "$x/name" ] || continue
    [ "$(cat "$x/name")" = "it5570_hwmon" ] || continue
    h="$x"
    break
done

cat "$h"/pwm[1-8] "$h"/fan[1-3]_input
cat "$h"/pwm*_dcr_raw "$h"/pwm_clock_src_sel_low_raw "$h"/pwm_clock_src_sel_high_raw
```

注意：

- `pwmN` 与 `fanN` 不保证一一对应。
- `fan3` 可能受 `pwm5_timeout_ctrl_raw` / `tach_switch2_raw` / `backlight_duty_raw` 等节点反映出的特殊模式影响。
- 若某些 `pwmN` 读值变化但所有 `fanN` 都不变，不代表读取失败，也可能是该 PWM 通道没有接风扇、被别的模式接管，或对应的是其他负载。

## 关键 raw/debug 节点

驱动会暴露一组只读 raw 调试节点，帮助你把文档寄存器和板级行为对应起来。典型包括：

```text
fan1_tach_raw / fan2_tach_raw / fan3_tach_raw
pwm1_dcr_raw .. pwm8_dcr_raw
pwm3_dcr_msb_raw / pwm4_dcr_msb_raw
pwm1_ctr_raw
pwm_group1_ctr_raw / pwm_group1_ctr_msb_raw
pwm_group2_ctr_raw / pwm_group3_ctr_raw
pwm1_cpr_raw
pwm_group4_cpr_raw / pwm_group4_cpr_msb_raw
pwm_group6_cpr_raw / pwm_group6_cpr_msb_raw
pwm_group7_cpr_raw / pwm_group7_cpr_msb_raw
pwm_polarity_raw
pwm_clock_freq_sel_raw
pwm_clock_src_sel_low_raw
pwm_clock_src_sel_high_raw
pwm_clock_gating_raw
pwm_clock_control_raw
pwm_6mhz_mode_raw
pwm_open_drain_raw
pwm_load_counter_ctrl_raw
tach_zone_int_status_raw
tach_switch_raw / tach_switch2_raw
pwm5_timeout_ctrl_raw / backlight_duty_raw
pwm1_led_enable_raw / pwm1_led_ctrl1_raw / pwm1_led_ctrl2_raw
pwm2_led_enable_raw / pwm2_led_ctrl1_raw / pwm2_led_ctrl2_raw
```

## 硬件访问说明

D2EC 访问使用的是探测阶段发现的 IT5570 SuperIO 配置端口。驱动会通过 Depth-2 寄存器写入 EC RAM 地址，再通过 `I2EC_DATA` 读取数据。

该驱动使用的 EC RAM 窗口如下：

```text
0x1800  PWM 和风扇转速计寄存器
0x1900  ADC 结果和 ADC 有效状态寄存器
```

驱动内部会串行化 D2EC 事务，避免并发 sysfs 读取时发生地址/数据访问交叉。标准 `pwmN` 读写只会改变 duty 寄存器，不会自动修改频率、时钟源、分组、极性、open-drain 或 TACH source 选择。

## 调试

启用 dynamic debug 以输出详细的 D2EC 寄存器读取日志：

```sh
sudo sh -c 'echo "module it5570_hwmon +p" > /sys/kernel/debug/dynamic_debug/control'
sudo dmesg -C
sudo insmod drv/it5570_hwmon.ko
dmesg | tail -n 200
sudo rmmod it5570_hwmon
sudo sh -c 'echo "module it5570_hwmon -p" > /sys/kernel/debug/dynamic_debug/control'
```

常用模块元数据检查：

```sh
modinfo drv/it5570_hwmon.ko
modinfo -F revision drv/it5570_hwmon.ko
modinfo -F vermagic drv/it5570_hwmon.ko
```

如果加载失败，首先确认 `vermagic` 与目标内核是否一致。

## 开发说明

- 除非能证明其他传输路径可以返回真实的 EC RAM 数据，否则应保持 D2EC 作为权威传感器路径。
- 不要添加伪造的传感器数值。不支持的传输路径应该返回错误，而不是伪造读数。
- 新增寄存器换算时，请在相应代码附近写清楚文档公式或寄存器语义。
- 源码布局：
  - `it5570_main.c`：模块参数、platform device/driver、SuperIO 探测、LDN 枚举和传输路径选择。
  - `it5570_core.c`：共享查找表、日志、ACPI 诊断、SuperIO 和 D2EC 字节访问。
  - `it5570_sensors.c`：hwmon 注册、sysfs 原始节点、EC 寄存器映射和传感器换算公式。

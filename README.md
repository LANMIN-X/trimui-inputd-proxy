# TrimUI Inputd Proxy

面向 **TrimUI Smart Pro** 的输入代理。程序读取被隐藏的物理手柄设备，通过 `uinput` 创建名为 `TRIMUI Player1` 的虚拟手柄并转发输入事件；同时将游戏的 `FF_RUMBLE` 请求转换为 GPIO 马达控制。

## 功能

- 保持原厂手柄的名称、VID/PID、按键和轴范围，避免游戏重新映射按键。
- 独占物理输入设备，避免真实设备和虚拟设备同时被应用读取。
- 支持 `FF_RUMBLE`，包括 effect 上传、擦除、播放和 `replay.delay`。
- 使用固定 50 Hz、按震动强度比例变化的 GPIO PWM。
- 初始化、设备断开和写入失败会明确报错并退出，不会静默继续运行。

## 编译

在目标设备或对应的 AArch64 Linux 交叉编译环境中执行：

```sh
gcc -O2 -Wall -Wextra -std=gnu11 -o trimui_inputd_proxy trimui_inputd_proxy.c
```

程序使用 Linux 的 `uinput` 和 input 头文件，不需要额外库。

## 运行前提

- 需要 root 权限，且内核已启用 `/dev/uinput`。
- 物理设备必须已通过外部启动脚本或规则暴露为 `/dev/input/trimui_raw`。本仓库不负责创建该路径。
- 震动马达路径固定为 `/sys/class/gpio/gpio227/value`；设备 GPIO 编号不同，需要先修改源码中的 `RUMBLE_GPIO_PATH` 后重新编译。

## 运行

确认上述路径都存在后：

```sh
chmod +x trimui_inputd_proxy
./trimui_inputd_proxy
```

程序启动后会对 `trimui_raw` 执行 `EVIOCGRAB`，物理手柄事件仅由虚拟手柄对外提供。按 `Ctrl-C` 或发送 `SIGTERM` 会销毁虚拟设备并关闭马达。

## 震动行为与限制

- 最多可保存 16 个 rumble effect；单马达同一时刻只播放一个 effect，新的播放请求会替换当前震动。
- `replay.delay` 会生效；单次震动最长限制为 3 秒，避免异常请求让马达持续运转。
- 仅声明并处理 `FF_RUMBLE`，不支持 `FF_GAIN` 或其他 FF effect 类型。
- 由于需要真实硬件的输入设备、uinput 和 GPIO，本项目应在设备上完成最终验证。

## 鸣谢

FF 震动实现思路参考 [Trimui_Smart_Pro_Inputd](https://github.com/Jpe230/Trimui_Smart_Pro_Inputd)。该项目是完整 inputd 实现；本项目定位为代理原厂 inputd 的输入与震动功能。

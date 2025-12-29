# TrimUI Smart Pro Input Proxy (Proxy FF)

一个用于 **TrimUI Smart Pro** 的输入代理程序，通过 **uinput** 创建虚拟手柄，并使用 **GPIO 实现真实 Force Feedback（震动）**，解决原生输入设备在部分游戏 / SDL2 环境下的兼容性与震动问题。

---

## ✨ 功能特性

- 创建 **uinput 虚拟手柄**
- 支持 **Force Feedback（FF Rumble）**
- 通过 **GPIO 控制真实震动马达**
- 全程 **非阻塞 I/O**，防止死锁
- 防止部分游戏发送高频垃圾震动指令
- 使用 **通用 VID / PID (0x0000:0x0000)**
  - 避免 SDL2 / Steam / RetroArch 误识别为 Xbox 手柄
  - 防止 D-Pad 自动反转等问题
- 适合只读 rootfs、固件内置、开机自启场景

---

## 🎮 适用场景

- TrimUI Smart Pro 原生系统
- OpenDingux / 定制 Linux 固件
- RetroArch / SDL2 游戏
- systemd / init.d 后台常驻服务
- 不依赖配置文件的嵌入式环境

---

## 🧱 架构说明

```
物理手柄 (/dev/input/eventX)
            │
            │ EVIOCGRAB（独占）
            ▼
   Input Proxy（本程序）
            │
            ├── 转发输入事件
            ▼
   uinput 虚拟手柄
            │
            ├── FF 震动请求（EV_FF）
            ▼
      GPIO 震动马达
```

---

## 🔧 关键设计点

### 虚拟手柄（uinput）

- 创建标准 ABS + KEY + FF 手柄
- 使用通用 VID / PID：

```
vendor  = 0x0000
product = 0x0000
```

- 避免 SDL2 误判为 Xbox / PS 控制器

### Force Feedback 震动

- 完整实现 uinput FF 协议：
  - UI_FF_UPLOAD
  - UI_FF_ERASE
  - EV_FF 播放指令
- 支持：
  - 强 / 弱震动自动取最大值
  - 时长限制（防止无限震动）
  - 状态防抖（相同状态不重复写 GPIO）

### GPIO 震动控制

- 使用 sysfs：

```
/sys/class/gpio/gpio227/value
```

- 状态缓存，避免高频写入
- 所有写操作均为 **非阻塞**

---

## 📌 硬编码配置

本版本已完全移除配置文件，所有参数硬编码，适合固件内置：

```
#define DEVICE_NAME     "TRIMUI Player1 (Proxy FF)"
#define RUMBLE_STRENGTH 100
```

---

## 🛠 编译方式

```
gcc -O2 -o trimui_input_proxy trimui_input_proxy.c -lm
```

---

## ▶️ 运行方式

```
./trimui_input_proxy
```

默认使用 `/dev/input/event3` 作为物理手柄。

---

## ⚠️ 注意事项

- 需要 root 权限
- 程序会独占物理输入设备（EVIOCGRAB）
- 退出时自动关闭震动并释放设备

---

## 📚 参考与致谢

Force Feedback 实现参考：

- Trimui_Smart_Pro_Inputd  
  https://github.com/Jpe230/Trimui_Smart_Pro_Inputd

在此基础上进行了非阻塞化、防抖与兼容性优化。
也可以直接用他的，不过他的这个按键无法全部正确隐射，需要获取硬件的按键码 才能正确全部被是识别。
我这个是代理inputd，他这个是一个完整的inputd

---

## 🤖 代码说明

- 代码由 **ChatGPT（GPT）** 与 **Gemini** 协助编写
- 人工进行结构设计、逻辑取舍与最终整合
- 定位为工程级 / 固件级工具代码

---

## 📄 License

本项目可自由用于学习、研究、固件集成与个人设备使用。

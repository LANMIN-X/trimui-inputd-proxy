/*
 * TrimUI Smart Pro 输入代理 (v6 终极版)
 *
 * 功能：
 * - 完美克隆原厂手柄 ID，无需重新映射按键
 * - 读取被隐藏的 trimui_raw 设备，解决双击问题
 * - 软件 PWM 算法，提供细腻震动反馈
 *
 * 编译：
 * gcc -O2 -o trimui_inputd_proxy proxy.c -lm
 */

#include <linux/uinput.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>

/* ============================================================
 * 配置与常量
 * ============================================================ */
// 完美伪装成原厂手柄
#define DEVICE_NAME      "TRIMUI Player1"
#define DEVICE_VENDOR    0x045e
#define DEVICE_PRODUCT   0x028e
#define DEVICE_VERSION   0x0114

// 配合脚本的隐藏路径
#define REAL_DEV_PATH    "/dev/input/trimui_raw"
#define RUMBLE_GPIO_PATH "/sys/class/gpio/gpio227/value"

// PWM 震动参数 (调节手感)
#define RUMBLE_DEADZONE   2000   // 忽略极微小的噪音信号
#define PWM_THRESHOLD     40000  // 超过此强度全速震动，低于此强度脉冲震动
#define SAFETY_TIMEOUT_MS 3000   // 最长震动时间，防止卡死

static volatile sig_atomic_t keep_running = 1;

/* ============================================================
 * GPIO 控制
 * ============================================================ */
static int g_gpio_fd = -1;
static int g_gpio_last_state = -1;

static void gpio_init(void) {
    g_gpio_fd = open(RUMBLE_GPIO_PATH, O_WRONLY | O_NONBLOCK);
}

static void gpio_set(int state) {
    if (state == g_gpio_last_state) return;
    if (g_gpio_fd >= 0) {
        char v = state ? '1' : '0';
        write(g_gpio_fd, &v, 1);
    }
    g_gpio_last_state = state;
}

/* ============================================================
 * 震动逻辑 (PWM 核心)
 * ============================================================ */
#define RUMBLE_MAX_EFFECTS 16

typedef struct {
    struct ff_effect effect;
    bool in_use;
} rumble_slot_t;

typedef struct {
    rumble_slot_t slots[RUMBLE_MAX_EFFECTS];
    
    bool active;           // 是否处于震动状态
    uint32_t magnitude;    // 当前震动总强度
    struct timespec stop_time; // 预计停止时间
    
    uint32_t pwm_counter;  // PWM 计数器
} rumble_ctx_t;

static void timespec_now(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static void timespec_add_ms(struct timespec *ts, unsigned int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static bool timespec_passed(const struct timespec *stop_at) {
    struct timespec now;
    timespec_now(&now);
    if (now.tv_sec != stop_at->tv_sec)
        return now.tv_sec > stop_at->tv_sec;
    return now.tv_nsec >= stop_at->tv_nsec;
}

static int rumble_upload(rumble_ctx_t *ctx, struct ff_effect *eff) {
    if (eff->type != FF_RUMBLE) return 0;
    int id = eff->id;
    if (id < 0) {
        for (int i = 0; i < RUMBLE_MAX_EFFECTS; i++) {
            if (!ctx->slots[i].in_use) { id = i; break; }
        }
        if (id < 0) return -ENOSPC;
    } else if (id >= RUMBLE_MAX_EFFECTS) return -EINVAL;

    ctx->slots[id].in_use = true;
    ctx->slots[id].effect = *eff;
    ctx->slots[id].effect.id = id;
    eff->id = id;
    return 0;
}

static int rumble_erase(rumble_ctx_t *ctx, int id) {
    if (id >= 0 && id < RUMBLE_MAX_EFFECTS) {
        ctx->slots[id].in_use = false;
        ctx->active = false;
        gpio_set(0);
    }
    return 0;
}

static void rumble_play(rumble_ctx_t *ctx, int id, int val) {
    if (val == 0) {
        ctx->active = false;
        gpio_set(0);
        return;
    }
    if (id < 0 || id >= RUMBLE_MAX_EFFECTS || !ctx->slots[id].in_use) return;

    struct ff_effect *e = &ctx->slots[id].effect;
    
    uint32_t mag = e->u.rumble.strong_magnitude + e->u.rumble.weak_magnitude;
    if (mag < RUMBLE_DEADZONE) {
        ctx->active = false;
        gpio_set(0);
        return;
    }

    ctx->magnitude = mag;
    unsigned int dur = e->replay.length;
    if (dur == 0 || dur > SAFETY_TIMEOUT_MS) dur = SAFETY_TIMEOUT_MS;

    timespec_now(&ctx->stop_time);
    timespec_add_ms(&ctx->stop_time, dur);
    
    ctx->active = true;
}

// 模拟 PWM 的心跳函数
static void rumble_tick(rumble_ctx_t *ctx) {
    if (!ctx->active) {
        gpio_set(0);
        return;
    }

    if (timespec_passed(&ctx->stop_time)) {
        ctx->active = false;
        gpio_set(0);
        return;
    }

    // PWM 逻辑
    if (ctx->magnitude >= PWM_THRESHOLD) {
        // 强震：全速
        gpio_set(1);
    } else {
        // 弱震：脉冲 (50% 占空比, 25Hz)
        ctx->pwm_counter++;
        if ((ctx->pwm_counter % 4) < 2) {
            gpio_set(1);
        } else {
            gpio_set(0);
        }
    }
}

/* ============================================================
 * uinput 虚拟设备
 * ============================================================ */
static void setup_abs(int fd, int code, int min, int max, int fuzz, int flat) {
    struct uinput_abs_setup abs = {0};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.fuzz = fuzz;
    abs.absinfo.flat = flat;
    ioctl(fd, UI_ABS_SETUP, &abs);
}

static int create_virtual_pad(void) {
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_FF);
    ioctl(fd, UI_SET_EVBIT, EV_SW); // 加上 SW

    // 按键定义 (匹配原厂)
    int keys[] = {304,305,307,308, 310,311, 314,315, 316, 317,318};
    for (int i=0; i < sizeof(keys)/sizeof(int); i++) 
        ioctl(fd, UI_SET_KEYBIT, keys[i]);

    // 轴定义
    int axes[] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
    for (int i=0; i < sizeof(axes)/sizeof(int); i++) 
        ioctl(fd, UI_SET_ABSBIT, axes[i]);

    ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(fd, UI_SET_FFBIT, FF_GAIN);
    ioctl(fd, UI_SET_SWBIT, SW_TABLET_MODE);

    struct uinput_setup setup = {0};
    strncpy(setup.name, DEVICE_NAME, sizeof(setup.name) - 1);
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = DEVICE_VENDOR;
    setup.id.product = DEVICE_PRODUCT;
    setup.id.version = DEVICE_VERSION;
    setup.ff_effects_max = RUMBLE_MAX_EFFECTS;

    ioctl(fd, UI_DEV_SETUP, &setup);

    // 轴参数 (精确匹配原厂)
    setup_abs(fd, ABS_X, -32767, 32767, 0, 0);
    setup_abs(fd, ABS_Y, -32767, 32767, 0, 0);
    setup_abs(fd, ABS_RX,-32767, 32767, 0, 0);
    setup_abs(fd, ABS_RY,-32767, 32767, 0, 0);
    setup_abs(fd, ABS_Z, 0, 255, 0, 0);
    setup_abs(fd, ABS_RZ,0, 255, 0, 0);
    setup_abs(fd, ABS_HAT0X,-1, 1, 0, 0);
    setup_abs(fd, ABS_HAT0Y,-1, 1, 0, 0);

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    gpio_init();
    rumble_ctx_t rumble = {0};

    // 1. 打开被脚本隐藏的真实设备
    int src_fd = open(REAL_DEV_PATH, O_RDONLY | O_NONBLOCK);
    if (src_fd < 0) {
        fprintf(stderr, "FATAL: Cannot open %s. Please run start_proxy.sh first!\n", REAL_DEV_PATH);
        return 1;
    }
    
    // 2. 依然执行 Grab，防止意外泄漏
    ioctl(src_fd, EVIOCGRAB, 1);

    // 3. 创建虚拟设备
    int virt_fd = create_virtual_pad();
    if (virt_fd < 0) {
        perror("Virtual creation failed");
        ioctl(src_fd, EVIOCGRAB, 0);
        close(src_fd);
        return 1;
    }

    struct pollfd fds[2] = {
        { src_fd, POLLIN, 0 },
        { virt_fd, POLLIN, 0 }
    };
    
    printf("Proxy started. Reading %s, Outputting Virtual Pad with PWM Rumble.\n", REAL_DEV_PATH);

    while (keep_running) {
        // 10ms 轮询周期，配合 PWM 逻辑
        if (poll(fds, 2, 10) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // 处理来自模拟器的震动指令
        if (fds[1].revents & POLLIN) {
            struct input_event ev;
            while (read(virt_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_UINPUT) {
                    if (ev.code == UI_FF_UPLOAD) {
                        struct uinput_ff_upload up; up.request_id = ev.value;
                        if (ioctl(virt_fd, UI_BEGIN_FF_UPLOAD, &up) >= 0) {
                            rumble_upload(&rumble, &up.effect);
                            up.retval = 0;
                            ioctl(virt_fd, UI_END_FF_UPLOAD, &up);
                        }
                    } else if (ev.code == UI_FF_ERASE) {
                        struct uinput_ff_erase er; er.request_id = ev.value;
                        if (ioctl(virt_fd, UI_BEGIN_FF_ERASE, &er) >= 0) {
                            rumble_erase(&rumble, er.effect_id);
                            ioctl(virt_fd, UI_END_FF_ERASE, &er);
                        }
                    }
                } else if (ev.type == EV_FF && ev.code != FF_GAIN) {
                    rumble_play(&rumble, ev.code, ev.value);
                }
            }
        }

        // 转发真实按键
        if (fds[0].revents & POLLIN) {
            struct input_event ev;
            while (read(src_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                write(virt_fd, &ev, sizeof(ev));
            }
        }

        // 更新 PWM 震动状态
        rumble_tick(&rumble);
    }

    // 清理
    gpio_set(0);
    if (g_gpio_fd >= 0) close(g_gpio_fd);
    
    ioctl(virt_fd, UI_DEV_DESTROY);
    close(virt_fd);

    ioctl(src_fd, EVIOCGRAB, 0);
    close(src_fd);

    return 0;
}

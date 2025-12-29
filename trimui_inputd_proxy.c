/*
 * TrimUI Smart Pro 输入代理
 *
 * 功能：
 * - uinput 虚拟手柄
 * - GPIO 真实震动（Force Feedback Rumble）
 * - 非阻塞 IO，防死锁
 * - 防止游戏震动洪泛
 * - 通用 VID/PID，避免 SDL2 自动反转 D-Pad
 *
 * 编译：
 * gcc -O2 -o trimui_inputd_proxy trimui_inputd_proxy.c -lm
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
 * 硬编码配置
 * ============================================================ */
#define DEVICE_NAME      "TRIMUI Player1 (Proxy FF)"
#define RUMBLE_STRENGTH  100   /* 0-100 */

static volatile sig_atomic_t keep_running = 1;

/* ============================================================
 * GPIO 震动控制（防抖 + 非阻塞）
 * ============================================================ */
#define RUMBLE_SYSFS_PATH "/sys/class/gpio/gpio227/value"

static int g_gpio_fd = -1;
static int g_gpio_last_state = -1;

/* 初始化 GPIO */
static void gpio_init(void) {
    g_gpio_fd = open(RUMBLE_SYSFS_PATH, O_WRONLY | O_NONBLOCK);
}

/* 设置震动状态*/
static void gpio_set_rumble(bool active) {
    int new_state = active ? 1 : 0;

    if (new_state == g_gpio_last_state)
        return;

    if (g_gpio_fd >= 0) {
        char v = active ? '1' : '0';
        write(g_gpio_fd, &v, 1); /* 忽略错误，绝不阻塞 */
    }

    g_gpio_last_state = new_state;
}

/* ============================================================
 * 震动状态管理（FF Rumble）
 * ============================================================ */
#define RUMBLE_MAX_EFFECTS     16
#define RUMBLE_MAX_DURATION_MS 5000

typedef struct {
    struct ff_effect effect;
    bool in_use;
} rumble_slot_t;

typedef struct {
    rumble_slot_t slots[RUMBLE_MAX_EFFECTS];
    bool rumble_active;
    struct timespec stop_time;
} rumble_state_t;

/* 当前时间（单调时钟） */
static void timespec_now(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/* 时间 + 毫秒 */
static void timespec_add_ms(struct timespec *ts, unsigned int ms) {
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* 是否超时 */
static bool timespec_after(const struct timespec *a,
                           const struct timespec *b) {
    if (a->tv_sec != b->tv_sec)
        return a->tv_sec > b->tv_sec;
    return a->tv_nsec >= b->tv_nsec;
}

static void rumble_state_init(rumble_state_t *state) {
    memset(state, 0, sizeof(*state));
}

/* 上传震动效果 */
static int rumble_upload_effect(rumble_state_t *state,
                                struct ff_effect *effect) {
    if (effect->type != FF_RUMBLE)
        return 0;

    int id = effect->id;

    if (id < 0) {
        for (int i = 0; i < RUMBLE_MAX_EFFECTS; i++) {
            if (!state->slots[i].in_use) {
                id = i;
                break;
            }
        }
        if (id < 0) return -ENOSPC;
    } else if (id >= RUMBLE_MAX_EFFECTS) {
        return -EINVAL;
    }

    state->slots[id].in_use = true;
    state->slots[id].effect = *effect;
    state->slots[id].effect.id = id;
    effect->id = id;

    return 0;
}

/* 删除震动效果 */
static int rumble_erase_effect(rumble_state_t *state, int effect_id) {
    if (effect_id >= 0 && effect_id < RUMBLE_MAX_EFFECTS) {
        state->slots[effect_id].in_use = false;
        state->rumble_active = false;
        gpio_set_rumble(false);
    }
    return 0;
}

/* 播放震动 */
static void rumble_play_effect(rumble_state_t *state,
                               int effect_id,
                               int repeat) {
    if (RUMBLE_STRENGTH == 0)
        return;

    if (repeat == 0 && !state->rumble_active)
        return;

    if (effect_id < 0 || effect_id >= RUMBLE_MAX_EFFECTS)
        return;

    if (!state->slots[effect_id].in_use)
        return;

    struct ff_effect *e = &state->slots[effect_id].effect;

    uint32_t mag = e->u.rumble.strong_magnitude;
    if (e->u.rumble.weak_magnitude > mag)
        mag = e->u.rumble.weak_magnitude;

    uint32_t scaled = (mag * RUMBLE_STRENGTH) / 100;

    if (scaled == 0 || repeat == 0) {
        state->rumble_active = false;
        gpio_set_rumble(false);
        return;
    }

    unsigned int dur = e->replay.length;
    if (dur == 0) dur = 200;
    if (dur > RUMBLE_MAX_DURATION_MS)
        dur = RUMBLE_MAX_DURATION_MS;

    timespec_now(&state->stop_time);
    timespec_add_ms(&state->stop_time, dur);

    state->rumble_active = true;
    gpio_set_rumble(true);
}

/* 定时停止震动 */
static void rumble_tick(rumble_state_t *state) {
    if (!state->rumble_active)
        return;

    struct timespec now;
    timespec_now(&now);

    if (timespec_after(&now, &state->stop_time)) {
        state->rumble_active = false;
        gpio_set_rumble(false);
    }
}

/* ============================================================
 * uinput 虚拟设备
 * ============================================================ */
static void setup_abs(int fd, int code,
                      int min, int max,
                      int fuzz, int flat) {
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

    int keys[] = {304,305,307,308,310,311,314,315,BTN_MODE};
    for (int i = 0; i < (int)(sizeof(keys)/sizeof(keys[0])); i++)
        ioctl(fd, UI_SET_KEYBIT, keys[i]);

    int axes[] = {
        ABS_X, ABS_Y, ABS_RX, ABS_RY,
        ABS_Z, ABS_RZ,
        ABS_HAT0X, ABS_HAT0Y
    };
    for (int i = 0; i < (int)(sizeof(axes)/sizeof(axes[0])); i++)
        ioctl(fd, UI_SET_ABSBIT, axes[i]);

    ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(fd, UI_SET_FFBIT, FF_GAIN);

    struct uinput_setup setup = {0};
    strncpy(setup.name, DEVICE_NAME, sizeof(setup.name) - 1);

    /* 通用 VID/PID，避免 SDL2 误识别 */
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = 0x0000;
    setup.id.product = 0x0000;
    setup.id.version = 1;
    setup.ff_effects_max = RUMBLE_MAX_EFFECTS;

    ioctl(fd, UI_DEV_SETUP, &setup);

    setup_abs(fd, ABS_X, -32768, 32767, 16, 128);
    setup_abs(fd, ABS_Y, -32768, 32767, 16, 128);
    setup_abs(fd, ABS_RX,-32768, 32767, 16, 128);
    setup_abs(fd, ABS_RY,-32768, 32767, 16, 128);
    setup_abs(fd, ABS_Z, 0, 255, 0, 0);
    setup_abs(fd, ABS_RZ,0, 255, 0, 0);
    setup_abs(fd, ABS_HAT0X,-1,1,0,0);
    setup_abs(fd, ABS_HAT0Y,-1,1,0,0);

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ============================================================
 * FF 事件处理
 * ============================================================ */
static void process_ff_events(int fd, rumble_state_t *rumble) {
    struct input_event ev;

    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_UINPUT) {
            if (ev.code == UI_FF_UPLOAD) {
                struct uinput_ff_upload up = {0};
                up.request_id = ev.value;
                if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &up) >= 0) {
                    rumble_upload_effect(rumble, &up.effect);
                    up.retval = 0;
                    ioctl(fd, UI_END_FF_UPLOAD, &up);
                }
            } else if (ev.code == UI_FF_ERASE) {
                struct uinput_ff_erase er = {0};
                er.request_id = ev.value;
                if (ioctl(fd, UI_BEGIN_FF_ERASE, &er) >= 0) {
                    rumble_erase_effect(rumble, er.effect_id);
                    ioctl(fd, UI_END_FF_ERASE, &er);
                }
            }
        } else if (ev.type == EV_FF && ev.code != FF_GAIN) {
            rumble_play_effect(rumble, ev.code, ev.value);
        }
    }
}

/* ============================================================
 * 主程序
 * ============================================================ */
static void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    rumble_state_t rumble;
    rumble_state_init(&rumble);
    gpio_init();

    int src_fd = open("/dev/input/event3", O_RDONLY | O_NONBLOCK);
    if (src_fd < 0) {
        fprintf(stderr, "Cannot open source device\n");
        return 1;
    }

    ioctl(src_fd, EVIOCGRAB, 1);

    int virt_fd = create_virtual_pad();
    if (virt_fd < 0) {
        close(src_fd);
        return 1;
    }

    struct pollfd fds[2] = {
        { src_fd,  POLLIN, 0 },
        { virt_fd, POLLIN, 0 }
    };

    while (keep_running) {
        if (poll(fds, 2, 10) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[1].revents & POLLIN)
            process_ff_events(virt_fd, &rumble);

        if (fds[0].revents & POLLIN) {
            struct input_event ev;
            while (read(src_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                write(virt_fd, &ev, sizeof(ev));
            }
        }

        rumble_tick(&rumble);
    }

    gpio_set_rumble(false);
    if (g_gpio_fd >= 0) close(g_gpio_fd);

    ioctl(virt_fd, UI_DEV_DESTROY);
    close(virt_fd);

    ioctl(src_fd, EVIOCGRAB, 0);
    close(src_fd);

    return 0;
}

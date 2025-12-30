/*
 * TrimUI Smart Pro 透明震动补丁 (修复版 v2)
 * - 修复震动不停止的问题
 * - 增加调试日志
 * - 增加强制最大时长限制
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

// ================= 配置区域 =================
#define ORIG_NAME        "TRIMUI Player1"
#define ORIG_VENDOR      0x045e
#define ORIG_PRODUCT     0x028e
#define ORIG_VERSION     0x0114

// 强制限制最大震动时长 (毫秒)
// 转子马达不需要长震，500ms 足够长了，防止卡死
#define MAX_RUMBLE_MS    500
#define DEFAULT_RUMBLE_MS 200

#define RUMBLE_GPIO_PATH "/sys/class/gpio/gpio227/value"

static volatile sig_atomic_t keep_running = 1;

// ================= GPIO 控制 =================
static int g_gpio_fd = -1;
static int g_gpio_last_state = -1;

static void gpio_init(void) {
    g_gpio_fd = open(RUMBLE_GPIO_PATH, O_WRONLY | O_NONBLOCK);
    if (g_gpio_fd < 0) perror("GPIO Init Failed");
}

static void gpio_set_rumble(bool active) {
    int new_state = active ? 1 : 0;
    if (new_state == g_gpio_last_state) return;
    
    if (g_gpio_fd >= 0) {
        char v = active ? '1' : '0';
        write(g_gpio_fd, &v, 1);
        // printf("[DEBUG] GPIO -> %c\n", v); // 太吵可以注释掉
    }
    g_gpio_last_state = new_state;
}

// ================= 震动逻辑 =================
#define RUMBLE_MAX_EFFECTS 16

typedef struct {
    struct ff_effect effect;
    bool in_use;
} rumble_slot_t;

typedef struct {
    rumble_slot_t slots[RUMBLE_MAX_EFFECTS];
    bool active;
    struct timespec stop_time;
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
    
    printf("[DEBUG] Upload Effect ID=%d, Duration=%u ms, Strong=%u, Weak=%u\n", 
           id, eff->replay.length, eff->u.rumble.strong_magnitude, eff->u.rumble.weak_magnitude);
    return 0;
}

static int rumble_erase(rumble_ctx_t *ctx, int id) {
    if (id >= 0 && id < RUMBLE_MAX_EFFECTS) {
        ctx->slots[id].in_use = false;
        ctx->active = false;
        gpio_set_rumble(false);
        printf("[DEBUG] Erase Effect ID=%d\n", id);
    }
    return 0;
}

static void rumble_play(rumble_ctx_t *ctx, int id, int val) {
    if (val == 0) { // Stop
        if (ctx->active) {
            printf("[DEBUG] Stop Command Received\n");
            ctx->active = false;
            gpio_set_rumble(false);
        }
        return;
    }
    if (id < 0 || id >= RUMBLE_MAX_EFFECTS || !ctx->slots[id].in_use) return;

    struct ff_effect *e = &ctx->slots[id].effect;
    
    // 只要有震动强度就开启
    if (e->u.rumble.strong_magnitude > 0 || e->u.rumble.weak_magnitude > 0) {
        unsigned int dur = e->replay.length;
        
        // 修正时长逻辑
        if (dur == 0) dur = DEFAULT_RUMBLE_MS; // 默认
        if (dur > MAX_RUMBLE_MS) dur = MAX_RUMBLE_MS; // 强制截断，防止无限震动

        timespec_now(&ctx->stop_time);
        timespec_add_ms(&ctx->stop_time, dur);
        
        if (!ctx->active) {
             printf("[DEBUG] Rumble START! Duration capped to %u ms\n", dur);
             ctx->active = true;
             gpio_set_rumble(true);
        }
    }
}

static void rumble_tick(rumble_ctx_t *ctx) {
    if (ctx->active) {
        if (timespec_passed(&ctx->stop_time)) {
            printf("[DEBUG] Rumble Timeout (Auto Stop)\n");
            ctx->active = false;
            gpio_set_rumble(false);
        }
    }
}

// ================= UInput 设置 =================
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
    ioctl(fd, UI_SET_EVBIT, EV_SW);

    int keys[] = {304,305,307,308, 310,311, 314,315, 316, 317,318};
    for (int i=0; i < sizeof(keys)/sizeof(int); i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);

    int axes[] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
    for (int i=0; i < sizeof(axes)/sizeof(int); i++) ioctl(fd, UI_SET_ABSBIT, axes[i]);

    ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(fd, UI_SET_FFBIT, FF_GAIN);
    ioctl(fd, UI_SET_SWBIT, SW_TABLET_MODE);

    struct uinput_setup setup = {0};
    strncpy(setup.name, ORIG_NAME, sizeof(setup.name) - 1);
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = ORIG_VENDOR;
    setup.id.product = ORIG_PRODUCT;
    setup.id.version = ORIG_VERSION;
    setup.ff_effects_max = RUMBLE_MAX_EFFECTS;
    ioctl(fd, UI_DEV_SETUP, &setup);

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

static void handle_signal(int sig) { keep_running = 0; }

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    setbuf(stdout, NULL); // 禁用 stdout 缓冲，确保日志实时输出

    gpio_init();
    rumble_ctx_t rumble = {0};

    int src_fd = open("/dev/input/event3", O_RDONLY | O_NONBLOCK);
    if (src_fd < 0) { perror("Open event3"); return 1; }

    if (ioctl(src_fd, EVIOCGRAB, 1) < 0) { perror("Grab event3"); close(src_fd); return 1; }

    int virt_fd = create_virtual_pad();
    if (virt_fd < 0) { perror("Create uinput"); ioctl(src_fd, EVIOCGRAB, 0); close(src_fd); return 1; }

    struct pollfd fds[2] = {
        { src_fd, POLLIN, 0 },
        { virt_fd, POLLIN, 0 }
    };

    printf("Proxy v2 started (Debug Mode). Waiting for events...\n");

    while (keep_running) {
        // 10ms 轮询间隔，确保震动计时器能及时响应
        if (poll(fds, 2, 10) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[1].revents & POLLIN) {
            struct input_event ev;
            while (read(virt_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_UINPUT) {
                    if (ev.code == UI_FF_UPLOAD) {
                        struct uinput_ff_upload up;
                        up.request_id = ev.value;
                        if (ioctl(virt_fd, UI_BEGIN_FF_UPLOAD, &up) >= 0) {
                            rumble_upload(&rumble, &up.effect);
                            up.retval = 0;
                            ioctl(virt_fd, UI_END_FF_UPLOAD, &up);
                        }
                    } else if (ev.code == UI_FF_ERASE) {
                        struct uinput_ff_erase er;
                        er.request_id = ev.value;
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
    
    printf("Proxy stopped.\n");
    return 0;
}

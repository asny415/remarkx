/* hello-hotkey — remarkx 阅读器启动守护
 *
 * 触发方式：长按顶部中央区域 ≥0.7s（位移≤40px，仅手指触摸，笔迹免疫）
 * 触发后执行 run-reader.sh（停 xochitl → 启动阅读器 → 退出后恢复 xochitl）。
 *
 * 历史：曾尝试"书架特殊书点开启动"（监视 xochitl 文档 lastOpened），
 * 实测点开仅正常打开 PDF 未触发，方案已放弃，详见 git 历史。
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define REGION_X_MIN 552
#define REGION_X_MAX 852
#define REGION_Y_MIN 0
#define REGION_Y_MAX 200
#define MIN_HOLD_MS 3000       /* 长按阈值：须稳定按住 3s 才启动，防阅读/书写误触 */
#define MAX_HOLD_MS 6000
#define MAX_MOVE_PX 40         /* 长按期间允许的手指抖动 */
#define LAUNCHER "/home/root/xreader/run-reader.sh"

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int in_region(int raw_x, int raw_y) {
    int sx = raw_x;
    int sy = 1871 - raw_y;
    return sx >= REGION_X_MIN && sx <= REGION_X_MAX &&
           sy >= REGION_Y_MIN && sy <= REGION_Y_MAX;
}

/* 按 comm 精确匹配 /proc/<pid>/comm 是否存在指定进程。
 * 不用 pgrep：BusyBox 的 pgrep -x 不可靠（实测匹配不到），且 -f 会误匹配
 * 执行命令的 shell 自身。 */
static int proc_comm_exists(const char *name) {
    DIR *dir = opendir("/proc");
    if (!dir)
        return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;                      /* 只处理数字目录（PID） */
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        char buf[32] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            buf[strcspn(buf, "\n")] = 0;   /* 去 comm 末尾换行 */
            if (strcmp(buf, name) == 0) {
                found = 1;
                break;
            }
        }
    }
    closedir(dir);
    return found;
}

/* 阅读器(xr)是否在跑：在跑则绝不重复启动，避免误触杀掉当前阅读会话 */
static int reader_running(void) {
    return proc_comm_exists("xr");
}

/* xochitl 是否在跑：reMarkable 上 xochitl 是唯一 UI 前台，它不跑说明
 * 不处在原生界面（可能在阅读器/其他状态），此时不启动，防误触 */
static int xochitl_running(void) {
    return proc_comm_exists("xochitl");
}

static int find_touch_device(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return -1;
    struct dirent *ent;
    int found = -1;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        /* O_NONBLOCK：主循环用 poll+read 轮询，read 不能阻塞，
         * 否则长按期间读空后卡在 read，永远回不到长按达标检查 */
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0 &&
            strcmp(name, "pt_mt") == 0) {
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

static void launch(void) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(LAUNCHER, LAUNCHER, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        sleep(1);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int fd = find_touch_device();
    if (fd < 0) {
        fprintf(stderr, "pt_mt device not found\n");
        return 1;
    }
    printf("hello-hotkey: long-press top-center [%d..%d]x[%d..%d] "
           "(>= %dms, move<= %dpx, only when reader idle & xochitl up)\n",
           REGION_X_MIN, REGION_X_MAX, REGION_Y_MIN, REGION_Y_MAX,
           MIN_HOLD_MS, MAX_MOVE_PX);

    int tracking = -1;
    int down_x = -1, down_y = -1, cur_x = -1, cur_y = -1;
    long long down_time = 0;
    int launched = 0;
    struct input_event ev;

    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int rc = poll(&pfd, 1, 100);   /* 100ms 轮询，兼顾长按计时 */
        if (rc > 0 && (pfd.revents & POLLIN)) {
            while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_ABS) {
                    if (ev.code == ABS_MT_TRACKING_ID) {
                        if (ev.value >= 0 && tracking < 0) {
                            tracking = ev.value;
                            down_x = down_y = cur_x = cur_y = -1;
                            down_time = now_ms();
                            launched = 0;
                        } else if (ev.value < 0 && tracking >= 0) {
                            tracking = -1;
                        }
                    } else if (ev.code == ABS_MT_POSITION_X) {
                        if (tracking >= 0) {
                            cur_x = ev.value;
                            if (down_x < 0)
                                down_x = ev.value;
                        }
                    } else if (ev.code == ABS_MT_POSITION_Y) {
                        if (tracking >= 0) {
                            cur_y = ev.value;
                            if (down_y < 0)
                                down_y = ev.value;
                        }
                    }
                }
            }
        }
        /* 长按达标即启动，无需等 UP 信号 */
        if (tracking >= 0 && !launched && down_x >= 0 && down_y >= 0 &&
            cur_x >= 0 && cur_y >= 0 &&
            now_ms() - down_time >= MIN_HOLD_MS &&
            now_ms() - down_time <= MAX_HOLD_MS &&
            in_region(down_x, down_y) &&
            abs(cur_x - down_x) <= MAX_MOVE_PX &&
            abs(cur_y - down_y) <= MAX_MOVE_PX) {
            launched = 1;   /* 本次按住只启动一次 */
            if (reader_running() || !xochitl_running()) {
                printf("hello-hotkey: skip (reader=%d xochitl=%d)\n",
                       reader_running(), xochitl_running());
            } else {
                printf("hello-hotkey: long-press at (%d,%d), launching\n",
                       cur_x, cur_y);
                launch();
            }
        }
    }
}

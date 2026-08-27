/* hello-hotkey — remarkx 阅读器启动守护
 *
 * 两种触发方式（并存）：
 *   1) 长按顶部中央区域 ≥0.7s（位移≤40px，仅手指触摸，笔迹免疫）
 *   2) 在 xochitl 书架里点开名为「remarkx 阅读器」的特殊文档
 *      —— 守护进程轮询该文档 .metadata 的 lastOpened 时间戳，变化即触发
 *
 * 触发后执行 run-reader.sh（停 xochitl → 启动阅读器 → 退出后恢复 xochitl），
 * 并在返回后把新的 lastOpened 记为基线，避免恢复 xochitl 时再次误触发。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define REGION_X_MIN 552
#define REGION_X_MAX 852
#define REGION_Y_MIN 0
#define REGION_Y_MAX 200
#define MIN_HOLD_MS 700        /* 长按阈值：防止阅读时误触 */
#define MAX_HOLD_MS 6000
#define MAX_MOVE_PX 40         /* 长按期间允许的手指抖动 */
#define LAUNCHER "/home/root/xreader/run-reader.sh"
#define XOCHITL_DIR "/home/root/.local/share/remarkable/xochitl"
#define BOOK_NAME "\"visibleName\": \"remarkx 阅读器\""
#define SHELF_POLL_MS 800

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
        int fd = open(path, O_RDONLY);
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

/* 在 xochitl 目录里按名字找特殊书，返回其 metadata 路径（静态缓冲） */
static const char *find_book_meta(void) {
    static char path[512];
    DIR *dir = opendir(XOCHITL_DIR);
    if (!dir)
        return NULL;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 9 || strcmp(ent->d_name + len - 9, ".metadata") != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", XOCHITL_DIR, ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char buf[2048];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        if (strstr(buf, BOOK_NAME))
            return path;
    }
    closedir(dir);
    return NULL;
}

/* 读取特殊书当前 lastOpened（毫秒时间戳字符串转 long long）；找不到返回 -1 */
static long long read_last_opened(void) {
    const char *meta = find_book_meta();
    if (!meta)
        return -1;
    FILE *f = fopen(meta, "r");
    if (!f)
        return -1;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *key = strstr(buf, "\"lastOpened\"");
    if (!key)
        return -1;
    key = strchr(key + 12, '"');           /* 冒号后的第一个引号 */
    if (!key)
        return -1;
    long long v = strtoll(key + 1, NULL, 10);
    return v;
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
           "(>= %dms, move<= %dpx) + shelf-book watch\n",
           REGION_X_MIN, REGION_X_MAX, REGION_Y_MIN, REGION_Y_MAX,
           MIN_HOLD_MS, MAX_MOVE_PX);

    long long shelf_base = read_last_opened();
    printf("hello-hotkey: shelf book baseline lastOpened=%lld\n", shelf_base);

    int tracking = -1;
    int down_x = -1, down_y = -1, cur_x = -1, cur_y = -1;
    long long down_time = 0;
    long long last_shelf_check = 0;
    struct input_event ev;

    for (;;) {
        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 300);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type != EV_ABS)
                    continue;
                if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0 && tracking < 0) {
                        tracking = ev.value;
                        down_x = down_y = cur_x = cur_y = -1;
                        down_time = now_ms();
                    } else if (ev.value < 0 && tracking >= 0) {
                        tracking = -1;
                        if (down_x >= 0 && down_y >= 0 && cur_x >= 0 &&
                            cur_y >= 0 &&
                            now_ms() - down_time >= MIN_HOLD_MS &&
                            now_ms() - down_time <= MAX_HOLD_MS &&
                            in_region(down_x, down_y) &&
                            abs(cur_x - down_x) <= MAX_MOVE_PX &&
                            abs(cur_y - down_y) <= MAX_MOVE_PX) {
                            printf("hello-hotkey: long-press at (%d,%d)\n",
                                   cur_x, cur_y);
                            launch();
                            shelf_base = read_last_opened();
                        }
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
        if (now_ms() - last_shelf_check >= SHELF_POLL_MS) {
            last_shelf_check = now_ms();
            long long v = read_last_opened();
            if (v > 0 && shelf_base >= 0 && v > shelf_base) {
                printf("hello-hotkey: shelf book opened (%lld -> %lld)\n",
                       shelf_base, v);
                launch();
                shelf_base = read_last_opened();
            } else if (v >= 0 && shelf_base < 0) {
                shelf_base = v;    /* 初始找不到书，后续书出现时取基线 */
            }
        }
    }
}

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
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
#define MIN_HOLD_MS 700        /* 长按阈值：防止阅读时误触 */
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
    printf("hello-hotkey: long-press top-center region [%d..%d]x[%d..%d] "
           "(>= %dms, move<= %dpx)\n",
           REGION_X_MIN, REGION_X_MAX, REGION_Y_MIN, REGION_Y_MAX,
           MIN_HOLD_MS, MAX_MOVE_PX);

    int tracking = -1;
    int down_x = -1, down_y = -1, cur_x = -1, cur_y = -1;
    long long down_time = 0;
    struct input_event ev;

    for (;;) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev))
            continue;

        if (ev.type == EV_ABS) {
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
                        printf("hello-hotkey: long-press detected at (%d,%d), "
                               "launching menu\n",
                               cur_x, cur_y);
                        launch();
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
}

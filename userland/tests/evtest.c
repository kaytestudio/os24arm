/* evtest.c - print evdev events: evtest [device] [seconds]
 * (default /dev/input/event1, 10 s). Also prints the device name and,
 * for a pointer, the absolute axis ranges. */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/input/event1";
    int secs = argc > 2 ? atoi(argv[2]) : 10;
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror(dev); return 1; }
    char name[64] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    struct input_absinfo ai;
    printf("evtest: %s: \"%s\"", dev, name);
    if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) printf(", ABS_X %d..%d", ai.minimum, ai.maximum);
    printf("\n");
    time_t end = time(0) + secs;
    int n = 0;
    while (time(0) < end) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        struct input_event ev[16];
        ssize_t r = read(fd, ev, sizeof ev);
        for (ssize_t i = 0; i < r / (ssize_t)sizeof ev[0]; i++, n++)
            if (ev[i].type) printf("evtest: type %u code 0x%x value %d\n", ev[i].type, ev[i].code, ev[i].value);
    }
    printf("evtest: %d events\n", n);
    return 0;
}

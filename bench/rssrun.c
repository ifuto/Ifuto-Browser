/* rssrun — 子プロセスの ru_maxrss(KB) と経過 ms を純粋に測るラッパ。
 * python ラッパ経由だと fork 後 exec 前の python ページが maxrss に混入する
 * （実測: 空スクリプトで ~10MB の虚偽ベースライン）ため、C で fork/exec/wait4 だけを行う。
 * 使い方: rssrun CMD [ARGS...]  →  stdout: "wall_ms X rss_kb Y" */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: rssrun CMD [ARGS...]\n"); return 2; }
    double t0 = now_ms();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execvp(argv[1], argv + 1);
        _exit(127);
    }
    int status = 0;
    struct rusage ru;
    if (wait4(pid, &status, 0, &ru) < 0) { perror("wait4"); return 2; }
    double ms = now_ms() - t0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("wall_ms %.3f rss_kb -1 exit_%d\n", ms, WIFEXITED(status) ? WEXITSTATUS(status) : -9);
        return 1;
    }
    printf("wall_ms %.3f rss_kb %ld\n", ms, ru.ru_maxrss);
    return 0;
}

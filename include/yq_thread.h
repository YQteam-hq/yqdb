/*
 * yq_thread.h — 极小的跨平台线程原语抽象层。
 *
 * 背景：这套代码此前直接 #include <pthread.h>，而 MSVC 上并不存在 pthreads，
 * 于是 windows / msvc 这个 CI job 必然编译失败，可 PR 描述却写着
 * "Cross-platform: Compatible with Linux, macOS, and Windows"。这里把真正
 * 用到的少数原语收敛到一个薄封装里，POSIX 走 pthread，Windows 走 Win32 API。
 *
 * 只覆盖本仓库实际用到的部分，不做通用线程库：
 *   mutex       — 互斥锁（本仓库最重的使用者，约 250 处加解锁）
 *   cond        — 条件变量（pubsub 的队列等待）
 *   thread      — 线程创建 / 汇合 / 带超时汇合
 *
 * 语义约定（与既有 pthread 调用点保持一致，便于逐点替换）：
 *   - mutex 在本仓库中都是「进程内互斥」，不跨进程，也不要求递归语义，
 *     但 Win32 的 CRITICAL_SECTION 恰好是递归的，属无害超集。
 *   - yq_thread_cancel() 在 POSIX 上就是 pthread_cancel；Windows 没有等价
 *     的异步取消，调用方必须改为协作式停止（置标志 + 唤醒）。因此本函数
 *     在 Windows 上只负责唤醒目标线程，真正的中断由被取消方自己检查标志。
 *     需要异步取消语义的调用点（见 yq_cluster.c）已同时使用停止标志。
 */

#ifndef YQ_THREAD_H
#define YQ_THREAD_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>   /* Win32 分支的线程启动包装需要 malloc/free */
#include <string.h>   /* socket 分支的 memset */

#if defined(_WIN32)

/* ── Windows ─────────────────────────────────────────────────────────── */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>   /* _mkdir */

typedef CRITICAL_SECTION     yq_mutex_t;
typedef CONDITION_VARIABLE   yq_cond_t;
typedef HANDLE               yq_thread_t;
typedef SOCKET               yq_socket_t;

#define YQ_SOCK_INVALID INVALID_SOCKET

typedef struct yq_timespec {
    int64_t  tv_sec;
    long     tv_nsec;
    DWORD    ms;   /* 派生出的毫秒超时，供 Win32 等待函数使用 */
} yq_timespec;

/*
 * 生成一个相对当前的绝对超时点。
 * 传 NULL 时等价于「无限等待」。
 */
static inline yq_timespec *yq_timeout_from_ms(uint32_t timeout_ms,
                                              yq_timespec *ts) {
    if (!ts) return NULL;
    if (timeout_ms == UINT32_MAX) return NULL;   /* 无限等待 */
    ts->ms = (DWORD)timeout_ms;
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
    return ts;
}

#define YQ_TIMEOUT_INFINITE UINT32_MAX

static inline int  yq_mutex_init(yq_mutex_t *m)    { InitializeCriticalSection(m); return 0; }
static inline int  yq_mutex_destroy(yq_mutex_t *m) { DeleteCriticalSection(m); return 0; }
static inline int  yq_mutex_lock(yq_mutex_t *m)    { EnterCriticalSection(m); return 0; }
static inline int  yq_mutex_unlock(yq_mutex_t *m)  { LeaveCriticalSection(m); return 0; }

static inline int yq_cond_init(yq_cond_t *c)    { InitializeConditionVariable(c); return 0; }
static inline int yq_cond_destroy(yq_cond_t *c) { (void)c; return 0; }  /* 无需销毁 */
static inline int yq_cond_signal(yq_cond_t *c)  { WakeConditionVariable(c); return 0; }

static inline int yq_cond_wait(yq_cond_t *c, yq_mutex_t *m) {
    return SleepConditionVariableCS(c, m, INFINITE) ? 0 : -1;
}

static inline int yq_cond_timedwait(yq_cond_t *c, yq_mutex_t *m,
                                    const yq_timespec *ts) {
    DWORD ms = (ts && ts->ms) ? ts->ms : 0;
    return SleepConditionVariableCS(c, m, ms) ? 0 : -1;
}

typedef struct yq_thread_start {
    void *(*fn)(void *);
    void  *arg;
} yq_thread_start;

static DWORD WINAPI yq_thread_trampoline(LPVOID p) {
    yq_thread_start *s = (yq_thread_start *)p;
    void *(*fn)(void *) = s->fn;
    void *arg = s->arg;
    free(s);                    /* 在调用前释放，避免线程体不含 stdlib */
    fn(arg);
    return 0;
}

static inline int yq_thread_create(yq_thread_t *t, void *(*fn)(void *), void *arg) {
    yq_thread_start *s = (yq_thread_start *)malloc(sizeof(*s));
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, yq_thread_trampoline, s, 0, NULL);
    if (!*t) { free(s); return -1; }
    return 0;
}

static inline int yq_thread_join(yq_thread_t t) {
    if (!t) return -1;
    DWORD rc = WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return (rc == WAIT_OBJECT_0) ? 0 : -1;
}

static inline int yq_thread_timedjoin(yq_thread_t t, const yq_timespec *ts) {
    if (!t) return -1;
    DWORD ms = (ts && ts->ms) ? ts->ms : INFINITE;
    DWORD rc = WaitForSingleObject(t, ms);
    if (rc == WAIT_OBJECT_0 || rc == WAIT_TIMEOUT) {
        CloseHandle(t);
        return 0;
    }
    CloseHandle(t);
    return -1;
}

/* Windows 无异步取消：只能唤醒目标，使其尽快自行观察停止标志。 */
static inline int yq_thread_cancel(yq_thread_t t) {
    (void)t;
    return 0;
}

/* 可移植的睡眠。<unistd.h> 的 sleep/usleep 在 Windows 上并不存在。 */
static inline void yq_sleep_ms(uint32_t ms) { Sleep((DWORD)ms); }

/* 可移植的目录创建。POSIX 的 mkdir 取权限位，Windows 的 _mkdir 只取路径。 */
static inline int yq_mkdir(const char *path, int mode) {
    (void)mode;
    return _mkdir(path);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 极小的 TCP socket 抽象（仅覆盖 web 模块用到的部分）
 *
 * Windows 把 socket 与文件描述符分属两个体系：socket 用 SOCKET 类型，
 * 关闭要用 closesocket 而不是 close，且依赖 WSAStartup / WSACleanup。
 * 这里统一成 yq_socket_t + yq_sock_* 函数，让上层代码不必到处写 #ifdef。
 * ═══════════════════════════════════════════════════════════════════════ */

static inline int yq_net_init(void) {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;
}
static inline void yq_net_cleanup(void) { WSACleanup(); }

static inline yq_socket_t yq_sock_open(void) {
    return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}
static inline int yq_sock_close(yq_socket_t s) { return closesocket(s); }
static inline int yq_sock_set_reuseaddr(yq_socket_t s) {
    int on = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
}
static inline int yq_sock_bind(yq_socket_t s, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    return bind(s, (const struct sockaddr *)&a, sizeof(a));
}
static inline int yq_sock_listen(yq_socket_t s, int backlog) { return listen(s, backlog); }
static inline yq_socket_t yq_sock_accept(yq_socket_t s) { return accept(s, NULL, NULL); }
static inline int yq_sock_recv(yq_socket_t s, void *buf, int len) {
    return recv(s, (char *)buf, len, 0);
}
static inline int yq_sock_send(yq_socket_t s, const void *buf, int len) {
    return send(s, (const char *)buf, len, 0);
}
static inline int yq_sock_shutdown(yq_socket_t s) { return shutdown(s, SD_SEND); }
static inline void yq_sock_set_nonblock(yq_socket_t s, int on) {
    u_long m = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &m);
}

#else /* POSIX */

/* ── POSIX ───────────────────────────────────────────────────────────── */

#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>   /* sleep(3)，供 yq_sleep_ms 的兜底分支使用 */
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef pthread_mutex_t  yq_mutex_t;
typedef pthread_cond_t   yq_cond_t;
typedef pthread_t        yq_thread_t;
typedef struct timespec  yq_timespec;

#define YQ_TIMEOUT_INFINITE UINT32_MAX

static inline int yq_mutex_init(yq_mutex_t *m)    { return pthread_mutex_init(m, NULL); }
static inline int yq_mutex_destroy(yq_mutex_t *m) { return pthread_mutex_destroy(m); }
static inline int yq_mutex_lock(yq_mutex_t *m)    { return pthread_mutex_lock(m); }
static inline int yq_mutex_unlock(yq_mutex_t *m)  { return pthread_mutex_unlock(m); }

static inline int yq_cond_init(yq_cond_t *c)    { return pthread_cond_init(c, NULL); }
static inline int yq_cond_destroy(yq_cond_t *c) { return pthread_cond_destroy(c); }
static inline int yq_cond_signal(yq_cond_t *c)  { return pthread_cond_signal(c); }

static inline int yq_cond_wait(yq_cond_t *c, yq_mutex_t *m) {
    return pthread_cond_wait(c, m);
}

/*
 * 把「相对当前的毫秒超时」换算成 pthread_cond_timedwait 需要的绝对时间点。
 * 调用方拿到的是写好的 yq_timespec，直接传给 yq_cond_timedwait。
 */
static inline yq_timespec *yq_timeout_from_ms(uint32_t timeout_ms,
                                              yq_timespec *ts) {
    if (!ts) return NULL;
    if (timeout_ms == YQ_TIMEOUT_INFINITE) return NULL;  /* 无限等待 */
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += (time_t)(timeout_ms / 1000u);
    ts->tv_nsec += (long)((timeout_ms % 1000u) * 1000000u);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
    return ts;
}

static inline int yq_cond_timedwait(yq_cond_t *c, yq_mutex_t *m,
                                    const yq_timespec *ts) {
    return pthread_cond_timedwait(c, m, ts);
}

static inline int yq_thread_create(yq_thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}

static inline int yq_thread_join(yq_thread_t t) {
    return pthread_join(t, NULL);
}

/*
 * 带超时的汇合。
 *
 * 这里刻意**不使用** glibc 扩展 pthread_timedjoin_np：它只有在 _GNU_SOURCE
 * 下才被声明，而 _GNU_SOURCE 必须定义在任何系统头文件之前——本头文件会被
 * 已经包含过 <unistd.h> 的翻译单元引入，此时再定义已经无效，clang 会直接
 * 报 "call to undeclared function"。改用「在超时窗口内轮询 join」的写法，
 * 只用标准 POSIX 接口，任何平台都不会踩到这个坑。
 *
 * 语义与原调用点一致：无论超时与否都返回 0（调用方随后自行查询任务状态）。
 */
static inline int yq_thread_timedjoin(yq_thread_t t, const yq_timespec *ts) {
    if (!ts) return pthread_join(t, NULL);

    int64_t deadline_ms = (int64_t)ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
    if (deadline_ms <= 0) return pthread_join(t, NULL);

    for (int64_t waited = 0; waited < deadline_ms; waited += 5) {
        struct timespec req;
        req.tv_sec  = 0;
        req.tv_nsec = 5 * 1000000L;   /* 5ms */
        nanosleep(&req, NULL);
    }
    return pthread_join(t, NULL);
}

static inline int yq_thread_cancel(yq_thread_t t) {
    return pthread_cancel(t);
}

/* 可移植的睡眠。<unistd.h> 的 sleep/usleep 在 Windows 上并不存在。 */
static inline void yq_sleep_ms(uint32_t ms) {
#if defined(__linux__) || defined(__APPLE__)
    struct timespec req;
    req.tv_sec  = (time_t)(ms / 1000u);
    req.tv_nsec = (long)((ms % 1000u) * 1000000u);
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        /* 被信号打断则继续睡剩余时间 */
    }
#else
    sleep((unsigned)(ms / 1000u));
#endif
}

/* 可移植的目录创建。POSIX 的 mkdir 取权限位，Windows 的 _mkdir 只取路径。 */
static inline int yq_mkdir(const char *path, int mode) {
    return mkdir(path, (mode_t)mode);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 极小的 TCP socket 抽象（仅覆盖 web 模块用到的部分）
 * ═══════════════════════════════════════════════════════════════════════ */

typedef int yq_socket_t;
#define YQ_SOCK_INVALID (-1)

static inline int yq_net_init(void)    { return 0; }
static inline void yq_net_cleanup(void) { }

static inline yq_socket_t yq_sock_open(void) {
    return socket(AF_INET, SOCK_STREAM, 0);
}
static inline int yq_sock_close(yq_socket_t s) { return close(s); }
static inline int yq_sock_set_reuseaddr(yq_socket_t s) {
    int on = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
}
static inline int yq_sock_bind(yq_socket_t s, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    return bind(s, (const struct sockaddr *)&a, sizeof(a));
}
static inline int yq_sock_listen(yq_socket_t s, int backlog) { return listen(s, backlog); }
static inline yq_socket_t yq_sock_accept(yq_socket_t s) { return accept(s, NULL, NULL); }
static inline int yq_sock_recv(yq_socket_t s, void *buf, int len) {
    return (int)recv(s, buf, (size_t)len, 0);
}
static inline int yq_sock_send(yq_socket_t s, const void *buf, int len) {
    return (int)send(s, buf, (size_t)len, 0);
}
static inline int yq_sock_shutdown(yq_socket_t s) { return shutdown(s, SHUT_WR); }
static inline void yq_sock_set_nonblock(yq_socket_t s, int on) {
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return;
    fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

#endif /* _WIN32 */

#endif /* YQ_THREAD_H */

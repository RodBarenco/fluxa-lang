/* watcher.h — File watcher for fluxa -dev mode (Sprint 7.b)
 *
 * Backends:
 *   Linux:  inotify (IN_CLOSE_WRITE | IN_MOVED_TO)
 *   macOS:  kqueue  (EVFILT_VNODE, NOTE_WRITE | NOTE_ATTRIB)
 *   Other:  poll-based fallback (stat mtime, 500ms interval)
 *
 * API:
 *   FWatcher *fw = fw_open(path);   -- start watching file
 *   int changed  = fw_wait(fw, ms); -- block up to ms ms; returns 1 if changed
 *   void fw_close(fw);              -- stop watching
 *
 * fw_wait returns:
 *   1  = file changed (reload needed)
 *   0  = timeout (no change)
 *  -1  = error (watcher invalidated, should reopen)
 */
#ifndef FLUXA_WATCHER_H
#define FLUXA_WATCHER_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/inotify.h>
#  include <unistd.h>
#  include <poll.h>
#  define FLUXA_WATCHER_INOTIFY 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#  include <sys/event.h>
#  include <sys/time.h>
#  include <fcntl.h>
#  include <unistd.h>
#  define FLUXA_WATCHER_KQUEUE 1
#else
#  define FLUXA_WATCHER_POLL 1
#endif

typedef struct {
    char path[512];
#if defined(FLUXA_WATCHER_INOTIFY)
    int  ifd;   /* inotify fd */
    int  wfd;   /* watch descriptor */
#elif defined(FLUXA_WATCHER_KQUEUE)
    int  kq;    /* kqueue fd */
    int  ffd;   /* file fd */
#endif
    /* fallback: mtime-based */
    time_t last_mtime;
} FWatcher;

static inline FWatcher *fw_open(const char *path) {
    FWatcher *fw = (FWatcher*)calloc(1, sizeof(FWatcher));
    if (!fw) return NULL;
    strncpy(fw->path, path, sizeof(fw->path) - 1);

    /* init mtime fallback baseline */
    struct stat st;
    fw->last_mtime = (stat(path, &st) == 0) ? st.st_mtime : 0;

#if defined(FLUXA_WATCHER_INOTIFY)
    fw->ifd = inotify_init1(IN_NONBLOCK);
    if (fw->ifd < 0) { free(fw); return NULL; }
    fw->wfd = inotify_add_watch(fw->ifd, path,
                  IN_CLOSE_WRITE | IN_MOVED_TO | IN_MODIFY);
    if (fw->wfd < 0) {
        close(fw->ifd); fw->ifd = -1;
        /* fall through to mtime fallback */
    }
#elif defined(FLUXA_WATCHER_KQUEUE)
    fw->kq  = kqueue();
    fw->ffd = open(path, O_RDONLY);
    if (fw->kq >= 0 && fw->ffd >= 0) {
        struct kevent ev;
        EV_SET(&ev, fw->ffd, EVFILT_VNODE,
               EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME, 0, NULL);
        kevent(fw->kq, &ev, 1, NULL, 0, NULL);
    }
#endif
    return fw;
}

static inline int fw_wait(FWatcher *fw, int timeout_ms) {
    if (!fw) return -1;

#if defined(FLUXA_WATCHER_INOTIFY)
    if (fw->ifd >= 0 && fw->wfd >= 0) {
        struct pollfd pfd; pfd.fd = fw->ifd; pfd.events = POLLIN;
        int r = poll(&pfd, 1, timeout_ms);
        if (r > 0 && (pfd.revents & POLLIN)) {
            /* drain events */
            char buf[4096] __attribute__((aligned(8)));
            ssize_t nr = read(fw->ifd, buf, sizeof(buf));
            (void)nr;
            return 1;
        }
        return (r == 0) ? 0 : -1;
    }
#elif defined(FLUXA_WATCHER_KQUEUE)
    if (fw->kq >= 0 && fw->ffd >= 0) {
        struct kevent ev;
        /* timespec is available on macOS/BSD where kqueue is used */
        struct timespec ts;
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        int r = kevent(fw->kq, NULL, 0, &ev, 1, &ts);
        if (r > 0) return 1;
        return (r == 0) ? 0 : -1;
    }
#endif

#if defined(FLUXA_WATCHER_POLL)
    /* mtime fallback — available when neither inotify nor kqueue compiled in */
    {
        /* Use select() as a portable sleep — no feature macros needed */
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        select(0, NULL, NULL, NULL, &tv);

        struct stat st;
        if (stat(fw->path, &st) != 0) return -1;
        if (st.st_mtime != fw->last_mtime) {
            fw->last_mtime = st.st_mtime;
            return 1;
        }
        return 0;
    }
#endif
    return 0;
}

static inline void fw_close(FWatcher *fw) {
    if (!fw) return;
#if defined(FLUXA_WATCHER_INOTIFY)
    if (fw->wfd >= 0) inotify_rm_watch(fw->ifd, fw->wfd);
    if (fw->ifd >= 0) close(fw->ifd);
#elif defined(FLUXA_WATCHER_KQUEUE)
    if (fw->ffd >= 0) close(fw->ffd);
    if (fw->kq  >= 0) close(fw->kq);
#endif
    free(fw);
}

#endif /* FLUXA_WATCHER_H */

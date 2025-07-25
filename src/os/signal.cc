/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <rango@swoole.com>                             |
  +----------------------------------------------------------------------+
*/

#include "swoole.h"
#include "swoole_api.h"
#include "swoole_signal.h"
#include "swoole_socket.h"
#include "swoole_reactor.h"

#ifdef HAVE_SIGNALFD
#include <sys/signalfd.h>
#endif

#include <csignal>

#ifdef HAVE_KQUEUE
#ifdef USE_KQUEUE_IDE_HELPER
#include "helper/kqueue.h"
#else
#include <sys/event.h>
#endif
#endif
#ifdef __NetBSD__
#include <sys/param.h>
#endif

using swoole::Event;
using swoole::Reactor;
using swoole::Signal;
using swoole::SignalHandler;
using swoole::network::Socket;

#ifdef HAVE_SIGNALFD
static SignalHandler swoole_signalfd_set(int signo, SignalHandler handler);
static bool swoole_signalfd_create();
static bool swoole_signalfd_setup(Reactor *reactor);
static int swoole_signalfd_event_callback(Reactor *reactor, Event *event);
static void swoole_signalfd_clear();
#endif

#ifdef HAVE_KQUEUE
static SignalHandler swoole_signal_kqueue_set(int signo, SignalHandler handler);
static void swoole_signal_kqueue_clear();
#endif

static void signal_handler_safety(int signo);
static void signal_handler_simple(int signo);

#ifdef HAVE_SIGNALFD
static sigset_t signalfd_mask;
static int signal_fd = -1;
static pid_t signalfd_create_pid;
static Socket *signal_socket = nullptr;
static inline bool swoole_signalfd_is_available() {
    return signal_fd != -1;
}
#endif
static Signal signals[SW_SIGNO_MAX];
static bool triggered_signals[SW_SIGNO_MAX];

char *swoole_signal_to_str(int sig) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "%s", strsignal(sig));
    if (strchr(buf, ':') == nullptr) {
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, ": %d", sig);
    }
    return buf;
}

void swoole_signal_block_all() {
    if (SwooleTG.signal_blocking_all) {
        return;
    }
    sigset_t mask;
    sigfillset(&mask);
    int ret = pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (ret < 0) {
        swoole_sys_warning("pthread_sigmask(SIG_BLOCK) failed");
    } else {
        SwooleTG.signal_blocking_all = true;
    }
}

void swoole_signal_unblock_all() {
    if (!SwooleTG.signal_blocking_all) {
        return;
    }
    sigset_t mask;
    sigfillset(&mask);
    int ret = pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
    if (ret < 0) {
        swoole_sys_warning("pthread_sigmask(SIG_UNBLOCK) failed");
    } else {
        SwooleTG.signal_blocking_all = false;
    }
}

/**
 * set new signal handler and return origin signal handler
 */
SignalHandler swoole_signal_set(int signo, SignalHandler func, int restart, int mask) {
    // ignore
    if (func == nullptr) {
        func = SIG_IGN;
    }
    // clear
    else if ((long) func == -1) {
        func = SIG_DFL;
    }

    if (func == SIG_IGN || func == SIG_DFL) {
        signals[signo].handler = nullptr;
        signals[signo].activated = false;
    }

    struct sigaction act{}, oact{};
    act.sa_handler = func;
    if (mask) {
        sigfillset(&act.sa_mask);
    } else {
        sigemptyset(&act.sa_mask);
    }
    act.sa_flags = 0;
    if (restart) {
        act.sa_flags |= SA_RESTART;
    }
    if (sigaction(signo, &act, &oact) < 0) {
        return nullptr;
    }
    return oact.sa_handler;
}

SW_API bool swoole_signal_isset(int signo) {
    return signals[signo].handler && signals[signo].activated;
}

/**
 * set new signal handler and return origin signal handler
 */
SignalHandler swoole_signal_set(int signo, SignalHandler handler, bool safety) {
#ifdef HAVE_SIGNALFD
    if (SwooleG.enable_signalfd && swoole_event_is_available()) {
        return swoole_signalfd_set(signo, handler);
    }
#endif
#ifdef HAVE_KQUEUE
    // SIGCHLD can not be monitored by kqueue, if blocked by SIG_IGN
    // see https://www.freebsd.org/cgi/man.cgi?kqueue
    // if there's no main reactor, signals cannot be monitored either
    if (SwooleG.enable_kqueue && swoole_event_is_available() && signo != SIGCHLD) {
        return swoole_signal_kqueue_set(signo, handler);
    }
#endif

    signals[signo].handler = handler;
    signals[signo].activated = true;
    signals[signo].signo = signo;
    return swoole_signal_set(signo, safety ? signal_handler_safety : signal_handler_simple, 0, 0);
}

static void signal_handler_safety(int signo) {
    triggered_signals[signo] = true;
    SwooleG.signal_dispatch = true;
}

static void signal_handler_simple(int signo) {
    if (sw_reactor()) {
        signal_handler_safety(signo);
    } else {
        static int _lock = 0;
        // discard signal
        if (_lock) {
            return;
        }
        _lock = 1;
        swoole_signal_callback(signo);
        _lock = 0;
    }
}

void swoole_signal_dispatch() {
    if (!SwooleG.signal_dispatch) {
        return;
    }
    SW_LOOP_N(SW_SIGNO_MAX) {
        if (triggered_signals[i]) {
            swoole_signal_callback(i);
            triggered_signals[i] = false;
        }
    }
    SwooleG.signal_dispatch = false;
}

void swoole_signal_callback(int signo) {
    if (signo >= SW_SIGNO_MAX) {
        swoole_warning("signal[%d] numberis invalid", signo);
        return;
    }
    SignalHandler callback = signals[signo].handler;
    if (!callback) {
        swoole_error_log(
            SW_LOG_WARNING, SW_ERROR_UNREGISTERED_SIGNAL, SW_UNREGISTERED_SIGNAL_FMT, swoole_signal_to_str(signo));
        return;
    }
    if (callback == SIG_IGN || callback == SIG_DFL) {
        return;
    }
    callback(signo);
}

SignalHandler swoole_signal_get_handler(int signo) {
    if (signo >= SW_SIGNO_MAX) {
        swoole_warning("signal[%d] numberis invalid", signo);
        return nullptr;
    } else {
        return signals[signo].handler;
    }
}

uint32_t swoole_signal_get_listener_num() {
    return SwooleG.signal_listener_num + SwooleG.signal_async_listener_num;
}

void swoole_signal_clear() {
#ifdef HAVE_SIGNALFD
    if (SwooleG.enable_signalfd && swoole_signalfd_is_available()) {
        swoole_signalfd_clear();
        return;
    }
#endif

#ifdef HAVE_KQUEUE
    if (SwooleG.enable_kqueue) {
        swoole_signal_kqueue_clear();
        return;
    }
#endif

    SW_LOOP_N(SW_SIGNO_MAX) {
        if (signals[i].activated) {
            swoole_signal_set(signals[i].signo, reinterpret_cast<SignalHandler>(-1), 1, 0);
        }
    }
    sw_memset_zero(signals, sizeof(signals));
}

#ifdef HAVE_SIGNALFD
void swoole_signalfd_init() {
    sigemptyset(&signalfd_mask);
    sw_memset_zero(signals, sizeof(signals));
}

/**
 * set new signal handler and return origin signal handler
 */
static SignalHandler swoole_signalfd_set(int signo, SignalHandler handler) {
    SignalHandler origin_handler = nullptr;

    if (handler == nullptr && signals[signo].activated) {
        sigdelset(&signalfd_mask, signo);
        sw_memset_zero(&signals[signo], sizeof(Signal));
    } else {
        sigaddset(&signalfd_mask, signo);
        origin_handler = signals[signo].handler;
        signals[signo].handler = handler;
        signals[signo].signo = signo;
        signals[signo].activated = true;
    }

    if (swoole_signalfd_is_available()) {
        sigprocmask(SIG_SETMASK, &signalfd_mask, nullptr);
        signalfd(signal_fd, &signalfd_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    }
    swoole_signalfd_setup(sw_reactor());

    return origin_handler;
}

static void swoole_signalfd_close() {
    if (!swoole_signalfd_is_available()) {
        return;
    }
    signal_socket->fd = -1;
    signal_socket->free();
    close(signal_fd);
    signal_socket = nullptr;
    signal_fd = -1;
}

static bool swoole_signalfd_create() {
    if (swoole_signalfd_is_available()) {
        return false;
    }

    signal_fd = signalfd(-1, &signalfd_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0) {
        swoole_sys_warning("signalfd() failed");
        return false;
    }
    signal_socket = swoole::make_socket(signal_fd, SW_FD_SIGNAL);
    if (sigprocmask(SIG_BLOCK, &signalfd_mask, nullptr) == -1) {
        swoole_sys_warning("sigprocmask() failed");
        swoole_signalfd_close();
        return false;
    }
    signalfd_create_pid = getpid();
    SwooleG.signal_fd = signal_fd;

    return true;
}

bool swoole_signalfd_setup(Reactor *reactor) {
    if (!swoole_signalfd_is_available() && !swoole_signalfd_create()) {
        return false;
    }
    if (!swoole_event_isset_handler(SW_FD_SIGNAL, SW_EVENT_READ)) {
        swoole_event_set_handler(SW_FD_SIGNAL, SW_EVENT_READ, swoole_signalfd_event_callback);
        reactor->set_exit_condition(Reactor::EXIT_CONDITION_SIGNALFD, [](Reactor *reactor, size_t &event_num) -> bool {
            event_num--;
            return true;
        });
        reactor->add_destroy_callback([](void *) {
            // child process removes signal socket, parent process will not be able to trigger signal
            if (signal_socket && signalfd_create_pid == getpid()) {
                swoole_event_del(signal_socket);
            }
        });
    }
    if (!(signal_socket->events & SW_EVENT_READ) && swoole_event_add(signal_socket, SW_EVENT_READ) < 0) {
        return false;
    }
    reactor->erase_end_callback(Reactor::PRIORITY_SIGNAL_CALLBACK);
    return true;
}

static void swoole_signalfd_clear() {
    if (!swoole_signalfd_is_available()) {
        return;
    }

    if (sigprocmask(SIG_UNBLOCK, &signalfd_mask, nullptr) < 0) {
        swoole_sys_warning("sigprocmask(SIG_UNBLOCK) failed");
    }
    sw_memset_zero(signals, sizeof(signals));
    sw_memset_zero(&signalfd_mask, sizeof(signalfd_mask));

    swoole_signalfd_close();
}

static int swoole_signalfd_event_callback(Reactor *reactor, Event *event) {
    signalfd_siginfo siginfo;
    ssize_t n = read(event->fd, &siginfo, sizeof(siginfo));
    if (n < 0) {
        swoole_sys_warning("read from signalfd failed");
        return SW_OK;
    }
    if (siginfo.ssi_signo >= SW_SIGNO_MAX) {
        swoole_warning("unknown signal[%d]", siginfo.ssi_signo);
        return SW_OK;
    }
    if (signals[siginfo.ssi_signo].activated) {
        SignalHandler handler = signals[siginfo.ssi_signo].handler;
        if (handler == SIG_IGN) {
            return SW_OK;
        } else if (handler) {
            handler(siginfo.ssi_signo);
        } else {
            swoole_error_log(SW_LOG_WARNING,
                             SW_ERROR_UNREGISTERED_SIGNAL,
                             SW_UNREGISTERED_SIGNAL_FMT,
                             swoole_signal_to_str(siginfo.ssi_signo));
        }
    }

    return SW_OK;
}
#endif

#ifdef HAVE_KQUEUE
/**
 * set new signal handler and return origin signal handler
 */
static SignalHandler swoole_signal_kqueue_set(int signo, SignalHandler handler) {
    struct kevent ev;
    SignalHandler origin_handler = nullptr;
    Reactor *reactor = sw_reactor();

    // clear signal
    if (handler == nullptr) {
        signal(signo, SIG_DFL);
        sw_memset_zero(&signals[signo], sizeof(Signal));
        EV_SET(&ev, signo, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    }
    // add/update signal
    else {
        signal(signo, SIG_IGN);
        origin_handler = signals[signo].handler;
        signals[signo].handler = handler;
        signals[signo].signo = signo;
        signals[signo].activated = true;
#if !defined(__NetBSD__) || (defined(__NetBSD__) && __NetBSD_Version__ >= 1000000000)
        auto sigptr = &signals[signo];
#else
        auto sigptr = reinterpret_cast<intptr_t>(&signals[signo]);
#endif
        // save swSignal* as udata
        EV_SET(&ev, signo, EVFILT_SIGNAL, EV_ADD, 0, 0, sigptr);
    }
    int n = kevent(reactor->native_handle, &ev, 1, nullptr, 0, nullptr);
    if (n < 0 && sw_unlikely(handler)) {
        swoole_sys_warning("kevent set signal[%d] error", signo);
    }

    return origin_handler;
}

static void swoole_signal_kqueue_clear() {
    SW_LOOP_N(SW_SIGNO_MAX) {
        if (signals[i].activated && swoole_event_is_available()) {
            signals[i].activated = false;
            signals[i].handler = nullptr;
            swoole_signal_kqueue_set(signals[i].signo, nullptr);
        }
    }
    sw_memset_zero(signals, sizeof(signals));
}
#endif

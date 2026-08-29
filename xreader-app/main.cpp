#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>

#include "inkitem.h"
#include "pagestore.h"
#include "powerkey.h"
#include "stylus.h"
#include "crashctx.h"

// 全局崩溃上下文（供 pagestore/xclient/renderer 记录当前操作）
static char g_crashCtx[192] = {0};
void remarkxSetCtx(const char *s)
{
    strncpy(g_crashCtx, s, sizeof(g_crashCtx) - 1);
    g_crashCtx[sizeof(g_crashCtx) - 1] = '\0';
}

namespace {
// 崩溃时把出错 PC(偏移) + 栈回溯写到 /home/root/xreader/crash.log。
// 主程序加载基址在启动期用 dladdr 记录（信号处理器里调用 dladdr 可能因
// 链接器锁死锁，故处理器只做纯算术 pc - base，可离线 addr2line 定位）。
static char g_altStack[128 * 1024];
static void *g_base = nullptr;

void crashHandler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = static_cast<ucontext_t *>(ctx);
    void *pc = reinterpret_cast<void *>(
        static_cast<uintptr_t>(uc->uc_mcontext.arm_pc));
    void *off = g_base ? reinterpret_cast<void *>(
                             reinterpret_cast<char *>(pc)
                             - reinterpret_cast<char *>(g_base))
                       : pc;
    void *frames[32];
    const int n = backtrace(frames, 32);
    FILE *f = fopen("/home/root/xreader/crash.log", "a");
    if (f) {
        const time_t now = time(nullptr);
        fprintf(f, "\n=== crash signal %d at %s  pc=%p off=%p fault=%p\n",
                sig, ctime(&now), pc, off, si->si_addr);
        fprintf(f, "  ctx: %s\n", g_crashCtx);
        // 记录崩溃时内存用量（statm：size/resident/shared/text/lib/data）
        FILE *sm = fopen("/proc/self/statm", "r");
        if (sm) {
            unsigned long size = 0, res = 0, shr = 0, txt = 0, lib = 0,
                          data = 0, dt = 0;
            if (fscanf(sm, "%lu %lu %lu %lu %lu %lu %lu", &size, &res, &shr,
                       &txt, &lib, &data, &dt) == 7) {
                fprintf(f, "  mem: VmSize=%luMB RSS=%luMB data=%luMB\n",
                        size / 256, res / 256, data / 256);
            }
            fclose(sm);
        }
        fflush(f);
        backtrace_symbols_fd(frames, n, fileno(f));
        fclose(f);
    }
    _exit(1);
}

void installCrashHandler()
{
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&installCrashHandler), &info))
        g_base = info.dli_fbase;

    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_altStack;
    ss.ss_size = sizeof(g_altStack);
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
}
class TouchSpy : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::TouchBegin:
            qInfo() << "TOUCH begin" << static_cast<QTouchEvent *>(ev)->points().first().position();
            break;
        case QEvent::TouchUpdate:
            qInfo() << "TOUCH update" << static_cast<QTouchEvent *>(ev)->points().first().position();
            break;
        case QEvent::TouchEnd:
            qInfo() << "TOUCH end" << static_cast<QTouchEvent *>(ev)->points().first().position();
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove:
            if (QCoreApplication::instance()->property("touchLog") == 0)
                qInfo() << "MOUSE" << ev->type();
            break;
        default:
            break;
        }
        return QObject::eventFilter(obj, ev);
    }
};
} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.installEventFilter(new TouchSpy(&app));
    installCrashHandler();
    qInfo() << "BUILD_INK=v9 standalone renderer";

    const QString baseDir = "/home/root/xreader";

    qmlRegisterType<InkItem>("xreader", 1, 0, "InkItem");

    Stylus stylus;
    stylus.start();
    stylus.loadCalib(baseDir + "/calib.json");

    PowerKey powerKey;
    powerKey.start();

    PageStore pageStore;
    pageStore.configure(baseDir);

    QQmlApplicationEngine engine;
    engine.addImageProvider("pages", pageStore.provider());
    engine.rootContext()->setContextProperty("stylusObj", &stylus);
    engine.rootContext()->setContextProperty("powerKeyObj", &powerKey);
    engine.rootContext()->setContextProperty("pageStore", &pageStore);
    engine.rootContext()->setContextProperty("baseDir", baseDir);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &engine,
                     [&](QObject *obj, const QUrl &url) {
                         Q_UNUSED(obj);
                         qInfo() << "objectCreated:" << url;
                         if (url.toString().endsWith("Main.qml")) {
                             pageStore.setWindow(qobject_cast<QQuickWindow *>(obj));
                             pageStore.start();
                         }
                     });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);

    const bool menuMode = app.arguments().contains("--menu");
    qInfo() << "BUILD_INK=v8" << (menuMode ? "menu" : "reader") << "mode";
    engine.loadFromModule("xreader", menuMode ? "Menu" : "Main");

    return app.exec();
}

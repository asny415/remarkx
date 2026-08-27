#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "inkitem.h"
#include "pagestore.h"
#include "powerkey.h"
#include "stylus.h"

namespace {
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
    qInfo() << "BUILD_INK=v7 powerkey-suspend";

    const QString baseDir = "/home/root/xreader";

    QString relay = "http://192.168.3.235:8788";
    QFile cf(baseDir + "/config");
    if (cf.open(QIODevice::ReadOnly)) {
        const QString line = QString::fromUtf8(cf.readAll()).trimmed();
        cf.close();
        if (!line.isEmpty())
            relay = line;
    }

    qmlRegisterType<InkItem>("xreader", 1, 0, "InkItem");

    Stylus stylus;
    stylus.start();
    stylus.loadCalib(baseDir + "/calib.json");

    PowerKey powerKey;
    powerKey.start();

    PageStore pageStore;
    pageStore.configure(relay, baseDir);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("stylusObj", &stylus);
    engine.rootContext()->setContextProperty("powerKeyObj", &powerKey);
    engine.rootContext()->setContextProperty("pageStore", &pageStore);
    engine.rootContext()->setContextProperty("baseDir", baseDir);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &engine,
                     [&](QObject *obj, const QUrl &url) {
                         Q_UNUSED(obj);
                         qInfo() << "objectCreated:" << url;
                         if (url.toString().endsWith("Main.qml"))
                             pageStore.start();
                     });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);

    engine.loadFromModule("xreader", "Main");

    return app.exec();
}

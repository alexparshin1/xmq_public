#include <QApplication>
#include <QCloseEvent>
#include <QCommandLineParser>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QWebEngineCertificateError>
#include <QWebEnginePage>
#include <QWebEngineView>

class ConsoleWindow final : public QWebEngineView {
public:
    explicit ConsoleWindow(const QUrl& url) {
        setWindowTitle(QStringLiteral("XMQ Console"));
        setWindowIcon(QIcon::fromTheme(QStringLiteral("xmq-tray"),
                                       QIcon(QStringLiteral(":/icons/xmq-tray.png"))));
        resize(1100, 750);

        connect(page(), &QWebEnginePage::certificateError, this,
                [this](const QWebEngineCertificateError& error) {
                    auto decision = error;
                    // The local broker commonly uses a self-signed certificate. Ask each
                    // time rather than silently trusting an arbitrary local service.
                    if (error.isOverridable() &&
                        QMessageBox::warning(this, QStringLiteral("XMQ certificate"),
                                             QStringLiteral("The certificate for %1 could not be verified:\n%2\n\nContinue to this site?")
                                                 .arg(error.url().toString(), error.description()),
                                             QMessageBox::Yes | QMessageBox::No,
                                             QMessageBox::No) == QMessageBox::Yes) {
                        decision.acceptCertificate();
                    } else {
                        decision.rejectCertificate();
                    }
                });
        load(url);
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        hide();
        event->ignore();
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("xmq_tray"));
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("XMQ console tray application"));
    parser.addHelpOption();
    parser.addOption({{QStringLiteral("u"), QStringLiteral("url")},
                      QStringLiteral("Console URL (default: https://localhost:1883)"),
                      QStringLiteral("url"), QStringLiteral("https://localhost:1883")});
    parser.process(app);

    const QUrl url(parser.value(QStringLiteral("url")));
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()) {
        qCritical("--url must be a valid HTTPS URL");
        return 2;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qCritical("No system tray is available in this desktop session");
        return 1;
    }

    ConsoleWindow window(url);
    QMenu menu;
    auto* openAction = menu.addAction(QStringLiteral("Open XMQ Console"));
    auto* reloadAction = menu.addAction(QStringLiteral("Reload"));
    menu.addSeparator();
    auto* quitAction = menu.addAction(QStringLiteral("Quit"));

    const auto showWindow = [&window]() {
        window.show();
        window.raise();
        window.activateWindow();
    };
    QObject::connect(openAction, &QAction::triggered, &app, showWindow);
    QObject::connect(reloadAction, &QAction::triggered, &window, &QWebEngineView::reload);
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);

    QSystemTrayIcon tray(QIcon::fromTheme(QStringLiteral("xmq-tray"),
                                         QIcon(QStringLiteral(":/icons/xmq-tray.png"))));
    tray.setToolTip(QStringLiteral("XMQ Console"));
    tray.setContextMenu(&menu);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &app,
                     [&showWindow](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                             showWindow();
                     });
    tray.show();
    showWindow();
    return app.exec();
}

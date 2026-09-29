// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <qt/splashscreen.h>

#include <clientversion.h>
#include <common/license_info.h>
#include <common/system.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/walletmodel.h>
#include <util/translation.h>

#include <condition_variable>
#include <list>
#include <mutex>

#include <QApplication>
#include <QCloseEvent>
#include <QPainter>
#include <QPointer>
#include <QRadialGradient>
#include <QScreen>
#include <qscopeguard.h>

// Core disconnection does not join already-entered callbacks. No callback
// owns/accesses the widget; backend registration and cleanup share this state.
struct SplashScreenNotificationState {
    std::mutex mutex;
    std::condition_variable idle;
    SplashScreen* target{nullptr};
    size_t active_registrations{0};
    bool wallet_subscription_started{false};
    bool message_queued{false};
    QString latest_message;
    std::list<std::unique_ptr<interfaces::Handler>> core_handlers;
    std::unique_ptr<interfaces::Handler> load_wallet_handler;
    std::list<std::unique_ptr<interfaces::Wallet>> wallets;
    std::list<std::unique_ptr<interfaces::Handler>> wallet_handlers;

    bool beginRegistration(bool init_wallet = false)
    {
        std::lock_guard lock{mutex};
        if (!target || (init_wallet && wallet_subscription_started)) return false;
        if (init_wallet) wallet_subscription_started = true;
        ++active_registrations;
        return true;
    }

    void endRegistration()
    {
        std::lock_guard lock{mutex};
        --active_registrations;
        idle.notify_all();
    }
};

namespace {
void DisconnectSplash(const std::shared_ptr<SplashScreenNotificationState>& state)
{
    std::list<std::unique_ptr<interfaces::Handler>> core_handlers, wallet_handlers;
    std::unique_ptr<interfaces::Handler> load_wallet_handler;
    std::list<std::unique_ptr<interfaces::Wallet>> wallets;
    {
        std::unique_lock lock{state->mutex};
        state->idle.wait(lock, [&] { return state->active_registrations == 0; });
        core_handlers.swap(state->core_handlers);
        wallet_handlers.swap(state->wallet_handlers);
        wallets.swap(state->wallets);
        load_wallet_handler.swap(state->load_wallet_handler);
    }
    // Never hold the shared-state mutex while taking a wallet/loader lock.
    // Normal stop runs here off-GUI, including final interface/DB releases.
    if (load_wallet_handler) load_wallet_handler->disconnect();
    for (const auto& handler : core_handlers) handler->disconnect();
    for (const auto& handler : wallet_handlers) handler->disconnect();
    wallet_handlers.clear();
    wallets.clear();
}

void QueueSplashMessage(const std::shared_ptr<SplashScreenNotificationState>& state, QString message)
{
    std::lock_guard lock{state->mutex};
    auto* target = state->target;
    if (!target) return;
    state->latest_message = std::move(message);
    if (state->message_queued) return;
    state->message_queued = true;
    QMetaObject::invokeMethod(target, [state, target] {
        QString message;
        {
            std::lock_guard lock{state->mutex};
            state->message_queued = false;
            if (state->target != target) return;
            message.swap(state->latest_message);
        }
        target->showMessage(message, Qt::AlignBottom | Qt::AlignHCenter, QColor(55, 55, 55));
    }, Qt::QueuedConnection);
}

void ShowSplashProgress(const std::shared_ptr<SplashScreenNotificationState>& state, const std::string& title, int progress, bool resume_possible)
{
    QueueSplashMessage(state, QString::fromStdString(title) + "\n" +
        (resume_possible ? SplashScreen::tr("(press q to shutdown and continue later)") : SplashScreen::tr("press q to shutdown")) +
        QString("\n%1%").arg(progress));
}

void SubscribeWalletProgress(const std::shared_ptr<SplashScreenNotificationState>& state, interfaces::Node* node)
{
#ifdef ENABLE_WALLET
    if (!state->beginRegistration(/*init_wallet=*/true)) return;
    const auto done = qScopeGuard([state] { state->endRegistration(); });
    if (!WalletModel::isWalletEnabled()) return;
    auto handler = node->walletLoader().handleLoadWallet([state](std::unique_ptr<interfaces::Wallet> wallet) {
        if (!state->beginRegistration()) return;
        const auto done = qScopeGuard([state] { state->endRegistration(); });
        auto progress_handler = wallet->handleShowProgress([state](const std::string& title, int progress) {
            ShowSplashProgress(state, title, progress, /*resume_possible=*/false);
        });
        std::lock_guard lock{state->mutex};
        state->wallet_handlers.emplace_back(std::move(progress_handler));
        state->wallets.emplace_back(std::move(wallet));
    });
    std::lock_guard lock{state->mutex};
    state->load_wallet_handler = std::move(handler);
#endif
}
} // namespace


SplashScreen::SplashScreen(const NetworkStyle* networkStyle)
    : QWidget()
{
    // set reference point, paddings
    int paddingRight            = 50;
    int paddingTop              = 50;
    int titleVersionVSpace      = 17;
    int titleCopyrightVSpace    = 40;

    float fontFactor            = 1.0;
    float devicePixelRatio      = 1.0;
    devicePixelRatio = static_cast<QGuiApplication*>(QCoreApplication::instance())->devicePixelRatio();

    // define text to place
    QString titleText       = CLIENT_NAME;
    QString versionText     = QString("Version %1").arg(QString::fromStdString(FormatFullVersion()));
    QString copyrightText   = QString::fromUtf8(CopyrightHolders(strprintf("\xc2\xA9 %u-%u ", 2009, COPYRIGHT_YEAR)).c_str());
    const QString& titleAddText    = networkStyle->getTitleAddText();

    QString font            = QApplication::font().toString();

    // create a bitmap according to device pixelratio
    QSize splashSize(480*devicePixelRatio,320*devicePixelRatio);
    pixmap = QPixmap(splashSize);

    // change to HiDPI if it makes sense
    pixmap.setDevicePixelRatio(devicePixelRatio);

    QPainter pixPaint(&pixmap);
    pixPaint.setPen(QColor(100,100,100));

    // draw a slightly radial gradient
    QRadialGradient gradient(QPoint(0,0), splashSize.width()/devicePixelRatio);
    gradient.setColorAt(0, Qt::white);
    gradient.setColorAt(1, QColor(247,247,247));
    QRect rGradient(QPoint(0,0), splashSize);
    pixPaint.fillRect(rGradient, gradient);

    // Draw the ConnectCoin icon; expected PNG size: 1024x1024.
    QRect rectIcon(QPoint(-150,-122), QSize(430,430));

    const QSize requiredSize(1024,1024);
    QPixmap icon(networkStyle->getAppIcon().pixmap(requiredSize));

    pixPaint.drawPixmap(rectIcon, icon);

    // check font size and drawing with
    pixPaint.setFont(QFont(font, 33*fontFactor));
    QFontMetrics fm = pixPaint.fontMetrics();
    int titleTextWidth = GUIUtil::TextWidth(fm, titleText);
    if (titleTextWidth > 176) {
        fontFactor = fontFactor * 176 / titleTextWidth;
    }

    pixPaint.setFont(QFont(font, 33*fontFactor));
    fm = pixPaint.fontMetrics();
    titleTextWidth  = GUIUtil::TextWidth(fm, titleText);
    pixPaint.drawText(pixmap.width()/devicePixelRatio-titleTextWidth-paddingRight,paddingTop,titleText);

    pixPaint.setFont(QFont(font, 15*fontFactor));

    // if the version string is too long, reduce size
    fm = pixPaint.fontMetrics();
    int versionTextWidth  = GUIUtil::TextWidth(fm, versionText);
    if(versionTextWidth > titleTextWidth+paddingRight-10) {
        pixPaint.setFont(QFont(font, 10*fontFactor));
        titleVersionVSpace -= 5;
    }
    pixPaint.drawText(pixmap.width()/devicePixelRatio-titleTextWidth-paddingRight+2,paddingTop+titleVersionVSpace,versionText);

    // draw copyright stuff
    {
        pixPaint.setFont(QFont(font, 10*fontFactor));
        const int x = pixmap.width()/devicePixelRatio-titleTextWidth-paddingRight;
        const int y = paddingTop+titleCopyrightVSpace;
        QRect copyrightRect(x, y, pixmap.width() - x - paddingRight, pixmap.height() - y);
        pixPaint.drawText(copyrightRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, copyrightText);
    }

    // draw additional text if special network
    if(!titleAddText.isEmpty()) {
        QFont boldFont = QFont(font, 10*fontFactor);
        boldFont.setWeight(QFont::Bold);
        pixPaint.setFont(boldFont);
        fm = pixPaint.fontMetrics();
        int titleAddTextWidth  = GUIUtil::TextWidth(fm, titleAddText);
        pixPaint.drawText(pixmap.width()/devicePixelRatio-titleAddTextWidth-10,15,titleAddText);
    }

    pixPaint.end();

    // Set window title
    setWindowTitle(titleText + " " + titleAddText);

    // Resize window and move to center of desktop, disallow resizing
    QRect r(QPoint(), QSize(pixmap.size().width()/devicePixelRatio,pixmap.size().height()/devicePixelRatio));
    resize(r.size());
    setFixedSize(r.size());
    move(QGuiApplication::primaryScreen()->geometry().center() - r.center());

    installEventFilter(this);

    GUIUtil::handleCloseWindowShortcut(this);
}

SplashScreen::~SplashScreen()
{
    if (!m_notifications) return;
    {
        std::lock_guard lock{m_notifications->mutex};
        m_notifications->target = nullptr;
    }
    // Non-reentrant safety fallback; normal stop already drained this state.
    DisconnectSplash(m_notifications);
}

void SplashScreen::stop()
{
    if (m_stopping) return;
    m_stopping = true;
    if (!m_notifications) return;
    const auto state = m_notifications;
    {
        std::lock_guard lock{state->mutex};
        state->target = nullptr;
    }
    // Loader disconnection can wait for a wallet load, and releasing the last
    // wallet interface may flush its database. Keep the splash alive while both
    // finish, with no backend list access from painting/message callbacks.
    const QPointer<SplashScreen> guard{this};
    GUIUtil::WaitForBackendTask(std::async(std::launch::async, [state] { DisconnectSplash(state); }), this);
    if (guard) QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
}

void SplashScreen::setNode(interfaces::Node& node)
{
    assert(!m_node);
    m_node = &node;
    subscribeToCoreSignals();
    if (m_shutdown) Q_EMIT shutdownRequested();
}

void SplashScreen::shutdown()
{
    m_shutdown = true;
    Q_EMIT shutdownRequested();
}

bool SplashScreen::eventFilter(QObject * obj, QEvent * ev) {
    if (ev->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(ev);
        if (keyEvent->key() == Qt::Key_Q) {
            shutdown();
        }
    }
    return QObject::eventFilter(obj, ev);
}

void SplashScreen::subscribeToCoreSignals()
{
    m_notifications = std::make_shared<SplashScreenNotificationState>();
    const auto state = m_notifications;
    state->target = this;
    state->core_handlers.emplace_back(m_node->handleInitMessage([state](const std::string& message) {
        QueueSplashMessage(state, QString::fromStdString(message));
    }));
    state->core_handlers.emplace_back(m_node->handleShowProgress([state](const std::string& title, int progress, bool resume_possible) {
        ShowSplashProgress(state, title, progress, resume_possible);
    }));
    state->core_handlers.emplace_back(m_node->handleInitWallet([state, node = m_node] { SubscribeWalletProgress(state, node); }));
}

void SplashScreen::showMessage(const QString &message, int alignment, const QColor &color)
{
    if (m_stopping) return;
    curMessage = message;
    curAlignment = alignment;
    curColor = color;
    update();
}

void SplashScreen::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.drawPixmap(0, 0, pixmap);
    QRect r = rect().adjusted(5, 5, -5, -5);
    painter.setPen(curColor);
    painter.drawText(r, curAlignment, curMessage);
}

void SplashScreen::closeEvent(QCloseEvent *event)
{
    shutdown(); // allows an "emergency" shutdown during startup
    event->ignore();
}

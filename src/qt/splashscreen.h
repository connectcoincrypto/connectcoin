// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_SPLASHSCREEN_H
#define CONNECTCOIN_QT_SPLASHSCREEN_H

#include <QWidget>

#include <memory>

class NetworkStyle;
struct SplashScreenNotificationState;

namespace interfaces {
class Handler;
class Node;
class Wallet;
};

/** Class for the splashscreen with information of the running client.
 *
 * @note this is intentionally not a QSplashScreen. ConnectCoin Core initialization
 * can take a long time, and in that case a progress window that cannot be
 * moved around and minimized has turned out to be frustrating to the user.
 */
class SplashScreen : public QWidget
{
    Q_OBJECT

public:
    explicit SplashScreen(const NetworkStyle *networkStyle);
    ~SplashScreen();
    void setNode(interfaces::Node& node);
    //! Disconnect and release backend subscriptions responsively before deletion.
    void stop();

protected:
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

public Q_SLOTS:
    /** Show message and progress */
    void showMessage(const QString &message, int alignment, const QColor &color);

Q_SIGNALS:
    void shutdownRequested();

protected:
    bool eventFilter(QObject * obj, QEvent * ev) override;

private:
    /** Connect core signals to splash screen */
    void subscribeToCoreSignals();
    /** Initiate shutdown */
    void shutdown();

    QPixmap pixmap;
    QString curMessage;
    QColor curColor;
    int curAlignment{0};

    interfaces::Node* m_node = nullptr;
    bool m_shutdown = false;
    bool m_stopping{false};
    std::shared_ptr<SplashScreenNotificationState> m_notifications;
};

#endif // CONNECTCOIN_QT_SPLASHSCREEN_H

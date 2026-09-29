// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/util.h>

#include <chrono>

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>

void ConfirmMessage(QString* text, std::chrono::milliseconds msec)
{
    auto* timer = new QTimer(qApp);
    timer->setInterval(10);
    QTimer::singleShot(std::chrono::seconds{30}, timer, &QObject::deleteLater);
    QObject::connect(timer, &QTimer::timeout, [timer, text]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->isVisible() && widget->inherits("QMessageBox")) {
                timer->stop();
                timer->deleteLater();
                QMessageBox* messageBox = qobject_cast<QMessageBox*>(widget);
                if (text) *text = messageBox->text();
                messageBox->defaultButton()->click();
                return;
            }
        }
    });
    QTimer::singleShot(msec, timer, [timer] { timer->start(); });
}

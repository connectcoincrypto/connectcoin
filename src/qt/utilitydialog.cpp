// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <qt/utilitydialog.h>

#include <qt/forms/ui_helpmessagedialog.h>

#include <qt/guiutil.h>

#include <clientversion.h>
#include <common/args.h>
#include <common/license_info.h>
#include <init.h>
#include <util/strencodings.h>

#include <cstdio>
#include <future>

#include <QCloseEvent>
#include <QLabel>
#include <QMainWindow>
#include <QRegularExpression>
#include <QString>
#include <QTextCursor>
#include <QTextTable>
#include <QVBoxLayout>

/** "Help message" or "About" dialog box */
HelpMessageDialog::HelpMessageDialog(QWidget *parent, bool about, const QString& coreOptions) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::HelpMessageDialog)
{
    ui->setupUi(this);

    QString version = QString{CLIENT_NAME} + " " + tr("version") + " " + QString::fromStdString(FormatFullVersion());

    if (about)
    {
        setWindowTitle(tr("About %1").arg(CLIENT_NAME));

        std::string licenseInfo = LicenseInfo();
        /// HTML-format the license message from the core
        QString licenseInfoHTML = QString::fromStdString(LicenseInfo());
        // Make URLs clickable
        QRegularExpression uri(QStringLiteral("<(.*)>"), QRegularExpression::InvertedGreedinessOption);
        licenseInfoHTML.replace(uri, QStringLiteral("<a href=\"\\1\">\\1</a>"));
        // Replace newlines with HTML breaks
        licenseInfoHTML.replace("\n", "<br>");

        ui->aboutMessage->setTextFormat(Qt::RichText);
        ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        text = version + "\n" + QString::fromStdString(FormatParagraph(licenseInfo));
        ui->aboutMessage->setText(version + "<br><br>" + licenseInfoHTML);
        ui->aboutMessage->setWordWrap(true);
        ui->helpMessage->setVisible(false);
    } else {
        setWindowTitle(tr("Command-line options"));
        QString header = "The connectcoin-qt application provides a graphical interface for interacting with " CLIENT_NAME ".\n\n"
                         "It combines the core functionalities of connectcoind with a user-friendly interface for wallet management, transaction history, and network statistics.\n\n"
                         "It is suitable for users who prefer a graphical over a command-line interface.\n\n"
                         "You can optionally specify a payment [URI], in e.g. the BIP21 URI format.\n\n"
                         "Usage: connectcoin-qt [options] [URI]\n\n";
        QTextCursor cursor(ui->helpMessage->document());
        // One document edit avoids repeated relayout for every option/row.
        cursor.beginEditBlock();
        cursor.insertText(version);
        cursor.insertBlock();
        cursor.insertText(header);
        cursor.insertBlock();

        text = version + "\n\n" + header + "\n" + coreOptions;

        QTextTableFormat tf;
        tf.setBorderStyle(QTextFrameFormat::BorderStyle_None);
        tf.setCellPadding(2);
        QVector<QTextLength> widths;
        widths << QTextLength(QTextLength::PercentageLength, 35);
        widths << QTextLength(QTextLength::PercentageLength, 65);
        tf.setColumnWidthConstraints(widths);

        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);

        QTextTable* table{nullptr};
        int option_row{0};
        const QTextCharFormat normal;
        for (const QString &line : coreOptions.split("\n")) {
            if (line.startsWith("  -"))
            {
                if (!table) table = cursor.insertTable(1, 2, tf);
                if (option_row >= table->rows()) table->appendRows(1);
                cursor = table->cellAt(option_row, 0).firstCursorPosition();
                cursor.insertText(line.trimmed(), normal);
                cursor = table->cellAt(option_row++, 1).firstCursorPosition();
            } else if (line.startsWith("   ")) {
                cursor.insertText(line.trimmed()+' ', normal);
            } else if (line.size() > 0) {
                // Address document positions/cells directly. Visual cursor
                // movement depends on layout, deferred by beginEditBlock().
                if (table) cursor.setPosition(table->lastPosition() + 1);
                cursor.insertText(line.trimmed(), bold);
                cursor.insertBlock();
                table = cursor.insertTable(1, 2, tf);
                option_row = 0;
            }
        }

        cursor.endEditBlock();
        ui->helpMessage->moveCursor(QTextCursor::Start);
        ui->scrollArea->setVisible(false);
        ui->aboutLogo->setVisible(false);
    }

    GUIUtil::handleCloseWindowShortcut(this);
}

HelpMessageDialog::~HelpMessageDialog()
{
    delete ui;
}

QString HelpMessageDialog::loadHelpOptions()
{
    // GetHelpMessage takes cs_args and formats the complete option list. A
    // settings-file writer may hold that same lock for an arbitrarily long IO.
    return GUIUtil::WaitForBackendTask(std::async(std::launch::async, [] {
        return QString::fromStdString(gArgs.GetHelpMessage());
    }));
}

void HelpMessageDialog::printToConsole()
{
    // On other operating systems, the expected action is to print the message to the console.
    tfm::format(std::cout, "%s", qPrintable(text));
}

void HelpMessageDialog::showOrPrint()
{
#if defined(WIN32)
    // On Windows, show a message box, as there is no stderr/stdout in windowed applications
    exec();
#else
    // On other operating systems, print help text to console
    printToConsole();
#endif
}

void HelpMessageDialog::on_okButton_accepted()
{
    close();
}


/** "Shutdown" window */
ShutdownWindow::ShutdownWindow(QWidget *parent, Qt::WindowFlags f):
    QWidget(parent, f)
{
    QVBoxLayout *layout = new QVBoxLayout();
    layout->addWidget(new QLabel(
        tr("%1 is shutting down…").arg(CLIENT_NAME) + "<br /><br />" +
        tr("Do not shut down the computer until this window disappears.")));
    setLayout(layout);

    GUIUtil::handleCloseWindowShortcut(this);
}

QWidget* ShutdownWindow::showShutdownWindow(QMainWindow* window)
{
    assert(window != nullptr);

    // Show a simple window indicating shutdown status
    QWidget *shutdownWindow = new ShutdownWindow();
    shutdownWindow->setWindowTitle(window->windowTitle());

    // Center shutdown window at where main window was
    const QPoint global = window->mapToGlobal(window->rect().center());
    shutdownWindow->move(global.x() - shutdownWindow->width() / 2, global.y() - shutdownWindow->height() / 2);
    shutdownWindow->show();
    return shutdownWindow;
}

void ShutdownWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
}

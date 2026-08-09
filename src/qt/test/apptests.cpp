// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <qt/test/apptests.h>

#include <chainparams.h>
#include <key.h>
#include <logging.h>
#include <qt/digibyte.h>
#include <qt/digibytegui.h>
#include <qt/networkstyle.h>
#include <qt/rpcconsole.h>
#include <shutdown.h>
#include <test/util/setup_common.h>
#include <validation.h>

#if defined(HAVE_CONFIG_H)
#include <config/digibyte-config.h>
#endif

#include <QAction>
#include <QAbstractButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QTextEdit>
#include <QTimer>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

namespace {
const QString DIGIDOLLAR_WARNING_SETTINGS_KEY = QStringLiteral("DigiDollar/ExperimentalWarningAccepted");

//! Regex find a string group inside of the console output
QString FindInConsole(const QString& output, const QString& pattern)
{
    const QRegularExpression re(pattern);
    return re.match(output).captured(1);
}

QAction* FindAction(DigiByteGUI* window, const QString& object_name, const QString& text_without_mnemonic)
{
    for (QAction* action : window->findChildren<QAction*>()) {
        QString action_text = action->text();
        action_text.remove(QLatin1Char('&'));
        if (action->objectName() == object_name || action_text == text_without_mnemonic) return action;
    }
    return nullptr;
}

QMessageBox* FindDigiDollarWarningDialog()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        QMessageBox* dialog = qobject_cast<QMessageBox*>(widget);
        if (dialog && dialog->objectName() == QStringLiteral("DigiDollarExperimentalWarningDialog") && dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}

void ClickDigiDollarAndHandleWarning(QAction* digi_dollar_action, bool remember, bool accept, bool& saw_dialog)
{
    saw_dialog = false;
    bool handled = false;
    QTimer::singleShot(0, [digi_dollar_action] {
        digi_dollar_action->activate(QAction::Trigger);
    });
    QTimer::singleShot(100, [&] {
        QMessageBox* dialog = FindDigiDollarWarningDialog();
        if (!dialog) {
            handled = true;
            return;
        }

        saw_dialog = true;
        QCOMPARE(dialog->windowTitle(), QStringLiteral("DigiDollar Experimental Feature"));
        QVERIFY2(dialog->text().contains(QStringLiteral("DigiDollar is experimental")), qPrintable(dialog->text()));
        QVERIFY2(dialog->text().contains(QStringLiteral("extensively tested")), qPrintable(dialog->text()));
        QVERIFY2(dialog->text().contains(QStringLiteral("bugs, security issues, or other risks")), qPrintable(dialog->text()));
        QVERIFY2(dialog->text().contains(QStringLiteral("at your own risk")), qPrintable(dialog->text()));
        QVERIFY2(dialog->informativeText().contains(QStringLiteral("funds you cannot afford to lose")), qPrintable(dialog->informativeText()));

        QCheckBox* check_box = dialog->findChild<QCheckBox*>(QStringLiteral("digiDollarExperimentalWarningDontShowAgain"));
        QVERIFY(check_box != nullptr);
        check_box->setChecked(remember);

        QAbstractButton* button_to_click = nullptr;
        const QString wanted_text = accept ? QStringLiteral("I Understand") : QStringLiteral("Cancel");
        for (QAbstractButton* button : dialog->buttons()) {
            if (button->text().remove(QLatin1Char('&')) == wanted_text) {
                button_to_click = button;
                break;
            }
        }
        QVERIFY2(button_to_click != nullptr, qPrintable(QStringLiteral("Missing %1 button").arg(wanted_text)));
        button_to_click->click();
        handled = true;
    });
    QTRY_VERIFY(handled);
}

//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QSignalSpy mw_spy(messagesWidget, &QTextEdit::textChanged);
    QVERIFY(mw_spy.isValid());
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    QVERIFY(mw_spy.wait(1000));
    QCOMPARE(mw_spy.count(), 4);
    const QString output = messagesWidget->toPlainText();
    const QString pattern = QStringLiteral("\"chain\": \"(\\w+)\"");
    QCOMPARE(FindInConsole(output, pattern), QString("regtest"));
}

//! Check that a Paymaster capability is absent from the console transcript,
//! result, and command history even when the RPC itself fails.
void TestSensitivePaymasterRpcCommand(RPCConsole* console)
{
    QTextEdit* messages_widget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* line_edit = console->findChild<QLineEdit*>("lineEdit");
    QVERIFY(messages_widget != nullptr);
    QVERIFY(line_edit != nullptr);

    constexpr auto SECRET = "PAYMASTER_CAPABILITY_MUST_NOT_APPEAR";
    QSignalSpy messages_spy(messages_widget, &QTextEdit::textChanged);
    QVERIFY(messages_spy.isValid());
    QTest::keyClicks(
        line_edit,
        QStringLiteral("requestpaymasterquote %1").arg(SECRET));
    QTest::keyClick(line_edit, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(
        messages_widget->toPlainText().contains(
            QStringLiteral("Sensitive command result hidden.")),
        1000);
    QVERIFY(!messages_widget->toPlainText().contains(SECRET));

    QTest::keyClick(line_edit, Qt::Key_Up);
    QCOMPARE(line_edit->text(), QStringLiteral("requestpaymasterquote(…)"));
    QVERIFY(!line_edit->text().contains(SECRET));
    line_edit->clear();
}
} // namespace

//! Entry point for DigiByteApplication tests.
void AppTests::appTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        QWARN("Skipping AppTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
              "with 'QT_QPA_PLATFORM=cocoa test_digibyte-qt' on mac, or else use a linux or windows build.");
        return;
    }
#endif

    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    m_app.parameterSetup();
    QVERIFY(m_app.createOptionsModel(/*resetSettings=*/true));
    QScopedPointer<const NetworkStyle> style(NetworkStyle::instantiate(Params().GetChainType()));
    m_app.setupPlatformStyle();
    m_app.createWindow(style.data());
    connect(&m_app, &DigiByteApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    m_app.baseInitialize();
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
}

//! Entry point for DigiByteGUI tests.
void AppTests::guiTests(DigiByteGUI* window)
{
    HandleCallback callback{"guiTests", *this};

    QSettings settings;
    settings.remove(DIGIDOLLAR_WARNING_SETTINGS_KEY);
    settings.sync();

    QAction* overview_action = FindAction(window, QStringLiteral("overviewAction"), QStringLiteral("Overview"));
    QVERIFY(overview_action != nullptr);
    QAction* digi_dollar_action = FindAction(window, QStringLiteral("digiDollarAction"), QStringLiteral("DigiDollar"));
    QVERIFY(digi_dollar_action != nullptr);

    overview_action->activate(QAction::Trigger);
    QVERIFY(overview_action->isChecked());

#ifdef Q_OS_WIN
    if (QApplication::platformName() == "minimal") {
        // Qt's Windows QMessageBox implementation accesses a native system menu
        // which the headless minimal plugin does not provide.
        // Skip only the native dialog interaction so the remaining GUI and RPC
        // console tests still run. Normal Windows runs retain the full dialog test.
        QWARN("Skipping the DigiDollar warning dialog interaction with Qt's Windows minimal platform plugin");
    } else
#endif
    {
        bool saw_dialog = false;
        ClickDigiDollarAndHandleWarning(digi_dollar_action, /*remember=*/false, /*accept=*/false, saw_dialog);
        QVERIFY2(saw_dialog, "First DigiDollar tab entry must show the experimental warning dialog");
        QVERIFY2(overview_action->isChecked(), "Canceling the experimental warning must keep the previous tab selected");
        QVERIFY(!settings.value(DIGIDOLLAR_WARNING_SETTINGS_KEY, false).toBool());

        ClickDigiDollarAndHandleWarning(digi_dollar_action, /*remember=*/true, /*accept=*/true, saw_dialog);
        QVERIFY2(saw_dialog, "Accepting DigiDollar for the first time must still show the warning dialog");
        QVERIFY(digi_dollar_action->isChecked());
        QVERIFY(settings.value(DIGIDOLLAR_WARNING_SETTINGS_KEY, false).toBool());

        overview_action->activate(QAction::Trigger);
        QVERIFY(overview_action->isChecked());
        ClickDigiDollarAndHandleWarning(digi_dollar_action, /*remember=*/false, /*accept=*/true, saw_dialog);
        QVERIFY2(!saw_dialog, "Remembered DigiDollar warning acceptance must suppress future warning dialogs");
        QVERIFY(digi_dollar_action->isChecked());
    }

    connect(window, &DigiByteGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    TestRpcCommand(console);
    TestSensitivePaymasterRpcCommand(console);
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.exit(0);
    }
}

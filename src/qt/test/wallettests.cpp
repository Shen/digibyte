// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <qt/test/wallettests.h>
#include <qt/test/util.h>

#include <wallet/coincontrol.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <node/interface_ui.h>
#include <key_io.h>
#include <qt/digibyteamountfield.h>
#include <qt/digibyteunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/receivecoinsdialog.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <script/solver.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <paymaster/provider.h>

#include <chrono>
#include <memory>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QObject>
#include <QPushButton>
#include <QTimer>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QListView>
#include <QDialogButtonBox>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
using wallet::WalletContext;
using wallet::WalletDescriptor;
using wallet::WalletRescanReserver;

namespace
{
//! Press "Yes" or "Cancel" buttons in modal send confirmation dialog.
void ConfirmSend(QString* text = nullptr, QMessageBox::StandardButton confirm_type = QMessageBox::Yes)
{
    QTimer::singleShot(0, [text, confirm_type]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("SendConfirmationDialog")) {
                SendConfirmationDialog* dialog = qobject_cast<SendConfirmationDialog*>(widget);
                if (text) *text = dialog->text();
                QAbstractButton* button = dialog->button(confirm_type);
                button->setEnabled(true);
                button->click();
            }
        }
    });
}

//! Send coins to address and return txid.
uint256 SendCoins(wallet::CWallet& wallet, SendCoinsDialog& sendCoinsDialog, const CTxDestination& address, CAmount amount, bool rbf,
                  QMessageBox::StandardButton confirm_type = QMessageBox::Yes)
{
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    entry->findChild<QValidatedLineEdit*>("payTo")->setText(QString::fromStdString(EncodeDestination(address)));
    entry->findChild<DigiByteAmountField*>("payAmount")->setValue(amount);
    /* DigiByte: Disabled RBF UI
    sendCoinsDialog.findChild<QFrame*>("frameFee")
        ->findChild<QFrame*>("frameFeeSelection")
        ->findChild<QCheckBox*>("optInRBF")
        ->setCheckState(rbf ? Qt::Checked : Qt::Unchecked);
    */
    uint256 txid;
    boost::signals2::scoped_connection c(wallet.NotifyTransactionChanged.connect([&txid](const uint256& hash, ChangeType status) {
        if (status == CT_NEW) txid = hash;
    }));
    ConfirmSend(/*text=*/nullptr, confirm_type);
    bool invoked = QMetaObject::invokeMethod(&sendCoinsDialog, "sendButtonClicked", Q_ARG(bool, false));
    assert(invoked);
    return txid;
}

//! Find index of txid in transaction list.
QModelIndex FindTx(const QAbstractItemModel& model, const uint256& txid)
{
    QString hash = QString::fromStdString(txid.ToString());
    int rows = model.rowCount({});
    for (int row = 0; row < rows; ++row) {
        QModelIndex index = model.index(row, 0, {});
        if (model.data(index, TransactionTableModel::TxHashRole) == hash) {
            return index;
        }
    }
    return {};
}

//! Invoke bumpfee on txid and check results.
/* Digibyte: Disable RBF
void BumpFee(TransactionView& view, const uint256& txid, bool expectDisabled, std::string expectError, bool cancel)
{
    QTableView* table = view.findChild<QTableView*>("transactionView");
    QModelIndex index = FindTx(*table->selectionModel()->model(), txid);
    QVERIFY2(index.isValid(), "Could not find BumpFee txid");

    // Select row in table, invoke context menu, and make sure bumpfee action is
    // enabled or disabled as expected.
    QAction* action = view.findChild<QAction*>("bumpFeeAction");
    table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    action->setEnabled(expectDisabled);
    table->customContextMenuRequested({});
    QCOMPARE(action->isEnabled(), !expectDisabled);

    action->setEnabled(true);
    QString text;
    if (expectError.empty()) {
        ConfirmSend(&text, cancel ? QMessageBox::Cancel : QMessageBox::Yes);
    } else {
        ConfirmMessage(&text, 0ms);
    }
    action->trigger();
    QVERIFY(text.indexOf(QString::fromStdString(expectError)) != -1);
}
*/

void CompareBalance(WalletModel& walletModel, CAmount expected_balance, QLabel* balance_label_to_check)
{
    DigiByteUnit unit = walletModel.getOptionsModel()->getDisplayUnit();
    QString balanceComparison = DigiByteUnits::formatWithUnit(unit, expected_balance, false, DigiByteUnits::SeparatorStyle::ALWAYS);
    QCOMPARE(balance_label_to_check->text().trimmed(), balanceComparison);
}

// Verify the 'useAvailableBalance' functionality. With and without manually selected coins.
// Case 1: No coin control selected coins.
// 'useAvailableBalance' should fill the amount edit box with the total available balance
// Case 2: With coin control selected coins.
// 'useAvailableBalance' should fill the amount edit box with the sum of the selected coins values.
void VerifyUseAvailableBalance(SendCoinsDialog& sendCoinsDialog, WalletModel& walletModel)
{
    // Verify first entry amount and "useAvailableBalance" button
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    QVERIFY(entries->count() == 1); // only one entry
    SendCoinsEntry* send_entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    QVERIFY(send_entry->getValue().amount == 0);
    // Now click "useAvailableBalance", check updated balance (the entire wallet balance should be set)
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QVERIFY(send_entry->getValue().amount == walletModel.getCachedBalance().balance);

    // Locking an input does not change ownership or the cached balance. Max
    // and the send precheck must nevertheless use only currently eligible coins.
    const CAmount owned_balance = walletModel.getCachedBalance().balance;
    const auto available_coins = walletModel.wallet().listCoins();
    QVERIFY(!available_coins.empty());
    QVERIFY(!available_coins.begin()->second.empty());
    const auto& [first_outpoint, first_output] = available_coins.begin()->second.front();
    QVERIFY(walletModel.wallet().lockCoin(first_outpoint, /*write_to_db=*/false));
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QCOMPARE(send_entry->getValue().amount, owned_balance - first_output.txout.nValue);
    QVERIFY(send_entry->getValue().fSubtractFeeFromAmount);
    QCOMPARE(walletModel.getCachedBalance().balance, owned_balance);
    QCOMPARE(walletModel.getAvailableBalance(nullptr), send_entry->getValue().amount);
    QVERIFY(walletModel.wallet().unlockCoin(first_outpoint));
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QCOMPARE(send_entry->getValue().amount, owned_balance);

    // When every input is unavailable, Max must clear an old amount.
    for (const auto& [destination, outputs] : available_coins) {
        for (const auto& [outpoint, output] : outputs) {
            QVERIFY(walletModel.wallet().lockCoin(outpoint, /*write_to_db=*/false));
        }
    }
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QCOMPARE(send_entry->getValue().amount, CAmount{0});
    for (const auto& [destination, outputs] : available_coins) {
        for (const auto& [outpoint, output] : outputs) {
            QVERIFY(walletModel.wallet().unlockCoin(outpoint));
        }
    }

    // Now manually select two coins and click on "useAvailableBalance". Then check updated balance
    // (only the sum of the selected coins should be set).
    int COINS_TO_SELECT = 2;
    auto coins = walletModel.wallet().listCoins();
    CAmount sum_selected_coins = 0;
    int selected = 0;
    QVERIFY(coins.size() == 1); // context check, coins received only on one destination
    for (const auto& [outpoint, tx_out] : coins.begin()->second) {
        sendCoinsDialog.getCoinControl()->Select(outpoint);
        sum_selected_coins += tx_out.txout.nValue;
        if (++selected == COINS_TO_SELECT) break;
    }
    QVERIFY(selected == COINS_TO_SELECT);

    // Now that we have 2 coins selected, "useAvailableBalance" should update the balance label only with
    // the sum of them.
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QVERIFY(send_entry->getValue().amount == sum_selected_coins);
}

void SyncUpWallet(const std::shared_ptr<wallet::CWallet>& wallet, interfaces::Node& node)
{
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    wallet::CWallet::ScanResult result = wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/true, /*save_progress=*/false);
    QCOMPARE(result.status, wallet::CWallet::ScanResult::SUCCESS);
    QCOMPARE(result.last_scanned_block, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    QVERIFY(result.last_failed_block.IsNull());
}

std::shared_ptr<wallet::CWallet> SetupLegacyWatchOnlyWallet(interfaces::Node& node, TestChain100Setup& test)
{
    std::shared_ptr<wallet::CWallet> wallet = std::make_shared<wallet::CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    wallet->LoadWallet();
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
        wallet->SetupLegacyScriptPubKeyMan();
        // Add watched key
        CPubKey pubKey = test.coinbaseKey.GetPubKey();
        bool import_keys = wallet->ImportPubKeys({pubKey.GetID()}, {{pubKey.GetID(), pubKey}} , /*key_origins=*/{}, /*add_keypool=*/false, /*internal=*/false, /*timestamp=*/1);
        assert(import_keys);
        wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    }
    SyncUpWallet(wallet, node);
    return wallet;
}

std::shared_ptr<wallet::CWallet> SetupDescriptorsWallet(interfaces::Node& node, TestChain100Setup& test)
{
    std::shared_ptr<wallet::CWallet> wallet = std::make_shared<wallet::CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    wallet->LoadWallet();
    LOCK(wallet->cs_wallet);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    wallet->SetupDescriptorScriptPubKeyMans();

    // Add the coinbase key
    FlatSigningProvider provider;
    std::string error;
    std::unique_ptr<Descriptor> desc = Parse("combo(" + EncodeSecret(test.coinbaseKey) + ")", provider, error, /* require_checksum=*/ false);
    assert(desc);
    WalletDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    if (!wallet->AddWalletDescriptor(w_desc, provider, "", false)) assert(false);
    CTxDestination dest = GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type);
    wallet->SetAddressBook(dest, "", wallet::AddressPurpose::RECEIVE);
    wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    SyncUpWallet(wallet, node);
    wallet->SetBroadcastTransactions(true);
    return wallet;
}

struct MiniGUI {
public:
    SendCoinsDialog sendCoinsDialog;
    TransactionView transactionView;
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;

    MiniGUI(interfaces::Node& node, const PlatformStyle* platformStyle) : sendCoinsDialog(platformStyle), transactionView(platformStyle), optionsModel(node) {
        bilingual_str error;
        QVERIFY(optionsModel.Init(error));
        clientModel = std::make_unique<ClientModel>(node, &optionsModel);
    }

    void initModelForWallet(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet, const PlatformStyle* platformStyle)
    {
        WalletContext& context = *node.walletLoader().context();
        AddWallet(context, wallet);
        walletModel = std::make_unique<WalletModel>(interfaces::MakeWallet(context, wallet), *clientModel, platformStyle);
        RemoveWallet(context, wallet, /* load_on_start= */ std::nullopt);
        sendCoinsDialog.setModel(walletModel.get());
        transactionView.setModel(walletModel.get());
        walletModel->startPollBalance();
    }

};

//! Simple qt wallet tests.
//
// Test widgets can be debugged interactively calling show() on them and
// manually running the event loop, e.g.:
//
//     sendCoinsDialog.show();
//     QEventLoop().exec();
//
// This also requires overriding the default minimal Qt platform:
//
//     QT_QPA_PLATFORM=xcb     src/qt/test/test_digibyte-qt  # Linux
//     QT_QPA_PLATFORM=windows src/qt/test/test_digibyte-qt  # Windows
//     QT_QPA_PLATFORM=cocoa   src/qt/test/test_digibyte-qt  # macOS
void TestGUI(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    // Create widgets for sending coins and listing transactions.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(node, platformStyle.get());
    mini_gui.initModelForWallet(node, wallet, platformStyle.get());
    WalletModel& walletModel = *mini_gui.walletModel;
    SendCoinsDialog& sendCoinsDialog = mini_gui.sendCoinsDialog;
    TransactionView& transactionView = mini_gui.transactionView;

    // Update walletModel cached balance which will trigger an update for the 'labelBalance' QLabel.
    walletModel.pollBalanceChanged();
    QTRY_VERIFY(walletModel.getCachedBalance().spendable.valid);
    // Check balance in send dialog
    CompareBalance(walletModel, walletModel.wallet().getBalance(), sendCoinsDialog.findChild<QLabel*>("labelBalance"));

    // Check 'UseAvailableBalance' functionality
    VerifyUseAvailableBalance(sendCoinsDialog, walletModel);

    {
        // Check balance in send dialog
        QLabel* balanceLabel = sendCoinsDialog.findChild<QLabel*>("labelBalance");
        QString balanceText = balanceLabel->text();
        DigiByteUnits::Unit unit = walletModel.getOptionsModel()->getDisplayUnit();
        CAmount balance = walletModel.wallet().getBalance();
        QString balanceComparison = DigiByteUnits::formatWithUnit(unit, balance, false, DigiByteUnits::SeparatorStyle::ALWAYS);
        QCOMPARE(balanceText, balanceComparison);
    }

    // Send two transactions, and verify they are added to transaction list.
    TransactionTableModel* transactionTableModel = walletModel.getTransactionTableModel();
    QCOMPARE(transactionTableModel->rowCount({}), 105);
    uint256 txid1 = SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 5 * COIN, /*rbf=*/false);
    uint256 txid2 = SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 10 * COIN, /*rbf=*/true);
    // Transaction table model updates on a QueuedConnection, so process events to ensure it's updated.
    qApp->processEvents();
    QCOMPARE(transactionTableModel->rowCount({}), 107);
    QVERIFY(FindTx(*transactionTableModel, txid1).isValid());
    QVERIFY(FindTx(*transactionTableModel, txid2).isValid());

    // Call bumpfee. Test disabled, canceled, enabled, then failing cases.
    // DigiByte: Disable BumpFee tests - RBF is disabled in DigiByte
    // BumpFee(transactionView, txid1, /*expectDisabled=*/true, /*expectError=*/"not BIP 125 replaceable", /*cancel=*/false);
    // BumpFee(transactionView, txid2, /*expectDisabled=*/false, /*expectError=*/{}, /*cancel=*/true);
    // BumpFee(transactionView, txid2, /*expectDisabled=*/false, /*expectError=*/{}, /*cancel=*/false);
    // BumpFee(transactionView, txid2, /*expectDisabled=*/true, /*expectError=*/"already bumped", /*cancel=*/false);

    // Check current balance on OverviewPage
    OverviewPage overviewPage(platformStyle.get());
    overviewPage.setWalletModel(&walletModel);
    walletModel.pollBalanceChanged(); // Manual balance polling update
    QTRY_COMPARE(walletModel.getCachedBalance().balance, walletModel.wallet().getBalance());
    CompareBalance(walletModel, walletModel.wallet().getBalance(), overviewPage.findChild<QLabel*>("labelBalance"));

    // Check Request Payment button
    ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
    receiveCoinsDialog.setModel(&walletModel);
    RecentRequestsTableModel* requestTableModel = walletModel.getRecentRequestsTableModel();

    // Label input
    QLineEdit* labelInput = receiveCoinsDialog.findChild<QLineEdit*>("reqLabel");
    labelInput->setText("TEST_LABEL_1");

    // Amount input
    DigiByteAmountField* amountInput = receiveCoinsDialog.findChild<DigiByteAmountField*>("reqAmount");
    amountInput->setValue(1);

    // Message input
    QLineEdit* messageInput = receiveCoinsDialog.findChild<QLineEdit*>("reqMessage");
    messageInput->setText("TEST_MESSAGE_1");
    int initialRowCount = requestTableModel->rowCount({});
    QPushButton* requestPaymentButton = receiveCoinsDialog.findChild<QPushButton*>("receiveButton");
    requestPaymentButton->click();
    QString address;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("ReceiveRequestDialog")) {
            ReceiveRequestDialog* receiveRequestDialog = qobject_cast<ReceiveRequestDialog*>(widget);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("payment_header")->text(), QString("Payment information"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("uri_tag")->text(), QString("URI:"));
            QString uri = receiveRequestDialog->QObject::findChild<QLabel*>("uri_content")->text();
            QCOMPARE(uri.count("digibyte:"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("address_tag")->text(), QString("Address:"));
            QVERIFY(address.isEmpty());
            address = receiveRequestDialog->QObject::findChild<QLabel*>("address_content")->text();
            QVERIFY(!address.isEmpty());

            QCOMPARE(uri.count("amount=0.00000001"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_tag")->text(), QString("Amount:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_content")->text(), QString::fromStdString("0.00000001 " + CURRENCY_UNIT));

            QCOMPARE(uri.count("label=TEST_LABEL_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_tag")->text(), QString("Label:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_content")->text(), QString("TEST_LABEL_1"));

            QCOMPARE(uri.count("message=TEST_MESSAGE_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_tag")->text(), QString("Message:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_content")->text(), QString("TEST_MESSAGE_1"));
        }
    }

    // Clear button
    QPushButton* clearButton = receiveCoinsDialog.findChild<QPushButton*>("clearButton");
    clearButton->click();
    QCOMPARE(labelInput->text(), QString(""));
    QCOMPARE(amountInput->value(), CAmount(0));
    QCOMPARE(messageInput->text(), QString(""));

    // Check addition to history
    int currentRowCount = requestTableModel->rowCount({});
    QCOMPARE(currentRowCount, initialRowCount+1);

    // Check addition to wallet
    std::vector<std::string> requests = walletModel.wallet().getAddressReceiveRequests();
    QCOMPARE(requests.size(), size_t{1});
    RecentRequestEntry entry;
    DataStream{MakeUCharSpan(requests[0])} >> entry;
    QCOMPARE(entry.nVersion, int{1});
    QCOMPARE(entry.id, int64_t{1});
    QVERIFY(entry.date.isValid());
    QCOMPARE(entry.recipient.address, address);
    QCOMPARE(entry.recipient.label, QString{"TEST_LABEL_1"});
    QCOMPARE(entry.recipient.amount, CAmount{1});
    QCOMPARE(entry.recipient.message, QString{"TEST_MESSAGE_1"});
    QCOMPARE(entry.recipient.sPaymentRequest, std::string{});
    QCOMPARE(entry.recipient.authenticatedMerchant, QString{});

    // Check Remove button
    QTableView* table = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
    table->selectRow(currentRowCount-1);
    QPushButton* removeRequestButton = receiveCoinsDialog.findChild<QPushButton*>("removeRequestButton");
    removeRequestButton->click();
    QCOMPARE(requestTableModel->rowCount({}), currentRowCount-1);

    // Check removal from wallet
    QCOMPARE(walletModel.wallet().getAddressReceiveRequests().size(), size_t{0});
}

void TestGUIWatchOnly(interfaces::Node& node, TestChain100Setup& test)
{
    const std::shared_ptr<wallet::CWallet>& wallet = SetupLegacyWatchOnlyWallet(node, test);

    // Create widgets and init models
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(node, platformStyle.get());
    mini_gui.initModelForWallet(node, wallet, platformStyle.get());
    WalletModel& walletModel = *mini_gui.walletModel;
    SendCoinsDialog& sendCoinsDialog = mini_gui.sendCoinsDialog;

    // Update walletModel cached balance which will trigger an update for the 'labelBalance' QLabel.
    walletModel.pollBalanceChanged();
    QTRY_VERIFY(walletModel.getCachedBalance().watch_only.valid);
    // Check balance in send dialog
    CompareBalance(walletModel, walletModel.wallet().getBalances().watch_only_balance,
                   sendCoinsDialog.findChild<QLabel*>("labelBalance"));

    const auto watch_coins = walletModel.wallet().listCoins();
    QVERIFY(!watch_coins.empty() && !watch_coins.begin()->second.empty());
    const auto& [watch_outpoint, watch_output] = watch_coins.begin()->second.front();
    const CAmount watch_owned = walletModel.getCachedBalance().watch_only_balance;
    QVERIFY(walletModel.wallet().lockCoin(watch_outpoint, false));
    walletModel.updateTransaction();
    QTRY_COMPARE(walletModel.getCachedBalance().watch_only.available, watch_owned - watch_output.txout.nValue);
    QCOMPARE(walletModel.getCachedBalance().watch_only.other_unavailable, watch_output.txout.nValue);
    CompareBalance(walletModel, watch_owned - watch_output.txout.nValue, sendCoinsDialog.findChild<QLabel*>("labelBalance"));
    QVERIFY(walletModel.wallet().unlockCoin(watch_outpoint));
    walletModel.updateTransaction();
    QTRY_COMPARE(walletModel.getCachedBalance().watch_only.available, watch_owned);

    // Set change address
    sendCoinsDialog.getCoinControl()->destChange = GetDestinationForKey(test.coinbaseKey.GetPubKey(), OutputType::LEGACY);

    // Time to reject "save" PSBT dialog ('SendCoins' locks the main thread until the dialog receives the event).
    QTimer timer;
    timer.setInterval(500);
    QObject::connect(&timer, &QTimer::timeout, [&](){
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("QMessageBox")) {
                QMessageBox* dialog = qobject_cast<QMessageBox*>(widget);
                QAbstractButton* button = dialog->button(QMessageBox::Discard);
                button->setEnabled(true);
                button->click();
                timer.stop();
                break;
            }
        }
    });
    timer.start(500);

    // Send tx and verify PSBT copied to the clipboard.
    SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 5 * COIN, /*rbf=*/false, QMessageBox::Save);
    const std::string& psbt_string = QApplication::clipboard()->text().toStdString();
    QVERIFY(!psbt_string.empty());

    // Decode psbt
    std::optional<std::vector<unsigned char>> decoded_psbt = DecodeBase64(psbt_string);
    QVERIFY(decoded_psbt);
    PartiallySignedTransaction psbt;
    std::string err;
    QVERIFY(DecodeRawPSBT(psbt, MakeByteSpan(*decoded_psbt), err));
}

void TestGUI(interfaces::Node& node)
{
    // Set up wallet and chain with 105 blocks (5 mature blocks for spending).
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);

    // "Full" GUI tests, use descriptor wallet
    const std::shared_ptr<wallet::CWallet>& desc_wallet = SetupDescriptorsWallet(node, test);
    TestGUI(node, desc_wallet);

    // Legacy watch-only wallet test
    // Verify PSBT creation.
    TestGUIWatchOnly(node, test);
}

} // namespace

void WalletTests::walletTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        QWARN("Skipping WalletTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
              "with 'QT_QPA_PLATFORM=cocoa test_digibyte-qt' on mac, or else use a linux or windows build.");
        return;
    }
#endif
    TestGUI(m_node);
}

void WalletTests::dgbBalanceBreakdown()
{
    using namespace DigiDollar::Paymaster;
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test);
    std::unique_ptr<const PlatformStyle> style(PlatformStyle::instantiate("other"));
    MiniGUI gui(m_node, style.get());
    gui.initModelForWallet(m_node, wallet, style.get());
    auto& model = *gui.walletModel;
    OverviewPage overview(style.get());
    overview.setClientModel(gui.clientModel.get());
    overview.setWalletModel(&model);
    wallet::CCoinControl control;
    CoinControlDialog coins(control, &model, style.get());

    // Synthetic, owned taproot outputs in a wallet fixture. No transaction is
    // funded, signed or broadcast by these display checks.
    CMutableTransaction funding;
    ProviderPoolEntry entry;
    {
        LOCK(wallet->cs_wallet);
        const auto address = wallet->GetNewDestination(OutputType::BECH32M, "balance test");
        QVERIFY(address);
        funding.vin.emplace_back(COutPoint{uint256::ONE, 0});
        funding.vout.emplace_back(COIN, GetScriptForDestination(*address));
        funding.vout.emplace_back(2 * COIN, GetScriptForDestination(*address));
        QVERIFY(wallet->AddToWallet(MakeTransactionRef(funding),
            wallet::TxStateConfirmed{wallet->GetLastBlockHash(), wallet->GetLastBlockHeight(), 0}));
        entry.outpoint = COutPoint{funding.GetHash(), 0};
        entry.script_pub_key = funding.vout[0].scriptPubKey;
        entry.dgb_value = DGBSatoshis{COIN};
        entry.updated_at = 1;
    }
    const auto owned = model.wallet().getBalances();
    QVERIFY(owned.spendable.valid);
    QCOMPARE(owned.spendable.available, model.getAvailableBalance(nullptr));
    QVERIFY(owned.immature_balance > 0);
    model.updateTransaction();
    QTRY_COMPARE(model.getCachedBalance().balance, owned.balance);
    QSignalSpy ownership_changes(&model, &WalletModel::balanceChanged);
    QVERIFY(wallet::WalletBatch{wallet->GetDatabase()}.WritePaymasterProviderPool({entry}));
    QVERIFY(model.wallet().lockCoin(entry.outpoint, false)); // pool + ordinary lock must count only once
    const COutPoint locked{funding.GetHash(), 1};
    QVERIFY(model.wallet().lockCoin(locked, false));
    model.updateTransaction();
    model.pollBalanceChanged();
    QTRY_COMPARE(model.getCachedBalance().spendable.paymaster_reserved, COIN);
    auto balances = model.getCachedBalance();
    QCOMPARE(balances.balance, owned.balance);
    QCOMPARE(balances.spendable.other_unavailable, 2 * COIN);
    QCOMPARE(balances.spendable.available, owned.balance - 3 * COIN);
    QCOMPARE(balances.immature_balance, owned.immature_balance);
    CompareBalance(model, balances.spendable.available, overview.findChild<QLabel*>("labelBalance"));
    CompareBalance(model, COIN, overview.findChild<QLabel*>("labelPaymasterReserved"));
    CompareBalance(model, 2 * COIN, overview.findChild<QLabel*>("labelOtherUnavailable"));
    CompareBalance(model, owned.balance + owned.immature_balance, overview.findChild<QLabel*>("labelTotal"));
    CompareBalance(model, balances.spendable.available, gui.sendCoinsDialog.findChild<QLabel*>("labelBalance"));
    QVERIFY(coins.findChild<QLabel*>("labelWalletBalances")->text().contains("Reserved for Paymaster:"));
    QVERIFY(gui.sendCoinsDialog.findChild<QLabel*>("labelBalanceDetails")->text().contains("Other unavailable:"));
    QVERIFY(ownership_changes.isEmpty()); // Reserve-only changes must not request DD history.
    overview.setPrivacy(true);
    QVERIFY(!overview.findChild<QLabel*>("labelPaymasterReserved")->text().contains("1.00000000"));
    overview.setPrivacy(false);
    CompareBalance(model, COIN, overview.findChild<QLabel*>("labelPaymasterReserved"));

    // Every protected pool state remains reserved; terminal states do not.
    // Manual overlap remains present, so the combined unavailable total stays fixed.
    for (const auto state : {PoolEntryState::AVAILABLE, PoolEntryState::RESERVED,
                            PoolEntryState::COMMITTED, PoolEntryState::INVALIDATED,
                            PoolEntryState::SPENT, PoolEntryState::RELEASED}) {
        auto variant = entry;
        variant.state = state;
        if (state == PoolEntryState::RESERVED || state == PoolEntryState::COMMITTED) variant.reservation_id = uint256::ONE;
        QVERIFY(wallet::WalletBatch{wallet->GetDatabase()}.WritePaymasterProviderPool({variant}));
        const auto snapshot = model.wallet().getBalances();
        const bool active = state == PoolEntryState::AVAILABLE || state == PoolEntryState::RESERVED || state == PoolEntryState::COMMITTED;
        QCOMPARE(snapshot.spendable.paymaster_reserved, active ? COIN : CAmount{0});
        QCOMPARE(snapshot.spendable.other_unavailable, active ? 2 * COIN : 3 * COIN);
        QCOMPARE(snapshot.balance, owned.balance);
    }
    QVERIFY(wallet::WalletBatch{wallet->GetDatabase()}.WritePaymasterProviderPool({entry}));

    // A release without a block/transaction notification is picked up by the
    // bounded periodic refresh. The still-manual lock moves it to Other unavailable.
    QVERIFY(wallet::WalletBatch{wallet->GetDatabase()}.WritePaymasterProviderPool({}));
    QTimer refresh;
    connect(&refresh, &QTimer::timeout, &model, &WalletModel::pollBalanceChanged);
    refresh.start(50);
    QTRY_COMPARE_WITH_TIMEOUT(model.getCachedBalance().spendable.paymaster_reserved, CAmount{0}, 7000);
    QCOMPARE(model.getCachedBalance().spendable.other_unavailable, 3 * COIN);

    // A busy wallet does not block Qt or temporarily report zero funds.
    {
        LOCK(wallet->cs_wallet);
        model.updateTransaction();
        model.pollBalanceChanged();
        bool event_processed{false};
        QTimer::singleShot(0, [&] { event_processed = true; });
        QTest::qWait(100);
        QVERIFY(event_processed);
        QCOMPARE(model.getCachedBalance().balance, owned.balance);
    }
    QVERIFY(model.wallet().unlockCoin(entry.outpoint));
    QVERIFY(model.wallet().unlockCoin(locked));
    model.updateTransaction();
    QTRY_COMPARE(model.getCachedBalance().spendable.available, owned.balance);
    QVERIFY(ownership_changes.isEmpty());

    // A pool output that is pending is counted in Pending, never also Reserved.
    CMutableTransaction pending{funding};
    pending.vin[0].prevout.n = 1;
    pending.vout.resize(1);
    {
        LOCK(wallet->cs_wallet);
        QVERIFY(wallet->AddToWallet(MakeTransactionRef(pending), wallet::TxStateInMempool{}));
    }
    auto pending_entry = entry;
    pending_entry.outpoint = COutPoint{pending.GetHash(), 0};
    pending_entry.state = PoolEntryState::PENDING_SUCCESSOR;
    pending_entry.reservation_id = pending_entry.origin_commit_key = uint256::ONE;
    QVERIFY(wallet::WalletBatch{wallet->GetDatabase()}.WritePaymasterProviderPool({pending_entry}));
    balances = model.wallet().getBalances();
    QCOMPARE(balances.unconfirmed_balance, COIN);
    QCOMPARE(balances.spendable.paymaster_reserved, CAmount{0});
    QCOMPARE(balances.balance, owned.balance);

    // Corrupt safety data keeps ownership readable, but availability is unknown
    // on all three surfaces. It must not be displayed as an empty/free pool.
    QVERIFY(wallet->GetDatabase().MakeBatch()->Write(std::string{wallet::DBKeys::PAYMASTER_PROVIDER_POOL}, std::string{"corrupt"}));
    model.updateTransaction();
    QTRY_VERIFY(!model.getCachedBalance().spendable.valid);
    QCOMPARE(model.getCachedBalance().balance, owned.balance);
    QCOMPARE(overview.findChild<QLabel*>("labelBalance")->text(), QStringLiteral("Unavailable"));
    QCOMPARE(gui.sendCoinsDialog.findChild<QLabel*>("labelBalance")->text(), QStringLiteral("Unavailable"));
    QVERIFY(coins.findChild<QLabel*>("labelWalletBalances")->text().contains("Availability is unknown"));
    QVERIFY(wallet::WalletBatch{wallet->GetDatabase()}.WritePaymasterProviderPool({}));
    model.updateTransaction();
    QTRY_VERIFY(model.getCachedBalance().spendable.valid);
    refresh.stop();
}

void WalletTests::sendValidationExplainsInvalidFields()
{
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test);
    std::unique_ptr<const PlatformStyle> style(PlatformStyle::instantiate("other"));
    MiniGUI gui(m_node, style.get());
    gui.initModelForWallet(m_node, wallet, style.get());
    auto& dialog = gui.sendCoinsDialog;
    auto* entries = dialog.findChild<QVBoxLayout*>("entries");
    auto* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    QVERIFY(entry);
    auto* address = entry->findChild<QValidatedLineEdit*>("payTo");
    auto* amount = entry->findChild<DigiByteAmountField*>("payAmount");
    QVERIFY(address);
    QVERIFY(amount);
    QSignalSpy messages(&dialog, &SendCoinsDialog::message);
    QSignalSpy unlocks(gui.walletModel.get(), &WalletModel::requireUnlock);
    const size_t before = wallet->mapWallet.size();
    const QString valid_address = QString::fromStdString(EncodeDestination(PKHash()));
    const auto check = [&](const QString& input, CAmount value, const QString& expected) {
        address->setText(input);
        amount->setValue(value);
        messages.clear();
        QVERIFY(QMetaObject::invokeMethod(&dialog, "sendButtonClicked", Q_ARG(bool, false)));
        QCOMPARE(messages.count(), 1);
        QVERIFY2(messages.at(0).at(1).toString().contains(expected), qPrintable(messages.at(0).at(1).toString()));
        QVERIFY(messages.at(0).at(2).toUInt() & CClientUIInterface::MODAL);
        QCOMPARE(wallet->mapWallet.size(), before);
        QCOMPARE(unlocks.count(), 0);
    };
    check("", 0, "Pay To");
    check("not-an-address", COIN, "valid DigiByte address");
    check(valid_address, 0, "greater than zero");
    check(valid_address, 1, "too small");

    // A valid first recipient must not hide an empty second recipient.
    address->setText(valid_address);
    amount->setValue(COIN);
    QVERIFY(dialog.addEntry());
    messages.clear();
    QVERIFY(QMetaObject::invokeMethod(&dialog, "sendButtonClicked", Q_ARG(bool, false)));
    QCOMPARE(messages.count(), 1);
    QVERIFY(messages.at(0).at(1).toString().contains("Pay To"));
    QCOMPARE(wallet->mapWallet.size(), before);
    QCOMPARE(unlocks.count(), 0);
}

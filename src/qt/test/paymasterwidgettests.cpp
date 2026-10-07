// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/paymasterwidgettests.h>
#include <qt/test/digidollartestutil.h>
#include <qt/test/util.h>

#include <coins.h>
#include <consensus/digidollar.h>
#include <digidollar/scripts.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <paymaster/provider.h>
#include <paymaster/setup.h>
#include <primitives/transaction.h>
#include <qt/clientmodel.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollarstatus.h>
#include <qt/digidollartab.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/paymasterconfirmation.h>
#include <qt/paymasteramount.h>
#include <qt/paymastersendwidget.h>
#include <qt/paymasterwidget.h>
#include <qt/paymasteroperation.h>
#include <paymaster/manager.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/paymasterwallet.h>
#include <script/standard.h>
#include <rpc/server.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <validation.h>
#include <wallet/ddcoincontrol.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/paymaster_internal.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <univalue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <memory>
#include <map>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <QApplication>
#include <QAccessible>
#include <QColor>
#include <QCheckBox>
#include <QCoreApplication>
#include <QFile>
#include <QFocusEvent>
#include <QFrame>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QComboBox>
#include <QRadioButton>
#include <QTableWidget>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QStackedWidget>
#include <QListWidget>
#include <QWizard>
#include <QTimer>
#include <QThread>
#include <QTemporaryDir>
#include <QtTest/QtTestWidgets>
#include <QtWidgets/qtestsupport_widgets.h>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
using wallet::WalletContext;
using wallet::WalletRescanReserver;

using DigiDollarTest::DigiDollarMiniGUI;
using DigiDollarTest::SetupDescriptorsWallet;

namespace {

UniValue PaymasterOverviewFinance()
{
    // Deliberately no events/daily_totals: include_events=false omits them.
    UniValue result;
    if (!result.read(R"({"period":"all","history_partially_reconstructable":false,
        "period_summaries":{"all":{"service_fee_income_cents":125,"dgb_operating_cost_satoshis":20000000,"successful_transfers":5}},
        "pool_capital":{"dgb_available_satoshis":500000000,"dgb_reserved_satoshis":100000000,"dgb_pending_satoshis":50000000,
        "carrier_base_cents":400,"carrier_earned_cents":25,"carrier_withdrawable_cents":20,"pending_maintenance_transactions":1}})"))
        throw std::runtime_error("Invalid overview finance fixture");
    result.pushKV("provider_id", std::string(64, 'a'));
    return result;
}

UniValue PaymasterLiquiditySlotStatus(int ready, int pending, int missing,
                                      int target)
{
    UniValue status{UniValue::VOBJ};
    status.pushKV("ready", ready);
    status.pushKV("pending", pending);
    status.pushKV("reserved", 0);
    status.pushKV("missing", missing);
    status.pushKV("target", target);
    status.pushKV("counted_toward_target",
                  std::min(target, ready + pending));
    return status;
}

UniValue PaymasterLiquidityStatus(const std::string& state, bool configured,
                                  bool approved, int missing,
                                  int pending = 0,
                                  bool targets_satisfy_provider_policy = true)
{
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("automatic_replenishment", true);
    policy.pushKV("paid_maintenance_approved", approved);
    policy.pushKV("target_admission_dgb", 3);
    policy.pushKV("target_operational_dgb", 1);
    policy.pushKV("target_admission_carriers", 3);
    policy.pushKV("target_operational_carriers",
                  targets_satisfy_provider_policy ? 1 : 0);
    policy.pushKV("maximum_maintenance_fee_per_transaction_satoshis", 10000000);
    policy.pushKV("maximum_maintenance_fee_per_hour_satoshis", 50000000);
    policy.pushKV("maximum_maintenance_fee_per_day_satoshis", 200000000);

    UniValue result{UniValue::VOBJ};
    result.pushKV("policy_configured", configured);
    result.pushKV("targets_satisfy_provider_policy",
                  targets_satisfy_provider_policy);
    result.pushKV("maintenance_state", state);
    result.pushKV("policy", std::move(policy));
    result.pushKV("admission_dgb",
                  PaymasterLiquiditySlotStatus(3, 0, 0, 3));
    result.pushKV("operational_dgb",
                  PaymasterLiquiditySlotStatus(
                      (missing > 0 || pending > 0) ? 0 : 1,
                      pending, missing, 1));
    result.pushKV("admission_carriers",
                  PaymasterLiquiditySlotStatus(3, 0, 0, 3));
    result.pushKV("operational_carriers",
                  PaymasterLiquiditySlotStatus(
                      targets_satisfy_provider_policy ? 1 : 0, 0, 0,
                      targets_satisfy_provider_policy ? 1 : 0));
    result.pushKV("maintenance_fee_reserved_satoshis", 1000000);
    result.pushKV("maintenance_fee_planned_satoshis", pending > 0 ? 0 : 1000000);
    result.pushKV("maintenance_fee_broadcast_satoshis", pending > 0 ? 1000000 : 0);
    result.pushKV("maintenance_fee_spent_last_hour_satoshis", 2000000);
    result.pushKV("maintenance_fee_spent_last_day_satoshis", 3000000);
    result.pushKV("carrier_base_cents", 100);
    result.pushKV("carrier_withdrawable_excess_cents", 6);
    result.pushKV("readiness_errors", UniValue{UniValue::VARR});
    return result;
}

UniValue PaymasterLiquidityPoolStatus()
{
    UniValue available{UniValue::VOBJ};
    available.pushKV("state", "available");
    available.pushKV("purpose", "operational");
    available.pushKV("asset", "dd_carrier");
    available.pushKV("txid",
                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    available.pushKV("vout", 1);
    available.pushKV("dgb_satoshis", 0);
    available.pushKV("dd_cents", 106);
    available.pushKV("confirmation_height", 120);
    available.pushKV("updated_at", 1000);

    UniValue pending_carrier{UniValue::VOBJ};
    pending_carrier.pushKV("state", "pending_successor");
    pending_carrier.pushKV("purpose", "operational");
    pending_carrier.pushKV("asset", "dd_carrier");
    pending_carrier.pushKV(
        "txid",
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    pending_carrier.pushKV("vout", 2);
    pending_carrier.pushKV("dgb_satoshis", 0);
    pending_carrier.pushKV("dd_cents", 103);
    pending_carrier.pushKV("confirmation_height", 0);
    pending_carrier.pushKV("updated_at", 1001);

    UniValue pending_dgb{UniValue::VOBJ};
    pending_dgb.pushKV("state", "pending_successor");
    pending_dgb.pushKV("purpose", "operational");
    pending_dgb.pushKV("asset", "dgb");
    pending_dgb.pushKV(
        "txid",
        "2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    pending_dgb.pushKV("vout", 3);
    pending_dgb.pushKV("dgb_satoshis", 10000000);
    pending_dgb.pushKV("dd_cents", 0);
    pending_dgb.pushKV("confirmation_height", 0);
    pending_dgb.pushKV("updated_at", 1002);

    UniValue pool{UniValue::VARR};
    pool.push_back(std::move(available));
    pool.push_back(std::move(pending_carrier));
    pool.push_back(std::move(pending_dgb));
    UniValue result{UniValue::VOBJ};
    result.pushKV("pool", std::move(pool));
    return result;
}

UniValue PaymasterClientSafetyStatus()
{
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("maximum_service_fee_per_transaction_cents", 100);
    policy.pushKV("maximum_service_fee_per_day_cents", 1000);

    UniValue result{UniValue::VOBJ};
    result.pushKV("configured", true);
    result.pushKV("policy", std::move(policy));
    result.pushKV("active_reservations", 0);
    result.pushKV("reserved_service_fee_cents", 0);
    result.pushKV("spent_service_fee_last_day_cents", 0);
    result.pushKV("available_service_fee_today_cents", 1000);
    return result;
}

UniValue PaymasterOffer(const std::string& display_name,
                        const std::string& provider_id,
                        const std::string& funding_model,
                        int64_t service_fee_cents,
                        int64_t payment_cents,
                        bool established_reputation,
                        int64_t expires_at,
                        int fee_rate_bps = 50,
                        int64_t maximum_user_paid_service_fee_cents = 0)
{
    UniValue offer{UniValue::VOBJ};
    offer.pushKV("display_name", display_name);
    offer.pushKV("provider_id", provider_id);
    offer.pushKV("offer_id", "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    offer.pushKV("policy_hash", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    offer.pushKV("funding_model", funding_model);
    offer.pushKV("fee_rate_bps",
                 funding_model == "user_paid" ? fee_rate_bps : 0);
    offer.pushKV("maximum_user_paid_service_fee_cents",
                 funding_model == "user_paid"
                     ? maximum_user_paid_service_fee_cents
                     : 0);
    offer.pushKV("service_fee_cents", service_fee_cents);
    offer.pushKV("payment_cents", payment_cents);
    offer.pushKV("user_total_cents", payment_cents + service_fee_cents);
    offer.pushKV("subtract_paymaster_fee_from_amount", false);
    offer.pushKV("reputation_sufficient_data", established_reputation);
    offer.pushKV("success_rate_basis_points", established_reputation ? 9875 : 0);
    offer.pushKV("expires_at", expires_at);
    return offer;
}

UniValue PaymasterPublicOffers(int64_t amount_cents = 325)
{
    UniValue offers{UniValue::VARR};
    offers.push_back(PaymasterOffer("Provider", std::string(64, 'b'), "user_paid", 2,
                                   amount_cents, false, QDateTime::currentSecsSinceEpoch() + 600));
    return offers;
}

UniValue EmptyPaymasterSessionList()
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("active_only", true);
    result.pushKV("count", 0);
    result.pushKV("next_cursor", UniValue{});
    result.pushKV("sessions", UniValue{UniValue::VARR});
    return result;
}

UniValue PaymasterSessionView(const std::string& state,
                              const std::string& artifact,
                              const std::string& attempt_state,
                              bool include_recipient = true,
                              int64_t recovery_expires_at = 2000000000,
                              bool recovery_expired = false)
{
    const std::string provider(64, 'b');
    UniValue session{UniValue::VOBJ};
    session.pushKV("request_id", "00000000-0000-4000-8000-000000000001");
    session.pushKV("session_id", std::string(64, '1'));
    session.pushKV("session_state", state);
    session.pushKV("payment_confirmed", state == "CONFIRMED");
    session.pushKV("status", state == "CONFIRMED" ? "success" :
                   state == "CANCELED_SAFE" ? "canceled" :
                   state == "FAILED" ? "failed" :
                   state == "CONFLICTED" ? "conflicted" : "pending");
    session.pushKV("pending_phase", "NONE");
    session.pushKV("broadcast_state",
                   state == "MEMPOOL" ? "accepted_mempool" :
                   state == "CONFIRMED" || state == "CANCELED_SAFE"
                       ? "confirmed" : "not_attempted");
    session.pushKV("confirmation_state",
                   state == "CONFIRMED" ? "payment_confirmed" :
                   state == "CANCELED_SAFE" ? "recovery_confirmed" :
                   state == "CONFLICTED" ? "conflicted" : "unconfirmed");
    session.pushKV("final", state == "CONFIRMED" ||
                                state == "CANCELED_SAFE" ||
                                state == "FAILED" || state == "CONFLICTED");
    if (include_recipient || !attempt_state.empty()) {
        session.pushKV("provider_id", provider);
        session.pushKV("policy_hash", std::string(64, 'a'));
    }
    session.pushKV("privacy_profile", "standard");
    if (include_recipient) {
        session.pushKV("to_address",
                       "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
        session.pushKV("requested_amount_cents", 325);
        session.pushKV("payment_cents", 325);
        session.pushKV("service_fee_cents", 2);
        session.pushKV("user_total_cents", 327);
    }
    session.pushKV("expires_at", int64_t{2000000000});
    if (state == "MEMPOOL" || state == "CONFIRMED") {
        session.pushKV("txid", std::string(64, 'e'));
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("action", "refresh");
    result.pushKV("session", std::move(session));
    if (attempt_state.empty()) {
        result.pushKV("attempt", UniValue{UniValue::VNULL});
    } else {
        UniValue attempt{UniValue::VOBJ};
        attempt.pushKV("attempt_state", attempt_state);
        attempt.pushKV("provider_id", provider);
        attempt.pushKV("privacy_profile", "standard");
        result.pushKV("attempt", std::move(attempt));
    }
    result.pushKV("artifact", artifact);
    if (artifact == "alternative_recovery") {
        UniValue recovery{UniValue::VOBJ};
        recovery.pushKV("expires_at", recovery_expires_at);
        recovery.pushKV("expired", recovery_expired);
        result.pushKV("recovery", std::move(recovery));
    } else {
        result.pushKV("recovery", UniValue{UniValue::VNULL});
    }
    result.pushKV("requires_attention",
                  state == "FAILED" || state == "CONFLICTED");
    UniValue actions{UniValue::VARR};
    actions.push_back("refresh");
    if (artifact == "none" && state != "FAILED" &&
        state != "CONFLICTED") {
        actions.push_back("resume");
        actions.push_back("abandon_unsigned");
        if (!attempt_state.empty()) actions.push_back("fallback");
    }
    if ((artifact == "user_psbt" || artifact == "final_transaction") &&
        state != "CONFIRMED") {
        actions.push_back("retry_same");
        actions.push_back("cancel_to_self");
    }
    if (artifact == "alternative_recovery") {
        actions.push_back("recover");
        actions.push_back("cancel_to_self");
    }
    result.pushKV("allowed_actions", std::move(actions));
    if (state == "MEMPOOL" || state == "CONFIRMED") {
        result.pushKV("result_status", "final_committed");
        result.pushKV("result_sequence", 1);
    } else {
        result.pushKV("result_status", UniValue{UniValue::VNULL});
        result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    }
    return result;
}

UniValue PaymasterAuthorizationResult(bool authorization_required,
                                      const std::string& commitment,
                                      const std::string& state,
                                      const std::string& artifact)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", "00000000-0000-4000-8000-000000000001");
    result.pushKV("session_id", std::string(64, '1'));
    result.pushKV("session_state", state);
    result.pushKV("status", "pending");
    result.pushKV("payment_confirmed", false);
    result.pushKV("pending_phase", "NONE");
    result.pushKV("broadcast_state", "not_attempted");
    result.pushKV("confirmation_state", "unconfirmed");
    result.pushKV("provider_id", std::string(64, 'b'));
    result.pushKV("offer_id", std::string(64, 'd'));
    result.pushKV("policy_hash", std::string(64, 'a'));
    result.pushKV("funding_model", "user_paid");
    result.pushKV("to_address",
                  "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    result.pushKV("privacy_profile", "standard");
    result.pushKV("artifact", artifact);
    result.pushKV("payment_cents", 325);
    result.pushKV("service_fee_cents", 2);
    result.pushKV("user_total_cents", 327);
    result.pushKV("expires_at", int64_t{2000000000});
    result.pushKV("result_status", UniValue{UniValue::VNULL});
    result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    result.pushKV("authorization_required", authorization_required);
    result.pushKV("authorization_commitment", commitment);
    return result;
}

} // namespace

void PaymasterWidgetTests::paymasterLiquidityPolicyDefaultsAndApprovalGuard()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QStackedWidget* paymaster_tabs = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterOperatorPages"));
    QWidget* liquidity_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterLiquidityPage"));
    QVERIFY(paymaster_tabs != nullptr);
    QVERIFY(liquidity_page != nullptr);
    tab.findChild<QStackedWidget*>("paymasterSettingsPages")->setEnabled(true);

    const int liquidity_index = paymaster_tabs->indexOf(liquidity_page);
    QVERIFY(liquidity_index >= 0);
    paymaster_tabs->widget(liquidity_index)->setEnabled(true);
    QVERIFY(liquidity_page->isEnabled());

    QCheckBox* automatic = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterAutomaticReplenishment"));
    QCheckBox* approved = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QLineEdit* per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLineEdit* per_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerHour"));
    QLineEdit* per_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerDay"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QSpinBox* operational_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalDgbSlots"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* maintenance_fee_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLabel* status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"));
    QLabel* target_save_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityTargetSaveStatus"));
    QPushButton* save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QPushButton* restore = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterRestoreLiquidityDefaults"));
    QPushButton* advanced = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterAdvancedLiquidityToggle"));
    QPushButton* primary_save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicyPrimary"));

    QVERIFY(automatic != nullptr);
    QVERIFY(approved != nullptr);
    QVERIFY(per_transaction != nullptr);
    QVERIFY(per_hour != nullptr);
    QVERIFY(per_day != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(operational_dgb != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(target_save_status != nullptr);
    QVERIFY(save != nullptr);
    QVERIFY(restore != nullptr);
    QVERIFY(advanced != nullptr);
    QVERIFY(primary_save != nullptr);

    auto* settings = tab.findChild<QStackedWidget*>("paymasterSettingsPages");
    auto* automation = tab.findChild<QWidget*>("paymasterAutomationPage");
    QVERIFY(settings && automation);
    paymaster_tabs->setCurrentWidget(settings);
    settings->setCurrentWidget(automation);
    paymaster_tabs->setCurrentWidget(liquidity_page);
    QVERIFY(primary_save->isVisibleTo(liquidity_page));
    QCOMPARE(primary_save->text(), QStringLiteral("Save liquidity settings"));
    paymaster_tabs->setCurrentIndex(liquidity_index);
    advanced->setChecked(true);
    QVERIFY(save->isHidden()); // Only the combined targets/costs form remains visible.
    QVERIFY(target_save_status->text().contains(
        QStringLiteral("Not saved yet")));

    QVERIFY(automatic->isChecked());
    QVERIFY(!approved->isChecked());
    QCOMPARE(admission_dgb->value(), 3);
    QCOMPARE(operational_dgb->value(), 1);
    QCOMPARE(admission_carriers->value(), 3);
    QCOMPARE(operational_carriers->value(), 1);
    const qint64 transaction_limit = qRound64(per_transaction->text().toDouble() * 100000000);
    const qint64 hourly_limit = qRound64(per_hour->text().toDouble() * 100000000);
    const qint64 daily_limit = qRound64(per_day->text().toDouble() * 100000000);
    QVERIFY(transaction_limit > 0);
    QVERIFY(hourly_limit >= transaction_limit);
    QVERIFY(daily_limit >= hourly_limit);

    approved->setChecked(true);
    automatic->setChecked(false);
    admission_dgb->setValue(8);
    per_transaction->setText(QStringLiteral("42"));
    QVERIFY(status->text().contains(QStringLiteral("Unsaved")));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Not saved")));
    restore->click();
    QVERIFY(automatic->isChecked());
    QVERIFY(!approved->isChecked());
    QCOMPARE(admission_dgb->value(), 3);
    QCOMPARE(per_transaction->text(), QStringLiteral("0.50000000"));
    QVERIFY(status->text().contains(QStringLiteral("Paid refill is disabled")));
    QVERIFY(save->toolTip().contains(QStringLiteral("does not start a stopped provider")));
    QVERIFY(primary_save->toolTip().contains(
        QStringLiteral("may immediately refill")));

    // The one-time setup draft must use the same proposal after a wallet
    // change, without carrying the previous wallet's draft or fee approval.
    auto* preparation_fee = tab.findChild<QLineEdit*>("paymasterPoolPreparationFee");
    QVERIFY(preparation_fee);
    QCOMPARE(preparation_fee->text(), QStringLiteral("0.50000000"));
    preparation_fee->setText(QStringLiteral("0.12500000"));
    tab.setWalletModel(nullptr);
    QCOMPARE(preparation_fee->text(), QStringLiteral("0.50000000"));
    QVERIFY(!approved->isChecked());
}

void PaymasterWidgetTests::paymasterLiquidityPolicySavePersistsVisibleValues()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QStringList commands;
    std::vector<UniValue> parameters;
    bool malformed_ack{false};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterliquiditypolicy") {
                if (malformed_ack) return UniValue{UniValue::VOBJ};
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });

    QStackedWidget* operator_tabs = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterOperatorPages"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->widget(index)->setEnabled(true);
        operator_tabs->widget(index)->setProperty("paymasterPageAvailable", true);
    }

    QCheckBox* automatic = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterAutomaticReplenishment"));
    QCheckBox* approved = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QSpinBox* operational_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalDgbSlots"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QLineEdit* per_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerHour"));
    QLineEdit* per_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerDay"));
    QPushButton* save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QPushButton* primary_save = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicyPrimary"));
    QLabel* status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"));
    QLabel* target_save_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityTargetSaveStatus"));
    QVERIFY(automatic != nullptr);
    QVERIFY(approved != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(operational_dgb != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(per_transaction != nullptr);
    QVERIFY(per_hour != nullptr);
    QVERIFY(per_day != nullptr);
    QVERIFY(save != nullptr);
    QVERIFY(primary_save != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(target_save_status != nullptr);

    automatic->setChecked(true);
    // Keeping paid maintenance disabled avoids a modal approval dialog while
    // still verifying that all displayed targets and finite limits are sent.
    approved->setChecked(false);
    admission_dgb->setValue(5);
    operational_dgb->setValue(2);
    admission_carriers->setValue(4);
    operational_carriers->setValue(2);
    per_transaction->setText(QStringLiteral("0.12000000"));
    per_hour->setText(QStringLiteral("0.60000000"));
    per_day->setText(QStringLiteral("2.40000000"));
    primary_save->click();

    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterliquiditypolicy"));
    QVERIFY(parameters.at(0).isArray());
    const UniValue& policy = parameters.at(0)[0];
    QCOMPARE(policy.find_value("automatic_replenishment").get_bool(), true);
    QCOMPARE(policy.find_value("paid_maintenance_approved").get_bool(), false);
    QCOMPARE(policy.find_value("target_admission_dgb").getInt<int>(), 5);
    QCOMPARE(policy.find_value("target_operational_dgb").getInt<int>(), 2);
    QCOMPARE(policy.find_value("target_admission_carriers").getInt<int>(), 4);
    QCOMPARE(policy.find_value("target_operational_carriers").getInt<int>(), 2);
    QCOMPARE(policy.find_value(
                 "maximum_maintenance_fee_per_transaction_satoshis")
                 .getInt<qint64>(),
             12000000);
    QCOMPARE(policy.find_value("maximum_maintenance_fee_per_hour_satoshis")
                 .getInt<qint64>(),
             60000000);
    QCOMPARE(policy.find_value("maximum_maintenance_fee_per_day_satoshis")
                 .getInt<qint64>(),
             240000000);
    QVERIFY(status->text().contains(QStringLiteral("saved successfully")));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Saved")));
    QVERIFY(commands.contains(QStringLiteral("getpaymasteroperatorinfo")));

    // A transport-level success is not a persistence acknowledgement. Keep
    // the edit dirty when Core does not echo the complete canonical policy.
    malformed_ack = true;
    admission_dgb->setValue(6);
    auto* capital_page = tab.findChild<QWidget*>("paymasterLiquidityPage");
    capital_page->setEnabled(true);
    capital_page->setProperty("paymasterPageAvailable", true);
    primary_save->click();
    QVERIFY(status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    QVERIFY(target_save_status->text().contains(QStringLiteral("Not saved")));
}

void PaymasterWidgetTests::paymasterLiquidityMaintenanceStatesAreReadable()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    // Maintenance now lives on Operating capital. This isolated presentation
    // fixture opens that page without invoking the unrelated setup wizard.
    auto* operator_tabs = tab.findChild<QStackedWidget*>(QStringLiteral("paymasterOperatorPages"));
    auto* capital_page = tab.findChild<QWidget*>(QStringLiteral("paymasterLiquidityPage"));
    QVERIFY(operator_tabs && capital_page);
    tab.findChild<QStackedWidget*>("paymasterSettingsPages")->setEnabled(true);

    operator_tabs->widget(operator_tabs->indexOf(capital_page))->setEnabled(true);

    QLabel* state = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceState"));
    QLabel* next_step = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceNextStep"));
    QLabel* cost = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityMaintenanceCost"));
    QLabel* slot_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquiditySlotStatus"));
    QLabel* budget = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityBudgetStatus"));
    QPushButton* approve = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterApproveLiquidityMaintenance"));
    QGroupBox* maintenance_card = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterLiquidityMaintenanceCard"));
    QSpinBox* admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QLineEdit* maintenance_fee_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterMaintenanceFeePerTransaction"));
    QCheckBox* paid_maintenance = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterPaidMaintenanceApproved"));
    QPushButton* maintenance_limits = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterMaintenanceLimitsToggle"));
    QPushButton* save_policy = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSaveLiquidityPolicy"));
    QVERIFY(state != nullptr);
    QVERIFY(next_step != nullptr);
    QVERIFY(cost != nullptr);
    QVERIFY(slot_status != nullptr);
    QVERIFY(budget != nullptr);
    QVERIFY(approve != nullptr);
    QVERIFY(maintenance_card != nullptr);
    QVERIFY(admission_carriers != nullptr);
    QVERIFY(operational_carriers != nullptr);
    QVERIFY(maintenance_fee_per_transaction != nullptr);
    QVERIFY(paid_maintenance != nullptr);
    QVERIFY(maintenance_limits != nullptr);
    QVERIFY(save_policy != nullptr);

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_target_configuration", true, true, 0, 0,
        /*targets_satisfy_provider_policy=*/false));
    QVERIFY(state->text().contains(
        QStringLiteral("cannot make this offer ready")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("one operational DigiDollar carrier")));
    QVERIFY(cost->text().contains(
        QStringLiteral("saved operational carrier target is zero")));
    QVERIFY(approve->text().contains(
        QStringLiteral("required liquidity targets")));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));

    // Repairing an old zero-carrier policy is one visible, reviewable save
    // step: load the minimum targets, expose the finite fee limits and wait
    // for the operator to use the explicit save/approval action.
    int refill_changes{0};
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            if (command == "setpaymasterliquiditypolicy" || command == "preparepaymasterpool") ++refill_changes;
            return UniValue{UniValue::VOBJ};
        });
    admission_carriers->setValue(0);
    operational_carriers->setValue(0);
    paid_maintenance->setChecked(false);
    approve->click();
    auto* settings = tab.findChild<QStackedWidget*>("paymasterSettingsPages");
    auto* automation = tab.findChild<QWidget*>("paymasterAutomationPage");
    QVERIFY(settings && automation);
    QCOMPARE(operator_tabs->currentWidget(), capital_page);
    QVERIFY(tab.findChild<QLabel*>("paymasterLiquidityPolicyStatus")->text().contains(
        QStringLiteral("DigiDollar 3 / 1")));
    QCOMPARE(admission_carriers->value(), 3);
    QCOMPARE(operational_carriers->value(), 1);
    QVERIFY(paid_maintenance->isChecked());
    QVERIFY(maintenance_limits->isChecked());
    QCOMPARE(save_policy->text(),
             QStringLiteral("Save targets and approve refill…"));
    QVERIFY(save_policy->isEnabledTo(save_policy->parentWidget()));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_maintenance_approval", true, false, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("targets are saved"), Qt::CaseInsensitive));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No target update is needed")));
    QVERIFY(cost->text().contains(QStringLiteral("not approved")));
    QVERIFY(approve->text().contains(
        QStringLiteral("bounded refill costs")));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("waiting for provider start")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("Start the provider")));
    QVERIFY(cost->text().contains(QStringLiteral("approved within finite limits")));
    QVERIFY(slot_status->text().contains(
        QStringLiteral("Operational DGB: 0 ready")));
    QVERIFY(budget->text().contains(QStringLiteral("reserved: 0.01000000 DGB")));
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("waiting"));

    // A stale Regtest tip prevents creation of the maintenance transaction.
    // The UI must not claim that replenishment is already happening while the
    // backend still reports zero pending slots.
    UniValue node_wait{UniValue::VOBJ};
    node_wait.pushKV("wallet_eligible", true);
    node_wait.pushKV("enabled", true);
    node_wait.pushKV("ready", false);
    node_wait.pushKV("wallet_locked", false);
    UniValue node_wait_errors{UniValue::VARR};
    node_wait_errors.push_back("PAYMASTER_NODE_NOT_READY");
    node_wait.pushKV("readiness_errors", std::move(node_wait_errors));
    tab.setPaymasterReadinessStatusForTesting(node_wait);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(
        QStringLiteral("waiting for a fresh Regtest block")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No refill transaction has been created yet")));
    QVERIFY(cost->text().contains(QStringLiteral("1 missing and 0 pending")));

    // The provider service may be running while Core safely rejects creation
    // of a refill whose estimated network fee exceeds the explicitly approved
    // per-transaction cap. This is an operator action, not an in-progress
    // transaction: zero pending slots must remain visible and the review
    // action must lead directly to the finite maintenance limits.
    UniValue fee_exceeded{UniValue::VOBJ};
    fee_exceeded.pushKV("wallet_eligible", true);
    fee_exceeded.pushKV("enabled", true);
    fee_exceeded.pushKV("ready", false);
    fee_exceeded.pushKV("running", true);
    fee_exceeded.pushKV("wallet_locked", false);
    fee_exceeded.pushKV("service_state", "replenishing_liquidity");
    fee_exceeded.pushKV("last_service_error",
                        "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
    UniValue fee_errors{UniValue::VARR};
    fee_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    fee_exceeded.pushKV("readiness_errors", std::move(fee_errors));
    tab.setPaymasterReadinessStatusForTesting(fee_exceeded);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(state->text().contains(QStringLiteral("refill paused"),
                                   Qt::CaseInsensitive));
    QVERIFY(next_step->text().contains(QStringLiteral("0.10000000 DGB")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("No transaction was created")));
    QVERIFY(cost->text().contains(QStringLiteral("1 missing and 0 pending")));
    QCOMPARE(approve->text(), QStringLiteral("Review refill cost limit…"));
    QVERIFY(!approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("action"));
    const QString reviewed_fee = maintenance_fee_per_transaction->text();
    approve->click();
    QVERIFY(maintenance_limits->isChecked());
    // Opening review does not increase a limit, grant authority or prepare
    // funds. Editing the form still needs a separate save and confirmation.
    QCOMPARE(maintenance_fee_per_transaction->text(), reviewed_fee);
    QCOMPARE(refill_changes, 0);
    QVERIFY(tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityPolicyStatus"))->text().contains(
            QStringLiteral("no transaction has been created"),
            Qt::CaseInsensitive));

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_liquidity_confirmation", true, true, 0, 1));
    QVERIFY(state->text().contains(QStringLiteral("waiting for confirmation")));
    QVERIFY(next_step->text().contains(QStringLiteral("1 pending slot")));
    QVERIFY(cost->text().contains(
        QStringLiteral("Fees of unconfirmed transactions: 0.01000000 DGB")));
    QVERIFY(cost->text().contains(
        QStringLiteral("Confirmed maintenance cost: 0.02000000 DGB")));

    UniValue refill_ready{UniValue::VOBJ};
    refill_ready.pushKV("wallet_eligible", true);
    refill_ready.pushKV("enabled", true);
    refill_ready.pushKV("ready", true);
    refill_ready.pushKV("running", true);
    refill_ready.pushKV("wallet_locked", false);
    refill_ready.pushKV("service_state", "active");
    refill_ready.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(refill_ready);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "ready", true, true, 0));
    QVERIFY(state->text().contains(QStringLiteral("targets are satisfied")));
    QVERIFY(next_step->text().contains(QStringLiteral("reused automatically")));
    QVERIFY(cost->text().contains(
        QStringLiteral("0.02000000 DGB in the rolling hour")));
    QVERIFY(cost->text().contains(
        QStringLiteral("0.03000000 DGB in the rolling day")));
    QVERIFY(approve->isHidden());
    QCOMPARE(maintenance_card->property("statusKind").toString(),
             QStringLiteral("ready"));
}

void PaymasterWidgetTests::paymasterExternalReadinessIsSeparatedFromConfiguration()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStackedWidget* operator_tabs = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterOperatorPages"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->widget(index)->setEnabled(true);
        operator_tabs->widget(index)->setProperty("paymasterPageAvailable", true);
    }

    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QLabel* next_step = tab.findChild<QLabel*>(
        QStringLiteral("paymasterNextStep"));
    QLabel* operation = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOverviewOperationStatus"));
    QLabel* liquidity = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOverviewLiquidityStatus"));
    QPushButton* liquidity_action = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterOverviewLiquidityAction"));
    QPushButton* operation_action = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterOverviewOperationAction"));
    QLabel* blockchain = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalBlockchainStatus"));
    QLabel* txindex = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalTxIndexStatus"));
    QLabel* broadcast = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalBroadcastStatus"));
    QLabel* activation = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalActivationStatus"));
    QLabel* oracle = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOracleStatus"));
    QLabel* oracle_note = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOracleNote"));
    QGroupBox* prerequisites = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterOverviewNextStep"));
    QLabel* other_requirement = tab.findChild<QLabel*>(
        QStringLiteral("paymasterExternalOtherStatus"));
    QPushButton* wizard = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterSetupWizard"));
    QLabel* provider_id = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOfferIdentityId"));
    QVERIFY(provider_status != nullptr);
    QVERIFY(next_step != nullptr);
    QVERIFY(operation != nullptr);
    QVERIFY(liquidity != nullptr);
    QVERIFY(liquidity_action != nullptr);
    QVERIFY(operation_action != nullptr);
    QVERIFY(blockchain != nullptr);
    QVERIFY(txindex != nullptr);
    QVERIFY(broadcast != nullptr);
    QVERIFY(activation != nullptr);
    QVERIFY(oracle != nullptr);
    QVERIFY(oracle_note != nullptr);
    QVERIFY(prerequisites != nullptr);
    QVERIFY(other_requirement != nullptr);
    QVERIFY(wizard != nullptr);
    QVERIFY(provider_id != nullptr);

    UniValue waiting{UniValue::VOBJ};
    waiting.pushKV("wallet_eligible", true);
    waiting.pushKV("enabled", true);
    waiting.pushKV("ready", false);
    waiting.pushKV("wallet_locked", false);
    const QString full_provider_id{
        QStringLiteral("371aaaaeea392e01f786ca55030708320e0f85cfed0de0ffcc820faa14d9af3d")};
    waiting.pushKV("provider_id", full_provider_id.toStdString());
    UniValue waiting_errors{UniValue::VARR};
    waiting_errors.push_back("PAYMASTER_NODE_NOT_READY");
    waiting.pushKV("readiness_errors", std::move(waiting_errors));
    waiting.pushKV("oracle_price_micro_usd", 0);
    tab.setPaymasterReadinessStatusForTesting(waiting);

    QVERIFY(provider_status->text().contains(
        QStringLiteral("Provider fully configured")));
    QVERIFY(!provider_status->text().contains(QStringLiteral("Setup incomplete")));
    QVERIFY(next_step->text().contains(
        QStringLiteral("Waiting for blockchain synchronization")));
    QVERIFY(operation->text().contains(QStringLiteral("Waiting")));
    QVERIFY(operation->text().contains(QStringLiteral("Regtest block")));
    QVERIFY(blockchain->text().contains(QStringLiteral("Waiting")));
    QVERIFY(txindex->text().contains(QStringLiteral("Ready")));
    QVERIFY(broadcast->text().contains(QStringLiteral("Waiting")));
    QVERIFY(activation->text().contains(QStringLiteral("Ready")));
    QVERIFY(oracle->text().contains(QStringLiteral("Not available yet")));
    QVERIFY(oracle_note->text().contains(
        QStringLiteral("Paymaster transfers do not require an Oracle price")));
    QVERIFY(!oracle_note->isHidden());
    QVERIFY(wizard->text().contains(QStringLiteral("Review setup")));
    QVERIFY(!operation_action->text().startsWith(QStringLiteral("Start")));
    QCOMPARE(provider_id->text(), full_provider_id);
    QVERIFY(provider_id->wordWrap());
    QVERIFY(provider_id->textInteractionFlags().testFlag(
        Qt::TextSelectableByMouse));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("waiting"));
    QVERIFY(other_requirement->isHidden());

    // A transient inventory shortage and a stale Regtest tip must not be
    // presented as unsaved provider settings. The Liquidity card owns the
    // refill action, while Operations explains that a fresh block is needed.
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_maintenance_approval", true, false, 1));
    UniValue stale_with_missing_slot{UniValue::VOBJ};
    stale_with_missing_slot.pushKV("wallet_eligible", true);
    stale_with_missing_slot.pushKV("enabled", true);
    stale_with_missing_slot.pushKV("ready", false);
    stale_with_missing_slot.pushKV("wallet_locked", false);
    UniValue stale_errors{UniValue::VARR};
    stale_errors.push_back("PAYMASTER_NODE_NOT_READY");
    stale_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    stale_with_missing_slot.pushKV("readiness_errors", std::move(stale_errors));
    stale_with_missing_slot.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(stale_with_missing_slot);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("fully configured")));
    QVERIFY(operation->text().contains(QStringLiteral("Regtest block")));
    QVERIFY(!operation->text().contains(
        QStringLiteral("complete the remaining setup")));
    QVERIFY(other_requirement->isHidden());

    UniValue ready{UniValue::VOBJ};
    ready.pushKV("wallet_eligible", true);
    ready.pushKV("enabled", true);
    ready.pushKV("ready", true);
    ready.pushKV("wallet_locked", false);
    ready.pushKV("readiness_errors", UniValue{UniValue::VARR});
    ready.pushKV("oracle_price_micro_usd", 0);
    tab.setPaymasterReadinessStatusForTesting(ready);
    QVERIFY(oracle->text().contains(QStringLiteral("Not available yet")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));

    // Once external readiness is green and bounded maintenance is approved,
    // a stopped provider must be presented with the actual next action. It is
    // not still being configured and it cannot prepare a refill while stopped.
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    UniValue stopped_with_missing_slot{UniValue::VOBJ};
    stopped_with_missing_slot.pushKV("wallet_eligible", true);
    stopped_with_missing_slot.pushKV("enabled", true);
    stopped_with_missing_slot.pushKV("ready", false);
    stopped_with_missing_slot.pushKV("running", false);
    stopped_with_missing_slot.pushKV("wallet_locked", false);
    stopped_with_missing_slot.pushKV("service_state", "stopped");
    UniValue stopped_errors{UniValue::VARR};
    stopped_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    stopped_with_missing_slot.pushKV("readiness_errors",
                                     std::move(stopped_errors));
    stopped_with_missing_slot.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(stopped_with_missing_slot);
    QVERIFY(next_step->text().contains(
        QStringLiteral("All local prerequisites are ready")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(operation->text().contains(
        QStringLiteral("start the provider to restore missing liquidity")));
    QCOMPARE(operation_action->text(),
             QStringLiteral("Review liquidity"));

    // The capital action opens the guarded refill/start confirmation. The
    // technical operation card only navigates to liquidity diagnostics. Qt 5.15
    // minimal/offscreen can crash in QMessageBox::showEvent, so headless runs
    // retain the state/action assertions above without opening a dialog.
    const bool native_dialogs_available =
        QApplication::platformName() != QLatin1String("minimal") &&
        QApplication::platformName() != QLatin1String("offscreen");
    if (native_dialogs_available) {
        const auto verify_start_action = [&](QPushButton* action) {
            bool confirmation_seen{false};
            QTimer::singleShot(0, [&] {
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    auto* confirmation = qobject_cast<QMessageBox*>(widget);
                    if (!confirmation ||
                        confirmation->windowTitle() !=
                            QLatin1String("Start provider")) {
                        continue;
                    }
                    confirmation_seen = true;
                    confirmation->button(QMessageBox::Cancel)->click();
                    break;
                }
            });
            const int overview_index = operator_tabs->currentIndex();
            action->click();
            QVERIFY(confirmation_seen);
            QCOMPARE(operator_tabs->currentIndex(), overview_index);
        };
        verify_start_action(liquidity_action);
        operation_action->click();
        QCOMPARE(operator_tabs->currentWidget()->objectName(), QStringLiteral("paymasterLiquidityPage"));
        operator_tabs->setCurrentIndex(0);
    }

    // A running provider can still pause before creating a maintenance
    // transaction when its estimated fee exceeds the operator-approved cap.
    // Operation must stop claiming that a refill is being prepared and route
    // both cards to the cost-limit review instead.
    UniValue fee_limited{UniValue::VOBJ};
    fee_limited.pushKV("wallet_eligible", true);
    fee_limited.pushKV("enabled", true);
    fee_limited.pushKV("ready", false);
    fee_limited.pushKV("running", true);
    fee_limited.pushKV("wallet_locked", false);
    fee_limited.pushKV("service_state", "replenishing_liquidity");
    fee_limited.pushKV("last_service_error",
                       "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
    UniValue fee_limited_errors{UniValue::VARR};
    fee_limited_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    fee_limited.pushKV("readiness_errors",
                       std::move(fee_limited_errors));
    tab.setPaymasterReadinessStatusForTesting(fee_limited);
    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "replenishing_liquidity", true, true, 1));
    QVERIFY(liquidity->text().contains(
        QStringLiteral("cost exceeds the approved limit")));
    QVERIFY(operation->text().contains(
        QStringLiteral("exceeds the approved cost limit")));
    QCOMPARE(liquidity_action->text(),
             QStringLiteral("Review refill cost limit"));
    QCOMPARE(operation_action->text(),
             QStringLiteral("Review refill cost limit"));

    UniValue activation_wait{UniValue::VOBJ};
    activation_wait.pushKV("wallet_eligible", true);
    activation_wait.pushKV("enabled", true);
    activation_wait.pushKV("ready", false);
    activation_wait.pushKV("wallet_locked", false);
    UniValue activation_errors{UniValue::VARR};
    activation_errors.push_back("PAYMASTER_NODE_NOT_READY");
    activation_wait.pushKV("readiness_errors", std::move(activation_errors));
    activation_wait.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(activation_wait);
    QVERIFY(!operation_action->text().startsWith(QStringLiteral("Start")));
    QVERIFY(oracle->text().contains(QStringLiteral("0.006500")));

    UniValue missing_policy{UniValue::VOBJ};
    missing_policy.pushKV("wallet_eligible", true);
    missing_policy.pushKV("enabled", true);
    missing_policy.pushKV("ready", false);
    missing_policy.pushKV("wallet_locked", false);
    UniValue setup_errors{UniValue::VARR};
    setup_errors.push_back("PAYMASTER_POLICY_NOT_FOUND");
    missing_policy.pushKV("readiness_errors", std::move(setup_errors));
    missing_policy.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(missing_policy);
    QVERIFY(provider_status->text().contains(QStringLiteral("Setup incomplete")));
    QVERIFY(operation->text().contains(QStringLiteral("Action required")));
    // A provider-configuration error belongs to the task cards, not to the
    // independent node-prerequisites card. The latter may truthfully be ready
    // while Offer or Liquidity still needs operator action.
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(other_requirement->isHidden());

    UniValue incomplete_targets{UniValue::VOBJ};
    incomplete_targets.pushKV("wallet_eligible", true);
    incomplete_targets.pushKV("enabled", true);
    incomplete_targets.pushKV("ready", false);
    incomplete_targets.pushKV("wallet_locked", false);
    UniValue target_errors{UniValue::VARR};
    target_errors.push_back("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE");
    target_errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    incomplete_targets.pushKV("readiness_errors", std::move(target_errors));
    incomplete_targets.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(incomplete_targets);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("liquidity change")));
    QVERIFY2(next_step->text().contains(
        QStringLiteral("saved DD reserve capacity is too low")), qPrintable(next_step->text()));
    QVERIFY(next_step->text().contains(QStringLiteral("only with your approval")));
    QVERIFY(operation->text().contains(
        QStringLiteral("payment-carrier target")));
    QVERIFY(!operation_action->text().startsWith(QStringLiteral("Start")));
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("ready"));
    QVERIFY(other_requirement->isHidden());

    UniValue unknown_failure{UniValue::VOBJ};
    unknown_failure.pushKV("wallet_eligible", true);
    unknown_failure.pushKV("enabled", true);
    unknown_failure.pushKV("ready", false);
    unknown_failure.pushKV("wallet_locked", false);
    UniValue unknown_errors{UniValue::VARR};
    unknown_errors.push_back("PAYMASTER_TEST_UNKNOWN_REQUIREMENT");
    unknown_failure.pushKV("readiness_errors", std::move(unknown_errors));
    unknown_failure.pushKV("oracle_price_micro_usd", 6500);
    tab.setPaymasterReadinessStatusForTesting(unknown_failure);
    QCOMPARE(prerequisites->property("statusKind").toString(),
             QStringLiteral("error"));
    QCOMPARE(other_requirement->property("statusKind").toString(),
             QStringLiteral("error"));
    QVERIFY(other_requirement->text().startsWith(QStringLiteral("!")));
}

void PaymasterWidgetTests::paymasterPendingStartUsesLiveStatusInsteadOfStaleModal()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(provider_status != nullptr);

    UniValue pending{UniValue::VOBJ};
    pending.pushKV("running", true);
    pending.pushKV("ready", false);
    pending.pushKV("operation_mode", "automatic");
    pending.pushKV("service_state", "replenishing_liquidity");

    const auto message_box_count = [] {
        const QWidgetList widgets = QApplication::topLevelWidgets();
        return std::count_if(
            widgets.cbegin(), widgets.cend(),
            [](QWidget* widget) { return qobject_cast<QMessageBox*>(widget); });
    };
    const int message_boxes_before = message_box_count();
    tab.setPaymasterStartResultForTesting(pending);
    const int message_boxes_after = message_box_count();

    QCOMPARE(message_boxes_after, message_boxes_before);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("Provider start accepted")));
    QVERIFY(provider_status->text().contains(
        QStringLiteral("restoring the saved liquidity targets")));

    tab.setPaymasterRpcExecutorForTesting(
        [](const std::string&, const UniValue&) {
            return UniValue{UniValue::VOBJ};
        });
    UniValue malformed{UniValue::VOBJ};
    malformed.pushKV("running", true);
    malformed.pushKV("ready", true);
    malformed.pushKV("service_state", "active");
    tab.setPaymasterStartResultForTesting(malformed);
    QVERIFY(provider_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
}

void PaymasterWidgetTests::paymasterCarrierWithdrawalActionsFailClosed()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStackedWidget* paymaster_tabs = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterOperatorPages"));
    QWidget* liquidity_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterLiquidityPage"));
    QVERIFY(paymaster_tabs != nullptr);
    QVERIFY(liquidity_page != nullptr);
    tab.findChild<QStackedWidget*>("paymasterSettingsPages")->setEnabled(true);

    const int liquidity_index = paymaster_tabs->indexOf(liquidity_page);
    QVERIFY(liquidity_index >= 0);
    paymaster_tabs->widget(liquidity_index)->setEnabled(true);
    QVERIFY(liquidity_page->isEnabled());

    QLabel* value = tab.findChild<QLabel*>(
        QStringLiteral("paymasterCarrierValueStatus"));
    QLabel* recycling = tab.findChild<QLabel*>(
        QStringLiteral("paymasterLiquidityRecyclingStatus"));
    QComboBox* selection = tab.findChild<QComboBox*>(
        QStringLiteral("paymasterReleaseCarrierSelection"));
    QPushButton* preview_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierExcess"));
    QPushButton* execute_excess = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierExcess"));
    QPushButton* preview_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewCarrierRelease"));
    QPushButton* execute_release = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecuteCarrierRelease"));
    QVERIFY(value != nullptr);
    QVERIFY(recycling != nullptr);
    QVERIFY(selection != nullptr);
    QVERIFY(preview_excess != nullptr);
    QVERIFY(execute_excess != nullptr);
    QVERIFY(preview_release != nullptr);
    QVERIFY(execute_release != nullptr);

    QVERIFY(!preview_excess->isEnabled());
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(!execute_excess->isEnabled());
    QVERIFY(!execute_release->isEnabled());

    tab.setPaymasterLiquidityStatusForTesting(PaymasterLiquidityStatus(
        "waiting_for_liquidity_confirmation", true, true, 0, 1));
    QVERIFY(value->text().contains(QStringLiteral("1.00 DD")));
    QVERIFY(value->text().contains(QStringLiteral("0.06 DD")));
    QVERIFY(!preview_excess->isEnabled());
    QVERIFY(preview_excess->toolTip().contains(QStringLiteral("Minimum payout: 1.00 DD")));
    QVERIFY(!execute_excess->isEnabled());

    tab.setPaymasterLiquidityPoolForTesting(PaymasterLiquidityPoolStatus());
    QCOMPARE(selection->count(), 1);
    QVERIFY(selection->currentText().contains(QStringLiteral("1.06 DD")));
    QVERIFY(recycling->text().contains(QStringLiteral("Carrier is being reused: 1.03 DD")));
    QVERIFY(recycling->text().contains(QStringLiteral("DGB successor slot is being reused: 0.10000000 DGB")));
    QVERIFY(preview_release->isEnabled());
    QVERIFY(!execute_release->isEnabled());

    UniValue malformed_entry{UniValue::VOBJ};
    malformed_entry.pushKV("state", "available");
    malformed_entry.pushKV("purpose", "operational");
    malformed_entry.pushKV("asset", "dd_carrier");
    malformed_entry.pushKV("dd_cents", 106);
    UniValue malformed_entries{UniValue::VARR};
    malformed_entries.push_back(std::move(malformed_entry));
    UniValue malformed_pool{UniValue::VOBJ};
    malformed_pool.pushKV("pool", std::move(malformed_entries));
    tab.setPaymasterLiquidityPoolForTesting(malformed_pool);
    QCOMPARE(selection->count(), 0);
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(recycling->text().contains(
        QStringLiteral("incomplete"), Qt::CaseInsensitive));

    UniValue empty_pool{UniValue::VOBJ};
    empty_pool.pushKV("pool", UniValue{UniValue::VARR});
    tab.setPaymasterLiquidityPoolForTesting(empty_pool);
    QCOMPARE(selection->count(), 0);
    QVERIFY(!preview_release->isEnabled());
    QVERIFY(!execute_release->isEnabled());
}

void PaymasterWidgetTests::paymasterCarrierWithdrawalPreviewsArePlanBound()
{
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto* capital = panel->findChild<QWidget*>("paymasterLiquidityPage");
    QVERIFY(capital);
    for (const auto* name : {"paymasterWithdrawTask", "paymasterReleaseTask", "paymasterRestoreTask"}) {
        auto* action = panel->findChild<QPushButton*>(name);
        QVERIFY(action && action->isVisibleTo(capital));
    }
    // Legacy execution surfaces must never offer another financial path.
    for (const auto* name : {"paymasterExecuteCarrierExcess", "paymasterExecuteCarrierRelease"}) {
        auto* action = panel->findChild<QPushButton*>(name);
        QVERIFY(action && !action->isVisibleTo(capital));
    }
    // Exact-plan, cancel, lost-reply and changed-selection contracts are
    // exercised for the shared flow by paymasterGuidedCapitalTasks.
}

void PaymasterWidgetTests::paymasterSafetyControlsDefaultFailClosed()
{
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    QWidget* safety_page = tab.findChild<QWidget*>(QStringLiteral("paymasterSafetyPolicyPage"));
    QLabel* warning = tab.findChild<QLabel*>(QStringLiteral("paymasterSafetyPolicyWarning"));
    QLabel* provider_status = tab.findChild<QLabel*>(QStringLiteral("paymasterProviderSafetyStatus"));
    QLabel* client_status = tab.findChild<QLabel*>(QStringLiteral("paymasterClientSafetyStatus"));
    QPushButton* provider_action = tab.findChild<QPushButton*>(QStringLiteral("paymasterOperatorNextAction"));
    QPushButton* save_provider = tab.findChild<QPushButton*>(QStringLiteral("savePaymasterProviderSafetyPolicy"));
    QPushButton* save_client = tab.findChild<QPushButton*>(QStringLiteral("savePaymasterClientSafetyPolicy"));
    QLineEdit* public_hour = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerHour"));
    QLineEdit* public_day = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerDay"));
    QSpinBox* quote_total = tab.findChild<QSpinBox*>(QStringLiteral("paymasterSafetyMaxActiveQuotesTotal"));
    QSpinBox* client_transfer = tab.findChild<QSpinBox*>(QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QSpinBox* client_day = tab.findChild<QSpinBox*>(QStringLiteral("paymasterClientSafetyMaxFeePerDay"));

    QVERIFY(safety_page != nullptr);
    QVERIFY(warning != nullptr);
    QVERIFY(provider_status != nullptr);
    QVERIFY(client_status != nullptr);
    QVERIFY(provider_action != nullptr);
    QVERIFY(save_provider != nullptr);
    QVERIFY(save_client != nullptr);
    QVERIFY(public_hour != nullptr);
    QVERIFY(public_day != nullptr);
    QVERIFY(quote_total != nullptr);
    QVERIFY(client_transfer != nullptr);
    QVERIFY(client_day != nullptr);
    QVERIFY(warning->text().contains(QStringLiteral("spending brake")));
    QVERIFY(provider_status->text().contains(QStringLiteral("not configured")));
    QVERIFY(client_status->text().contains(QStringLiteral("not configured")));
    QCOMPARE(public_hour->text(), QStringLiteral("0.00000000"));
    QCOMPARE(public_day->text(), QStringLiteral("0.00000000"));
    QCOMPARE(provider_action->text(), QStringLiteral("Refresh status")); // No start without a current operator snapshot.
    QVERIFY(quote_total->value() > 0);
    QVERIFY(client_day->value() >= client_transfer->value());

    const QStringList funding_prefixes{
        QStringLiteral("paymasterSafetyUserPaid"),
        QStringLiteral("paymasterSafetyPublicSponsored"),
        QStringLiteral("paymasterSafetyRestrictedSponsored"),
    };
    const QStringList limit_suffixes{
        QStringLiteral("MaxNetworkFeePerTransaction"),
        QStringLiteral("MaxReservedNetworkFee"),
        QStringLiteral("MaxNetworkFeePerHour"),
        QStringLiteral("MaxNetworkFeePerDay"),
        QStringLiteral("MaxCompletedPerHour"),
        QStringLiteral("MaxCompletedPerDay"),
    };
    for (const QString& prefix : funding_prefixes) {
        QVERIFY2(tab.findChild<QPushButton*>(prefix + QStringLiteral("RestoreDefaults")) != nullptr,
                 qPrintable(QStringLiteral("Missing Paymaster safety reset for %1")
                                .arg(prefix)));
        for (const QString& suffix : limit_suffixes) {
            const QString object_name = prefix + suffix;
            QVERIFY2(tab.findChild<QWidget*>(object_name) != nullptr,
                     qPrintable(QStringLiteral("Missing Paymaster safety control %1")
                                    .arg(object_name)));
        }
    }
    const QStringList quote_controls{
        QStringLiteral("paymasterSafetyMaxActiveQuotesTotal"),
        QStringLiteral("paymasterSafetyMaxActiveQuotesPerNetgroup"),
        QStringLiteral("paymasterSafetyMaxActiveQuotesPerRecipient"),
        QStringLiteral("paymasterSafetyMaxQuoteRequestsPerNetgroupMinute"),
    };
    for (const QString& object_name : quote_controls) {
        QVERIFY2(tab.findChild<QSpinBox*>(object_name) != nullptr,
                 qPrintable(QStringLiteral("Missing Paymaster quote limit %1")
                                .arg(object_name)));
    }
}

void PaymasterWidgetTests::paymasterSafetyPolicyDisplaysFiniteDisabledSemantics()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());

    QLabel* user_paid_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyUserPaidMode"));
    QLabel* public_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMode"));
    QLabel* restricted_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterSafetyRestrictedSponsoredMode"));
    QLabel* client_mode = tab.findChild<QLabel*>(
        QStringLiteral("paymasterClientSafetyMode"));
    QLineEdit* public_per_transaction = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterSafetyPublicSponsoredMaxNetworkFeePerTransaction"));
    QSpinBox* client_transfer = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QSpinBox* client_day = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerDay"));

    QVERIFY(user_paid_mode != nullptr);
    QVERIFY(public_mode != nullptr);
    QVERIFY(restricted_mode != nullptr);
    QVERIFY(client_mode != nullptr);
    QVERIFY(public_per_transaction != nullptr);
    QVERIFY(client_transfer != nullptr);
    QVERIFY(client_day != nullptr);

    QVERIFY(user_paid_mode->text().contains(QStringLiteral("Unsaved limits")));
    QVERIFY(user_paid_mode->text().contains(QStringLiteral("DGB per transfer")));
    QVERIFY(public_mode->text().contains(QStringLiteral("Disabled")));
    QVERIFY(public_mode->text().contains(QStringLiteral("never means unlimited")));
    QVERIFY(restricted_mode->text().contains(QStringLiteral("Disabled")));

    public_per_transaction->setText(QStringLiteral("1"));
    QVERIFY(public_mode->text().contains(QStringLiteral("Incomplete")));
    QVERIFY(!public_mode->text().contains(QStringLiteral("unlimited")));

    client_transfer->setValue(0);
    client_day->setValue(0);
    QVERIFY(client_mode->text().contains(QStringLiteral("Zero service-fee budget")));
    QVERIFY(client_mode->text().contains(QStringLiteral("zero-fee")));
    QVERIFY(client_mode->text().contains(QStringLiteral("never means unlimited")));
    client_transfer->setValue(1);
    QVERIFY(client_mode->text().contains(QStringLiteral("Unsaved limits")));
}

void PaymasterWidgetTests::paymasterInjectedRpcCoversConfigurationWorkflows()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;
    std::vector<UniValue> parameters;
    bool malformed_operating_ack{false};
    bool malformed_provider_safety_ack{false};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterpolicy") {
                if (malformed_operating_ack) {
                    return UniValue{UniValue::VOBJ};
                }
                UniValue result = params[0];
                result.pushKV(
                    "policy_hash",
                    "2222222222222222222222222222222222222222222222222222222222222222");
                return result;
            }
            if (command == "setpaymastersafetypolicy") {
                if (malformed_provider_safety_ack) {
                    return UniValue{UniValue::VOBJ};
                }
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "getpaymasterinfo" ||
                command == "getpaymastersafetystatus") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });

    // A new wallet intentionally keeps configuration tabs locked until the
    // operator chooses guided or expert setup. This contract test exercises
    // the controls behind that onboarding gate, so unlock only the local tab
    // container without altering persisted settings.
    QStackedWidget* operator_tabs = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterOperatorPages"));
    QStackedWidget* settings_pages = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterSettingsPages"));
    QTimer* status_refresh_timer = tab.findChild<QTimer*>(
        QStringLiteral("paymasterSetupStatusTimer"));
    QVERIFY(operator_tabs != nullptr);
    QVERIFY(settings_pages != nullptr);
    QVERIFY(status_refresh_timer != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->widget(index)->setEnabled(true);
        operator_tabs->widget(index)->setProperty("paymasterPageAvailable", true);
    }
    status_refresh_timer->start();
    operator_tabs->setCurrentWidget(settings_pages);
    QVERIFY(!status_refresh_timer->isActive());
    operator_tabs->setCurrentIndex(0);
    QVERIFY(status_refresh_timer->isActive());

    QPushButton* create_identity = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterCreateIdentity"));
    QLineEdit* display_name = tab.findChild<QLineEdit*>(
        QStringLiteral("paymasterDisplayName"));
    QPushButton* save_policy = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterPolicy"));
    QSpinBox* provider_fee_cap = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterPolicyMaximumUserPaidServiceFeeCents"));
    QPushButton* save_safety = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterProviderSafetyPolicy"));
    QPushButton* save_client_safety = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterClientSafetyPolicy"));
    QSpinBox* client_per_transaction = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterClientSafetyMaxFeePerTransaction"));
    QLabel* client_safety_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterClientSafetyStatus"));
    QLabel* provider_safety_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderSafetyStatus"));
    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(create_identity != nullptr);
    QVERIFY(display_name != nullptr);
    QVERIFY(save_policy != nullptr);
    QVERIFY(provider_fee_cap != nullptr);
    QVERIFY(save_safety != nullptr);
    QVERIFY(save_client_safety != nullptr);
    QVERIFY(client_per_transaction != nullptr);
    QVERIFY(client_safety_status != nullptr);
    QVERIFY(provider_safety_status != nullptr);
    QVERIFY(provider_status != nullptr);

    display_name->setText(QStringLiteral("test-provider"));
    create_identity->setEnabled(true);
    create_identity->click();
    QCOMPARE(commands.value(0), QStringLiteral("createpaymasteridentity"));
    QVERIFY(parameters.at(0).isArray());
    QCOMPARE(QString::fromStdString(parameters.at(0)[0].get_str()),
             QStringLiteral("test-provider"));
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasteroperatorinfo"));

    commands.clear();
    parameters.clear();
    provider_fee_cap->setLocale(QLocale(QLocale::German));
    QLineEdit* provider_fee_cap_editor =
        provider_fee_cap->findChild<QLineEdit*>();
    QVERIFY(provider_fee_cap_editor != nullptr);
    provider_fee_cap_editor->setText(QStringLiteral("1.00 DD"));
    save_policy->setEnabled(true);
    save_policy->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterpolicy"));
    QVERIFY(parameters.at(0)[0].find_value("funding_models").isArray());
    QCOMPARE(parameters.at(0)[0].find_value(
                 "maximum_user_paid_service_fee_cents").getInt<int64_t>(),
             int64_t{100});
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasteroperatorinfo"));

    commands.clear();
    parameters.clear();
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_safety->setEnabled(true);
    save_safety->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymastersafetypolicy"));
    QVERIFY(parameters.at(0)[0].find_value("user_paid").isObject());
    QCOMPARE(commands.value(1), QStringLiteral("getpaymastersafetystatus"));

    commands.clear();
    parameters.clear();
    malformed_operating_ack = true;
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_policy->setEnabled(true);
    save_policy->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterpolicy"));
    QVERIFY(provider_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    malformed_operating_ack = false;

    commands.clear();
    parameters.clear();
    malformed_provider_safety_ack = true;
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_safety->setEnabled(true);
    save_safety->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymastersafetypolicy"));
    QVERIFY(provider_safety_status->text().contains(
        QStringLiteral("did not confirm"), Qt::CaseInsensitive));
    malformed_provider_safety_ack = false;

    commands.clear();
    parameters.clear();
    client_per_transaction->setValue(101);
    save_client_safety->setEnabled(true);
    save_client_safety->click();
    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterclientsafetypolicy"));
    QVERIFY(client_safety_status->text().contains(
        QStringLiteral("not confirmed"), Qt::CaseInsensitive));

    commands.clear();
    parameters.clear();
    UniValue running{UniValue::VOBJ};
    running.pushKV("wallet_eligible", true);
    running.pushKV("settings_present", true);
    running.pushKV("enabled", true);
    running.pushKV("running", true);
    running.pushKV("ready", true);
    running.pushKV("wallet_locked", false);
    running.pushKV("has_identity", true);
    running.pushKV("has_policy", true);
    running.pushKV("has_safety_policy", true);
    running.pushKV("operation_mode", "automatic");
    running.pushKV("autostart", false);
    running.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(running);
    save_policy->setEnabled(true);
    save_policy->click();
    QVERIFY(commands.isEmpty());
    QVERIFY(provider_status->text().contains(
        QStringLiteral("Pause the Paymaster provider")));
}

void PaymasterWidgetTests::paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;
    std::string omitted_preparation_field;
    std::vector<UniValue> parameters;

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            UniValue result{UniValue::VOBJ};
            if (command == "preparepaymasterpool") {
                if (omitted_preparation_field != "accepted") result.pushKV("accepted", params[0].find_value("execute").isTrue());
                result.pushKV("cancelled", false);
                result.pushKV("preparation", UniValue{UniValue::VARR});
                if (omitted_preparation_field != "maximum_fee_satoshis") result.pushKV("maximum_fee_satoshis", params[0].find_value("maximum_fee_satoshis"));
                if (omitted_preparation_field != "maximum_total_fee_satoshis") result.pushKV("maximum_total_fee_satoshis", 20000000);
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "5555555555555555555555555555555555555555555555555555555555555555");
                result.pushKV("admission_dgb_slots",
                              params[0].find_value(
                                  "admission_dgb_slots").getInt<int>());
                result.pushKV("operational_dgb_slots",
                              params[0].find_value(
                                  "operational_dgb_slots").getInt<int>());
                result.pushKV("admission_carrier_slots",
                              params[0].find_value(
                                  "admission_carrier_slots").getInt<int>());
                result.pushKV("operational_carrier_slots",
                              params[0].find_value(
                                  "operational_carrier_slots").getInt<int>());
                result.pushKV("missing_admission_dgb_slots", 1);
                result.pushKV("missing_operational_dgb_slots", 0);
                result.pushKV("missing_admission_carrier_slots", 0);
                result.pushKV("missing_operational_carrier_slots", 0);
                result.pushKV("admission_dgb_satoshis_each", 10000000);
                result.pushKV("operational_dgb_satoshis_each", 20000000);
                result.pushKV("carrier_cents_each", 100);
                result.pushKV("total_output_satoshis", 10000000);
                result.pushKV("total_carrier_cents", 0);
            } else if (command == "rebalancepaymasterpool") {
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "6666666666666666666666666666666666666666666666666666666666666666");
                result.pushKV("retired_admission_dgb_slots", 1);
                result.pushKV("retired_operational_dgb_slots", 0);
                result.pushKV("retired_admission_carrier_slots", 0);
                result.pushKV("retired_operational_carrier_slots", 0);
                result.pushKV("retired_dgb_satoshis", 10000000);
                result.pushKV("retired_carrier_cents", 0);
            } else if (command == "setpaymasterruntimesettings") {
                result.pushKV("operation_mode", "automatic");
                result.pushKV("autostart", true);
            }
            return result;
        });

    QStackedWidget* operator_tabs = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterOperatorPages"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->widget(index)->setEnabled(true);
        operator_tabs->widget(index)->setProperty("paymasterPageAvailable", true);
    }

    QPushButton* preview = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewPoolPreparation"));
    QPushButton* execute_preparation = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecutePoolPreparation"));
    QPushButton* preview_retirement = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterPreviewPoolRetirement"));
    QPushButton* execute_retirement = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterExecutePoolRetirement"));
    QSpinBox* admission_dgb = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionDgbSlots"));
    QCheckBox* autostart = tab.findChild<QCheckBox*>(
        QStringLiteral("paymasterProviderAutostart"));
    QPushButton* save_runtime = tab.findChild<QPushButton*>(
        QStringLiteral("savePaymasterRuntimeSettings"));
    QLabel* runtime_result = tab.findChild<QLabel*>(
        QStringLiteral("paymasterRuntimeSettingsResult"));
    QVERIFY(preview != nullptr);
    QVERIFY(execute_preparation != nullptr);
    QVERIFY(preview_retirement != nullptr);
    QVERIFY(execute_retirement != nullptr);
    QVERIFY(admission_dgb != nullptr);
    QVERIFY(autostart != nullptr);
    QVERIFY(save_runtime != nullptr);
    QVERIFY(runtime_result != nullptr);

    // Capital previews now use the common task controller, whose exact
    // funding contract is covered by the guided preparation/capital tests.
    QVERIFY(!execute_preparation->isVisibleTo(execute_preparation->window()));
    QVERIFY(!execute_retirement->isVisibleTo(execute_retirement->window()));
    commands.clear();
    parameters.clear();
    UniValue stopped{UniValue::VOBJ};
    stopped.pushKV("running", false);
    stopped.pushKV("wallet_eligible", true);
    stopped.pushKV("has_policy", true);
    stopped.pushKV("operation_mode", "manual");
    tab.setPaymasterReadinessStatusForTesting(stopped);
    auto* processing = tab.findChild<QComboBox*>("paymasterOperationMode");
    QVERIFY(processing);
    processing->setCurrentIndex(processing->findData(QStringLiteral("automatic")));
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterruntimesettings"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("operation_mode").get_str()),
             QStringLiteral("automatic"));
    QVERIFY(parameters.at(0)[0].find_value("autostart").isNull());
    QCOMPARE(parameters.at(0)[0].size(), size_t{1});
    QVERIFY(runtime_result->text().contains(QStringLiteral("autostart"),
                                            Qt::CaseInsensitive));
    QCOMPARE(commands.value(1), QStringLiteral("getpaymasteroperatorinfo"));

    commands.clear();
    parameters.clear();
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterruntimesettings") {
                UniValue malformed{UniValue::VOBJ};
                malformed.pushKV("operation_mode", "automatic");
                return malformed;
            }
            throw std::runtime_error(
                "unexpected malformed runtime workflow RPC");
        });
    processing->setCurrentIndex(processing->findData(QStringLiteral("manual")));
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0),
             QStringLiteral("setpaymasterruntimesettings"));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("not confirmed"), Qt::CaseInsensitive));
    QVERIFY(save_runtime->isEnabled());

    commands.clear();
    parameters.clear();
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "setpaymasterruntimesettings") {
                throw std::runtime_error("injected runtime rejection");
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            return UniValue{UniValue::VOBJ};
        });
    processing->setCurrentIndex(processing->findData(QStringLiteral("manual")));
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    save_runtime->setEnabled(true);
    save_runtime->click();
    QCOMPARE(commands.value(0), QStringLiteral("setpaymasterruntimesettings"));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("not saved"), Qt::CaseInsensitive));
    QVERIFY(runtime_result->text().contains(
        QStringLiteral("injected runtime rejection")));
    QVERIFY(save_runtime->isEnabled());

    commands.clear();
    parameters.clear();
}

void PaymasterWidgetTests::paymasterManualActivityUsesExpectedRpcAndReadableStates()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    tab.setPaymasterMutationSnapshotsAvailableForTesting(true, true, true);
    QStringList commands;

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            commands.push_back(QString::fromStdString(command));
            if (command == "processpaymasterrequests") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("processed", true);
                result.pushKV("message_type", "quote");
                result.pushKV("queued", true);
                return result;
            }
            if (command == "listpaymasterreservations") {
                UniValue result{UniValue::VARR};
                UniValue reservation{UniValue::VOBJ};
                reservation.pushKV("purpose", "operational");
                reservation.pushKV("asset", "dd_carrier");
                reservation.pushKV("dd_cents", 103);
                reservation.pushKV("state", "reserved");
                result.push_back(std::move(reservation));
                return result;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error("injected follow-up refresh unavailable");
            }
            throw std::runtime_error("unexpected injected activity RPC");
        });

    QStackedWidget* operator_tabs = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterOperatorPages"));
    QVERIFY(operator_tabs != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->widget(index)->setEnabled(true);
        operator_tabs->widget(index)->setProperty("paymasterPageAvailable", true);
    }

    UniValue running{UniValue::VOBJ};
    running.pushKV("wallet_eligible", true);
    running.pushKV("enabled", true);
    running.pushKV("ready", true);
    running.pushKV("running", true);
    running.pushKV("wallet_locked", false);
    running.pushKV("operation_mode", "manual");
    running.pushKV("service_state", "manual");
    running.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(running);

    QPushButton* process_request = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterProcessOneRequest"));
    QPushButton* refresh = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterRefreshReservations"));
    QLabel* action_result = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivityActionResult"));
    QLabel* summary = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivitySummary"));
    QVERIFY(process_request != nullptr);
    QVERIFY(refresh != nullptr);
    QVERIFY(action_result != nullptr);
    QVERIFY(summary != nullptr);
    QVERIFY(process_request->isEnabled());

    process_request->click();
    QCOMPARE(commands.value(0), QStringLiteral("processpaymasterrequests"));
    QVERIFY(action_result->text().contains(QStringLiteral("quote request")));
    QVERIFY(action_result->text().contains(QStringLiteral("queued")));

    commands.clear();
    refresh->click();
    QCOMPARE(commands.value(0), QStringLiteral("listpaymasterreservations"));
    QVERIFY(summary->text().contains(QStringLiteral("1 unavailable")));
    auto* reservations = tab.findChild<QTableWidget*>("paymasterActivityReservations");
    QVERIFY(reservations);
    QCOMPARE(reservations->rowCount(), 1);
    QVERIFY(reservations->item(0, 1)->text().contains(QStringLiteral("1.03 DD carrier")));
    QVERIFY(reservations->item(0, 2)->text().contains(QStringLiteral("reserved for in-progress work")));
}

void PaymasterWidgetTests::paymasterFinancesAndBackupWorkflow()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarTab tab(platform_style.get());
    QStringList commands;
    std::vector<UniValue> parameters;

    const auto summary = [](qint64 income, qint64 cost,
                            qint64 transfers) {
        UniValue value{UniValue::VOBJ};
        value.pushKV("service_fee_income_cents", income);
        value.pushKV("dgb_operating_cost_satoshis", cost);
        value.pushKV("successful_transfers", transfers);
        return value;
    };
    const std::string provider_id(64, 'a');
    UniValue finance{UniValue::VOBJ};
    finance.pushKV("provider_id", provider_id);
    finance.pushKV("period", "30d");
    finance.pushKV("service_fee_income_cents", 125);
    finance.pushKV("dgb_operating_cost_satoshis", 100000);
    finance.pushKV("successful_transfers", 5);
    finance.pushKV("average_service_fee_cents", 25);
    finance.pushKV("user_paid_transfers", 3);
    finance.pushKV("public_sponsored_transfers", 1);
    finance.pushKV("restricted_sponsored_transfers", 1);
    const auto model_summary = [](qint64 transfers, qint64 income,
                                  qint64 cost) {
        UniValue value{UniValue::VOBJ};
        value.pushKV("successful_transfers", transfers);
        value.pushKV("service_fee_income_cents", income);
        value.pushKV("dgb_operating_cost_satoshis", cost);
        return value;
    };
    UniValue model_breakdown{UniValue::VOBJ};
    model_breakdown.pushKV("user_paid", model_summary(3, 125, 60000));
    model_breakdown.pushKV("public_sponsored", model_summary(1, 0, 20000));
    model_breakdown.pushKV("restricted_sponsored",
                           model_summary(1, 0, 20000));
    finance.pushKV("model_breakdown", std::move(model_breakdown));
    UniValue summaries{UniValue::VOBJ};
    summaries.pushKV("today", summary(25, 10000, 1));
    summaries.pushKV("7d", summary(75, 50000, 3));
    summaries.pushKV("30d", summary(125, 100000, 5));
    summaries.pushKV("all", summary(250, 200000, 10));
    finance.pushKV("period_summaries", std::move(summaries));
    finance.pushKV("history_partially_reconstructable", false);
    finance.pushKV("history_complete_from", 100);
    finance.pushKV("backup_required", true);
    finance.pushKV("last_successful_backup_at", 0);
    finance.pushKV("external_backup_acknowledged_at", 0);
    finance.pushKV("oracle_price_micro_usd", 500000);
    finance.pushKV("valuation_time", 200);
    finance.pushKV("estimated_result_usd", 1.2495);
    UniValue capital{UniValue::VOBJ};
    capital.pushKV("dgb_available_satoshis", 500000000);
    capital.pushKV("dgb_reserved_satoshis", 100000000);
    capital.pushKV("dgb_pending_satoshis", 50000000);
    capital.pushKV("carrier_base_cents", 400);
    capital.pushKV("carrier_earned_cents", 25);
    capital.pushKV("carrier_withdrawable_cents", 20);
    capital.pushKV("pending_maintenance_transactions", 1);
    finance.pushKV("pool_capital", std::move(capital));
    UniValue daily_totals{UniValue::VARR};
    UniValue day{UniValue::VOBJ};
    day.pushKV("day_start", 86400);
    day.pushKV("service_fee_income_cents", 25);
    day.pushKV("dgb_operating_cost_satoshis", 10000);
    day.pushKV("successful_transfers", 1);
    day.pushKV("maintenance_transactions", 2);
    daily_totals.push_back(std::move(day));
    finance.pushKV("daily_totals", std::move(daily_totals));
    UniValue events{UniValue::VARR};
    UniValue event{UniValue::VOBJ};
    event.pushKV("event_id", std::string(64, 'b'));
    event.pushKV("kind", "transfer");
    event.pushKV("state", "confirmed");
    event.pushKV("dd_income_cents", 25);
    event.pushKV("dgb_cost_satoshis", 10000);
    event.pushKV("created_at", 150);
    event.pushKV("confirmed_at", 160);
    event.pushKV("funding_model", "user_paid");
    event.pushKV("transaction_id", std::string(64, 'c'));
    events.push_back(std::move(event));
    finance.pushKV("events", std::move(events));

    UniValue finance_without_oracle{UniValue::VOBJ};
    UniValue finance_partial_history{UniValue::VOBJ};
    const std::vector<std::string>& finance_keys = finance.getKeys();
    const std::vector<UniValue>& finance_values = finance.getValues();
    for (size_t index = 0; index < finance_keys.size(); ++index) {
        if (finance_keys.at(index) == "oracle_price_micro_usd" ||
            finance_keys.at(index) == "valuation_time" ||
            finance_keys.at(index) == "estimated_result_usd") {
            continue;
        }
        finance_without_oracle.pushKV(finance_keys.at(index),
                                      finance_values.at(index));
        if (finance_keys.at(index) !=
                "history_partially_reconstructable" &&
            finance_keys.at(index) != "history_complete_from") {
            finance_partial_history.pushKV(finance_keys.at(index),
                                           finance_values.at(index));
        }
    }
    finance_partial_history.pushKV(
        "history_partially_reconstructable", true);
    finance_partial_history.pushKV("history_complete_from", 100);
    const auto finance_for_period = [](const UniValue& source,
                                       const std::string& period) {
        UniValue result{UniValue::VOBJ};
        const std::vector<std::string>& keys = source.getKeys();
        const std::vector<UniValue>& values = source.getValues();
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys.at(index) == "period") continue;
            result.pushKV(keys.at(index), values.at(index));
        }
        result.pushKV("period", period);
        const auto& totals = source.find_value("period_summaries").find_value(period);
        for (const auto* key : {"service_fee_income_cents", "dgb_operating_cost_satoshis", "successful_transfers"})
            result.pushKV(key, totals.find_value(key));
        return result;
    };
    const auto finance_page_for_period = [](
        const UniValue& source, const std::string& period,
        UniValue page_events, const std::string& next_cursor) {
        UniValue result{UniValue::VOBJ};
        const std::vector<std::string>& keys = source.getKeys();
        const std::vector<UniValue>& values = source.getValues();
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys.at(index) == "period" || keys.at(index) == "events" ||
                keys.at(index) == "next_cursor") {
                continue;
            }
            result.pushKV(keys.at(index), values.at(index));
        }
        result.pushKV("period", period);
        result.pushKV("events", std::move(page_events));
        if (next_cursor.empty()) {
            result.pushKV("next_cursor", UniValue{UniValue::VNULL});
        } else {
            result.pushKV("next_cursor", next_cursor);
        }
        return result;
    };
    constexpr qint64 LARGE_EXPORT_EVENTS{10251};
    bool oracle_available{true};
    bool partial_history{false};
    bool malformed_finance{false};
    bool large_export{false};
    bool switch_period_during_request{false};
    QComboBox* period_control{nullptr};

    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasterfinancestatus") {
                const std::string requested_period =
                    params[0].find_value("period").get_str();
                if (switch_period_during_request && period_control) {
                    switch_period_during_request = false;
                    period_control->setCurrentIndex(period_control->findData(
                        QStringLiteral("today")));
                }
                if (malformed_finance) {
                    UniValue malformed{UniValue::VOBJ};
                    malformed.pushKV("period", requested_period);
                    return malformed;
                }
                if (large_export) {
                    const UniValue& cursor_value =
                        params[0].find_value("cursor");
                    qulonglong first_index{0};
                    if (!cursor_value.isNull()) {
                        if (!cursor_value.isStr()) {
                            throw std::runtime_error(
                                "finance cursor is not a string");
                        }
                        bool cursor_ok{false};
                        first_index = QString::fromStdString(
                            cursor_value.get_str()).toULongLong(
                                &cursor_ok, 16);
                        if (!cursor_ok || first_index >=
                                static_cast<qulonglong>(
                                    LARGE_EXPORT_EVENTS)) {
                            throw std::runtime_error(
                                "finance cursor is outside the test corpus");
                        }
                    }
                    const qulonglong end_index = std::min(
                        first_index + qulonglong{250},
                        static_cast<qulonglong>(LARGE_EXPORT_EVENTS));
                    UniValue page_events{UniValue::VARR};
                    for (qulonglong index = first_index;
                         index < end_index; ++index) {
                        const std::string event_id =
                            QStringLiteral("%1")
                                .arg(index + 1, 64, 16,
                                     QLatin1Char('0'))
                                .toStdString();
                        UniValue event{UniValue::VOBJ};
                        event.pushKV("event_id", event_id);
                        event.pushKV("kind", "transfer");
                        event.pushKV("state", "confirmed");
                        event.pushKV("dd_income_cents", 1);
                        event.pushKV("dgb_cost_satoshis", 2);
                        event.pushKV("created_at",
                                     static_cast<int64_t>(1000 + index));
                        event.pushKV("confirmed_at",
                                     static_cast<int64_t>(1001 + index));
                        event.pushKV("funding_model", "user_paid");
                        event.pushKV("transaction_id",
                                     std::string(64, 'c'));
                        page_events.push_back(std::move(event));
                    }
                    const std::string next_cursor =
                        end_index < static_cast<qulonglong>(
                                        LARGE_EXPORT_EVENTS)
                        ? QStringLiteral("%1")
                              .arg(end_index, 64, 16,
                                   QLatin1Char('0'))
                              .toStdString()
                        : std::string{};
                    return finance_page_for_period(
                        finance, requested_period,
                        std::move(page_events), next_cursor);
                }
                const UniValue& selected = partial_history
                    ? finance_partial_history
                    : oracle_available ? finance : finance_without_oracle;
                return finance_for_period(selected, requested_period);
            }
            if (command == "acknowledgepaymasterproviderbackup") {
                UniValue malformed{UniValue::VOBJ};
                malformed.pushKV("acknowledged", true);
                return malformed;
            }
            if (command == "getpaymasterinfo") {
                throw std::runtime_error(
                    "injected follow-up provider refresh unavailable");
            }
            throw std::runtime_error("unexpected injected finance RPC");
        });

    QStackedWidget* operator_tabs = tab.findChild<QStackedWidget*>(
        QStringLiteral("paymasterOperatorPages"));
    QWidget* finance_page = tab.findChild<QWidget*>(
        QStringLiteral("paymasterFinancesPage"));
    QVERIFY(operator_tabs != nullptr);
    QVERIFY(finance_page != nullptr);
    for (int index = 0; index < operator_tabs->count(); ++index) {
        operator_tabs->widget(index)->setEnabled(true);
        operator_tabs->widget(index)->setProperty("paymasterPageAvailable", true);
    }
    finance_page->setEnabled(true);
    finance_page->setProperty("paymasterPageAvailable", true);

    operator_tabs->setCurrentWidget(finance_page);

    QVERIFY(commands.contains(QStringLiteral("getpaymasterfinancestatus")));

    QLabel* period_income = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinancePeriodIncome2"));
    QLabel* result_estimate = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceResultEstimate"));
    QLabel* model_breakdown_label = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceModelBreakdown"));
    QLabel* pool_dgb = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinancePoolDgb"));
    QGroupBox* history_notice = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterFinanceHistoryNotice"));
    QLabel* history_notice_text = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceHistoryNoticeText"));
    QTableWidget* booking_table = tab.findChild<QTableWidget*>(
        QStringLiteral("paymasterFinanceEvents"));
    QTableWidget* daily_table = tab.findChild<QTableWidget*>(
        QStringLiteral("paymasterFinanceDailyTotals"));
    QWidget* details = tab.findChild<QWidget*>(
        QStringLiteral("paymasterFinanceDetailsPanel"));
    QPushButton* details_toggle = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceDetailsToggle"));
    QGroupBox* backup_notice = tab.findChild<QGroupBox*>(
        QStringLiteral("paymasterOverviewBackupNotice"));
    QLabel* backup_provider = tab.findChild<QLabel*>(
        QStringLiteral("paymasterOverviewBackupProviderId"));
    QPushButton* backup_now = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterOverviewBackupNow"));
    QPushButton* external_backup = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterOverviewExternalBackup"));
    QPushButton* finance_refresh = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceRefresh"));
    QPushButton* finance_export = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterFinanceExport"));
    QComboBox* period = tab.findChild<QComboBox*>(
        QStringLiteral("paymasterFinancePeriod"));
    QLabel* history_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterFinanceHistoryStatus"));
    auto* selected_summary = tab.findChild<QLabel*>(QStringLiteral("paymasterFinanceSelectedSummary"));
    QVERIFY(selected_summary);
    QVERIFY(selected_summary->text().contains(QStringLiteral("1.25 DD")));
    auto* overview_finance = tab.findChild<QLabel*>("paymasterOverviewFinanceStatus");
    QVERIFY(overview_finance);
    // The detail page is 30d, but the overview stays explicitly all-time.
    QVERIFY(overview_finance->text().contains("All time · confirmed"));
    auto* overview_income = tab.findChild<QLabel*>("paymasterCapital_income");
    QVERIFY(overview_income);
    QVERIFY(overview_income->text().contains("2.50 DD"));
    QVERIFY(period_income != nullptr);
    QVERIFY(result_estimate != nullptr);
    QVERIFY(model_breakdown_label != nullptr);
    QVERIFY(pool_dgb != nullptr);
    QVERIFY(history_notice != nullptr);
    QVERIFY(history_notice_text != nullptr);
    QVERIFY(booking_table != nullptr);
    QVERIFY(daily_table != nullptr);
    QVERIFY(details != nullptr);
    QVERIFY(details_toggle != nullptr);
    QVERIFY(!tab.findChild<QWidget*>(QStringLiteral("paymasterFinanceBackupNotice")));
    QVERIFY(!tab.findChild<QPushButton*>(QStringLiteral("paymasterFinanceBackupNow")));
    QVERIFY(backup_notice != nullptr);
    QVERIFY(backup_provider != nullptr);
    QVERIFY(backup_now != nullptr);
    QVERIFY(external_backup != nullptr);
    QVERIFY(finance_refresh != nullptr);
    QVERIFY(finance_export != nullptr);
    QVERIFY(period != nullptr);
    QVERIFY(history_status != nullptr);
    period_control = period;
    QCOMPARE(QString::fromStdString(
                 parameters.front()[0].find_value("period").get_str()),
             QStringLiteral("30d"));
    QCOMPARE(parameters.front()[0].find_value("include_events").get_bool(),
             true);
    QCOMPARE(parameters.front()[0].find_value("limit").getInt<int>(), 250);
    QVERIFY(period_income->text().contains(QStringLiteral("Service fees")));
    QVERIFY(result_estimate->text().contains(QStringLiteral("USD")));
    QVERIFY(model_breakdown_label->text().contains(
        QStringLiteral("User paid: 3 transfer")));
    QVERIFY(model_breakdown_label->text().contains(
        QStringLiteral("0.00060000 DGB cost")));
    QVERIFY(pool_dgb->text().contains(QStringLiteral("available")));
    QVERIFY(history_notice->isHidden());
    QCOMPARE(booking_table->rowCount(), 1);
    QCOMPARE(booking_table->columnCount(), 6);
    QCOMPARE(daily_table->rowCount(), 1);
    QCOMPARE(daily_table->columnCount(), 5);
    QCOMPARE(daily_table->item(0, 1)->text(), QStringLiteral("0.25"));
    QCOMPARE(daily_table->item(0, 4)->text(), QStringLiteral("2"));
    QVERIFY(daily_table->item(0, 1)->textAlignment() & Qt::AlignRight);
    QVERIFY(booking_table->item(0, 3)->textAlignment() & Qt::AlignRight);
    QVERIFY(booking_table->item(0, 4)->textAlignment() & Qt::AlignRight);
    // The fixture remains hidden. Polish its ancestor hierarchy as showing
    // the real window would, before testing stylesheet changes on its tables.
    tab.ensurePolished();
    for (const auto& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
        QFile css(":/css/" + theme);
        QVERIFY(css.open(QIODevice::ReadOnly));
        tab.setStyleSheet(QString::fromUtf8(css.readAll()));
        for (auto* table : {daily_table, booking_table}) {
            table->ensurePolished();
            QCOMPARE(table->palette().color(QPalette::Base).name(),
                     theme == QLatin1String("dark") ? QStringLiteral("#09251a") : QStringLiteral("#ffffff"));
            QCOMPARE(table->palette().color(QPalette::Text).name(),
                     theme == QLatin1String("dark") ? QStringLiteral("#e4f8ec") : QStringLiteral("#163e28"));
            const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_FINANCE_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                table->setRowCount(2);
                for (int column = 0; column < table->columnCount(); ++column)
                    table->setItem(1, column, table->item(0, column)->clone());
                table->resize(1050, 220);
                QVERIFY(table->grab().save(screenshot + "-" + theme + "-" + table->objectName() + ".png"));
                table->setRowCount(1);
            }
        }
    }
    finance_page->setEnabled(true);
    finance_page->setProperty("paymasterPageAvailable", true); // This finance-only fixture has no configured provider.
    QVERIFY(details->isHidden());
    details_toggle->click();
    QVERIFY(!details->isHidden());
    QVERIFY(!backup_notice->isHidden());
    QVERIFY(backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(!backup_provider->text().contains(QStringLiteral("…")));
    tab.setPrivacy(true);
    QVERIFY(!backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(backup_provider->toolTip().isEmpty());
    QVERIFY(backup_provider->accessibleName().isEmpty());
    QVERIFY(backup_provider->accessibleDescription().isEmpty());
    QVERIFY(!selected_summary->text().contains(QStringLiteral("1.25")));
    QVERIFY(booking_table->isHidden());
    QVERIFY(daily_table->isHidden());
    QVERIFY(!finance_export->isEnabled());
    tab.setPrivacy(false);
    // This isolated fixture has no configured provider, so the normal setup
    // access refresh closes the tab that the test opened manually above.
    // Reopen only that presentation surface before checking cached export.
    finance_page->setEnabled(true);
    finance_page->setProperty("paymasterPageAvailable", true);
    operator_tabs->setCurrentWidget(finance_page);

    QVERIFY(backup_provider->text().contains(
        QString::fromStdString(provider_id)));
    QVERIFY(!booking_table->isHidden());
    QVERIFY(!daily_table->isHidden());
    QTRY_VERIFY(finance_export->isEnabled());
    commands.clear();
    parameters.clear();
    period->setCurrentIndex(period->findData(QStringLiteral("today")));
    QCOMPARE(commands.value(0),
             QStringLiteral("getpaymasterfinancestatus"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("period").get_str()),
             QStringLiteral("today"));
    const auto open_backup_settings = [&] {
        auto* settings = tab.findChild<QStackedWidget*>("paymasterSettingsPages");
        auto* connection = tab.findChild<QWidget*>("paymasterManagementPage");
        QVERIFY(settings && connection);
        operator_tabs->widget(operator_tabs->indexOf(settings))->setEnabled(true);
        operator_tabs->setCurrentWidget(settings);
        settings->setCurrentWidget(connection);
        QVERIFY(connection->isAncestorOf(backup_notice));
        QVERIFY(backup_now->isEnabled());
    };
    open_backup_settings();
    QSignalSpy backup_requested(
        &tab, &DigiDollarTab::providerWalletBackupRequested);
    backup_now->click();
    QCOMPARE(backup_requested.count(), 1);
    finance_page->setEnabled(true);
    finance_page->setProperty("paymasterPageAvailable", true);
    operator_tabs->setCurrentWidget(finance_page);

    oracle_available = false;
    finance_refresh->click();
    QVERIFY(result_estimate->text().contains(
        QStringLiteral("No current Oracle price")));
    partial_history = true;
    finance_refresh->click();
    QVERIFY(!history_notice->isHidden());
    QVERIFY(history_notice_text->text().contains(
        QStringLiteral("Earlier Paymaster transfers may be absent")));
    QVERIFY(history_notice_text->text().contains(
        QStringLiteral("no earlier income is estimated")));
    // If the selection changes during an asynchronous request, the old
    // period must never be rendered under the new combo-box label. The first
    // result is discarded and a replacement request is issued immediately.
    partial_history = false;
    oracle_available = true;
    period->setCurrentIndex(period->findData(QStringLiteral("30d")));
    commands.clear();
    parameters.clear();
    switch_period_during_request = true;
    finance_refresh->click();
    QCOMPARE(commands.size(), 2);
    QCOMPARE(QString::fromStdString(
                 parameters.at(0)[0].find_value("period").get_str()),
             QStringLiteral("30d"));
    QCOMPARE(QString::fromStdString(
                 parameters.at(1)[0].find_value("period").get_str()),
             QStringLiteral("today"));
    QCOMPARE(period->currentData().toString(), QStringLiteral("today"));
    QVERIFY(selected_summary->text().contains(QStringLiteral("0.25 DD")));
    malformed_finance = true;
    finance_refresh->click();
    QVERIFY(selected_summary->text().contains(QStringLiteral("incomplete")));
    QVERIFY(history_status->text().contains(
        QStringLiteral("incomplete"), Qt::CaseInsensitive));
    // Retain the last complete authoritative rows instead of replacing them
    // with plausible-looking zero totals from a malformed object.
    QCOMPARE(booking_table->rowCount(), 1);
    QCOMPARE(daily_table->rowCount(), 1);
    malformed_finance = false;
    const bool native_dialogs_available =
        QApplication::platformName() != QLatin1String("minimal") &&
        QApplication::platformName() != QLatin1String("offscreen");
    if (native_dialogs_available) {
        open_backup_settings();
        bool confirmation_seen{false};
        QTimer::singleShot(0, [&] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* confirmation = qobject_cast<QMessageBox*>(widget);
                if (!confirmation ||
                    confirmation->windowTitle() !=
                        QLatin1String("Acknowledge external wallet backup")) {
                    continue;
                }
                confirmation_seen = true;
                // Exercise the real confirmation: the static helper returns
                // clickedButton(), not the value supplied to done().
                QAbstractButton* yes = confirmation->button(QMessageBox::Yes);
                QVERIFY(yes != nullptr);
                yes->click();
                break;
            }
        });
        external_backup->click();
        QVERIFY(confirmation_seen);
        QVERIFY(!backup_notice->isHidden());
        QVERIFY(history_status->text().contains(
            QStringLiteral("not confirm"), Qt::CaseInsensitive));
    }

    // Technical event and transaction identifiers are intentionally absent
    // from the normal booking table and remain backend-only detail data.
    for (int column = 0; column < booking_table->columnCount(); ++column) {
        const QTableWidgetItem* item = booking_table->item(0, column);
        QVERIFY(item != nullptr);
        QVERIFY(!item->text().contains(QString(16, QLatin1Char('b'))));
        QVERIFY(!item->text().contains(QString(16, QLatin1Char('c'))));
    }

    QTemporaryDir export_directory;
    QVERIFY(export_directory.isValid());
    const QString export_filename =
        export_directory.filePath(QStringLiteral("paymaster-finance.csv"));
    tab.setPaymasterFinanceExportFilenameForTesting(export_filename);
    large_export = true;
    commands.clear();
    parameters.clear();
    bool event_loop_yielded{false};
    int writing_heartbeats{0};
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (history_status->text().contains(QStringLiteral("Writing complete export"))) ++writing_heartbeats;
    });
    heartbeat.start();
    QTimer::singleShot(0, &tab,
                       [&event_loop_yielded] { event_loop_yielded = true; });
    finance_export->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        history_status->text().contains(
            QStringLiteral("10251 booking(s)")),
        30000);
    QVERIFY(event_loop_yielded);
    QVERIFY2(writing_heartbeats > 0, "Qt must process events during final CSV serialization and writing");
    heartbeat.stop();
    QCOMPARE(commands.count(QStringLiteral("getpaymasterfinancestatus")),
             42);
    QCOMPARE(commands.size(), static_cast<int>(parameters.size()));
    for (size_t index = 0; index < parameters.size(); ++index) {
        // Provider refreshes may legitimately be queued between export pages.
        // Only the finance calls participate in the cursor contract below.
        if (commands.at(static_cast<int>(index)) !=
            QLatin1String("getpaymasterfinancestatus")) {
            continue;
        }
        const UniValue& page_parameters = parameters.at(index);
        QVERIFY2(page_parameters.isArray() && page_parameters.size() == 1,
                 qPrintable(QStringLiteral(
                     "Finance export RPC %1 has malformed parameters")
                                .arg(index)));
        const UniValue& limit = page_parameters[0].find_value("limit");
        QVERIFY2(limit.isNum(),
                 qPrintable(QStringLiteral(
                     "Finance export RPC %1 omitted its page limit")
                                .arg(index)));
        QCOMPARE(limit.getInt<int>(), 250);
    }
    QFile export_file(export_filename);
    QVERIFY(export_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray exported = export_file.readAll();
    QCOMPARE(exported.count('\n'),
             static_cast<int>(LARGE_EXPORT_EVENTS + 1));
}

void PaymasterWidgetTests::paymasterClientConfirmationRequiresObservation()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test, "qt-paymaster-confirmation-observation");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(mini_gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    QVERIFY(client);
    UniValue snapshot = PaymasterSessionView("MEMPOOL", "final_transaction", "MEMPOOL");
    QStringList dialogs;
    QStringList calls;
    form.setDialogHandlerForTesting([&](QMessageBox::Icon, const QString& title, const QString&,
                                       QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        dialogs.push_back(title);
        return QMessageBox::Ok;
    });
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue&) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        calls.push_back(QString::fromStdString(command));
        if (command == "resolvepaymastersession") return snapshot;
        throw std::runtime_error("unexpected payment mutation during observation");
    });
    form.setWalletModel(mini_gui.walletModel.get());
    const QString recipient = QString::fromStdString(
        snapshot.find_value("session").find_value("to_address").get_str());
    client->setPaymasterSessionForTesting(QStringLiteral("MEMPOOL"), QStringLiteral("final_transaction"),
        true, recipient, 3.25, QStringLiteral("MEMPOOL"));
    dialogs.clear();
    calls.clear();
    QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(dialogs.isEmpty());
    QCOMPARE(calls, QStringList{QStringLiteral("resolvepaymastersession")});
    snapshot = PaymasterSessionView("CONFIRMED", "final_transaction", "MEMPOOL");
    UniValue missing = snapshot.find_value("session");
    missing.pushKV("payment_confirmed", false);
    missing.pushKV("status", "pending");
    missing.pushKV("confirmation_state", "unconfirmed");
    snapshot.pushKV("session", missing);
    QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(dialogs.contains(QStringLiteral("Paymaster payment status unavailable")));
    QVERIFY(!dialogs.contains(QStringLiteral("Payment sent")));
    QCOMPARE(calls.size(), 2);
}

void PaymasterWidgetTests::paymasterConfirmationGuardDetectsMaterialChanges()
{
    // A provider result proves submission, not recipient confirmation.
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("final_committed")));
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CONFIRMED"),
        QStringLiteral("success"), QStringLiteral("final_committed"), true, true));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CONFIRMED"),
        QStringLiteral("success"), QStringLiteral("final_committed"), true, false));
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CONFIRMED"),
        QStringLiteral("success"), QString{}, true, true));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CONFIRMED"),
        QStringLiteral("success"), QString{}, true, false));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CONFIRMED"),
        QStringLiteral("success"), QStringLiteral("unknown_result"), true, true));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("PENDING_PROVIDER"),
        QStringLiteral("success"), QString{}, true, true));
    QVERIFY(IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QString{}, QStringLiteral("success"),
        QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("FAILED"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString{}, QStringLiteral("MEMPOOL"), QString{},
        QStringLiteral("broadcast_attempted")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString{}, QString{}, QStringLiteral("success"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('0')), QString{}, QStringLiteral("success"),
        QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QStringLiteral("not-a-transaction-id"), QString{},
        QStringLiteral("success"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QString{},
        QStringLiteral("unexpected"), QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("PENDING_PROVIDER"),
        QString{}, QString{}));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('0')), QStringLiteral("CONFIRMED"),
        QString{}, QStringLiteral("final_committed")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CONFLICTED"),
        QString{}, QStringLiteral("final_committed")));
    QVERIFY(!IsValidatedPaymasterCompletion(
        QString(64, QLatin1Char('1')), QStringLiteral("CANCELED_SAFE"),
        QString{}, QStringLiteral("final_committed")));

    // `final` is a persistence/terminal marker, not a payment-success marker.
    // Exercise every terminal state with both marker values and both txid
    // shapes so a future decoder cannot accidentally reintroduce that trust.
    const QString valid_txid(64, QLatin1Char('7'));
    const QStringList terminal_states{
        QStringLiteral("MEMPOOL"), QStringLiteral("CONFIRMED"),
        QStringLiteral("FAILED"), QStringLiteral("CONFLICTED"),
        QStringLiteral("CANCELED_SAFE")};
    for (const QString& terminal_state : terminal_states) {
        for (const bool reported_final : {false, true}) {
            for (const bool has_txid : {false, true}) {
                const bool expected = false; // no local confirmation evidence
                QCOMPARE(IsValidatedPaymasterCompletion(
                             has_txid ? valid_txid : QString{},
                             terminal_state, QString{},
                             QStringLiteral("final_committed"),
                             reported_final),
                         expected);
            }
        }
    }
    for (const bool reported_final : {false, true}) {
        QCOMPARE(IsValidatedPaymasterCompletion(
                     valid_txid, QString{}, QStringLiteral("success"),
                     QString{}, reported_final),
                 true);
        QCOMPARE(IsValidatedPaymasterCompletion(
                     QString{}, QString{}, QStringLiteral("success"),
                     QString{}, reported_final),
                 false);
    }

    PaymasterConfirmationGuard guard;
    PaymasterConfirmationSelection incomplete;
    QVERIFY(guard.RequiresConfirmation(incomplete));
    QCOMPARE(guard.ChangedFields(incomplete), QStringList{QStringLiteral("incomplete")});
    QVERIFY(!guard.Accept(incomplete));
    QVERIFY(!guard.HasAcceptedSelection());

    PaymasterConfirmationSelection accepted;
    accepted.provider_id = QString(64, QLatin1Char('1'));
    accepted.offer_id = QString(64, QLatin1Char('2'));
    accepted.policy_hash = QString(64, QLatin1Char('3'));
    accepted.funding_model = QStringLiteral("user_paid");
    accepted.recipient = QStringLiteral("dgb1-recipient");
    accepted.authorization_commitment = QString(64, QLatin1Char('4'));
    accepted.payment_cents = 1250;
    accepted.service_fee_cents = 25;
    accepted.user_total_cents = 1275;
    accepted.expires_at = 2000000000;
    QVERIFY(guard.RequiresConfirmation(accepted));
    QCOMPARE(guard.ChangedFields(accepted),
              QStringList({QStringLiteral("provider"), QStringLiteral("offer"),
                           QStringLiteral("policy"), QStringLiteral("funding_model"),
                           QStringLiteral("recipient"), QStringLiteral("amount"),
                           QStringLiteral("service_fee"),
                           QStringLiteral("total"),
                           QStringLiteral("expiry"),
                           QStringLiteral("authorization_commitment")}));
    QVERIFY(guard.Accept(accepted));
    QVERIFY(guard.HasAcceptedSelection());
    QVERIFY(!guard.RequiresConfirmation(accepted));
    QVERIFY(guard.ChangedFields(accepted).isEmpty());

    PaymasterConfirmationSelection changed = accepted;
    changed.provider_id = QString(64, QLatin1Char('5'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("provider")});
    changed = accepted;
    changed.offer_id = QString(64, QLatin1Char('6'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("offer")});
    changed = accepted;
    changed.policy_hash = QString(64, QLatin1Char('7'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("policy")});
    changed = accepted;
    changed.funding_model = QStringLiteral("sponsored");
    changed.service_fee_cents = 0;
    changed.user_total_cents = changed.payment_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("funding_model"),
                          QStringLiteral("service_fee"),
                          QStringLiteral("total")}));
    changed = accepted;
    changed.recipient = QStringLiteral("dgb1-other");
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("recipient")});
    changed = accepted;
    ++changed.payment_cents;
    ++changed.user_total_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("amount"), QStringLiteral("total")}));
    changed = accepted;
    ++changed.service_fee_cents;
    ++changed.user_total_cents;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList({QStringLiteral("service_fee"), QStringLiteral("total")}));
    changed = accepted;
    ++changed.user_total_cents;
    QVERIFY(!changed.IsComplete());
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("incomplete")});
    changed = accepted;
    changed.funding_model = QStringLiteral("sponsored");
    QVERIFY(!changed.IsComplete());
    changed = accepted;
    ++changed.expires_at;
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed), QStringList{QStringLiteral("expiry")});
    changed = accepted;
    changed.authorization_commitment = QString(64, QLatin1Char('8'));
    QVERIFY(guard.RequiresConfirmation(changed));
    QCOMPARE(guard.ChangedFields(changed),
             QStringList{QStringLiteral("authorization_commitment")});

    guard.Reset();
    QVERIFY(!guard.HasAcceptedSelection());
    QVERIFY(guard.RequiresConfirmation(accepted));
}

void PaymasterWidgetTests::paymasterRecoveryConfirmationGuardDetectsMaterialChanges()
{
    PaymasterRecoveryConfirmationGuard guard;
    PaymasterRecoveryConfirmationSelection incomplete;
    QVERIFY(guard.RequiresConfirmation(incomplete));
    QCOMPARE(guard.ChangedFields(incomplete),
             QStringList{QStringLiteral("incomplete")});
    QVERIFY(!guard.Accept(incomplete));
    QVERIFY(!guard.HasAcceptedSelection());

    PaymasterRecoveryConfirmationSelection accepted;
    accepted.recovery_provider_id = QString(64, QLatin1Char('1'));
    accepted.privacy_profile = QStringLiteral("high");
    accepted.offer_id = QString(64, QLatin1Char('2'));
    accepted.policy_hash = QString(64, QLatin1Char('3'));
    accepted.original_commit_key = QString(64, QLatin1Char('4'));
    accepted.original_template_commitment =
        QString(64, QLatin1Char('5'));
    accepted.wallet_returns = QStringList{
        QStringLiteral("0:wallet-script-a:1000"),
        QStringLiteral("1:wallet-script-b:250")};
    accepted.authorization_commitment = QString(64, QLatin1Char('6'));
    accepted.maximum_service_fee_cents = 25;
    accepted.service_fee_cents = 20;
    accepted.network_fee_satoshis = 1500;
    accepted.expires_at = 2000000000;

    QVERIFY(guard.RequiresConfirmation(accepted));
    QCOMPARE(guard.ChangedFields(accepted),
             QStringList({QStringLiteral("recovery_provider"),
                          QStringLiteral("privacy"), QStringLiteral("offer"),
                          QStringLiteral("policy"),
                          QStringLiteral("original_commit"),
                          QStringLiteral("original_template_commitment"),
                          QStringLiteral("wallet_returns"),
                          QStringLiteral("maximum_service_fee"),
                          QStringLiteral("service_fee"),
                          QStringLiteral("network_fee"),
                          QStringLiteral("expiry"),
                          QStringLiteral("authorization_commitment")}));
    QVERIFY(guard.Accept(accepted));
    QVERIFY(guard.HasAcceptedSelection());
    QVERIFY(!guard.RequiresConfirmation(accepted));
    QVERIFY(guard.ChangedFields(accepted).isEmpty());

    const auto expect_change = [&guard, &accepted](
                                   PaymasterRecoveryConfirmationSelection changed,
                                   const QString& field) {
        QVERIFY(guard.RequiresConfirmation(changed));
        QCOMPARE(guard.ChangedFields(changed), QStringList{field});
        QVERIFY(!changed.IsComplete() || guard.HasAcceptedSelection());
        QVERIFY(!guard.RequiresConfirmation(accepted));
    };

    PaymasterRecoveryConfirmationSelection changed = accepted;
    changed.recovery_provider_id = QString(64, QLatin1Char('7'));
    expect_change(changed, QStringLiteral("recovery_provider"));
    changed = accepted;
    changed.privacy_profile = QStringLiteral("standard");
    expect_change(changed, QStringLiteral("privacy"));
    changed = accepted;
    changed.offer_id = QString(64, QLatin1Char('8'));
    expect_change(changed, QStringLiteral("offer"));
    changed = accepted;
    changed.policy_hash = QString(64, QLatin1Char('9'));
    expect_change(changed, QStringLiteral("policy"));
    changed = accepted;
    changed.original_commit_key = QString(64, QLatin1Char('a'));
    expect_change(changed, QStringLiteral("original_commit"));
    changed = accepted;
    changed.original_template_commitment = QString(64, QLatin1Char('b'));
    expect_change(changed, QStringLiteral("original_template_commitment"));
    changed = accepted;
    changed.wallet_returns[0] = QStringLiteral("0:other-wallet-script:1000");
    expect_change(changed, QStringLiteral("wallet_returns"));
    changed = accepted;
    ++changed.maximum_service_fee_cents;
    expect_change(changed, QStringLiteral("maximum_service_fee"));
    changed = accepted;
    ++changed.service_fee_cents;
    expect_change(changed, QStringLiteral("service_fee"));
    changed = accepted;
    ++changed.network_fee_satoshis;
    expect_change(changed, QStringLiteral("network_fee"));
    changed = accepted;
    ++changed.expires_at;
    expect_change(changed, QStringLiteral("expiry"));
    changed = accepted;
    changed.authorization_commitment = QString(64, QLatin1Char('c'));
    expect_change(changed, QStringLiteral("authorization_commitment"));

    guard.Reset();
    QVERIFY(!guard.HasAcceptedSelection());
    QVERIFY(guard.RequiresConfirmation(accepted));
}

void PaymasterWidgetTests::paymasterClientOfferPreviewUsesExactRpcAndPlainText()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-offers");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    std::vector<UniValue> parameters;
    int offer_calls{0};
    bool waiting_seen{false};
    bool progress_seen{false};
    bool refresh_disabled_while_busy{false};
    bool stale_rows_cleared_before_rpc{false};
    bool duplicate_refresh_blocked{false};
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            parameters.push_back(params);
            if (command == "getpaymasteroffers") {
                ++offer_calls;
                QLabel* status = send_widget.findChild<QLabel*>(
                    QStringLiteral("paymasterOffersStatus"));
                QPushButton* refresh = send_widget.findChild<QPushButton*>(
                    QStringLiteral("refreshPaymasterOffers"));
                QTableWidget* table = send_widget.findChild<QTableWidget*>(
                    QStringLiteral("paymasterOffers"));
                waiting_seen = status &&
                               status->property("statusKind").toString() == QStringLiteral("waiting");
                auto* icon = send_widget.findChild<QLabel*>(QStringLiteral("paymasterOfferStateIcon"));
                progress_seen = icon && !icon->isHidden() && !icon->pixmap(Qt::ReturnByValue).isNull() &&
                                refresh->text() == QStringLiteral("Checking…");
                refresh_disabled_while_busy = refresh && !refresh->isEnabled();
                stale_rows_cleared_before_rpc = table && table->rowCount() == 0;
                const int calls_before_reentry = offer_calls;
                QMetaObject::invokeMethod(
                    send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection);
                duplicate_refresh_blocked = offer_calls == calls_before_reentry;

                UniValue offers{UniValue::VARR};
                offers.push_back(PaymasterOffer(
                    "<b>Alice & Co</b>",
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                    "user_paid",
                    1, 325, true, 2000000000, 50, 1));
                offers.push_back(PaymasterOffer(
                    "Sponsor",
                    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                    "sponsored",
                    0, 325, false, 2000000100));
                return offers;
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    commands.clear();
    parameters.clear();

    QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingPaymaster"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QVERIFY(paymaster != nullptr);
    QVERIFY(amount != nullptr);
    QVERIFY(table != nullptr);
    QVERIFY(status != nullptr);

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    table->setRowCount(1); // Must disappear before the next RPC is entered.
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));

    QCOMPARE(offer_calls, 1);
    QVERIFY(waiting_seen);
    QVERIFY(progress_seen);
    QVERIFY(refresh_disabled_while_busy);
    QVERIFY(stale_rows_cleared_before_rpc);
    QVERIFY(duplicate_refresh_blocked);
    QCOMPARE(commands, QStringList{QStringLiteral("getpaymasteroffers")});
    QCOMPARE(parameters.size(), size_t{1});
    QVERIFY(parameters.front().isArray());
    QCOMPARE(parameters.front().size(), size_t{2});
    QCOMPARE(parameters.front()[0].getInt<int64_t>(), int64_t{325});
    const UniValue& preview_options = parameters.front()[1];
    QVERIFY(preview_options.isObject());
    QCOMPARE(preview_options.find_value(
                 "subtract_paymaster_fee_from_amount").get_bool(), false);
    QCOMPARE(preview_options.find_value(
                 "maximum_paymaster_fee_cents").getInt<int64_t>(),
             int64_t{100});

    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("<b>Alice & Co</b>"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Service fee in $DD"));
    QCOMPARE(table->item(0, 2)->text(),
             QStringLiteral("0.01 $DD (≈ 0.31%) · max 0.01 $DD"));
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("3.26 $DD"));
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("98.75%"));
    QVERIFY(table->item(0, 5)->text().contains(QStringLiteral("2033")));
    QVERIFY(table->item(0, 0)->toolTip().contains(QString(64, QLatin1Char('b'))));
    QCOMPARE(table->item(1, 1)->text(), QStringLiteral("Sponsored by provider"));
    QCOMPARE(table->item(1, 4)->text(),
             QStringLiteral("New provider — not enough history yet"));
    // The checkmark denotes finding public offers, not payment authorization.
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("ready"));
    QCOMPARE(status->text(), QStringLiteral("Public Paymaster offers found: 2"));
    auto* updated = send_widget.findChild<QLabel*>(QStringLiteral("paymasterOffersUpdated"));
    auto* help = send_widget.findChild<QLabel*>(QStringLiteral("paymasterOfferCheckHelp"));
    QVERIFY(updated && help);
    QVERIFY(updated->text().contains(QStringLiteral("List updated:")));
    QVERIFY(updated->toolTip().contains(QStringLiteral("up to 10 minutes")));
    QVERIFY(help->text().contains(QStringLiteral("Connection not checked yet")));
    QVERIFY(!status->text().contains(QStringLiteral("List updated:")));
    QVERIFY(help->text().contains(QStringLiteral("before signing")));
    QVERIFY(help->toolTip().contains(QStringLiteral("can still fail")));
    QCOMPARE(status->accessibleDescription(), status->text());
    auto* icon = send_widget.findChild<QLabel*>(QStringLiteral("paymasterOfferStateIcon"));
    auto* animation = send_widget.findChild<QTimer*>(QStringLiteral("paymasterOfferIconTimer"));
    auto* frame = send_widget.findChild<QFrame*>(QStringLiteral("paymasterOfferCheckFrame"));
    auto* advanced = send_widget.findChild<QFrame*>(QStringLiteral("advancedPaymasterSettings"));
    auto* refresh = send_widget.findChild<QPushButton*>(QStringLiteral("refreshPaymasterOffers"));
    QVERIFY(frame && advanced && icon && animation && refresh);
    QVERIFY(advanced->isHidden());
    QVERIFY(!frame->isHidden()); // Search status is visible without expert settings.
    QCOMPARE(icon->text(), QStringLiteral("✓"));
    QVERIFY(!animation->isActive());
    QCOMPARE(icon->accessibleName(), status->text());
    QCOMPARE(refresh->text(), QStringLiteral("Refresh offers"));
    const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_OFFERS_SCREENSHOT");
    for (const QString& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
        QFile css(":/css/" + theme);
        QVERIFY(css.open(QIODevice::ReadOnly));
        send_widget.setStyleSheet(QString::fromUtf8(css.readAll()));
        for (int width : {900, 1700}) {
            send_widget.resize(width, 1100);
            send_widget.show();
            QCoreApplication::processEvents();
            QVERIFY(refresh->width() < frame->width() / 3);
            QVERIFY(status->geometry().right() < refresh->geometry().left());
            QVERIFY(icon->geometry().right() < status->geometry().left());
            QVERIFY(status->geometry().bottom() < help->geometry().top());
            QVERIFY(help->geometry().bottom() < updated->geometry().top());
            QVERIFY(status->font().pointSizeF() >= help->font().pointSizeF() + 4);
            QVERIFY(status->height() >= status->heightForWidth(status->width()));
            QVERIFY(help->height() >= help->heightForWidth(help->width()));
            QVERIFY(!help->font().bold());
            QVERIFY(!updated->font().bold());
            if (!screenshot.isEmpty()) {
                const auto* safety = send_widget.findChild<QFrame*>(QStringLiteral("paymasterClientSafetyFrame"));
                const QRect area(frame->mapTo(&send_widget, QPoint(0, 0)),
                                 QSize(frame->width(), safety->mapTo(&send_widget, QPoint(0, safety->height())).y() - frame->mapTo(&send_widget, QPoint(0, 0)).y()));
                QVERIFY(send_widget.grab(area).save(screenshot + "-" + theme + "-" + QString::number(width) + ".png"));
            }
        }
    }
    send_widget.hide();
    auto* client = send_widget.findChild<PaymasterSendWidget*>();
    client->setPrivacy(true);
    QVERIFY(icon->isHidden());
    QVERIFY(icon->text().isEmpty());
    QVERIFY(icon->accessibleName().isEmpty());
    QVERIFY(!updated->text().contains(QStringLiteral("List updated:")));
    QVERIFY(updated->toolTip().isEmpty());
    client->setPrivacy(false);
    QCOMPARE(icon->text(), QStringLiteral("✓"));
    QVERIFY(updated->text().contains(QStringLiteral("List updated:")));
    QVERIFY(QMetaObject::invokeMethod(send_widget.findChild<PaymasterSendWidget*>(),
                                      "expirePaymasterOfferPreview", Qt::DirectConnection));
    QCOMPARE(offer_calls, 1); // Expiry invalidates locally; it does not search or send.
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(status->text().contains(QStringLiteral("expired")));
    QVERIFY(!updated->text().contains(QStringLiteral("List updated:")));
    QVERIFY(updated->toolTip().isEmpty());
    QVERIFY(refresh->isEnabled());
}

void PaymasterWidgetTests::paymasterClientOfferPreviewInvalidatesAndHandlesFailures()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-offer-states");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    int offer_response{0};
    QLineEdit* in_flight_amount{nullptr};
    bool switch_wallet_during_request{false};
    QString dialog_title;
    QString dialog_message;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& title, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            dialog_title = title;
            dialog_message = message;
            return QMessageBox::Ok;
        });
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command != "getpaymasteroffers") {
                throw std::runtime_error("unexpected Paymaster RPC");
            }
            if (offer_response == 2) {
                throw std::runtime_error("preview transport failed");
            }
            if (offer_response == 3) {
                UniValue malformed{UniValue::VARR};
                UniValue offer{UniValue::VOBJ};
                offer.pushKV("display_name", "Malformed provider");
                offer.pushKV("funding_model", "user_paid");
                offer.pushKV("service_fee_cents", 2);
                // Missing payment, total, reputation, expiry and bindings.
                malformed.push_back(std::move(offer));
                return malformed;
            }
            if (offer_response == 4 && in_flight_amount) {
                // Simulate a user edit while the production asynchronous RPC
                // is in flight. The returned offer is bound to the old amount
                // and must therefore remain invisible.
                in_flight_amount->setText(QStringLiteral("3.26"));
            }
            if (offer_response == 5 && switch_wallet_during_request) {
                // A late reply owned by the previously selected wallet must
                // not repopulate the reset state after a wallet switch.
                switch_wallet_during_request = false;
                send_widget.setWalletModel(nullptr);
                send_widget.setWalletModel(mini_gui.walletModel.get());
            }
            UniValue offers{UniValue::VARR};
            if (offer_response == 0 || offer_response == 4 ||
                offer_response == 5) {
                offers.push_back(PaymasterOffer(
                    "Provider",
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                    "user_paid",
                    2, 325, true, 2000000000));
            }
            return offers;
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());

    QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingPaymaster"));
    QRadioButton* direct_dgb = send_widget.findChild<QRadioButton*>(
        QStringLiteral("feeFundingDgb"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QCheckBox* subtract = send_widget.findChild<QCheckBox*>(
        QStringLiteral("subtractPaymasterFeeFromAmount"));
    QSpinBox* fee_cap = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterFeeCap"));
    QSpinBox* attempts = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterMaximumAttempts"));
    QComboBox* privacy = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterPrivacy"));
    QComboBox* selection = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterSelection"));
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QPushButton* refresh = send_widget.findChild<QPushButton*>(
        QStringLiteral("refreshPaymasterOffers"));
    QVERIFY(paymaster && direct_dgb && amount && subtract && fee_cap && attempts);
    QVERIFY(privacy && selection && table && status && refresh);
    in_flight_amount = amount;

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 1);

    const auto seed_stale_row = [&] {
        table->setRowCount(1);
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("stale")));
    };
    amount->setText(QStringLiteral("3.26"));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("info"));
    seed_stale_row();
    subtract->setChecked(true);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    fee_cap->setValue(fee_cap->value() - 1);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    privacy->setCurrentIndex(privacy->findData(QStringLiteral("high")));
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    selection->setCurrentIndex(
        selection->findData(QStringLiteral("privacy_weighted")));
    QCOMPARE(table->rowCount(), 0);
    privacy->setCurrentIndex(privacy->findData(QStringLiteral("standard")));
    seed_stale_row();
    attempts->setValue(2);
    QCOMPARE(table->rowCount(), 0);
    seed_stale_row();
    direct_dgb->setChecked(true);
    QCOMPARE(table->rowCount(), 0);

    paymaster->setChecked(true);
    amount->setText(QStringLiteral("3.25"));
    subtract->setChecked(false);
    offer_response = 1;
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("info"));
    QVERIFY(status->text().contains(QStringLiteral("No public offer for this amount yet.")));
    QCOMPARE(refresh->text(), QStringLiteral("Check again"));
    QVERIFY(!send_widget.findChild<QTimer*>(QStringLiteral("paymasterOfferIconTimer"))->isActive());

    offer_response = 2;
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("error"));
    QVERIFY(refresh->isEnabled());
    QCOMPARE(dialog_title, QStringLiteral("Paymaster offers unavailable"));
    QCOMPARE(dialog_message, QStringLiteral("preview transport failed"));
    QCOMPARE(refresh->text(), QStringLiteral("Try again"));
    QVERIFY(!send_widget.findChild<QTimer*>(QStringLiteral("paymasterOfferIconTimer"))->isActive());

    offer_response = 3;
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("error"));
    QVERIFY(status->text().contains(QStringLiteral("malformed")));
    QCOMPARE(dialog_title, QStringLiteral("Paymaster offers unavailable"));
    QVERIFY(dialog_message.contains(QStringLiteral("invalid Paymaster offer preview")));

    offer_response = 4;
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(amount->text(), QStringLiteral("3.26"));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(),
             QStringLiteral("info"));
    QVERIFY(status->text().contains(QStringLiteral("inputs changed"),
                                    Qt::CaseInsensitive));
    QVERIFY(refresh->isEnabled());

    offer_response = 5;
    switch_wallet_during_request = true;
    amount->setText(QStringLiteral("3.25"));
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(refresh->isEnabled());

    offer_response = 0;
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 1);

    seed_stale_row();
    send_widget.setWalletModel(nullptr);
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("info"));
    QVERIFY(!refresh->isEnabled());

    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [](const std::string& command, const UniValue&) {
            if (command != "getpaymasterclientsafetystatus") {
                throw std::runtime_error("unexpected client-safety RPC");
            }
            UniValue malformed{UniValue::VOBJ};
            malformed.pushKV("configured", true);
            malformed.pushKV("policy", UniValue{UniValue::VOBJ});
            return malformed;
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    QLabel* client_safety = send_widget.findChild<QLabel*>(
        QStringLiteral("sendPaymasterClientSafetyStatus"));
    QVERIFY(client_safety != nullptr);
    QVERIFY(client_safety->toolTip().contains(
        QStringLiteral("incomplete Paymaster client-safety status")));
}

void PaymasterWidgetTests::paymasterClientAuthorizationIsTwoStageAndFailClosed()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-authorization");
    const SecureString passphrase{"qt-paymaster-client-authorization"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    bool unlock_succeeded{false};
    QObject::connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock,
                     [&] {
                         unlock_succeeded =
                             mini_gui.walletModel->setWalletLocked(
                                 false, passphrase);
                     });

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QMessageBox::StandardButton authorization_answer{QMessageBox::Cancel};
    QStringList dialog_titles;
    std::vector<bool> wallet_locked_during_dialog;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& title, const QString&,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            dialog_titles.push_back(title);
            wallet_locked_during_dialog.push_back(wallet->IsLocked());
            return title == QStringLiteral("Confirm exact Paymaster authorization")
                ? authorization_answer
                : QMessageBox::Ok;
        });

    std::vector<UniValue> send_parameters;
    int send_calls{0};
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "resolvepaymastersession") {
                if (params[1].get_str() == "abandon_unsigned") {
                    auto result = PaymasterSessionView("FAILED", "none", "REJECTED");
                    result.pushKV("requires_attention", false);
                    return result;
                }
                return PaymasterSessionView(
                    "AWAITING_USER_SIGNATURE", "none", "QUOTED");
            }
            if (command != "senddigidollar") {
                throw std::runtime_error("unexpected Paymaster RPC");
            }
            ++send_calls;
            send_parameters.push_back(params);
            if (send_calls <= 2) {
                return PaymasterAuthorizationResult(
                    true, std::string(64, 'c'),
                    "AWAITING_USER_SIGNATURE", "none");
            }
            return PaymasterAuthorizationResult(
                false, std::string(64, 'f'),
                "PENDING_PROVIDER", "user_psbt");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("AWAITING_USER_SIGNATURE"), QStringLiteral("none"),
        true, QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("QUOTED"), QString{},
        {QStringLiteral("refresh"), QStringLiteral("resume")}, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QLabel* state = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionState"));
    QVERIFY(primary != nullptr);
    QVERIFY(state != nullptr);
    QCOMPARE(primary->text(), QStringLiteral("Review exact offer"));

    primary->click();
    QCOMPARE(send_calls, 1);
    QVERIFY(state->text().contains(QStringLiteral("canceled")));
    QCOMPARE(send_parameters.at(0)[5].get_str(), std::string{"cents"});
    const UniValue& canceled_options = send_parameters.at(0)[6];
    QCOMPARE(canceled_options.find_value("prepare_only").get_bool(), true);
    QVERIFY(canceled_options.find_value("authorization_commitment").isNull());

    authorization_answer = QMessageBox::Yes;
    // A new explicit order is needed after cancellation; the old quote cannot
    // silently be resumed and signed.
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("AWAITING_USER_SIGNATURE"), QStringLiteral("none"),
        true, QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("QUOTED"), QString{},
        {QStringLiteral("refresh"), QStringLiteral("resume")}, true);
    QCOMPARE(primary->text(), QStringLiteral("Review exact offer"));
    primary->click();
    QCOMPARE(send_calls, 3);
    const UniValue& reviewed_options = send_parameters.at(1)[6];
    const UniValue& authorized_options = send_parameters.at(2)[6];
    QCOMPARE(reviewed_options.find_value("prepare_only").get_bool(), true);
    QVERIFY(reviewed_options.find_value("authorization_commitment").isNull());
    QVERIFY(authorized_options.find_value("prepare_only").isNull());
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("authorization_commitment").get_str()),
             QString(64, QLatin1Char('c')));
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("selection").get_str()),
             QStringLiteral("lowest_total_cost"));
    QCOMPARE(QString::fromStdString(
                 authorized_options.find_value("privacy").get_str()),
             QStringLiteral("standard"));
    QVERIFY(state->text().contains(
        QStringLiteral("authorization blocked: commitment changed or missing")));
    QCOMPARE(dialog_titles.count(
                 QStringLiteral("Confirm exact Paymaster authorization")), 2);
    QVERIFY(dialog_titles.contains(QStringLiteral("Paymaster authorization blocked")));
    QVERIFY(unlock_succeeded);
    QVERIFY(!wallet_locked_during_dialog.empty());
    QVERIFY(std::all_of(wallet_locked_during_dialog.begin(),
                        wallet_locked_during_dialog.end(),
                        [](bool locked) { return locked; }));
    QVERIFY(wallet->IsLocked());
}

void PaymasterWidgetTests::paymasterClientLiveSendProgressesAcrossAsyncPhases_data()
{
    QTest::addColumn<QString>("outcome");
    for (const char* outcome : {"cancel_review", "empty_preview", "authorized", "stop", "rpc_error", "endpoint_unreachable", "session_disappeared", "quote_expires", "wallet_close", "auto_cancel", "auto_prepare", "privacy_prepare", "review_wallet_switch", "unlock_wallet_switch", "review_changed_controls"}) {
        QTest::newRow(outcome) << QString::fromLatin1(outcome);
    }
}

void PaymasterWidgetTests::paymasterClientLiveSendProgressesAcrossAsyncPhases()
{
    QFETCH(QString, outcome);
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test, "qt-paymaster-live-send");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    auto* client = send_widget.findChild<PaymasterSendWidget*>();
    QVERIFY(client);
    const bool approves = outcome == QStringLiteral("authorized") || outcome == QStringLiteral("review_changed_controls") || outcome == QStringLiteral("unlock_wallet_switch");
    const SecureString passphrase{"qt-paymaster-unlock-switch"};
    if (outcome == QStringLiteral("unlock_wallet_switch")) {
        QVERIFY(wallet->EncryptWallet(passphrase));
        QObject::connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock, &send_widget, [&] {
            QVERIFY(mini_gui.walletModel->setWalletLocked(false, passphrase));
            send_widget.setWalletModel(nullptr);
            send_widget.setWalletModel(mini_gui.walletModel.get());
        });
    }
    int reviews{0};
    QString failure_title;
    QString failure_message;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& title, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            if (title == QStringLiteral("Confirm exact Paymaster authorization")) {
                ++reviews;
                if (outcome == QStringLiteral("review_wallet_switch")) {
                    // Close and reopen the same model while the old approval
                    // dialog is still returning. Pointer equality is insufficient.
                    send_widget.setWalletModel(nullptr);
                    send_widget.setWalletModel(mini_gui.walletModel.get());
                    return QMessageBox::Yes;
                }
                if (outcome == QStringLiteral("review_changed_controls")) {
                    send_widget.findChild<QComboBox*>(QStringLiteral("paymasterFeeMode"))->setCurrentIndex(0);
                }
                return approves ? QMessageBox::Yes : QMessageBox::Cancel;
            }
            failure_title = title;
            failure_message = message;
            return QMessageBox::Yes;
        });
    std::vector<UniValue> sends;
    QString request_id;
    int refreshes{0};
    bool canceled{false};
    bool offer_available = outcome != QStringLiteral("empty_preview");
    client->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
            if (command == "getpaymasteroffers") return offer_available ? PaymasterPublicOffers() : UniValue{UniValue::VARR};
            if (command == "listdigidollarsendsessions") {
                return EmptyPaymasterSessionList();
            }
            if (command == "resolvepaymastersession") {
                ++refreshes;
                if (outcome == QStringLiteral("session_disappeared")) {
                    throw std::runtime_error("PAYMASTER_SESSION_NOT_FOUND: Paymaster session not found");
                }
                auto result = sends.size() >= 5 ? PaymasterSessionView("PENDING_PROVIDER", "user_psbt", "USER_SIGNED") : PaymasterSessionView("CREATED", "none", "", false);
                if (outcome == QStringLiteral("quote_expires") && sends.size() >= 4) {
                    result = PaymasterSessionView("FAILED", "none", "REJECTED");
                    result.pushKV("requires_attention", false);
                    // Core retains the historical inputs even after releasing
                    // reservations; requires_attention proves none remain live.
                    UniValue session = result.find_value("session");
                    UniValue inputs{UniValue::VARR};
                    UniValue input{UniValue::VOBJ};
                    input.pushKV("txid", std::string(64, 'a'));
                    input.pushKV("vout", 1);
                    inputs.push_back(input);
                    session.pushKV("reserved_user_inputs", inputs);
                    session.pushKV("provider_attempts", 1);
                    result.pushKV("session", session);
                }
                if (params[1].get_str() == "abandon_unsigned") canceled = true;
                if (canceled) {
                    result = PaymasterSessionView("FAILED", "none", "REJECTED");
                    result.pushKV("requires_attention", false);
                }
                UniValue session = result.find_value("session");
                session.pushKV("request_id", request_id.toStdString());
                result.pushKV("session", session);
                return result;
            }
            if (command != "senddigidollar") throw std::runtime_error("unexpected live-send RPC");
            sends.push_back(params);
            request_id = QString::fromStdString(params[6].find_value("request_id").get_str());
            if ((outcome == QStringLiteral("rpc_error") || outcome == QStringLiteral("endpoint_unreachable") || outcome == QStringLiteral("session_disappeared")) && sends.size() == 2) {
                throw std::runtime_error(outcome == QStringLiteral("endpoint_unreachable") ? "PAYMASTER_PROXY_OR_ENDPOINT_UNREACHABLE" : "PAYMASTER_DIRECT_CONNECTION_FAILED");
            }
            if (outcome == QStringLiteral("quote_expires") && sends.size() == 4) {
                throw std::runtime_error("PAYMASTER_NO_ELIGIBLE_OFFER");
            }
            const size_t phase = sends.size();
            auto result = PaymasterAuthorizationResult(
                phase == 4, phase >= 4 ? std::string(64, 'c') : "",
                phase == 1 ? "CREATED" : phase == 2 ? "INPUTS_RESERVED" :
                                     phase <= 4     ? "AWAITING_USER_SIGNATURE" :
                                     phase == 5     ? "PENDING_PROVIDER" :
                                                      "MEMPOOL",
                phase < 5 ? "none" : phase == 5 ? "user_psbt" :
                                                  "final_transaction");
            result.pushKV("request_id", request_id.toStdString());
            if (phase == 1) {
                result.pushKV("connection_pending", true);
                UniValue transport{UniValue::VOBJ};
                transport.pushKV("state", "connecting");
                result.pushKV("transport", transport);
            }
            if (phase == 6) {
                result.pushKV("txid", std::string(64, 'e'));
                result.pushKV("broadcast_state", "accepted_mempool");
            }
            // Match the real send RPC: artifact classification is supplied by
            // the authoritative refresh envelope, not the initial send reply.
            UniValue wire_result{UniValue::VOBJ};
            for (const auto& key : result.getKeys()) {
                if (key != "artifact" && !(phase == 3 && key == "authorization_required")) {
                    wire_result.pushKV(key, result.find_value(key));
                }
            }
            return wire_result;
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    auto* mode = send_widget.findChild<QComboBox*>(QStringLiteral("paymasterFeeMode"));
    QVERIFY(mode);
    const bool automatic = outcome == QStringLiteral("auto_cancel");
    if (automatic) {
        mini_gui.walletModel->pollBalanceChanged();
        QVERIFY(mini_gui.walletModel->getAvailableDGBBalance() > 0);
    }
    mode->setCurrentIndex(mode->findData(automatic || outcome == QStringLiteral("auto_prepare") ? QStringLiteral("auto") : QStringLiteral("paymaster")));
    QCOMPARE(send_widget.findChild<QPushButton*>(QStringLiteral("sendButton"))->text(),
             QStringLiteral("Send payment"));
    send_widget.findChild<QLineEdit*>("addressEdit")->setText(QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"));
    send_widget.findChild<QLineEdit*>("amountEdit")->setText(QStringLiteral("3.25"));
    send_widget.setAvailableDigiDollarBalanceForTesting(1000);
    if (outcome == QStringLiteral("empty_preview")) {
        QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection));
        QCOMPARE(send_widget.findChild<QTableWidget*>("paymasterOffers")->rowCount(), 0);
        QVERIFY(send_widget.findChild<QLabel*>("paymasterOffersStatus")->text().contains("No public offer"));
        auto* prepare = send_widget.findChild<QPushButton*>("sendButton");
        QVERIFY(!prepare->isEnabled());
        QVERIFY(prepare->toolTip().contains("Wait for a current Paymaster offer"));
        client->send(QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"), 325);
        QVERIFY(sends.empty()); // Empty preview cannot prepare, reserve or sign.
        offer_available = true;
    }
    if (!automatic) {
        QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection));
        QVERIFY(client->hasCurrentPaymasterOffer());
        QVERIFY(send_widget.findChild<QPushButton*>("sendButton")->isEnabled());
    }
    if (outcome == QStringLiteral("privacy_prepare")) client->setPrivacy(true);
    // Fail promptly if preparation still opens the old redundant confirmation.
    bool redundant_dialog{false};
    bool automatic_dialog{false};
    QTimer::singleShot(0, &send_widget, [&redundant_dialog, &automatic_dialog] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("paymasterOfferRequestConfirmation")) {
                redundant_dialog = true;
                if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->done(QMessageBox::Cancel);
            } else if (widget->objectName() == QStringLiteral("digiDollarSendConfirmation")) {
                automatic_dialog = true;
                if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->done(QMessageBox::Cancel);
            }
        }
    });
    client->send(QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"), 325);
    QCoreApplication::processEvents();
    QVERIFY(!redundant_dialog);
    if (automatic || outcome == QStringLiteral("privacy_prepare")) {
        QCOMPARE(automatic_dialog, automatic);
        QVERIFY(sends.empty());
        QCOMPARE(reviews, 0);
        return;
    }
    QVERIFY(!automatic_dialog);
    QCOMPARE(sends.size(), size_t{1});
    if (outcome == QStringLiteral("auto_prepare")) {
        QCOMPARE(sends[0][6].find_value("fee_mode").get_str(), std::string{"paymaster"});
        // DGB arriving during preparation cannot switch this request to direct spending.
        mini_gui.walletModel->pollBalanceChanged();
        QVERIFY(mini_gui.walletModel->getAvailableDGBBalance() > 0);
    }
    auto* next_step = send_widget.findChild<QLabel*>(QStringLiteral("paymasterSessionNextStep"));
    QVERIFY(next_step);
    QVERIFY(next_step->text().contains(QStringLiteral("Provider found")));
    QVERIFY(next_step->text().contains(QStringLiteral("automatically")));
    QVERIFY(next_step->text().contains(QStringLiteral("no refresh")));
    const auto poll = [&] { return QMetaObject::invokeMethod(client, "pollPaymasterSession", Qt::DirectConnection); };
    if (outcome == QStringLiteral("stop") || outcome == QStringLiteral("wallet_close")) {
        if (outcome == QStringLiteral("stop")) {
            QVERIFY(QMetaObject::invokeMethod(client, "cancelPaymasterQuote", Qt::DirectConnection));
        } else {
            send_widget.setWalletModel(nullptr);
        }
        QVERIFY(poll());
        QCOMPARE(sends.size(), size_t{1});
        return;
    }
    QVERIFY(poll());
    QCOMPARE(sends.size(), size_t{2});
    QCOMPARE(sends[1].write(), sends[0].write());
    if (outcome == QStringLiteral("session_disappeared")) {
        QCOMPARE(reviews, 0);
        QCOMPARE(refreshes, 1);
        QVERIFY(!canceled);
        QVERIFY(send_widget.findChild<QLineEdit*>("addressEdit")->isReadOnly());
        QVERIFY(send_widget.findChild<QLabel*>("paymasterTransferNotice")->text().contains("saved status could not be verified"));
        QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterSessionState", Qt::DirectConnection));
        QCOMPARE(refreshes, 2);
        QCOMPARE(sends.size(), size_t{2});
        return;
    }
    if (outcome == QStringLiteral("rpc_error") || outcome == QStringLiteral("endpoint_unreachable")) {
        QVERIFY(poll());
        QCOMPARE(sends.size(), size_t{2});
        QCOMPARE(reviews, 0);
        QCOMPARE(refreshes, 3); // Refresh, one cancellation, durable outcome read-back.
        QVERIFY(failure_title.isEmpty());
        QVERIFY(failure_message.isEmpty());
        QVERIFY(send_widget.findChild<QLabel*>("paymasterTransferNotice")->text().contains("No payment was sent"));
        QVERIFY(!send_widget.findChild<QLineEdit*>("addressEdit")->isReadOnly());
        QVERIFY(sends.back()[6].find_value("prepare_only").isTrue());
        QVERIFY(sends.back()[6].find_value("authorization_commitment").isNull());
        return;
    }
    QCOMPARE(refreshes, 0);
    QVERIFY(poll());
    QCOMPARE(sends.size(), size_t{3});
    QCOMPARE(reviews, 0);
    QVERIFY(next_step->text().contains(QStringLiteral("Waiting for the provider's exact offer")));
    auto* primary = send_widget.findChild<QPushButton*>(QStringLiteral("paymasterSessionPrimaryAction"));
    QCOMPARE(primary->text(), QStringLiteral("Preparing payment…"));
    QVERIFY(!primary->isEnabled());
    QVERIFY(poll());
    if (outcome == QStringLiteral("quote_expires")) {
        QCOMPARE(reviews, 0);
        QCOMPARE(refreshes, 1);
        QVERIFY(send_widget.findChild<QFrame*>("paymasterSessionFrame")->isHidden());
        QVERIFY(send_widget.findChild<QLabel*>("paymasterTransferNotice")->text().contains("No payment was sent"));
        QVERIFY(!send_widget.findChild<QLineEdit*>(QStringLiteral("addressEdit"))->isReadOnly());
        QCOMPARE(sends.size(), size_t{4});
        return;
    }
    QCOMPARE(reviews, 1);
    if (outcome == QStringLiteral("review_wallet_switch") || outcome == QStringLiteral("unlock_wallet_switch")) {
        QCOMPARE(sends.size(), size_t{4});
        QCOMPARE(send_widget.findChild<QLabel*>(QStringLiteral("paymasterSessionState"))->text(), QStringLiteral("No active session"));
        QVERIFY(!client->isBusy());
        return;
    }
    for (size_t i = 1; i < 4; ++i)
        QCOMPARE(sends[i].write(), sends[0].write());
    for (size_t i = 0; i < 4; ++i) {
        QVERIFY(sends[i][6].find_value("prepare_only").isTrue());
        QVERIFY(sends[i][6].find_value("authorization_commitment").isNull());
        QVERIFY(sends[i][6].find_value("retry_transport").isNull());
    }
    if (approves) {
        QCOMPARE(sends.size(), size_t{5});
        QCOMPARE(sends[4][6].find_value("fee_mode").get_str(), std::string{"paymaster"});
        QCOMPARE(sends[4][6].find_value("maximum_paymaster_fee_cents").write(), sends[0][6].find_value("maximum_paymaster_fee_cents").write());
        QCOMPARE(sends[4][6].find_value("authorization_commitment").get_str(), std::string(64, 'c'));
        QVERIFY(poll());
        QCOMPARE(sends.size(), size_t{6});
        QCOMPARE(sends[5].write(), sends[4].write());
        QVERIFY(poll()); // After submission, observe only; never repeat the send.
        QCOMPARE(sends.size(), size_t{6});
    } else {
        QCOMPARE(sends.size(), size_t{4});
        QVERIFY(!send_widget.findChild<QLineEdit*>("addressEdit")->isReadOnly());
        QVERIFY(poll()); // Canceling the exact review must end active continuation.
        QCOMPARE(sends.size(), size_t{4});
    }
}

void PaymasterWidgetTests::paymasterClientSessionActionMatrix_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<QString>("artifact");
    QTest::addColumn<QString>("attempt_state");
    QTest::addColumn<QString>("pending_phase");
    QTest::addColumn<QStringList>("allowed_actions");
    QTest::addColumn<QString>("primary_text");
    QTest::addColumn<bool>("retry_visible");
    QTest::addColumn<bool>("fallback_visible");
    QTest::addColumn<bool>("recover_visible");
    QTest::addColumn<bool>("abandon_visible");
    QTest::addColumn<bool>("more_visible");

    QTest::newRow("created-unsigned")
        << QStringLiteral("CREATED") << QStringLiteral("none") << QString{}
        << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Resume preparation")
        << false << false << false << true << true;
    QTest::newRow("created-core-allows-refresh-only")
        << QStringLiteral("CREATED") << QStringLiteral("none") << QString{}
        << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh")}
        << QStringLiteral("Check current status")
        << false << false << false << false << false;
    QTest::newRow("awaiting-unlock-unsigned")
        << QStringLiteral("AWAITING_WALLET_UNLOCK") << QStringLiteral("none")
        << QStringLiteral("CANDIDATE") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("fallback"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Unlock and continue")
        << false << true << false << true << true;
    QTest::newRow("exact-offer-review")
        << QStringLiteral("AWAITING_USER_SIGNATURE") << QStringLiteral("none")
        << QStringLiteral("QUOTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("resume"),
                       QStringLiteral("fallback"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Review exact offer")
        << false << true << false << true << true;
    QTest::newRow("authorized-signed")
        << QStringLiteral("AUTHORIZED") << QStringLiteral("user_psbt")
        << QStringLiteral("QUOTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("provider-pending-signed")
        << QStringLiteral("PENDING_PROVIDER") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("mempool-final")
        << QStringLiteral("MEMPOOL") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Check current status")
        << true << false << true << false << true;
    QTest::newRow("failed-unsigned")
        << QStringLiteral("FAILED") << QStringLiteral("none")
        << QStringLiteral("REJECTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"),
                       QStringLiteral("abandon_unsigned")}
        << QStringLiteral("Check current status")
        << false << false << false << true << true;
    QTest::newRow("failed-signed")
        << QStringLiteral("FAILED") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Start safe recovery")
        << true << false << false << false << true;
    QTest::newRow("failed-signed-core-denies-recovery")
        << QStringLiteral("FAILED") << QStringLiteral("user_psbt")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same")}
        << QStringLiteral("Check current status")
        << true << false << false << false << true;
    QTest::newRow("conflicted-final")
        << QStringLiteral("CONFLICTED") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh"), QStringLiteral("retry_same"),
                       QStringLiteral("cancel_to_self")}
        << QStringLiteral("Start safe recovery")
        << true << false << false << false << true;
    QTest::newRow("contradictory-unsigned-provider-pending")
        << QStringLiteral("PENDING_PROVIDER") << QStringLiteral("none")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{QStringLiteral("refresh")}
        << QStringLiteral("Check current status")
        << false << false << false << false << false;
    QTest::newRow("confirmed-terminal")
        << QStringLiteral("CONFIRMED") << QStringLiteral("final_transaction")
        << QStringLiteral("SUBMITTED") << QStringLiteral("NONE")
        << QStringList{}
        << QStringLiteral("Start a new transfer")
        << false << false << false << false << false;
    QTest::newRow("canceled-terminal")
        << QStringLiteral("CANCELED_SAFE") << QStringLiteral("none")
        << QStringLiteral("REJECTED") << QStringLiteral("NONE")
        << QStringList{}
        << QStringLiteral("Start a new transfer")
        << false << false << false << false << false;
}

void PaymasterWidgetTests::paymasterClientSessionActionMatrix()
{
    QFETCH(QString, state);
    QFETCH(QString, artifact);
    QFETCH(QString, attempt_state);
    QFETCH(QString, pending_phase);
    QFETCH(QStringList, allowed_actions);
    QFETCH(QString, primary_text);
    QFETCH(bool, retry_visible);
    QFETCH(bool, fallback_visible);
    QFETCH(bool, recover_visible);
    QFETCH(bool, abandon_visible);
    QFETCH(bool, more_visible);

    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarSendWidget send_widget(platform_style.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        state, artifact, true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, attempt_state, pending_phase, allowed_actions, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* retry = send_widget.findChild<QPushButton*>(
        QStringLiteral("retryPaymasterSession"));
    QPushButton* fallback = send_widget.findChild<QPushButton*>(
        QStringLiteral("fallbackPaymasterSession"));
    QPushButton* recover = send_widget.findChild<QPushButton*>(
        QStringLiteral("recoverPaymasterSession"));
    QPushButton* abandon = send_widget.findChild<QPushButton*>(
        QStringLiteral("abandonUnsignedPaymasterSession"));
    QPushButton* more = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionMoreOptions"));
    QLineEdit* address = send_widget.findChild<QLineEdit*>(
        QStringLiteral("addressEdit"));
    QLineEdit* amount = send_widget.findChild<QLineEdit*>(
        QStringLiteral("amountEdit"));
    QVERIFY(primary && retry && fallback && recover && abandon && more);
    QVERIFY(address && amount);

    QCOMPARE(primary->text(), primary_text);
    QCOMPARE(!retry->isHidden(), retry_visible);
    QCOMPARE(!fallback->isHidden(), fallback_visible);
    QCOMPARE(!recover->isHidden(), recover_visible);
    QCOMPARE(!abandon->isHidden(), abandon_visible);
    QCOMPARE(!more->isHidden(), more_visible);
    QVERIFY(address->isReadOnly());
    QVERIFY(amount->isReadOnly());
}

void PaymasterWidgetTests::paymasterClientSessionRpcActionsAreBound()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-session-actions");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    std::vector<UniValue> send_parameters;
    QString action_warning;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString&, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            action_warning = message;
            return QMessageBox::Ok;
        });
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "INPUTS_RESERVED", "none", "CANDIDATE");
            }
            if (command == "senddigidollar") {
                send_parameters.push_back(params);
                return PaymasterSessionView(
                    "INPUTS_RESERVED", "none", "CANDIDATE");
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    commands.clear();
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"));

    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "retryPaymasterSession", Qt::DirectConnection));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QVERIFY(primary != nullptr);
    QCOMPARE(primary->text(), QStringLiteral("Resume preparation"));
    primary->click();

    QCOMPARE(resolve_actions,
             QStringList({QStringLiteral("refresh"),
                          QStringLiteral("refresh"),
                          QStringLiteral("refresh")}));
    QVERIFY(action_warning.contains(
        QStringLiteral("no longer allows"), Qt::CaseInsensitive));
    QCOMPARE(commands,
             QStringList({QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("resolvepaymastersession"),
                          QStringLiteral("senddigidollar")}));
    QCOMPARE(send_parameters.size(), size_t{1});
    const UniValue options = send_parameters.front()[6];
    QCOMPARE(options.find_value("prepare_only").get_bool(), true);
    QVERIFY(options.find_value("retry_transport").isTrue());
    QVERIFY(options.find_value("authorization_commitment").isNull());
    QVERIFY(QMetaObject::invokeMethod(send_widget.findChild<PaymasterSendWidget*>(), "pollPaymasterSession", Qt::DirectConnection));
    QCOMPARE(send_parameters.size(), size_t{2});
    QVERIFY(send_parameters.back()[6].find_value("retry_transport").isNull());
    QCOMPARE(QString::fromStdString(options.find_value("request_id").get_str()),
             QStringLiteral("00000000-0000-4000-8000-000000000001"));

    auto* paymaster = send_widget.findChild<PaymasterSendWidget*>();
    int exact_retries{0};
    bool completed{false};
    bool retry_error{false};
    paymaster->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
            if (command != "resolvepaymastersession")
                throw std::runtime_error("restored retry must not create a new payment");
            const auto action = params[1].get_str();
            if (action == "retry_same") {
                if (retry_error) throw std::runtime_error("PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE");
                if (params[0].find_value("request_id").get_str() != "00000000-0000-4000-8000-000000000001")
                    throw std::runtime_error("retry changed the durable request");
                completed = ++exact_retries == 2;
            }
            return completed ? PaymasterSessionView("CONFIRMED", "final_transaction", "CONFIRMED") :
                               PaymasterSessionView("PENDING_PROVIDER", "user_psbt", "USER_SIGNED");
        });
    paymaster->setPaymasterSessionForTesting(
        QStringLiteral("PENDING_PROVIDER"), QStringLiteral("user_psbt"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("USER_SIGNED"));
    // Restoring and observing alone never grant authority to resubmit.
    QVERIFY(QMetaObject::invokeMethod(paymaster, "pollPaymasterSession", Qt::DirectConnection));
    QCOMPARE(exact_retries, 0);
    QVERIFY(QMetaObject::invokeMethod(paymaster, "retryPaymasterSession", Qt::DirectConnection));
    QCOMPARE(exact_retries, 1);
    QVERIFY(QMetaObject::invokeMethod(paymaster, "pollPaymasterSession", Qt::DirectConnection));
    QCOMPARE(exact_retries, 2);
    QVERIFY(completed);
    QVERIFY(QMetaObject::invokeMethod(paymaster, "pollPaymasterSession", Qt::DirectConnection));
    QCOMPARE(exact_retries, 2);

    completed = false;
    exact_retries = 0;
    paymaster->setPaymasterSessionForTesting("PENDING_PROVIDER", "user_psbt", true,
        "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx", 3.25, "USER_SIGNED");
    QVERIFY(QMetaObject::invokeMethod(paymaster, "retryPaymasterSession", Qt::DirectConnection));
    QCOMPARE(exact_retries, 1);
    paymaster->setPrivacy(true);
    paymaster->setPrivacy(false);
    QVERIFY(QMetaObject::invokeMethod(paymaster, "pollPaymasterSession", Qt::DirectConnection));
    QCOMPARE(exact_retries, 1);
    retry_error = true;
    QVERIFY(QMetaObject::invokeMethod(paymaster, "retryPaymasterSession", Qt::DirectConnection));
    QVERIFY(action_warning.contains("may already have completed"));
    QVERIFY(action_warning.contains("PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE"));
    retry_error = false;
    QVERIFY(QMetaObject::invokeMethod(paymaster, "pollPaymasterSession", Qt::DirectConnection));
    QCOMPARE(exact_retries, 1);

    // Recovery messages use the same surface in both application themes.
    const QString original_stylesheet = qApp->styleSheet();
    for (const auto* theme : {"dark", "light"}) {
        QFile css(QString(":/css/") + theme);
        QVERIFY(css.open(QIODevice::ReadOnly));
        const QString stylesheet = QString::fromUtf8(css.readAll());
        qApp->setStyleSheet(stylesheet);
        send_widget.setStyleSheet(stylesheet);
        send_widget.ensurePolished();
        QMessageBox message(QMessageBox::Warning, "Exact retry paused",
            "The existing transfer could not be reconciled yet. It may already have completed.",
            QMessageBox::Ok, &send_widget);
        message.ensurePolished();
        message.show();
        QVERIFY(QTest::qWaitForWindowExposed(&message));
        QCOMPARE(message.palette().color(QPalette::Window),
                 QColor(QString::fromLatin1(theme) == "dark" ? "#0b2419" : "#eef9f2"));
        const QString capture = qEnvironmentVariable("DIGIBYTE_PAYMASTER_RETRY_SCREENSHOT");
        if (!capture.isEmpty()) {
            message.show();
            QVERIFY(QTest::qWaitForWindowExposed(&message));
            QVERIFY(message.grab().save(capture + "-" + theme + ".png"));
        }
    }
    qApp->setStyleSheet(original_stylesheet);

}

void PaymasterWidgetTests::paymasterClientRestartCreatedSessionWithoutRecipientIsReadOnly()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-recipientless-restart");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 1);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                UniValue sessions{UniValue::VARR};
                UniValue session{UniValue::VOBJ};
                session.pushKV(
                    "request_id",
                    "00000000-0000-4000-8000-000000000001");
                session.pushKV("session_id", std::string(64, '1'));
                session.pushKV("session_state", "CREATED");
                sessions.push_back(std::move(session));
                listed.pushKV("sessions", std::move(sessions));
                return listed;
            }
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "CREATED", "none", "", /*include_recipient=*/false);
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });

    send_widget.setWalletModel(mini_gui.walletModel.get());

    QLabel* transfer = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionTransfer"));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* abandon = send_widget.findChild<QPushButton*>(
        QStringLiteral("abandonUnsignedPaymasterSession"));
    QVERIFY(transfer && primary && abandon);
    QVERIFY(transfer->text().contains(
        QStringLiteral("not yet available"), Qt::CaseInsensitive));
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    QVERIFY(primary->isEnabled());
    QVERIFY(!abandon->isHidden());
    QVERIFY(!commands.contains(QStringLiteral("senddigidollar")));

    primary->click();
    QCOMPARE(resolve_actions,
             QStringList({QStringLiteral("refresh"),
                          QStringLiteral("refresh")}));
    QVERIFY(!commands.contains(QStringLiteral("senddigidollar")));
}

void PaymasterWidgetTests::paymasterClientRecipientlessCancellation_data()
{
    QTest::addColumn<QString>("outcome");
    for (const char* outcome : {"cancel", "lost_response", "already_closed",
                                "not_final", "attention", "signed", "attempt",
                                "reserved", "txid", "pending", "actions", "missing_count"}) {
        QTest::newRow(outcome) << QString::fromLatin1(outcome);
    }
}

void PaymasterWidgetTests::paymasterClientRecipientlessCancellation()
{
    QFETCH(QString, outcome);
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test, "qt-paymaster-recipientless-cancel");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    auto* client = send_widget.findChild<PaymasterSendWidget*>();
    QVERIFY(client);

    bool closed{false};
    int confirmations{0};
    QStringList warnings;
    QStringList actions;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon icon, const QString&, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            if (icon == QMessageBox::Question) {
                ++confirmations;
                return QMessageBox::Yes;
            }
            warnings.push_back(message);
            return QMessageBox::Ok;
        });
    client->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 0);
                listed.pushKV("sessions", UniValue{UniValue::VARR});
                return listed;
            }
            if (command != "resolvepaymastersession") {
                throw std::runtime_error("Cancellation must never prepare or sign another payment");
            }
            const auto action = QString::fromStdString(params[1].get_str());
            actions.push_back(action);
            if (action == QStringLiteral("abandon_unsigned")) {
                closed = true;
                if (outcome == QStringLiteral("lost_response")) {
                    throw std::runtime_error("Cancellation response was lost");
                }
            } else if (action != QStringLiteral("refresh")) {
                throw std::runtime_error("Unexpected session mutation");
            }
            if (!closed) return PaymasterSessionView("CREATED", "none", "", false);

            // Match Core's post-abandon snapshot, including the absence of an
            // attempt, recipient and any reserved inputs. FAILED alone is not
            // proof that another transfer can safely be composed.
            UniValue result = PaymasterSessionView("FAILED", "none", "", false);
            result.pushKV("requires_attention", false);
            UniValue session = result.find_value("session");
            session.pushKV("provider_attempts", 0);
            session.pushKV("reserved_user_inputs", UniValue{UniValue::VARR});
            if (outcome == QStringLiteral("not_final")) session.pushKV("final", false);
            if (outcome == QStringLiteral("attention")) result.pushKV("requires_attention", true);
            if (outcome == QStringLiteral("signed") || outcome == QStringLiteral("attempt")) {
                UniValue attempt{UniValue::VOBJ};
                attempt.pushKV("attempt_state", outcome == QStringLiteral("signed") ? "USER_SIGNED" : "REJECTED");
                result.pushKV("attempt", std::move(attempt));
                if (outcome == QStringLiteral("signed")) result.pushKV("artifact", "user_psbt");
            }
            if (outcome == QStringLiteral("reserved")) {
                UniValue inputs{UniValue::VARR};
                inputs.push_back(std::string(64, 'a') + ":0");
                session.pushKV("reserved_user_inputs", std::move(inputs));
            }
            if (outcome == QStringLiteral("txid")) session.pushKV("txid", std::string(64, 'e'));
            if (outcome == QStringLiteral("pending")) session.pushKV("pending_phase", "USER_SIGNATURE_SENT");
            if (outcome == QStringLiteral("actions")) {
                UniValue unexpected{UniValue::VARR};
                unexpected.push_back("refresh");
                unexpected.push_back("resume");
                result.pushKV("allowed_actions", std::move(unexpected));
            }
            if (outcome == QStringLiteral("missing_count")) session.pushKV("provider_attempts", UniValue{});
            result.pushKV("session", std::move(session));
            return result;
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    client->setPaymasterSessionForTesting(
        QStringLiteral("CREATED"), QStringLiteral("none"), true, QString{}, 0.0,
        QString{}, QString{}, {QStringLiteral("refresh"), QStringLiteral("abandon_unsigned")}, true);
    auto* primary = send_widget.findChild<QPushButton*>(QStringLiteral("paymasterSessionPrimaryAction"));
    auto* abandon = send_widget.findChild<QPushButton*>(QStringLiteral("abandonUnsignedPaymasterSession"));
    auto* address = send_widget.findChild<QLineEdit*>(QStringLiteral("addressEdit"));
    auto* amount = send_widget.findChild<QLineEdit*>(QStringLiteral("amountEdit"));
    QVERIFY(primary && abandon && address && amount);
    primary->click();
    QVERIFY(warnings.isEmpty());
    QVERIFY(address->isReadOnly());

    if (outcome == QStringLiteral("cancel") || outcome == QStringLiteral("lost_response")) {
        abandon->click();
        QCOMPARE(confirmations, 1);
        QCOMPARE(actions.count(QStringLiteral("abandon_unsigned")), 1);
        if (outcome == QStringLiteral("lost_response")) {
            QCOMPARE(warnings.size(), 1);
            warnings.clear();
            QVERIFY(address->isReadOnly());
            // The next read-only refresh must recover the successful Core
            // cancellation without repeating it or signing another payment.
            primary->click();
            QCOMPARE(primary->text(), QStringLiteral("Start a new transfer"));
            primary->click();
        }
    } else {
        closed = true;
        if (outcome == QStringLiteral("already_closed")) {
            // Cancellation completed elsewhere before the preflight refresh.
            abandon->click();
            QCOMPARE(confirmations, 0);
            QVERIFY(!address->isReadOnly());
        } else {
            primary->click();
            QCOMPARE(warnings.size(), 1);
            QVERIFY(warnings.front().contains(QStringLiteral("incomplete or inconsistent")));
            QVERIFY(primary->text() != QStringLiteral("Start a new transfer"));
            QVERIFY(address->isReadOnly());
            QVERIFY(amount->isReadOnly());
            QCOMPARE(actions.count(QStringLiteral("abandon_unsigned")), 0);
            return;
        }
        QCOMPARE(actions.count(QStringLiteral("abandon_unsigned")), 0);
    }
    QVERIFY(warnings.isEmpty());
    QVERIFY(!address->isReadOnly());
    QVERIFY(!amount->isReadOnly());
    const int requests_before_typing = actions.size();
    address->clear();
    QTest::keyClicks(address, "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    QCOMPARE(address->text(), QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"));
    QCOMPARE(actions.size(), requests_before_typing);
}

void PaymasterWidgetTests::paymasterClientMultipleRestartSessionsRequireSelection()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-multiple-restart");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QStringList commands;
    QStringList resolve_actions;
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            commands.push_back(QString::fromStdString(command));
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 2);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                UniValue sessions{UniValue::VARR};
                for (int index = 1; index <= 2; ++index) {
                    UniValue session{UniValue::VOBJ};
                    session.pushKV(
                        "request_id",
                        index == 1
                            ? "00000000-0000-4000-8000-000000000001"
                            : "00000000-0000-4000-8000-000000000002");
                    session.pushKV("session_id",
                                   std::string(64, index == 1 ? '1' : '2'));
                    session.pushKV("session_state",
                                   index == 1 ? "CREATED" : "INPUTS_RESERVED");
                    session.pushKV("requested_amount_cents", 300 + index);
                    sessions.push_back(std::move(session));
                }
                listed.pushKV("sessions", std::move(sessions));
                return listed;
            }
            if (command == "resolvepaymastersession") {
                resolve_actions.push_back(
                    QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "CREATED", "none", "", /*include_recipient=*/false);
            }
            throw std::runtime_error(
                "persisted-session selection must not sign or recover");
        });

    send_widget.setWalletModel(mini_gui.walletModel.get());

    QFrame* chooser = send_widget.findChild<QFrame*>(
        QStringLiteral("persistedPaymasterSessionsFrame"));
    QComboBox* sessions = send_widget.findChild<QComboBox*>(
        QStringLiteral("persistedPaymasterSessions"));
    QPushButton* review = send_widget.findChild<QPushButton*>(
        QStringLiteral("loadPersistedPaymasterSession"));
    QVERIFY(chooser && sessions && review);
    QVERIFY(!chooser->isHidden());
    QCOMPARE(sessions->count(), 2);
    QCOMPARE(commands, QStringList{QStringLiteral(
                           "listdigidollarsendsessions")});
    QVERIFY(resolve_actions.isEmpty());

    // Selecting a persisted item performs exactly one read-only refresh. It
    // never resumes preparation, unlocks, signs, broadcasts or recovers.
    review->click();
    QCOMPARE(resolve_actions, QStringList{QStringLiteral("refresh")});
    QCOMPARE(commands,
             QStringList({QStringLiteral("listdigidollarsendsessions"),
                          QStringLiteral("resolvepaymastersession")}));
}

void PaymasterWidgetTests::paymasterClientRecoveryExpiryIsFailClosed()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-recovery-expiry");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    bool inconsistent_expiry{false};
    QString warning;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString&, const QString& message,
            QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            warning = message;
            return QMessageBox::Ok;
        });
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue&) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "listdigidollarsendsessions") {
                UniValue listed{UniValue::VOBJ};
                listed.pushKV("active_only", true);
                listed.pushKV("count", 0);
                listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
                listed.pushKV("sessions", UniValue{UniValue::VARR});
                return listed;
            }
            if (command == "resolvepaymastersession") {
                return PaymasterSessionView(
                    "FAILED", "alternative_recovery", "CONFLICTED",
                    /*include_recipient=*/true,
                    /*recovery_expires_at=*/1,
                    /*recovery_expired=*/!inconsistent_expiry);
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("FAILED"), QStringLiteral("alternative_recovery"),
        true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CONFLICTED"), QStringLiteral("NONE"),
        {QStringLiteral("refresh"), QStringLiteral("recover"),
         QStringLiteral("cancel_to_self")}, true);

    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState", Qt::DirectConnection));
    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QPushButton* recover = send_widget.findChild<QPushButton*>(
        QStringLiteral("recoverPaymasterSession"));
    QVERIFY(primary && recover);
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    QVERIFY(recover->isHidden());

    inconsistent_expiry = true;
    warning.clear();
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(warning.contains(
        QStringLiteral("inconsistent recovery expiry"),
        Qt::CaseInsensitive));
}

void PaymasterWidgetTests::paymasterClientDelayedCallbackIgnoresWalletClose()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-delayed-callback");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    const auto initial_rpc = [](const std::string& command,
                                const UniValue&) {
        if (command == "getpaymasterclientsafetystatus") {
            return PaymasterClientSafetyStatus();
        }
        if (command == "listdigidollarsendsessions") {
            UniValue listed{UniValue::VOBJ};
            listed.pushKV("active_only", true);
            listed.pushKV("count", 0);
            listed.pushKV("next_cursor", UniValue{UniValue::VNULL});
            listed.pushKV("sessions", UniValue{UniValue::VARR});
            return listed;
        }
        throw std::runtime_error("unexpected Paymaster RPC");
    };

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(initial_rpc);
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"), QStringLiteral("NONE"),
        {QStringLiteral("refresh")}, true);
    WalletModel::RpcCallback delayed_callback;
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterAsyncRpcExecutorForTesting(
        [&](const std::string&, const UniValue&,
            WalletModel::RpcCallback callback) {
            delayed_callback = std::move(callback);
        });
    QVERIFY(QMetaObject::invokeMethod(
        send_widget.findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState", Qt::DirectConnection));
    QVERIFY(static_cast<bool>(delayed_callback));

    QLabel* state = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterSessionState"));
    QVERIFY(state != nullptr);
    send_widget.setWalletModel(nullptr);
    QCOMPARE(state->text(), QStringLiteral("No active session"));
    delayed_callback(
        PaymasterSessionView("INPUTS_RESERVED", "none", "CANDIDATE"),
        QString{});
    QCOMPARE(state->text(), QStringLiteral("No active session"));

    auto* destroyed_widget = new DigiDollarSendWidget(
        mini_gui.platformStyle.get());
    destroyed_widget->findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(initial_rpc);
    destroyed_widget->setWalletModel(mini_gui.walletModel.get());
    destroyed_widget->findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("INPUTS_RESERVED"), QStringLiteral("none"), true,
        QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("CANDIDATE"), QStringLiteral("NONE"),
        {QStringLiteral("refresh")}, true);
    WalletModel::RpcCallback destroyed_callback;
    destroyed_widget->findChild<PaymasterSendWidget*>()->setPaymasterAsyncRpcExecutorForTesting(
        [&](const std::string&, const UniValue&,
            WalletModel::RpcCallback callback) {
            destroyed_callback = std::move(callback);
        });
    QVERIFY(QMetaObject::invokeMethod(
        destroyed_widget->findChild<PaymasterSendWidget*>(), "refreshPaymasterSessionState",
        Qt::DirectConnection));
    QVERIFY(static_cast<bool>(destroyed_callback));
    QPointer<DigiDollarSendWidget> destroyed_guard{destroyed_widget};
    delete destroyed_widget;
    QVERIFY(destroyed_guard.isNull());
    destroyed_callback(
        PaymasterSessionView("INPUTS_RESERVED", "none", "CANDIDATE"),
        QString{});
}

void PaymasterWidgetTests::paymasterClientUnlockLeaseSurvivesWalletModelClose()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-unlock-model-close");
    const SecureString passphrase{"qt-paymaster-unlock-model-close"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    bool unlock_succeeded{false};
    QObject::connect(mini_gui.walletModel.get(), &WalletModel::requireUnlock,
                     [&] {
                         unlock_succeeded =
                             mini_gui.walletModel->setWalletLocked(
                                 false, passphrase);
                     });

    std::shared_ptr<WalletModel::UnlockContext> unlock =
        PaymasterQt::RequestUnlock(*mini_gui.walletModel);
    QVERIFY(unlock_succeeded);
    QVERIFY(unlock->isValid());
    QVERIFY(!wallet->IsLocked());

    // The asynchronous lease owns the wallet interface directly. Destroying
    // the GUI model first must neither dereference it later nor leave the
    // encrypted wallet unlocked when the callback finally releases its lease.
    mini_gui.walletModel.reset();
    unlock.reset();
    QVERIFY(wallet->IsLocked());
}

void PaymasterWidgetTests::paymasterSigningWaitKeepsGuiResponsive()
{
    TestChain100Setup test;
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test, "qt-paymaster-signing-wait");
    loader->registerRpcs();
    auto& context = *loader->context();
    struct Cleanup {
        wallet::WalletContext& context;
        const std::shared_ptr<wallet::CWallet>& wallet;
        ~Cleanup() { RemoveWallet(context, wallet, std::nullopt); }
    } cleanup{context, wallet};
    const SecureString passphrase{"qt-paymaster-signing-wait"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->Unlock(passphrase));
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    AddWallet(context, wallet);

    std::promise<void> held, release;
    auto ready = held.get_future();
    auto release_signal = release.get_future();
    bool timed_out{false};
    std::thread holder([&] {
        LOCK(wallet->cs_wallet);
        held.set_value();
        timed_out = release_signal.wait_for(std::chrono::milliseconds(500)) == std::future_status::timeout;
    });
    ready.wait();
    QElapsedTimer elapsed;
    elapsed.start();
    bool completed{false};
    QString error;
    PaymasterQt::ExecuteSigningRpcAsync(*gui.walletModel, "getwalletinfo", UniValue{UniValue::VARR}, [] { return true; },
        [&](UniValue result, QString failure) {
            completed = true;
            error = failure;
            if (failure.isEmpty()) QCOMPARE(result.find_value("walletname").get_str(), wallet->GetName());
        });
    QCoreApplication::processEvents();
    const bool completed_while_locked = completed;
    const auto duration = elapsed.elapsed();
    release.set_value();
    holder.join();
    QVERIFY(!completed_while_locked);
    QVERIFY2(!timed_out, qPrintable(QStringLiteral("Signing preflight blocked Qt for %1 ms").arg(duration)));
    qInfo("Paymaster signing preflight returned in %lld ms with cs_wallet held", duration);
    QTRY_VERIFY(completed);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!wallet->IsLocked()); // Do not relock a wallet that was already unlocked.

    QString outcome;
    bool current{true};
    int prompts{0};
    QObject::connect(gui.walletModel.get(), &WalletModel::requireUnlock, [&] {
        ++prompts;
        QCOMPARE(QThread::currentThread(), QCoreApplication::instance()->thread());
        if (outcome == "cancel") return;
        QVERIFY(wallet->Unlock(passphrase));
        if (outcome == "switch") current = false;
        if (outcome == "close") gui.walletModel.reset();
    });
    for (const QString& next : {QStringLiteral("success"), QStringLiteral("rpc_error"), QStringLiteral("cancel"), QStringLiteral("switch"), QStringLiteral("close")}) {
        outcome = next;
        QVERIFY(wallet->Lock());
        current = true;
        completed = false;
        const int before = prompts;
        PaymasterQt::ExecuteSigningRpcAsync(*gui.walletModel, outcome == "rpc_error" ? "unknown-paymaster-test-rpc" : "getwalletinfo",
            UniValue{UniValue::VARR}, [&] { return current; },
            [&](UniValue, QString failure) {
                QVERIFY(wallet->IsLocked()); // Relock precedes delivery, including errors.
                error = failure;
                completed = true;
            });
        QTRY_COMPARE(prompts, before + 1);
        if (outcome == "switch" || outcome == "close") {
            QTRY_VERIFY(wallet->IsLocked());
            QVERIFY(!completed);
        } else {
            QTRY_VERIFY(completed);
            if (outcome == "success") QVERIFY2(error.isEmpty(), qPrintable(error));
            if (outcome == "rpc_error") QVERIFY(!error.isEmpty());
            if (outcome == "cancel") QCOMPARE(error, QStringLiteral("PAYMASTER_WALLET_UNLOCK_CANCELLED"));
        }
    }
}

void PaymasterWidgetTests::paymasterClientDatabaseReadErrorIsActionable()
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(
        m_node, test, "qt-paymaster-client-database-error");
    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);

    DigiDollarSendWidget send_widget(mini_gui.platformStyle.get());
    QString title;
    QString message;
    QStringList actions;
    send_widget.setDialogHandlerForTesting(
        [&](QMessageBox::Icon, const QString& dialog_title,
            const QString& dialog_message, QMessageBox::StandardButtons,
            QMessageBox::StandardButton) {
            title = dialog_title;
            message = dialog_message;
            return QMessageBox::Ok;
        });
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasterclientsafetystatus") {
                return PaymasterClientSafetyStatus();
            }
            if (command == "resolvepaymastersession") {
                actions.push_back(QString::fromStdString(params[1].get_str()));
                return PaymasterSessionView(
                    "AWAITING_USER_SIGNATURE", "none", "QUOTED");
            }
            if (command == "senddigidollar") {
                throw std::runtime_error("PAYMASTER_SESSION_DATABASE_READ");
            }
            throw std::runtime_error("unexpected Paymaster RPC");
        });
    send_widget.setWalletModel(mini_gui.walletModel.get());
    send_widget.findChild<PaymasterSendWidget*>()->setPaymasterSessionForTesting(
        QStringLiteral("AWAITING_USER_SIGNATURE"), QStringLiteral("none"),
        true, QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"),
        3.25, QStringLiteral("QUOTED"), QString{},
        {QStringLiteral("refresh"), QStringLiteral("resume")}, true);

    QPushButton* primary = send_widget.findChild<QPushButton*>(
        QStringLiteral("paymasterSessionPrimaryAction"));
    QVERIFY(primary != nullptr);
    primary->click();

    QVERIFY(title.isEmpty());
    QVERIFY(message.isEmpty()); // No modal chain for a failed live attempt.
    auto* notice = send_widget.findChild<QLabel*>("paymasterTransferNotice");
    QVERIFY(notice && !notice->isHidden());
    QVERIFY(notice->text().contains("Wallet data could not be read reliably"));
    QVERIFY(notice->text().contains("reservations and authorizations remain protected"));
    QVERIFY(notice->text().contains("Preserve the wallet backup and debug log"));
    QVERIFY(!notice->text().contains("No payment was sent"));
    QVERIFY(!notice->text().contains("Wallet synchronization status"));
    QVERIFY(send_widget.findChild<QLineEdit*>("addressEdit")->isReadOnly());
    QCOMPARE(actions, (QStringList{"refresh", "refresh"}));
    QCOMPARE(primary->text(), QStringLiteral("Check current status"));
    primary->click();
    QCOMPARE(actions, (QStringList{"refresh", "refresh", "refresh"}));
    send_widget.setPrivacy(true);
    QVERIFY(notice->isHidden());
}

void PaymasterWidgetTests::paymasterClientControlsAreAccessible()
{
    std::unique_ptr<const PlatformStyle> platform_style(
        PlatformStyle::instantiate("other"));
    DigiDollarSendWidget send_widget(platform_style.get());
    QTableWidget* table = send_widget.findChild<QTableWidget*>(
        QStringLiteral("paymasterOffers"));
    QLabel* status = send_widget.findChild<QLabel*>(
        QStringLiteral("paymasterOffersStatus"));
    QPushButton* refresh = send_widget.findChild<QPushButton*>(
        QStringLiteral("refreshPaymasterOffers"));
    QSpinBox* fee_cap = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterFeeCap"));
    QSpinBox* attempts = send_widget.findChild<QSpinBox*>(
        QStringLiteral("paymasterMaximumAttempts"));
    QComboBox* privacy = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterPrivacy"));
    QComboBox* selection = send_widget.findChild<QComboBox*>(
        QStringLiteral("paymasterSelection"));
    QVERIFY(table && status && refresh && fee_cap && attempts && privacy && selection);

    QCOMPARE(table->selectionMode(), QAbstractItemView::NoSelection);
    QCOMPARE(table->focusPolicy(), Qt::StrongFocus);
    QVERIFY(table->alternatingRowColors());
    QVERIFY(table->verticalHeader()->isHidden());
    QCOMPARE(table->horizontalHeader()->objectName(),
             QStringLiteral("paymasterOffersHeader"));
    QVERIFY(!table->accessibleName().isEmpty());
    QVERIFY(!table->accessibleDescription().isEmpty());
    QCOMPARE(status->accessibleDescription(), status->text());
    QVERIFY(!refresh->accessibleDescription().isEmpty());
    QVERIFY(!fee_cap->accessibleName().isEmpty());
    QVERIFY(!attempts->accessibleName().isEmpty());
    QVERIFY(!privacy->accessibleName().isEmpty());
    QVERIFY(!selection->accessibleName().isEmpty());
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Provider"));
    QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("Announcement expires"));

    QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(table);
    QVERIFY(interface != nullptr);
    QCOMPARE(interface->role(), QAccessible::Table);

    table->setRowCount(2);
    table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Provider A")));
    table->setItem(1, 0, new QTableWidgetItem(QStringLiteral("Provider B")));
    send_widget.resize(1200, 900);
    send_widget.show();
    QFrame* advanced = send_widget.findChild<QFrame*>(
        QStringLiteral("advancedPaymasterSettings"));
    QVERIFY(advanced != nullptr);
    advanced->show();
    table->show();
    QCoreApplication::processEvents();
    table->setCurrentCell(0, 0);
    QTest::keyClick(table, Qt::Key_Down);
    QCOMPARE(table->currentRow(), 1);
}

void PaymasterWidgetTests::paymasterOfferTableRendersGreenTheme()
{
    struct RenderStats {
        bool stylesheet_loaded{false};
        bool table_visible{false};
        int total_pixels{0};
        int green_pixels{0};
        int blue_pixels{0};
    };

    const QString original_stylesheet = qApp->styleSheet();
    const QPalette original_palette = qApp->palette();
    const auto render_theme = [](const QString& resource) {
        RenderStats stats;
        QFile file(resource);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return stats;
        stats.stylesheet_loaded = true;
        qApp->setStyleSheet(QString::fromUtf8(file.readAll()));

        std::unique_ptr<const PlatformStyle> platform_style(
            PlatformStyle::instantiate("other"));
        DigiDollarSendWidget send_widget(platform_style.get());
        QRadioButton* paymaster = send_widget.findChild<QRadioButton*>(
            QStringLiteral("feeFundingPaymaster"));
        QPushButton* advanced_toggle = send_widget.findChild<QPushButton*>(
            QStringLiteral("advancedPaymasterSettingsToggle"));
        QTableWidget* table = send_widget.findChild<QTableWidget*>(
            QStringLiteral("paymasterOffers"));
        if (!paymaster || !advanced_toggle || !table) return stats;
        paymaster->setChecked(true);
        advanced_toggle->setChecked(true);
        table->setRowCount(1);
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Provider")));
        send_widget.resize(1500, 1200);
        send_widget.show();
        table->resize(1200, 160);
        QCoreApplication::processEvents();
        QTest::qWait(25);
        stats.table_visible = table->isVisibleTo(&send_widget);

        QHeaderView* horizontal_header = table->horizontalHeader();
        const QImage header = horizontal_header->grab().toImage()
                                  .convertToFormat(QImage::Format_RGB32);
        // The minimal Qt platform can leave an unpainted viewport strip after
        // the last section. Restrict the sample to the actual header sections
        // so that the window size does not dilute their rendered colour.
        const int painted_width = std::min(header.width(), horizontal_header->length());
        for (int y = 1; y + 1 < header.height(); ++y) {
            for (int x = 1; x + 1 < painted_width; ++x) {
                const QColor color = QColor::fromRgb(header.pixel(x, y));
                ++stats.total_pixels;
                if (color.green() > color.red() + 20 &&
                    color.green() > color.blue() + 10) {
                    ++stats.green_pixels;
                }
                if (color.blue() > color.green() + 30) {
                    ++stats.blue_pixels;
                }
            }
        }
        return stats;
    };

    const RenderStats light = render_theme(QStringLiteral(":/css/light"));
    const RenderStats dark = render_theme(QStringLiteral(":/css/dark"));
    qApp->setStyleSheet(original_stylesheet);
    qApp->setPalette(original_palette);
    QCoreApplication::processEvents();

    const auto require_green_header = [](const RenderStats& stats,
                                         const QString& theme) {
        QVERIFY2(stats.stylesheet_loaded,
                 qPrintable(QStringLiteral("could not load %1 stylesheet resource").arg(theme)));
        QVERIFY2(stats.table_visible,
                 qPrintable(QStringLiteral("Paymaster offer table was not visible under %1").arg(theme)));
        QVERIFY(stats.total_pixels > 0);
        QVERIFY2(stats.green_pixels * 100 >= stats.total_pixels * 40,
                 qPrintable(QStringLiteral(
                     "%1 rendered Paymaster header is not predominantly green (%2/%3 green, %4 blue pixels)")
                                .arg(theme)
                                .arg(stats.green_pixels)
                                .arg(stats.total_pixels)
                                .arg(stats.blue_pixels)));
        QVERIFY2(stats.blue_pixels * 100 < stats.total_pixels * 5,
                 qPrintable(QStringLiteral(
                     "%1 rendered Paymaster header leaked the DGB blue palette (%2/%3 blue pixels)")
                                .arg(theme)
                                .arg(stats.blue_pixels)
                                .arg(stats.total_pixels)));
    };
    require_green_header(light, QStringLiteral("light"));
    require_green_header(dark, QStringLiteral("dark"));
}

void PaymasterWidgetTests::paymasterGuidedSetupRendersConsistentTheme()
{
    struct RenderStats {
        bool wizard_opened{false};
        bool expected_local_theme{false};
        bool scroll_visible{false};
        bool surfaces_use_qss{false};
        bool native_fill_disabled{false};
        bool offer_layout{false};
        bool reserve_layout{false};
        bool refill_independent_of_safety{false};
        int total_pixels{0};
        int dark_pixels{0};
        int light_pixels{0};
    };

    const QString original_stylesheet = qApp->styleSheet();
    const QPalette original_palette = qApp->palette();
    qApp->setStyleSheet(QString{});

    const auto render_theme = [&](const QColor& window, const QColor& text,
                                  const QString& expected_background) {
        RenderStats stats;
        QPalette palette = original_palette;
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, window);
        palette.setColor(QPalette::Text, text);
        qApp->setPalette(palette);

        std::unique_ptr<const PlatformStyle> platform_style(
            PlatformStyle::instantiate("other"));
        DigiDollarTab tab(platform_style.get());
        QPushButton* guided_setup = tab.findChild<QPushButton*>(
            QStringLiteral("paymasterChooseGuidedSetup"));
        if (!guided_setup) return stats;

        QTimer::singleShot(0, [&] {
            auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
            if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
                wizard = nullptr;
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    auto* candidate = qobject_cast<QWizard*>(widget);
                    if (candidate &&
                        candidate->objectName() == QLatin1String("PaymasterSetupWizard")) {
                        wizard = candidate;
                        break;
                    }
                }
            }
            if (!wizard) return;

            stats.wizard_opened = true;
            stats.expected_local_theme = wizard->styleSheet().contains(
                expected_background, Qt::CaseInsensitive);
            QScrollArea* scroll = wizard->findChild<QScrollArea*>(
                QStringLiteral("paymasterSetupRequirementsScroll"));
            QWidget* contents = wizard->findChild<QWidget*>(
                QStringLiteral("paymasterSetupRequirementsContent"));
            QFrame* wallet_card = wizard->findChild<QFrame*>(
                QStringLiteral("paymasterSetupWalletCard"));
            if (scroll && contents && wallet_card) {
                QCoreApplication::processEvents();
                stats.scroll_visible = scroll->isVisibleTo(wizard);
                stats.surfaces_use_qss =
                    scroll->testAttribute(Qt::WA_StyledBackground) &&
                    scroll->viewport()->testAttribute(Qt::WA_StyledBackground) &&
                    contents->testAttribute(Qt::WA_StyledBackground);
                stats.native_fill_disabled =
                    !scroll->viewport()->autoFillBackground() &&
                    !contents->autoFillBackground();

                const QImage image = scroll->viewport()->grab().toImage()
                                         .convertToFormat(QImage::Format_RGB32);
                for (int y = 1; y + 1 < image.height(); ++y) {
                    for (int x = 1; x + 1 < image.width(); ++x) {
                        const QColor color = QColor::fromRgb(image.pixel(x, y));
                        ++stats.total_pixels;
                        if (color.lightness() < 100) ++stats.dark_pixels;
                        if (color.lightness() > 210) ++stats.light_pixels;
                    }
                }
            }
            auto* offer = wizard->findChild<QScrollArea*>("paymasterSetupOfferScroll");
            for (const int id : wizard->pageIds()) {
                if (wizard->page(id)->objectName() == QLatin1String("paymasterSetupOfferPage")) {
                    wizard->setStartId(id);
                    wizard->restart();
                    break;
                }
            }
            QCoreApplication::processEvents();
            stats.offer_layout = offer && offer->isVisibleTo(wizard) && offer->widgetResizable() &&
                !offer->widget()->autoFillBackground() && !offer->viewport()->autoFillBackground() &&
                offer->horizontalScrollBar()->maximum() == 0 &&
                offer->widget()->palette().color(QPalette::Window) == QColor(expected_background);
            const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_SETUP_SCREENSHOT");
            if (!screenshot.isEmpty()) wizard->grab().save(screenshot + "-" + expected_background.mid(1) + ".png");
            auto* reserve_preset = wizard->findChild<QComboBox*>("paymasterSetupReservePreset");
            auto* manual = wizard->findChild<QWidget*>("paymasterSetupManualReserveTargets");
            auto* refill_fee = wizard->findChild<QLineEdit*>("paymasterSetupCustomMaintenancePerTransaction");
            auto* safety_profile = wizard->findChild<QComboBox*>("paymasterSetupSafetyProfile");
            auto* liquidity_scroll = wizard->findChild<QScrollArea*>("paymasterSetupLiquidityScroll");
            if (reserve_preset && manual && refill_fee && safety_profile && liquidity_scroll) {
                reserve_preset->setCurrentIndex(reserve_preset->findData(3));
                safety_profile->setCurrentIndex(safety_profile->findData(QStringLiteral("conservative")));
                stats.refill_independent_of_safety = refill_fee->text() == QLatin1String("0.75000000");
                for (int id : wizard->pageIds()) {
                    if (wizard->page(id)->objectName() == QLatin1String("paymasterSetupLiquidityPage")) {
                        wizard->setStartId(id);
                        wizard->restart();
                        break;
                    }
                }
                QCoreApplication::processEvents();
                stats.reserve_layout = reserve_preset->isVisibleTo(wizard) && !manual->isVisibleTo(wizard) &&
                    refill_fee->isVisibleTo(wizard) && liquidity_scroll->horizontalScrollBar()->maximum() == 0;
                if (!screenshot.isEmpty()) wizard->grab().save(screenshot + "-reserves-" + expected_background.mid(1) + ".png");
            }
            wizard->reject();
        });
        guided_setup->click();
        return stats;
    };

    const RenderStats dark = render_theme(
        QColor(QStringLiteral("#0b2419")), QColor(QStringLiteral("#ffffff")),
        QStringLiteral("#0b2419"));
    const RenderStats light = render_theme(
        QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#123f2b")),
        QStringLiteral("#eef9f2"));

    qApp->setStyleSheet(original_stylesheet);
    qApp->setPalette(original_palette);
    QCoreApplication::processEvents();

    const auto require_common_contract = [](const RenderStats& stats,
                                            const QString& theme) {
        QVERIFY2(stats.wizard_opened,
                 qPrintable(QStringLiteral("Paymaster setup wizard did not open under %1").arg(theme)));
        QVERIFY2(stats.expected_local_theme,
                 qPrintable(QStringLiteral("Paymaster setup wizard selected the wrong %1 theme").arg(theme)));
        QVERIFY2(stats.scroll_visible,
                 qPrintable(QStringLiteral("Paymaster setup content was not visible under %1").arg(theme)));
        QVERIFY2(stats.surfaces_use_qss,
                 qPrintable(QStringLiteral("Paymaster setup surfaces bypassed QSS under %1").arg(theme)));
        QVERIFY2(stats.native_fill_disabled,
                 qPrintable(QStringLiteral("Paymaster setup leaked a native background under %1").arg(theme)));
        QVERIFY(stats.total_pixels > 0);
        QVERIFY2(stats.offer_layout, qPrintable(theme));
        QVERIFY2(stats.reserve_layout, qPrintable(theme));
        QVERIFY2(stats.refill_independent_of_safety, qPrintable(theme));
    };
    require_common_contract(dark, QStringLiteral("dark"));
    require_common_contract(light, QStringLiteral("light"));

    QVERIFY2(dark.dark_pixels * 100 >= dark.total_pixels * 60,
             qPrintable(QStringLiteral(
                 "dark Paymaster setup surface was not predominantly dark (%1/%2 dark, %3 light pixels)")
                            .arg(dark.dark_pixels)
                            .arg(dark.total_pixels)
                            .arg(dark.light_pixels)));
    QVERIFY2(dark.light_pixels * 100 < dark.total_pixels * 25,
             qPrintable(QStringLiteral(
                 "dark Paymaster setup leaked a large light surface (%1/%2 light pixels)")
                            .arg(dark.light_pixels)
                            .arg(dark.total_pixels)));
    QVERIFY2(light.light_pixels * 100 >= light.total_pixels * 60,
             qPrintable(QStringLiteral(
                 "light Paymaster setup surface was not predominantly light (%1/%2 light, %3 dark pixels)")
                            .arg(light.light_pixels)
                            .arg(light.total_pixels)
                            .arg(light.dark_pixels)));
}

void PaymasterWidgetTests::paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep_data()
{
    QTest::addColumn<bool>("deferred_funding");
    QTest::addColumn<bool>("encrypted_start");
    QTest::newRow("already-funded") << false << false;
    QTest::newRow("disabled-provider-accepted-pending") << true << false;
    QTest::newRow("encrypted-start") << true << true;
}

void PaymasterWidgetTests::paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep()
{
    QFETCH(bool, deferred_funding);
    QFETCH(bool, encrypted_start);
    int operating_unlocks{0}, reviewed_starts{0};
    bool operating_default_seen{false};
    bool missing_funding = deferred_funding;
    int pool_approvals{0};
    bool funding_approved_while_disabled{false};
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({},
            GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(
        *test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    m_node.setContext(&test.m_node);
    const std::shared_ptr<wallet::CWallet>& wallet =
        SetupDescriptorsWallet(m_node, test, "qt-paymaster-guided-retry");

    DigiDollarMiniGUI mini_gui(m_node);
    mini_gui.initModelForWallet(m_node, wallet);
    DigiDollarTab tab(mini_gui.platformStyle.get());

    int operating_policy_attempts{0};
    int safety_attempts{0};
    UniValue advertised_policy;
    UniValue last_preview_policy;
    UniValue safety_policy;
    UniValue liquidity_policy;
    QList<UniValue> safety_payloads;
    QList<bool> enabled_requests;
    QStringList setup_write_order;
    bool fail_safety_snapshot{true};
    bool malformed_safety_snapshot{false};
    bool malformed_liquidity_snapshot{false};
    bool rpc_settings_present{false};
    bool rpc_enabled{false};
    bool rpc_running{false};
    bool rpc_ready{false};
    bool malformed_provider_snapshot{false};
    int mutation_rpc_calls{0};
    std::string rpc_operation_mode{"automatic"};
    bool rpc_autostart{false};
    const auto suggested_liquidity_policy = [] {
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("automatic_replenishment", true);
        policy.pushKV("paid_maintenance_approved", false);
        policy.pushKV("target_admission_dgb", 3);
        policy.pushKV("target_operational_dgb", 1);
        policy.pushKV("target_admission_carriers", 0);
        policy.pushKV("target_operational_carriers", 0);
        policy.pushKV(
            "maximum_maintenance_fee_per_transaction_satoshis", 20000000);
        policy.pushKV(
            "maximum_maintenance_fee_per_hour_satoshis", 200000000);
        policy.pushKV(
            "maximum_maintenance_fee_per_day_satoshis", 1000000000);
        return policy;
    };
    tab.setPaymasterRpcExecutorForTesting(
        [&](const std::string& command, const UniValue& params) {
            if (command == "startpaymaster" ||
                command == "setpaymasterenabled" ||
                (command == "preparepaymasterpool" && params[0].find_value("execute").isTrue()) ||
                command == "rebalancepaymasterpool") {
                ++mutation_rpc_calls;
            }
            if (command == "getpaymasterinfo" || command == "getpaymasteroperatorinfo") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("wallet_eligible", true);
                result.pushKV("settings_present", rpc_settings_present);
                result.pushKV("provider_id",
                              "1111111111111111111111111111111111111111111111111111111111111111");
                result.pushKV("enabled", rpc_enabled);
                result.pushKV("running", rpc_running);
                result.pushKV("ready", rpc_ready);
                result.pushKV("wallet_locked", false);
                result.pushKV("pool_ready", !missing_funding);
                result.pushKV("operation_mode", rpc_operation_mode);
                if (malformed_provider_snapshot) {
                    result.pushKV("autostart", "not-a-boolean");
                } else {
                    result.pushKV("autostart", rpc_autostart);
                }
                result.pushKV("service_state",
                              rpc_running ? "active" : "stopped");
                UniValue service_queue{UniValue::VOBJ};
                service_queue.pushKV("waiting_requests", 0);
                service_queue.pushKV("waiting_submits", 0);
                result.pushKV("service_queue", std::move(service_queue));
                UniValue pool{UniValue::VOBJ};
                pool.pushKV("entries", 0);
                pool.pushKV("reserved", 0);
                pool.pushKV("admission_dgb", 0);
                pool.pushKV("admission_carriers", 0);
                pool.pushKV("operational_dgb", 0);
                pool.pushKV("operational_carriers", 0);
                pool.pushKV("complete_operational_slots", 0);
                result.pushKV("pool", std::move(pool));
                result.pushKV("readiness_errors", UniValue{UniValue::VARR});
                if (advertised_policy.isObject()) {
                    result.pushKV("policy", advertised_policy);
                }
                if (command == "getpaymasteroperatorinfo") {
                    UniValue aggregate{UniValue::VOBJ}, safety{UniValue::VOBJ};
                    aggregate.pushKV("schema_version", 1);
                    aggregate.pushKV("network", "regtest");
                    aggregate.pushKV("wallet", "qt-paymaster-guided-retry");
                    aggregate.pushKV("wallet_generation", "fixture-load");
                    if (fail_safety_snapshot) throw std::runtime_error("injected safety snapshot unavailable");
                    auto liquidity = PaymasterLiquidityStatus(missing_funding ? "replenishing_liquidity" : "ready",
                        liquidity_policy.isObject(), true, missing_funding ? 1 : 0);
                    liquidity.pushKV("policy", malformed_liquidity_snapshot ? UniValue{UniValue::VOBJ} :
                        liquidity_policy.isObject() ? liquidity_policy : suggested_liquidity_policy());
                    result.pushKV("liquidity", liquidity);
                    result.pushKV("preparation", UniValue{UniValue::VARR});
                    result.pushKV("active_operations", UniValue{UniValue::VARR});
                    aggregate.pushKV("unlocked_until", 0);
                    aggregate.pushKV("encrypted", encrypted_start);
                    aggregate.pushKV("observed_at", QDateTime::currentSecsSinceEpoch());
                    UniValue diagnostic, diagnostics{UniValue::VARR};
                    diagnostic.read(R"({"code":"PAYMASTER_LOCAL_READY","state":"ready","action":"start","area":"service"})");
                    diagnostics.push_back(diagnostic);
                    aggregate.pushKV("diagnostics", diagnostics);
                    aggregate.pushKV("provider", result);
                    safety.pushKV("configured", malformed_safety_snapshot || safety_policy.isObject());
                    if (malformed_safety_snapshot || safety_policy.isObject()) safety.pushKV("policy", malformed_safety_snapshot ? UniValue{UniValue::VOBJ} : safety_policy);
                    UniValue usage;
                    usage.read(R"({"reserved_network_fee_satoshis":0,"spent_network_fee_last_hour_satoshis":0,"spent_network_fee_last_day_satoshis":0,"active_quotes":0,"completed_last_hour":0,"completed_last_day":0,"can_accept_minimum_quote":true,"errors":[]})");
                    for (const auto* name : {"user_paid", "public_sponsored", "restricted_sponsored"}) safety.pushKV(name, usage);
                    aggregate.pushKV("safety", safety);
                    return aggregate;
                }
                return result;
            }
            if (command == "getpaymasterliquiditystatus") {
                UniValue result{UniValue::VOBJ};
                if (malformed_liquidity_snapshot) {
                    result.pushKV("policy_configured", true);
                    result.pushKV(
                        "targets_satisfy_provider_policy", true);
                    result.pushKV("policy", UniValue{UniValue::VOBJ});
                    return result;
                }
                const bool configured = liquidity_policy.isObject();
                const UniValue policy = configured
                    ? liquidity_policy
                    : suggested_liquidity_policy();
                result.pushKV("policy_configured", configured);
                result.pushKV("policy", policy);
                result.pushKV("targets_satisfy_provider_policy", true);
                result.pushKV(
                    "maintenance_state",
                    configured ? "ready"
                               : "waiting_for_maintenance_approval");
                const auto add_slot = [&](const char* name,
                                          const char* target_name) {
                    const int target =
                        policy.find_value(target_name).getInt<int>();
                    result.pushKV(
                        name,
                        PaymasterLiquiditySlotStatus(
                            target, 0, 0, target));
                };
                add_slot("admission_dgb", "target_admission_dgb");
                add_slot("operational_dgb", "target_operational_dgb");
                add_slot("admission_carriers",
                         "target_admission_carriers");
                add_slot("operational_carriers",
                         "target_operational_carriers");
                result.pushKV("maintenance_fee_reserved_satoshis", 0);
                result.pushKV(
                    "maintenance_fee_spent_last_hour_satoshis", 0);
                result.pushKV(
                    "maintenance_fee_spent_last_day_satoshis", 0);
                result.pushKV("carrier_base_cents", 0);
                result.pushKV("carrier_withdrawable_excess_cents", 0);
                result.pushKV("readiness_errors", UniValue{UniValue::VARR});
                return result;
            }
            if (command == "getpaymasterpoolinfo") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("pool", UniValue{UniValue::VARR});
                return result;
            }
            if (command == "getpaymastersafetystatus") {
                if (fail_safety_snapshot) {
                    throw std::runtime_error("injected safety snapshot unavailable");
                }
                UniValue result{UniValue::VOBJ};
                if (malformed_safety_snapshot) {
                    result.pushKV("configured", true);
                    result.pushKV("policy", UniValue{UniValue::VOBJ});
                    return result;
                }
                const bool configured = safety_policy.isObject();
                result.pushKV("configured", configured);
                if (configured) result.pushKV("policy", safety_policy);
                return result;
            }
            if (command == "getpaymasterclientsafetystatus") {
                UniValue result{UniValue::VOBJ};
                result.pushKV("configured", false);
                return result;
            }
            if (command == "walletpassphrase") {
                if (params[1].getInt<int64_t>() != 0 || !params[2].isTrue()) throw std::runtime_error("Expected continuous operating unlock");
                ++operating_unlocks;
                return UniValue{};
            }
            if (command == "startpaymaster") {
                if (operating_unlocks != 1 || !params[0].find_value("wait_for_readiness").isTrue()) throw std::runtime_error("Expected operating unlock before reviewed start");
                ++reviewed_starts;
                UniValue result{UniValue::VOBJ};
                result.pushKV("start_requested", true);
                result.pushKV("running", false);
                return result;
            }
            if (command == "stoppaymaster") {
                if (!params[0].find_value("persistent").isTrue() || !params[0].find_value("pause_setup").isTrue()) throw std::runtime_error("Expected persistent setup pause");
                setup_write_order.push_back(QString::fromStdString(command));
                rpc_enabled = false;
                rpc_autostart = false;
                rpc_running = false;
                UniValue result{UniValue::VOBJ};
                result.pushKV("running", false);
                return result;
            }
            if (command == "setpaymasterpolicy") {
                ++operating_policy_attempts;
                setup_write_order.push_back(QString::fromStdString(command));
                advertised_policy = params[0];
                UniValue result = params[0];
                result.pushKV(
                    "policy_hash",
                    "3333333333333333333333333333333333333333333333333333333333333333");
                return result;
            }
            if (command == "setpaymastersafetypolicy") {
                ++safety_attempts;
                setup_write_order.push_back(QString::fromStdString(command));
                safety_payloads.push_back(params[0]);
                if (safety_attempts == 1) {
                    throw std::runtime_error("injected safety retry");
                }
                safety_policy = params[0];
                UniValue result = params[0];
                result.pushKV("updated_at", safety_attempts);
                return result;
            }
            if (command == "setpaymasterenabled") {
                setup_write_order.push_back(QString::fromStdString(command));
                enabled_requests.push_back(params[0].get_bool());
                rpc_settings_present = true;
                rpc_enabled = params[0].get_bool();
                if (!rpc_enabled) rpc_running = false;
                UniValue result{UniValue::VOBJ};
                result.pushKV("enabled", params[0].get_bool());
                return result;
            }
            if (command == "setpaymasterliquiditypolicy") {
                setup_write_order.push_back(QString::fromStdString(command));
                liquidity_policy = params[0];
                UniValue result = params[0];
                result.pushKV("updated_at", 1234);
                return result;
            }
            if (command == "setpaymasterruntimesettings") {
                setup_write_order.push_back(QString::fromStdString(command));
                rpc_operation_mode =
                    params[0].find_value("operation_mode").get_str();
                rpc_autostart = params[0].find_value("autostart").get_bool();
                return params[0];
            }
            if (command == "preparepaymasterpool") {
                if (params[0].find_value("preview_policy").isObject()) last_preview_policy = params[0].find_value("preview_policy");
                UniValue result{UniValue::VOBJ};
                result.pushKV("preview_only", params[0].find_value("preview_policy").isObject());
                result.pushKV("accepted", params[0].find_value("execute").isTrue());
                result.pushKV("cancelled", false);
                result.pushKV("preparation", UniValue{UniValue::VARR});
                result.pushKV("maximum_fee_satoshis", 20000000);
                result.pushKV("maximum_total_fee_satoshis", 20000000);
                result.pushKV("executed", false);
                result.pushKV(
                    "plan_id",
                    "4444444444444444444444444444444444444444444444444444444444444444");
                result.pushKV("admission_dgb_slots",
                              params[0].find_value(
                                  "admission_dgb_slots").getInt<int>());
                result.pushKV("operational_dgb_slots",
                              params[0].find_value(
                                  "operational_dgb_slots").getInt<int>());
                result.pushKV("admission_carrier_slots",
                              params[0].find_value(
                                  "admission_carrier_slots").getInt<int>());
                result.pushKV("operational_carrier_slots",
                              params[0].find_value(
                                  "operational_carrier_slots").getInt<int>());
                if (params[0].find_value("execute").isTrue()) {
                    ++pool_approvals;
                    funding_approved_while_disabled = !rpc_enabled && !rpc_running;
                }
                result.pushKV("missing_admission_dgb_slots", missing_funding ? 1 : 0);
                result.pushKV("missing_operational_dgb_slots", 0);
                result.pushKV("missing_admission_carrier_slots", 0);
                result.pushKV("missing_operational_carrier_slots", 0);
                result.pushKV("admission_dgb_satoshis_each", 10000000);
                result.pushKV("operational_dgb_satoshis_each", 10000000);
                result.pushKV("carrier_cents_each", 100);
                result.pushKV("total_output_satoshis", 0);
                result.pushKV("total_carrier_cents", 0);
                return result;
            }
            throw std::runtime_error("injected unrelated status unavailable");
        });
    tab.setWalletModel(mini_gui.walletModel.get());
    tab.setClientModel(mini_gui.clientModel.get());

    QPushButton* guided_setup = tab.findChild<QPushButton*>(
        QStringLiteral("paymasterChooseGuidedSetup"));
    QVERIFY(guided_setup != nullptr);

    QLabel* provider_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterProviderStatus"));
    QVERIFY(provider_status != nullptr);
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    fail_safety_snapshot = false;
    malformed_safety_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_safety_snapshot = false;
    malformed_liquidity_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_liquidity_snapshot = false;
    malformed_provider_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    guided_setup->click();
    QVERIFY(provider_status->text().contains(QStringLiteral(
        "complete, exactly representable safety and liquidity snapshots")));
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);

    malformed_provider_snapshot = false;
    tab.setWalletModel(mini_gui.walletModel.get());

    // Model the unconfigured status that previously replaced the UI's 3/1
    // carrier preset with zeroes before the wizard was opened.
    UniValue unconfigured_liquidity_policy{UniValue::VOBJ};
    unconfigured_liquidity_policy.pushKV("automatic_replenishment", true);
    unconfigured_liquidity_policy.pushKV("paid_maintenance_approved", false);
    unconfigured_liquidity_policy.pushKV("target_admission_dgb", 3);
    unconfigured_liquidity_policy.pushKV("target_operational_dgb", 1);
    unconfigured_liquidity_policy.pushKV("target_admission_carriers", 0);
    unconfigured_liquidity_policy.pushKV("target_operational_carriers", 0);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_transaction_satoshis", 20000000);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_hour_satoshis", 200000000);
    unconfigured_liquidity_policy.pushKV(
        "maximum_maintenance_fee_per_day_satoshis", 1000000000);
    UniValue unconfigured_liquidity{UniValue::VOBJ};
    unconfigured_liquidity.pushKV("policy_configured", false);
    unconfigured_liquidity.pushKV(
        "targets_satisfy_provider_policy", true);
    unconfigured_liquidity.pushKV("policy",
                                  unconfigured_liquidity_policy);
    tab.setPaymasterLiquidityStatusForTesting(unconfigured_liquidity);
    QSpinBox* configured_admission_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterAdmissionCarrierSlots"));
    QSpinBox* configured_operational_carriers = tab.findChild<QSpinBox*>(
        QStringLiteral("paymasterOperationalCarrierSlots"));
    QVERIFY(configured_admission_carriers != nullptr);
    QVERIFY(configured_operational_carriers != nullptr);
    QCOMPARE(configured_admission_carriers->value(), 0);
    QCOMPARE(configured_operational_carriers->value(), 0);

    bool review_cancelled_without_writes{false};
    bool seven_numbered_pages{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        QCheckBox* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        if (!wallet_confirmation) {
            wizard->reject();
            return;
        }
        seven_numbered_pages = wizard->pageIds().size() == 7;
        int step{0};
        for (const int id : wizard->pageIds())
            seven_numbered_pages &= wizard->page(id)->title().startsWith(
                QStringLiteral("Step %1 of 7").arg(++step));
        wallet_confirmation->setChecked(true);
        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            wizard->next();
        }
        review_cancelled_without_writes = wizard->currentPage() &&
            wizard->currentPage()->objectName() ==
                QLatin1String("paymasterSetupReviewPage");
        wizard->reject();
    });
    guided_setup->click();
    QVERIFY(review_cancelled_without_writes);
    QVERIFY(seven_numbered_pages);
    QCOMPARE(operating_policy_attempts, 0);
    QCOMPARE(safety_attempts, 0);
    QVERIFY(liquidity_policy.isNull());

    bool wizard_opened{false};
    bool user_paid_defaults_ready{false};
    bool recommended_default{false};
    bool identity_name_validation_ready{false};
    bool sponsored_policy_defaults_remove_carrier_targets{false};
    bool profile_keeps_network_fee{false};
    bool approval_invalidated_by_custom_limit{false};
    bool maintenance_start_copy_consistent{false};
    bool safety_page_visited{false};
    bool compact_pages_scroll{false};
    bool first_failure_visible{false};
    bool durable_phase_is_close_only{false};
    bool retry_feedback_visible{false};
    bool retry_completed{false};
    bool funding_confirmation_seen{false};
    QString completion_text;
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        wizard_opened = true;

        QCheckBox* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        QCheckBox* user_paid = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupUserPaid"));
        QCheckBox* sponsored = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupSponsored"));
        QComboBox* sponsorship_scope = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSponsorshipScope"));
        QLineEdit* display_name = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupDisplayName"));
        QDoubleSpinBox* service_fee = wizard->findChild<QDoubleSpinBox*>(
            QStringLiteral("paymasterSetupServiceFeePercent"));
        QSpinBox* minimum_payment = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupMinimumPayment"));
        QSpinBox* network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        QComboBox* safety_profile = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSafetyProfile"));
        QLineEdit* custom_per_transaction = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerTransaction"));
        QLineEdit* custom_reserved = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomReserved"));
        QLineEdit* custom_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerHour"));
        QLineEdit* custom_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerDay"));
        QSpinBox* custom_completed_per_hour = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomCompletedPerHour"));
        QSpinBox* custom_completed_per_day = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomCompletedPerDay"));
        QSpinBox* custom_quotes_total = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesTotal"));
        QSpinBox* custom_quotes_per_netgroup = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesPerNetgroup"));
        QSpinBox* custom_quotes_per_recipient = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesPerRecipient"));
        QSpinBox* custom_requests_per_netgroup = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxQuoteRequestsPerNetgroupMinute"));
        QLineEdit* custom_maintenance_per_transaction = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerTransaction"));
        QLineEdit* custom_maintenance_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerHour"));
        QLineEdit* custom_maintenance_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerDay"));
        QSpinBox* admission_dgb = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionDgbSlots"));
        QSpinBox* operational_dgb = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalDgbSlots"));
        QSpinBox* admission_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionCarrierSlots"));
        QSpinBox* operational_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalCarrierSlots"));
        QPushButton* restore_funding_defaults = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreFundingDefaults"));
        QPushButton* restore_policy_defaults = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        QCheckBox* maintenance_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupMaintenanceApproval"));
        QPushButton* retry = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRetry"));
        QLabel* result = wizard->findChild<QLabel*>(
            QStringLiteral("paymasterSetupProgressResult"));
        auto* progress_page = wizard->findChild<QWizardPage*>(
            QStringLiteral("paymasterSetupProgressPage"));
        auto* safety_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupSafetyScroll"));
        auto* liquidity_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupLiquidityScroll"));
        auto* review_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupReviewScroll"));
        auto* progress_scroll = wizard->findChild<QScrollArea*>(
            QStringLiteral("paymasterSetupProgressScroll"));
        if (!wallet_confirmation || !user_paid || !sponsored ||
            !sponsorship_scope || !display_name || !service_fee ||
            !minimum_payment || !network_fee ||
            !safety_profile || !custom_per_transaction ||
            !custom_reserved || !custom_per_hour ||
            !custom_per_day || !custom_completed_per_hour ||
            !custom_completed_per_day || !custom_quotes_total ||
            !custom_quotes_per_netgroup || !custom_quotes_per_recipient ||
            !custom_requests_per_netgroup ||
            !custom_maintenance_per_transaction ||
            !custom_maintenance_per_hour || !custom_maintenance_per_day ||
            !admission_dgb || !operational_dgb ||
            !admission_carriers || !operational_carriers ||
            !restore_funding_defaults || !restore_policy_defaults ||
            !maintenance_confirmation || !retry || !result || !progress_page ||
            !safety_scroll || !liquidity_scroll || !review_scroll ||
            !progress_scroll) {
            wizard->reject();
            return;
        }
        wizard->resize(560, 440);
        compact_pages_scroll = safety_scroll->widgetResizable() &&
            liquidity_scroll->widgetResizable() &&
            review_scroll->widgetResizable() &&
            progress_scroll->widgetResizable() &&
            safety_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            liquidity_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            review_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff &&
            progress_scroll->horizontalScrollBarPolicy() ==
                Qt::ScrollBarAlwaysOff;

        user_paid_defaults_ready = user_paid->isChecked() &&
            !sponsored->isChecked() && admission_dgb->value() >= 3 &&
            operational_dgb->value() >= 1 && admission_carriers->value() >= 3 &&
            operational_carriers->value() >= 1;
        recommended_default = safety_profile->currentData().toString() ==
            QLatin1String("recommended");

        const QString valid_display_name(32, QLatin1Char('A'));
        display_name->setText(valid_display_name);
        const bool valid_name_accepted = display_name->hasAcceptableInput() &&
            display_name->maxLength() == 32;
        display_name->setText(QStringLiteral("invalid/name"));
        const bool slash_rejected = !display_name->hasAcceptableInput();
        display_name->setText(QStringLiteral("invalid@name"));
        const bool at_rejected = !display_name->hasAcceptableInput();
        display_name->setText(QString::fromUtf8("N\xC3\xA4me"));
        const bool unicode_rejected = !display_name->hasAcceptableInput();
        display_name->clear();
        identity_name_validation_ready = valid_name_accepted &&
            slash_rejected && at_rejected && unicode_rejected;

        user_paid->setChecked(false);
        sponsored->setChecked(true);
        sponsorship_scope->setCurrentIndex(sponsorship_scope->findData(
            QStringLiteral("restricted")));
        restore_policy_defaults->click();
        sponsored_policy_defaults_remove_carrier_targets =
            !user_paid->isChecked() &&
            sponsored->isChecked() && !service_fee->isEnabled() &&
            service_fee->value() == 0.0 && minimum_payment->minimum() == 100 &&
            admission_carriers->value() == 0 &&
            operational_carriers->value() == 0 && !admission_carriers->isEnabled() && !operational_carriers->isEnabled();
        restore_funding_defaults->click();
        restore_policy_defaults->click();

        wallet_confirmation->setChecked(true);
        user_paid->setChecked(true);
        sponsored->setChecked(false);
        // Safety profiles constrain aggregate/request exposure, not the
        // operator's independently chosen ceiling for one valid transfer.
        network_fee->setValue(30000000);
        const bool fee_before_profile_change =
            network_fee->value() == 30000000;
        safety_profile->setCurrentIndex(
            safety_profile->findData(QStringLiteral("custom")));
        profile_keeps_network_fee = fee_before_profile_change &&
            network_fee->value() == 30000000;
        custom_per_transaction->setText(QStringLiteral("0.30000000"));
        custom_reserved->setText(QStringLiteral("0.40000000"));
        custom_per_hour->setText(QStringLiteral("90071992.54740993"));
        custom_per_day->setText(QStringLiteral("21000000000.00000000"));
        custom_completed_per_hour->setValue(7);
        custom_completed_per_day->setValue(30);
        custom_quotes_total->setValue(20);
        custom_quotes_per_netgroup->setValue(5);
        custom_quotes_per_recipient->setValue(3);
        custom_requests_per_netgroup->setValue(12);
        custom_maintenance_per_transaction->setText(QStringLiteral("0.15000000"));
        custom_maintenance_per_hour->setText(QStringLiteral("0.50000000"));
        custom_maintenance_per_day->setText(QStringLiteral("2.00000000"));

        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            if (wizard->currentPage()->objectName() ==
                QLatin1String("paymasterSetupSafetyPage")) {
                safety_page_visited = true;
            }
            wizard->next();
        }
        if (!wizard->currentPage() ||
            wizard->currentPage()->objectName() !=
                QLatin1String("paymasterSetupReviewPage")) {
            wizard->reject();
            return;
        }
        maintenance_start_copy_consistent =
            maintenance_confirmation->text().contains(
                QStringLiteral("approval alone does not start"),
                Qt::CaseInsensitive) &&
            !maintenance_confirmation->text().contains(
                QStringLiteral("until I start it separately"),
                Qt::CaseInsensitive);
        maintenance_confirmation->setChecked(true);
        custom_maintenance_per_day->setText(QStringLiteral("2.10000000"));
        approval_invalidated_by_custom_limit =
            !maintenance_confirmation->isChecked();
        custom_maintenance_per_day->setText(QStringLiteral("2.00000000"));
        maintenance_confirmation->setChecked(true);

        auto* start_when_ready = wizard->findChild<QCheckBox*>("paymasterSetupStartWhenReady");
        QVERIFY(start_when_ready);
        start_when_ready->setChecked(encrypted_start);
        wizard->next();

        first_failure_visible =
            wizard->currentPage() == progress_page && retry->isVisible() &&
            result->text().contains(QStringLiteral("injected safety retry"));
        durable_phase_is_close_only =
            wizard->buttonText(QWizard::CancelButton) == QStringLiteral("Close");
        retry->click();
        retry_feedback_visible = !retry->isEnabled() &&
            result->text().contains(QStringLiteral("Retrying"));
        QTimer::singleShot(0, [&funding_confirmation_seen] {
            if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                funding_confirmation_seen = true;
                message->reject();
            }
        });
        QCoreApplication::processEvents();
        completion_text = result->text();
        retry_completed = progress_page->isComplete() && !retry->isVisible();
        wizard->reject();
    });
    QTimer operating_answer;
    connect(&operating_answer, &QTimer::timeout, &tab, [&] {
        if (auto* dialog = tab.findChild<QDialog*>("paymasterOperatingUnlockDialog"); dialog && dialog->isVisible()) {
            auto* mode = dialog->findChild<QComboBox*>("paymasterOperatingUnlockMode");
            operating_default_seen = mode && mode->currentIndex() == 0;
            dialog->accept();
        } else if (auto* password = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            password->setTextValue(QStringLiteral("test passphrase"));
            password->accept();
        }
    });
    operating_answer.start(10);
    guided_setup->click();
    operating_answer.stop();

    QCOMPARE(operating_default_seen, encrypted_start);
    QCOMPARE(operating_unlocks, encrypted_start ? 1 : 0);
    QCOMPARE(reviewed_starts, encrypted_start ? 1 : 0);
    QVERIFY(wizard_opened);
    QVERIFY(user_paid_defaults_ready);
    QVERIFY(recommended_default);
    QVERIFY(identity_name_validation_ready);
    QVERIFY(sponsored_policy_defaults_remove_carrier_targets);
    QVERIFY(profile_keeps_network_fee);
    QVERIFY(approval_invalidated_by_custom_limit);
    QVERIFY(maintenance_start_copy_consistent);
    QVERIFY(safety_page_visited);
    QVERIFY(compact_pages_scroll);
    QVERIFY(first_failure_visible);
    QVERIFY(durable_phase_is_close_only);
    QVERIFY(retry_feedback_visible);
    QVERIFY2(retry_completed, qPrintable(completion_text));
    QVERIFY(!funding_confirmation_seen);
    QVERIFY(completion_text.contains(QStringLiteral("Configuration saved")));
    QVERIFY(completion_text.contains(QStringLiteral("saved autostart")));
    QCOMPARE(completion_text.contains(QStringLiteral("Waiting for approved pool preparation or blockchain confirmations")), deferred_funding);
    QCOMPARE(operating_policy_attempts, 1);
    QCOMPARE(safety_attempts, 2);
    QCOMPARE(enabled_requests, QList<bool>{true});
    QCOMPARE(pool_approvals, deferred_funding ? 1 : 0);
    if (deferred_funding) QVERIFY(funding_approved_while_disabled);
    missing_funding = false;

    // A complete status may enable provider and funding actions. Any later
    // malformed provider snapshot must revoke all of them immediately and a
    // click on the now-disabled controls must not reach a mutation RPC.
    rpc_ready = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    const QStringList fail_closed_actions{
        QStringLiteral("paymasterPreviewPoolPreparation"),
        QStringLiteral("paymasterPreviewPoolRetirement"),
    };
    for (const QString& object_name : fail_closed_actions) {
        QPushButton* action = tab.findChild<QPushButton*>(object_name);
        QVERIFY2(action != nullptr, qPrintable(object_name));
        QVERIFY2(action->isEnabled(),
                 qPrintable(QStringLiteral("Expected a valid snapshot to enable %1")
                                .arg(object_name)));
    }
    malformed_provider_snapshot = true;
    tab.setWalletModel(mini_gui.walletModel.get());
    const int mutation_calls_before_disabled_clicks = mutation_rpc_calls;
    for (const QString& object_name : fail_closed_actions) {
        QPushButton* action = tab.findChild<QPushButton*>(object_name);
        QVERIFY2(action != nullptr, qPrintable(object_name));
        QVERIFY2(!action->isEnabled(),
                 qPrintable(QStringLiteral("Malformed status left %1 enabled")
                                .arg(object_name)));
        action->click();
    }
    QCOMPARE(mutation_rpc_calls, mutation_calls_before_disabled_clicks);
    malformed_provider_snapshot = false;
    rpc_ready = false;
    tab.setWalletModel(mini_gui.walletModel.get());

    const qint64 advertised_fee = advertised_policy.find_value(
        "maximum_network_fee_dgb_satoshis").getInt<qint64>();
    const UniValue& user_paid_safety = safety_policy.find_value("user_paid");
    const qint64 safety_fee = user_paid_safety.find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>();
    QCOMPARE(advertised_fee, 30000000);
    QCOMPARE(safety_fee, advertised_fee);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_reserved_network_fee_satoshis").getInt<qint64>(), 40000000);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_network_fee_per_hour_satoshis").getInt<qint64>(),
        9007199254740993LL);
    QCOMPARE(user_paid_safety.find_value(
        "maximum_network_fee_per_day_satoshis").getInt<qint64>(),
        2100000000000000000LL);
    QCOMPARE(user_paid_safety.find_value("maximum_completed_per_hour").getInt<int>(), 7);
    QCOMPARE(user_paid_safety.find_value("maximum_completed_per_day").getInt<int>(), 30);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_total").getInt<int>(), 20);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_per_netgroup").getInt<int>(), 5);
    QCOMPARE(safety_policy.find_value(
        "maximum_active_quotes_per_recipient").getInt<int>(), 3);
    QCOMPARE(safety_policy.find_value(
        "maximum_quote_requests_per_netgroup_per_minute").getInt<int>(), 12);
    QCOMPARE(liquidity_policy.find_value("target_admission_dgb").getInt<int>(), 3);
    QCOMPARE(liquidity_policy.find_value("target_operational_dgb").getInt<int>(), 1);
    QCOMPARE(liquidity_policy.find_value("target_admission_carriers").getInt<int>(), 3);
    QCOMPARE(liquidity_policy.find_value("target_operational_carriers").getInt<int>(), 1);
    QVERIFY(liquidity_policy.find_value("automatic_replenishment").get_bool());
    QVERIFY(liquidity_policy.find_value("paid_maintenance_approved").get_bool());
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_transaction_satoshis").getInt<qint64>(), 15000000);
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_hour_satoshis").getInt<qint64>(), 50000000);
    QCOMPARE(liquidity_policy.find_value(
        "maximum_maintenance_fee_per_day_satoshis").getInt<qint64>(), 200000000);

    UniValue disabled_existing{UniValue::VOBJ};
    disabled_existing.pushKV("wallet_eligible", true);
    disabled_existing.pushKV("settings_present", true);
    disabled_existing.pushKV("enabled", false);
    disabled_existing.pushKV("running", false);
    disabled_existing.pushKV("ready", false);
    disabled_existing.pushKV("wallet_locked", false);
    disabled_existing.pushKV("has_identity", true);
    disabled_existing.pushKV("has_policy", true);
    disabled_existing.pushKV("has_safety_policy", true);
    disabled_existing.pushKV("operation_mode", "automatic");
    disabled_existing.pushKV("autostart", false);
    disabled_existing.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(disabled_existing);

    bool reopened_existing_custom_values{false};
    bool restore_default_actions_available{false};
    bool safety_restore_keeps_refill_limits{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() != QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        auto* profile = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSafetyProfile"));
        auto* reopened_network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        auto* reopened_reserved = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomReserved"));
        auto* reopened_per_hour = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerHour"));
        auto* reopened_per_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomPerDay"));
        auto* reopened_quotes_total = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupCustomMaxActiveQuotesTotal"));
        auto* reopened_maintenance_day = wizard->findChild<QLineEdit*>(
            QStringLiteral("paymasterSetupCustomMaintenancePerDay"));
        auto* reopened_automatic = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutomaticReplenishment"));
        auto* reopened_operation_mode = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupOperationMode"));
        auto* reopened_autostart = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutostart"));
        auto* reopened_provider_enabled = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupProviderEnabled"));
        auto* restore_identity = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreIdentityDefaults"));
        auto* restore_funding = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreFundingDefaults"));
        auto* restore_policy = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        auto* restore_safety = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreSafetyDefaults"));
        auto* restore_liquidity = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreLiquidityDefaults"));
        if (!profile || !reopened_network_fee || !reopened_reserved ||
            !reopened_per_hour || !reopened_per_day || !reopened_quotes_total ||
            !reopened_maintenance_day || !reopened_automatic ||
            !reopened_operation_mode || !reopened_autostart ||
            !reopened_provider_enabled ||
            !restore_identity || !restore_funding || !restore_policy ||
            !restore_safety || !restore_liquidity) {
            wizard->reject();
            return;
        }
        reopened_existing_custom_values =
            profile->currentData().toString() == QLatin1String("custom") &&
            reopened_network_fee->value() == 30000000 &&
            reopened_reserved->text() == QLatin1String("0.40000000") &&
            reopened_per_hour->text() ==
                QLatin1String("90071992.54740993") &&
            reopened_per_day->text() ==
                QLatin1String("21000000000.00000000") &&
            reopened_quotes_total->value() == 20 &&
            reopened_maintenance_day->text() == QLatin1String("2.00000000") &&
            reopened_automatic->isChecked() &&
            reopened_operation_mode->currentData().toString() ==
                QLatin1String("automatic") &&
            !reopened_autostart->isChecked() &&
            !reopened_provider_enabled->isChecked();

        restore_identity->click();
        restore_funding->click();
        restore_policy->click();
        restore_safety->click();
        safety_restore_keeps_refill_limits = reopened_maintenance_day->text() == QLatin1String("2.00000000");
        restore_liquidity->click();
        restore_default_actions_available =
            profile->currentData().toString() == QLatin1String("recommended") &&
            reopened_network_fee->value() == 20000000 &&
            reopened_quotes_total->value() == 16 &&
            reopened_provider_enabled->isChecked();
        wizard->reject();
    });
    guided_setup->click();
    QVERIFY(reopened_existing_custom_values);
    QVERIFY(safety_restore_keeps_refill_limits);
    QVERIFY(restore_default_actions_available);

    setup_write_order.clear();
    rpc_settings_present = true;
    rpc_enabled = true;
    UniValue enabled_existing{UniValue::VOBJ};
    enabled_existing.pushKV("wallet_eligible", true);
    enabled_existing.pushKV("settings_present", true);
    enabled_existing.pushKV("enabled", true);
    enabled_existing.pushKV("running", false);
    enabled_existing.pushKV("ready", false);
    enabled_existing.pushKV("wallet_locked", false);
    enabled_existing.pushKV("has_identity", true);
    enabled_existing.pushKV("has_policy", true);
    enabled_existing.pushKV("has_safety_policy", true);
    enabled_existing.pushKV("operation_mode", "automatic");
    enabled_existing.pushKV("autostart", true);
    enabled_existing.pushKV("readiness_errors", UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(enabled_existing);

    bool reconfiguration_completed{false};
    bool sponsored_carrier_targets_cleared{false};
    bool disabled_target_retained{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(
            QApplication::activeModalWidget());
        if (!wizard || wizard->objectName() !=
                QLatin1String("PaymasterSetupWizard")) {
            return;
        }
        auto* wallet_confirmation = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupWalletConfirmation"));
        auto* user_paid = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupUserPaid"));
        auto* sponsored = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupSponsored"));
        auto* scope = wizard->findChild<QComboBox*>(
            QStringLiteral("paymasterSetupSponsorshipScope"));
        auto* fee = wizard->findChild<QDoubleSpinBox*>(
            QStringLiteral("paymasterSetupServiceFeePercent"));
        auto* minimum = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupMinimumPayment"));
        auto* network_fee = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupNetworkFee"));
        auto* admission_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupAdmissionCarrierSlots"));
        auto* operational_carriers = wizard->findChild<QSpinBox*>(
            QStringLiteral("paymasterSetupOperationalCarrierSlots"));
        auto* provider_enabled = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupProviderEnabled"));
        auto* autostart = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupAutostart"));
        auto* restore_policy = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestorePolicyDefaults"));
        auto* restore_safety = wizard->findChild<QPushButton*>(
            QStringLiteral("paymasterSetupRestoreSafetyDefaults"));
        auto* approval = wizard->findChild<QCheckBox*>(
            QStringLiteral("paymasterSetupMaintenanceApproval"));
        auto* progress_page = wizard->findChild<QWizardPage*>(
            QStringLiteral("paymasterSetupProgressPage"));
        if (!wallet_confirmation || !user_paid || !sponsored || !scope ||
            !fee || !minimum || !network_fee || !admission_carriers ||
            !operational_carriers || !provider_enabled || !autostart ||
            !restore_policy || !restore_safety || !approval ||
            !progress_page) {
            wizard->reject();
            return;
        }

        wallet_confirmation->setChecked(true);
        user_paid->setChecked(false);
        sponsored->setChecked(true);
        scope->setCurrentIndex(scope->findData(QStringLiteral("restricted")));
        restore_policy->click();
        network_fee->setValue(10000000);
        restore_safety->click();
        provider_enabled->setChecked(false);
        autostart->setChecked(true);
        sponsored_carrier_targets_cleared = admission_carriers->value() == 0 &&
            operational_carriers->value() == 0;
        disabled_target_retained = !provider_enabled->isChecked();

        int page_guard{0};
        while (wizard->currentPage() &&
               wizard->currentPage()->objectName() !=
                   QLatin1String("paymasterSetupReviewPage") &&
               page_guard++ < 10) {
            wizard->next();
        }
        if (!wizard->currentPage() ||
            wizard->currentPage()->objectName() !=
                QLatin1String("paymasterSetupReviewPage")) {
            wizard->reject();
            return;
        }
        approval->setChecked(true);
        wizard->next();
        QCoreApplication::processEvents();
        reconfiguration_completed = progress_page->isComplete();
        wizard->reject();
    });
    guided_setup->click();

    QVERIFY(reconfiguration_completed);
    QVERIFY(sponsored_carrier_targets_cleared);
    QVERIFY(disabled_target_retained);
    QCOMPARE(operating_policy_attempts, 2);
    QCOMPARE(safety_attempts, 4);
    QCOMPARE(enabled_requests, QList<bool>({true, false}));
    QCOMPARE(setup_write_order, QStringList({
                                    QStringLiteral("stoppaymaster"),
                                    QStringLiteral("setpaymastersafetypolicy"),
                                    QStringLiteral("setpaymasterpolicy"),
                                    QStringLiteral("setpaymastersafetypolicy"),
                                    QStringLiteral("setpaymasterliquiditypolicy"),
                                    QStringLiteral("setpaymasterruntimesettings"),
                                    QStringLiteral("setpaymasterenabled"),
                                }));
    QCOMPARE(advertised_policy.find_value("sponsorship_scope").get_str(),
             std::string{"restricted"});
    QCOMPARE(advertised_policy.find_value("fee_rate_bps").getInt<int>(), 0);
    QCOMPARE(advertised_policy.find_value(
                 "maximum_user_paid_service_fee_cents").getInt<int64_t>(),
             int64_t{0});
    QCOMPARE(advertised_policy.find_value(
        "maximum_network_fee_dgb_satoshis").getInt<qint64>(), 10000000);
    const UniValue& final_models =
        advertised_policy.find_value("funding_models");
    QCOMPARE(final_models.size(), 1U);
    QCOMPARE(final_models[0].get_str(), std::string{"sponsored"});
    const UniValue& bridge = safety_payloads.at(2);
    QCOMPARE(bridge.find_value("user_paid").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);
    QCOMPARE(bridge.find_value("restricted_sponsored").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);
    // Core also validates inactive classes against the new advertised ceiling.
    // The review shows the reduced per-transfer ceiling; aggregate limits stay saved.
    QCOMPARE(safety_policy.find_value("user_paid").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);
    QCOMPARE(safety_policy.find_value("restricted_sponsored").find_value(
        "maximum_network_fee_per_transaction_satoshis").getInt<qint64>(),
        10000000);

    // An out-of-range QSpinBox value remains the exact policy in both preview
    // and execution. It must never be silently replaced by the display limit.
    advertised_policy.pushKV("maximum_network_fee_dgb_satoshis", int64_t{3000000000});
    rpc_autostart = true;
    auto* refresh_setup = tab.findChild<QPushButton*>("paymasterOverviewRefresh");
    QVERIFY(refresh_setup);
    refresh_setup->click();
    bool retained_large_policy{false};
    bool restored_autostart_matches{false};
    QTimer::singleShot(0, [&] {
        auto* wizard = qobject_cast<QWizard*>(QApplication::activeModalWidget());
        if (!wizard) return;
        auto* wallet_confirmation = wizard->findChild<QCheckBox*>("paymasterSetupWalletConfirmation");
        auto* approval = wizard->findChild<QCheckBox*>("paymasterSetupMaintenanceApproval");
        auto* restore_liquidity = wizard->findChild<QPushButton*>("paymasterSetupRestoreLiquidityDefaults");
        auto* autostart = wizard->findChild<QCheckBox*>("paymasterSetupAutostart");
        auto* enabled = wizard->findChild<QCheckBox*>("paymasterSetupProviderEnabled");
        auto* progress = wizard->findChild<QWizardPage*>("paymasterSetupProgressPage");
        if (!wallet_confirmation || !approval || !restore_liquidity || !autostart || !enabled || !progress) { wizard->reject(); return; }
        wallet_confirmation->setChecked(true);
        restore_liquidity->click();
        restored_autostart_matches = autostart->isChecked() == rpc_autostart;
        enabled->setChecked(false);
        for (int i = 0; i < 10 && wizard->currentPage()->objectName() != QLatin1String("paymasterSetupReviewPage"); ++i) wizard->next();
        if (last_preview_policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>() == 3000000000) {
            approval->setChecked(true);
            wizard->next();
            QCoreApplication::processEvents();
            retained_large_policy = progress->isComplete() && advertised_policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>() == 3000000000;
        }
        wizard->reject();
    });
    guided_setup->click();
    QVERIFY(restored_autostart_matches);
    QVERIFY(retained_large_policy);

    UniValue disabled_with_autostart{UniValue::VOBJ};
    disabled_with_autostart.pushKV("wallet_eligible", true);
    disabled_with_autostart.pushKV("settings_present", true);
    disabled_with_autostart.pushKV("enabled", false);
    disabled_with_autostart.pushKV("running", false);
    disabled_with_autostart.pushKV("ready", false);
    disabled_with_autostart.pushKV("wallet_locked", false);
    disabled_with_autostart.pushKV("has_identity", true);
    disabled_with_autostart.pushKV("has_policy", true);
    disabled_with_autostart.pushKV("has_safety_policy", true);
    disabled_with_autostart.pushKV("operation_mode", "automatic");
    disabled_with_autostart.pushKV("autostart", true);
    disabled_with_autostart.pushKV("service_state", "stopped");
    disabled_with_autostart.pushKV("readiness_errors",
                                   UniValue{UniValue::VARR});
    tab.setPaymasterReadinessStatusForTesting(disabled_with_autostart);
    QLabel* runtime_status = tab.findChild<QLabel*>(
        QStringLiteral("paymasterActivityRuntimeStatus"));
    QVERIFY(runtime_status != nullptr);
    QVERIFY(runtime_status->text().contains(
        QStringLiteral("configuration is disabled"), Qt::CaseInsensitive));
    QVERIFY(!runtime_status->text().contains(
        QStringLiteral("will start it"), Qt::CaseInsensitive));
}


void PaymasterWidgetTests::paymasterPoolPreparationStoredFeeDiagnostics()
{
    using namespace DigiDollar::Paymaster;
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-pool-fee-diagnostics");
    CKey key;
    key.MakeNewKey(true);
    ProviderMaintenanceOutput output;
    output.asset = PoolAsset::DD_CARRIER;
    output.purpose = PoolPurpose::OPERATIONAL;
    output.script_pub_key = GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
    output.carrier_value = DDCents{100};
    ProviderMaintenanceRecord record;
    record.kind = ProviderMaintenanceKind::PREPARE_CARRIER;
    record.operation_id = uint256S("a1");
    record.plan_id = uint256S("a2");
    record.preparation_authorization = uint256S("a3");
    record.preparation_request = uint256S("a4");
    record.maximum_fee = DGBSatoshis{20000000};
    record.created_at = record.updated_at = 1000;
    record.outputs = {output};
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    const std::vector<std::string> errors{
        "PAYMASTER_POOL_FEE_LIMIT",
        "PAYMASTER_POOL_FEE_LIMIT: estimated fee 0.25 DGB exceeds approved setup limit 0.20 DGB",
        "PAYMASTER_POOL_FEE_ESTIMATE_EXCEEDED: signed fee 0.15 DGB exceeds planned fee 0.1 DGB; approved setup limit 0.20 DGB",
        "PAYMASTER_POOL_FEE_INVALID",
        "PAYMASTER_POOL_WAITING_FUNDS: insufficient DD"};
    LOCK(wallet->cs_wallet);
    wallet::WalletBatch batch{wallet->GetDatabase()};
    for (const auto& error : errors) {
        record.preparation_error = error;
        ProviderMaintenanceLedger ledger;
        ledger.records = {record};
        QVERIFY(batch.WritePaymasterMaintenanceLedger(ledger));
        const auto records = wallet::paymaster_rpc::internal::PoolPreparationToJSON(*wallet);
        QCOMPARE(records.size(), size_t{1});
        const auto& step = records[0];
        const bool fee_detail = error.rfind("PAYMASTER_POOL_FEE_", 0) == 0 && error.find(':') != std::string::npos;
        QCOMPARE(step.find_value("error").get_str(), fee_detail ? error.substr(0, error.find(':')) : error);
        QCOMPARE(step.find_value("diagnostic").isStr(), fee_detail);
        QCOMPARE(step.find_value("state").get_str(), std::string{"pending_creation"});
        QCOMPARE(step.find_value("maximum_fee_satoshis").getInt<int64_t>(), int64_t{20000000});
        QVERIFY(step.find_value("txid").isNull());
        if (fee_detail) {
            auto pool = PaymasterLiquidityPoolStatus();
            pool.pushKV("preparation", records);
            panel->setLiquidityPoolForTesting(pool);
            QVERIFY(panel->findChild<QLabel*>("paymasterPoolPreparationStatus")->text().contains(QString::fromStdString(step.find_value("diagnostic").get_str())));
        }
        ProviderMaintenanceLedger after;
        QVERIFY(batch.ReadPaymasterMaintenanceLedger(after));
        QCOMPARE(after.records[0].preparation_error, error);
        QCOMPARE(after.records[0].maximum_fee.value, int64_t{20000000});
        QVERIFY(after.records[0].transaction_id.IsNull());
    }
}

void PaymasterWidgetTests::paymasterPoolPreparationDiagnostics_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<QString>("error");
    QTest::addColumn<QString>("expected_action");
    QTest::addColumn<QString>("expected_text");
    QTest::newRow("fee_limit") << QStringLiteral("pending_creation") << QStringLiteral("PAYMASTER_POOL_FEE_LIMIT") << QStringLiteral("review_liquidity") << QStringLiteral("0.20000000 DGB");
    QTest::newRow("invalid_fee") << QStringLiteral("pending_creation") << QStringLiteral("PAYMASTER_POOL_FEE_INVALID") << QStringLiteral("inspect_error") << QStringLiteral("Raising the approved setup limit does not resolve");
    QTest::newRow("estimate_exceeded") << QStringLiteral("pending_creation") << QStringLiteral("PAYMASTER_POOL_FEE_ESTIMATE_EXCEEDED") << QStringLiteral("inspect_error") << QStringLiteral("Raising the approved setup limit does not resolve");
    QTest::newRow("funding") << QStringLiteral("pending_creation") << QStringLiteral("PAYMASTER_POOL_WAITING_FUNDS: insufficient DD") << QStringLiteral("review_liquidity") << QStringLiteral("Waiting for usable wallet funds");
    QTest::newRow("parent_confirmation") << QStringLiteral("pending_creation") << QStringLiteral("PAYMASTER_POOL_WAITING_CONFIRMATION") << QStringLiteral("wait") << QStringLiteral("funding inputs to confirm");
    QTest::newRow("saved_transaction") << QStringLiteral("pending_confirmation") << QStringLiteral("PAYMASTER_POOL_WAITING_CONFIRMATION") << QStringLiteral("wait") << QStringLiteral("saved transaction is waiting");
    QTest::newRow("queued") << QStringLiteral("pending_creation") << QStringLiteral("") << QStringLiteral("wait") << QStringLiteral("30 seconds");
    QTest::newRow("policy_changed") << QStringLiteral("pending_creation") << QStringLiteral("PAYMASTER_POOL_POLICY_CHANGED") << QStringLiteral("review_liquidity") << QStringLiteral("policy changed");
    QTest::newRow("locked") << QStringLiteral("pending_creation") << QStringLiteral("PAYMASTER_WALLET_LOCKED") << QStringLiteral("unlock") << QStringLiteral("wallet unlock");
    QTest::newRow("disabled") << QStringLiteral("pending_creation") << QStringLiteral("PAYMASTER_PROVIDER_DISABLED") << QStringLiteral("review_liquidity") << QStringLiteral("configuration is disabled");
    QTest::newRow("conflict") << QStringLiteral("conflict") << QStringLiteral("PAYMASTER_POOL_TRANSACTION_CONFLICT") << QStringLiteral("inspect_error") << QStringLiteral("has a conflict");
    QTest::newRow("unknown") << QStringLiteral("pending_creation") << QStringLiteral("FUTURE_POOL_ERROR") << QStringLiteral("inspect_error") << QStringLiteral("diagnostic review");
    QTest::newRow("unknown_state") << QStringLiteral("future_state") << QStringLiteral("") << QStringLiteral("inspect_error") << QStringLiteral("diagnostic review");
}

void PaymasterWidgetTests::paymasterPoolPreparationDiagnostics()
{
    QFETCH(QString, state);
    QFETCH(QString, error);
    QFETCH(QString, expected_action);
    QFETCH(QString, expected_text);
    UniValue step{UniValue::VOBJ};
    step.pushKV("plan_id", std::string(64, 'a'));
    step.pushKV("state", state.toStdString());
    step.pushKV("error", error.toStdString());
    step.pushKV("asset", "dd_carrier");
    step.pushKV("maximum_fee_satoshis", 20000000);
    if (state == QLatin1String("pending_confirmation")) step.pushKV("txid", std::string(64, 'b'));
    UniValue steps{UniValue::VARR};
    steps.push_back(step);
    UniValue snapshot;
    QVERIFY(snapshot.read(R"({"schema_version":1,"unlocked_until":0,"provider":{"running":true,"ready":false,"enabled":true,"autostart":false,"wallet_locked":false,"service_state":"waiting_for_readiness","last_service_error":"PAYMASTER_POOL_PREPARATION_PENDING","readiness_errors":["PAYMASTER_OPERATIONAL_SLOT_MISSING"],"transport":{"listener_ready":true},"backup_status":{"required":true}}})"));
    UniValue provider = snapshot.find_value("provider");
    provider.pushKV("preparation", steps);
    snapshot.pushKV("provider", provider);
    const UniValue diagnostics = DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100);
    QCOMPARE(QString::fromStdString(diagnostics[0].find_value("action").get_str()), expected_action);
    snapshot.pushKV("diagnostics", diagnostics);

    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    int mutations{0};
    panel->setRpcExecutorForTesting([&](const std::string&, const UniValue&) { ++mutations; return UniValue{UniValue::VOBJ}; });
    panel->setMutationSnapshotsAvailableForTesting(true, true, true);
    auto pool = PaymasterLiquidityPoolStatus();
    pool.pushKV("preparation", steps);
    panel->setLiquidityPoolForTesting(pool);
    panel->setOperatorStatusForTesting(snapshot);
    auto* details = panel->findChild<QLabel*>("paymasterPoolPreparationStatus");
    auto* cancel = panel->findChild<QPushButton*>("paymasterCancelPoolPreparation");
    auto* headline = panel->findChild<QLabel*>("paymasterOperatorHeadline");
    auto* hint = panel->findChild<QLabel*>("paymasterOperatorHint");
    QVERIFY(details && cancel && headline && hint);
    QVERIFY2(details->text().contains(expected_text), qPrintable(details->text()));
    if (expected_action == QLatin1String("wait") || expected_action == QLatin1String("review_liquidity")) {
        if (error == QLatin1String("PAYMASTER_POOL_FEE_LIMIT")) {
            QVERIFY(headline->text().contains("Setup fee"));
            QVERIFY(hint->text().contains("new plan"));
        } else {
            QVERIFY(headline->text().contains(QStringLiteral("Pool preparation")));
            QVERIFY(hint->text() == details->text().section(QLatin1Char('\n'), 0, 0));
        }
    }
    QCOMPARE(!cancel->isHidden(), state == QLatin1String("pending_creation"));
    QCOMPARE(mutations, 0);
    panel->setPrivacy(true);
    QVERIFY(details->text().isEmpty());
    QVERIFY(cancel->isHidden());
}

void PaymasterWidgetTests::paymasterPoolPreparationCancellation_data()
{
    QTest::addColumn<QString>("decision");
    for (const char* outcome : {"approve", "decline", "privacy", "wallet_change", "signed_race"})
        QTest::newRow(outcome) << QString::fromLatin1(outcome);
}

void PaymasterWidgetTests::paymasterPoolPreparationCancellation()
{
    QFETCH(QString, decision);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    UniValue step;
    QVERIFY(step.read(R"({"plan_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","asset":"dd_carrier","state":"pending_creation","error":"PAYMASTER_POOL_FEE_LIMIT","maximum_fee_satoshis":20000000})"));
    UniValue steps{UniValue::VARR};
    steps.push_back(step);
    auto pool = PaymasterLiquidityPoolStatus();
    pool.pushKV("preparation", steps);
    int cancels{0};
    int other_mutations{0};
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command.rfind("get", 0) == 0) throw std::runtime_error("test follow-up unavailable");
        if (command != "preparepaymasterpool") {
            ++other_mutations;
            return UniValue{};
        }
        ++cancels;
        if (!params[0].find_value("cancel").isTrue() || !params[0].find_value("execute").isTrue() ||
            params[0].find_value("plan_id").get_str() != std::string(64, 'a') || params[0].size() != 7) ++other_mutations;
        UniValue result{UniValue::VOBJ};
        result.pushKV("plan_id", std::string(64, 'a'));
        result.pushKV("cancelled", decision != QLatin1String("signed_race"));
        UniValue returned{UniValue::VARR};
        if (decision == QLatin1String("signed_race")) {
            step.pushKV("state", "pending_confirmation");
            step.pushKV("txid", std::string(64, 'b'));
            returned.push_back(step);
        }
        result.pushKV("preparation", returned);
        return result;
    });
    panel->setMutationSnapshotsAvailableForTesting(true, true, true);
    panel->setLiquidityPoolForTesting(pool);
    auto* tabs = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    for (int i = 0; i < tabs->count(); ++i)
        tabs->widget(i)->setEnabled(true);
    auto* cancel = panel->findChild<QPushButton*>("paymasterCancelPoolPreparation");
    QVERIFY(cancel && cancel->isEnabled());
    QTimer answer;
    connect(&answer, &QTimer::timeout, panel.get(), [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        answer.stop();
        if (decision == QLatin1String("privacy"))
            panel->setPrivacy(true);
        else {
            if (decision == QLatin1String("wallet_change")) panel->setWalletModel(nullptr);
            box->done(decision == QLatin1String("decline") ? QMessageBox::No : QMessageBox::Yes);
        }
    });
    answer.start(5);
    cancel->click();
    QCOMPARE(cancels, decision == QLatin1String("approve") || decision == QLatin1String("signed_race") ? 1 : 0);
    QCOMPARE(other_mutations, 0);
    if (cancels) {
        QVERIFY(cancel->isHidden());
        auto* output = panel->findChild<QPlainTextEdit*>("paymasterLiquidityResult");
        QVERIFY(output);
        QVERIFY(output->toPlainText().contains(decision == QLatin1String("signed_race") ? QStringLiteral("not confirmed") : QStringLiteral("cancelled")));
    }
}

void PaymasterWidgetTests::paymasterOperatorOverviewGuidesAndFailsClosed()
{
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    panel->setObjectName(QStringLiteral("paymasterWidget"));
    panel->setRpcExecutorForTesting([](const std::string&, const UniValue&) { return UniValue{UniValue::VOBJ}; });
    auto* tabs = panel->findChild<QStackedWidget*>(QStringLiteral("paymasterOperatorPages"));
    auto* settings = panel->findChild<QStackedWidget*>(QStringLiteral("paymasterSettingsPages"));
    auto* headline = panel->findChild<QLabel*>(QStringLiteral("paymasterOperatorHeadline"));
    auto* hint = panel->findChild<QLabel*>(QStringLiteral("paymasterOperatorHint"));
    auto* connection = panel->findChild<QLabel*>(QStringLiteral("paymasterOperatorConnection"));
    auto* action = panel->findChild<QPushButton*>(QStringLiteral("paymasterOperatorNextAction"));
    auto* pause = panel->findChild<QPushButton*>(QStringLiteral("paymasterOperatorPause"));
    auto* card = panel->findChild<QGroupBox*>(QStringLiteral("paymasterOperatorCard"));
    QVERIFY(tabs && settings && headline && hint && connection && action && pause && card);
    QCOMPARE(tabs->count(), 5);
    QCOMPARE(settings->count(), 6);
    auto* details = panel->findChild<QWidget*>(QStringLiteral("paymasterTechnicalDetails"));
    QVERIFY(details);
    QVERIFY(details->isHidden());
    auto* details_toggle = panel->findChild<QPushButton*>("paymasterTechnicalDetailsToggle");
    QVERIFY(details_toggle);
    details_toggle->setChecked(true);
    QVERIFY(!details->isHidden());
    for (const char* removed : {"paymasterStartProvider", "paymasterStopProvider", "paymasterEnableProvider"}) {
        QVERIFY(!panel->findChild<QPushButton*>(removed));
    }
    QVERIFY(card->isAncestorOf(action));
    QVERIFY(card->isAncestorOf(pause));
    QCOMPARE(pause->text(), QStringLiteral("Pause provider…"));
    details_toggle->setChecked(false);
    auto snapshot = [](const char* code, const char* diagnostic_state, const char* next, bool running, bool ready, bool locked) {
        UniValue value;
        value.read(R"({"schema_version":1,"unlocked_until":0,"provider":{"running":false,"ready":true,"wallet_locked":false,"service_state":"stopped","transport":{"listener_ready":true},"backup_status":{"required":false}},"diagnostics":[]})");
        UniValue provider = value.find_value("provider");
        provider.pushKV("running", running);
        provider.pushKV("ready", ready);
        provider.pushKV("wallet_locked", locked);
        provider.pushKV("service_state", running ? "active" : "stopped");
        value.pushKV("provider", provider);
        UniValue diagnostic{UniValue::VOBJ};
        diagnostic.pushKV("code", code);
        diagnostic.pushKV("state", diagnostic_state);
        diagnostic.pushKV("action", next);
        diagnostic.pushKV("area", "budgets");
        UniValue diagnostics{UniValue::VARR};
        diagnostics.push_back(diagnostic);
        value.pushKV("diagnostics", diagnostics);
        return value;
    };
    const UniValue ready = snapshot("PAYMASTER_LOCAL_READY", "ready", "start", false, true, false);
    panel->setOperatorStatusForTesting(ready);
    QVERIFY(headline->text().contains(QStringLiteral("Ready to start")));
    QVERIFY(action->text().contains(QStringLiteral("Start provider")));
    QVERIFY(pause->isHidden());
    QVERIFY(connection->text().contains(QStringLiteral("no current measurement")));
    QVERIFY(!hint->text().contains(QStringLiteral("PAYMASTER_")));

    const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_UX_SCREENSHOT");
    for (const auto& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
        QFile stylesheet(QStringLiteral(":/css/") + theme);
        QVERIFY(stylesheet.open(QIODevice::ReadOnly));
        panel->setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
        panel->findChild<QWidget*>(QStringLiteral("paymasterSetupChoice"))->hide();
        panel->findChild<QWidget*>(QStringLiteral("paymasterConfiguredOverview"))->show();
        card->show();
        panel->resize(1180, 900);
        panel->show();
        QCoreApplication::processEvents();
        const auto luminance = [](const QColor& color) {
            const auto channel = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
            return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
        };
        const double background = luminance(card->palette().color(QPalette::Window));
        for (auto* text : {headline, hint, connection}) {
            const double foreground = luminance(text->palette().color(QPalette::WindowText));
            const double contrast = (std::max(background, foreground) + 0.05) / (std::min(background, foreground) + 0.05);
            QVERIFY2(contrast >= 4.5, qPrintable(theme + QStringLiteral(" operator text contrast: ") + text->objectName()));
        }
        if (!screenshot.isEmpty()) {
            QVERIFY(panel->grab().save(theme == QLatin1String("dark") ? screenshot : screenshot + QStringLiteral(".light.png")));
        }
    }

    panel->setOperatorStatusForTesting(snapshot("PAYMASTER_PROVIDER_DISABLED", "action_required", "enable", false, false, false));
    QCOMPARE(action->text(), QStringLiteral("Resume provider…"));

    // Core running is not evidence of active processing when the wallet is locked.
    panel->setOperatorStatusForTesting(snapshot("PAYMASTER_WALLET_LOCKED", "action_required", "unlock", true, false, true));
    QVERIFY(headline->text().contains(QStringLiteral("Waiting for wallet unlock")));
    QVERIFY(action->text().contains(QStringLiteral("Unlock wallet")));
    QVERIFY(!pause->isHidden());
    QVERIFY(card->property("statusKind").toString() != QLatin1String("ready"));

    for (int i = 0; i < tabs->count(); ++i)
        tabs->widget(i)->setEnabled(true);
    panel->setOperatorStatusForTesting(snapshot("PAYMASTER_SAFETY_LIMIT_EXHAUSTED", "action_required", "review_budget", true, false, false));
    action->click();
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget*>(settings));
    QCOMPARE(settings->currentWidget()->objectName(), QStringLiteral("paymasterSafetyPolicyPage"));
    QVERIFY(hint->text().contains(QStringLiteral("never raised automatically")));

    tabs->setCurrentIndex(0);
    panel->setOperatorStatusForTesting(snapshot("FUTURE_STATUS", "unknown", "inspect_error", true, true, false));
    QVERIFY(!headline->text().contains(QStringLiteral("running")));
    QVERIFY(!hint->text().contains(QStringLiteral("FUTURE_STATUS")));
    action->click();
    QVERIFY(!details->isHidden());
    panel->setOperatorStatusForTesting(snapshot("FUTURE_READY", "ready", "start", false, true, false));
    QVERIFY(!action->text().contains(QStringLiteral("Start provider")));
    panel->setOperatorStatusForTesting(UniValue{UniValue::VOBJ});
    QVERIFY(headline->text().contains(QStringLiteral("unavailable")));
    QVERIFY(!action->text().contains(QStringLiteral("Start")));

    // Use actual Core diagnostics: reserved payment capacity and unconfirmed
    // successors are normal waits, not an unknown provider failure.
    int unexpected_mutations{0};
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue&) {
        if (command.rfind("get", 0) != 0 && command.rfind("list", 0) != 0) ++unexpected_mutations;
        return UniValue{UniValue::VOBJ};
    });
    auto* capital = panel->findChild<QLabel*>("paymasterOverviewLiquidityStatus");
    auto* capital_action = panel->findChild<QPushButton*>("paymasterOverviewLiquidityAction");
    QVERIFY(capital && capital_action);
    for (const bool reserved : {true, false}) {
        UniValue waiting = ready;
        auto provider = waiting.find_value("provider");
        provider.pushKV("running", true);
        provider.pushKV("ready", false);
        provider.pushKV("service_state", "waiting_for_liquidity_confirmation");
        provider.pushKV("last_service_error", "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING");
        UniValue errors{UniValue::VARR}, pool{UniValue::VOBJ};
        errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
        provider.pushKV("readiness_errors", errors);
        pool.pushKV("reserved", reserved ? 2 : 0);
        provider.pushKV("pool", pool);
        waiting.pushKV("provider", provider);
        waiting.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
        panel->setOperatorStatusForTesting(waiting);
        QCOMPARE(headline->text(), reserved ? QStringLiteral("Payment in progress") : QStringLiteral("Waiting for reserve confirmations"));
        QVERIFY(hint->text().contains(QStringLiteral("automatically")));
        QVERIFY(!action->text().contains(QStringLiteral("Start")));
        QCOMPARE(card->property("statusKind").toString(), QStringLiteral("waiting"));
        QVERIFY(capital->text().contains(reserved ? QStringLiteral("reserved for a payment") : QStringLiteral("blockchain confirmation")));
        QCOMPARE(capital_action->text(), QStringLiteral("View progress"));
        action->click();
        if (reserved) QVERIFY(tabs->currentWidget() != static_cast<QWidget*>(settings));
        else QCOMPARE(tabs->currentWidget()->objectName(), QStringLiteral("paymasterLiquidityPage"));

        // Unknown diagnostics take priority even with an expected service wait.
        errors.push_back("PAYMASTER_INVALID_MAINTENANCE_LEDGER");
        provider.pushKV("readiness_errors", errors);
        waiting.pushKV("provider", provider);
        waiting.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
        panel->setOperatorStatusForTesting(waiting);
        QCOMPARE(headline->text(), QStringLiteral("Status needs review"));
        QCOMPARE(action->text(), QStringLiteral("View diagnostics"));
    }
    QCOMPARE(unexpected_mutations, 0);

    // Expired unlock information cannot leave the prior start action visible.
    UniValue expired = ready;
    expired.pushKV("unlocked_until", int64_t{1});
    panel->setOperatorStatusForTesting(expired);
    QVERIFY(action->text().contains(QStringLiteral("Unlock wallet")));
    panel->setOperatorStatusForTesting(ready);
    panel->setPrivacy(true);
    QVERIFY(card->isHidden());
    QVERIFY(connection->text().isEmpty());
    QVERIFY(!action->isEnabled());
    QVERIFY(!action->toolTip().contains(QStringLiteral("Start")));
    panel->setPrivacy(false);
    QVERIFY(!action->text().contains(QStringLiteral("Start")));
}

void PaymasterWidgetTests::paymasterOperatingUnlock_data()
{
    QTest::addColumn<QString>("choice");
    for (const auto* choice : {"continuous", "timed", "cancel", "privacy", "wallet_change", "privacy_password", "wallet_password"})
        QTest::newRow(choice) << QString::fromLatin1(choice);
}
void PaymasterWidgetTests::paymasterOperatingUnlock()
{
    QFETCH(QString, choice);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    UniValue snapshot;
    QVERIFY(snapshot.read(R"({"schema_version":1,"encrypted":true,"unlocked_until":12345678900,"provider":{"running":false,"ready":true,"wallet_locked":false,"service_state":"stopped"},"diagnostics":[]})"));
    std::vector<UniValue> unlocks;
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "walletpassphrase") { unlocks.push_back(params); return UniValue{}; }
        if (command == "getpaymasteroperatorinfo") return snapshot;
        throw std::runtime_error("test follow-up unavailable");
    });
    auto* tabs = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    auto* settings = panel->findChild<QStackedWidget*>("paymasterSettingsPages");
    QVERIFY(tabs && settings);
    tabs->widget(tabs->indexOf(settings))->setEnabled(true);
    auto* button = panel->findChild<QPushButton*>("paymasterUnlockOperation");
    QVERIFY(button);
    QVERIFY(button->isEnabled());
    bool saw_default{false}, saw_password{false};
    QTimer answer;
    connect(&answer, &QTimer::timeout, panel.get(), [&] {
        if (auto* dialog = panel->findChild<QDialog*>("paymasterOperatingUnlockDialog"); dialog && dialog->isVisible()) {
            auto* mode = dialog->findChild<QComboBox*>("paymasterOperatingUnlockMode");
            auto* seconds = dialog->findChild<QSpinBox*>("paymasterOperatingUnlockSeconds");
            saw_default = mode && mode->currentIndex() == 0 && seconds && !seconds->isEnabled();
            if (choice == QLatin1String("timed")) { mode->setCurrentIndex(1); seconds->setValue(7200); }
            if (choice == QLatin1String("privacy")) panel->setPrivacy(true);
            if (choice == QLatin1String("wallet_change")) panel->setWalletModel(nullptr);
            dialog->done(choice == QLatin1String("cancel") ? QDialog::Rejected : QDialog::Accepted);
        } else if (auto* password = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            saw_password = true;
            if (choice == QLatin1String("privacy_password")) panel->setPrivacy(true);
            if (choice == QLatin1String("wallet_password")) panel->setWalletModel(nullptr);
            password->setTextValue(QStringLiteral("test passphrase"));
            password->accept();
        }
    });
    answer.start(10);
    button->click();
    answer.stop();
    QVERIFY(saw_default);
    const bool approved = choice == QLatin1String("continuous") || choice == QLatin1String("timed");
    QCOMPARE(saw_password, approved || choice == QLatin1String("privacy_password") || choice == QLatin1String("wallet_password"));
    QCOMPARE(unlocks.size(), approved ? size_t{1} : size_t{0});
    if (approved) {
        QCOMPARE(unlocks[0][0].get_str(), std::string{"test passphrase"});
        QCOMPARE(unlocks[0][1].getInt<int64_t>(), choice == QLatin1String("continuous") ? int64_t{0} : int64_t{7200});
        QCOMPARE(unlocks[0][2].get_bool(), choice == QLatin1String("continuous"));
    }
}

void PaymasterWidgetTests::paymasterClientFallbackContinues_data()
{
    QTest::addColumn<QString>("outcome");
    for (const char* value : {"continue", "restored_gross", "no_resume", "signed", "rpc_error"})
        QTest::newRow(value) << QString::fromLatin1(value);
}

void PaymasterWidgetTests::paymasterClientFallbackContinues()
{
    QFETCH(QString, outcome);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-fallback-continuation");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    form.setDialogHandlerForTesting([](QMessageBox::Icon, const QString&, const QString&, QMessageBox::StandardButtons, QMessageBox::StandardButton) { return QMessageBox::Cancel; });
    QStringList actions;
    std::vector<UniValue> sends;
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "listdigidollarsendsessions") return EmptyPaymasterSessionList();
        if (command == "senddigidollar") {
            sends.push_back(params);
            return PaymasterSessionView("INPUTS_RESERVED", "none", "CANDIDATE");
        }
        if (command != "resolvepaymastersession") throw std::runtime_error("unexpected RPC");
        const auto action = params[1].get_str();
        actions.push_back(QString::fromStdString(action));
        if (action == "fallback" && outcome == QLatin1String("rpc_error")) throw std::runtime_error("PAYMASTER_FALLBACK_FAILED");
        auto result = PaymasterSessionView("INPUTS_RESERVED", "none", action == "fallback" ? "REJECTED" : "CANDIDATE");
        if (action == "fallback" && outcome == QLatin1String("no_resume")) {
            UniValue allowed{UniValue::VARR};
            allowed.push_back("refresh");
            allowed.push_back("abandon_unsigned");
            result.pushKV("allowed_actions", allowed);
        }
        if (outcome == QLatin1String("restored_gross")) {
            auto session = result.find_value("session");
            session.pushKV("requested_fee_mode", "auto");
            session.pushKV("privacy_profile", "high");
            session.pushKV("subtract_paymaster_fee_from_amount", true);
            session.pushKV("send_all_spendable_dd", true);
            result.pushKV("session", session);
            auto attempt = result.find_value("attempt");
            attempt.pushKV("privacy_profile", "high");
            result.pushKV("attempt", attempt);
        }
        if (outcome == QLatin1String("signed")) return PaymasterSessionView("PENDING_PROVIDER", "user_psbt", "USER_SIGNED");
        return result;
    });
    form.setWalletModel(gui.walletModel.get());
    client->setPaymasterSessionForTesting("INPUTS_RESERVED", "none", true,
                                          "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx", 3.25, "CANDIDATE");
    QVERIFY(QMetaObject::invokeMethod(client, "fallbackPaymasterSession", Qt::DirectConnection));
    if (outcome == QLatin1String("signed"))
        QCOMPARE(actions, QStringList{"refresh"});
    else
        QCOMPARE(actions, QStringList({"refresh", "fallback"}));
    if (outcome != QLatin1String("continue") && outcome != QLatin1String("restored_gross")) {
        QVERIFY(sends.empty());
        QVERIFY(!client->isBusy());
        return;
    }
    QCOMPARE(sends.size(), size_t{1});
    QCOMPARE(sends.front()[6].find_value("fee_mode").get_str(), outcome == QLatin1String("restored_gross") ? std::string{"auto"} : std::string{"paymaster"});
    if (outcome == QLatin1String("restored_gross")) {
        QVERIFY(sends.front()[6].find_value("subtract_paymaster_fee_from_amount").isTrue());
        QVERIFY(sends.front()[6].find_value("send_all_spendable_dd").isTrue());
        QCOMPARE(sends.front()[6].find_value("privacy").get_str(), std::string{"high"});
        QCOMPARE(sends.front()[6].find_value("maximum_provider_attempts").getInt<int>(), 1);
    }
    QVERIFY(sends.front()[6].find_value("prepare_only").isTrue());
    QVERIFY(sends.front()[6].find_value("authorization_commitment").isNull());
    QCOMPARE(sends.front()[6].find_value("request_id").get_str(), std::string{"00000000-0000-4000-8000-000000000001"});
    QVERIFY(QMetaObject::invokeMethod(client, "pollPaymasterSession", Qt::DirectConnection));
    QCOMPARE(sends.size(), size_t{2});
    QCOMPARE(sends.front().write(), sends.back().write());
}

void PaymasterWidgetTests::paymasterClientSessionDiscoveryFailures_data()
{
    QTest::addColumn<QString>("outcome");
    for (const char* value : {"rpc_error", "reservation_conflict", "privacy_retry", "missing_fields", "count", "cursor", "bad_entry", "wallet_switch"})
        QTest::newRow(value) << QString::fromLatin1(value);
}

void PaymasterWidgetTests::paymasterClientSessionDiscoveryFailures()
{
    QFETCH(QString, outcome);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-discovery-failure");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    int send_calls{0};
    int dialogs{0};
    QString dialog_message;
    form.setDialogHandlerForTesting([&](QMessageBox::Icon, const QString& title, const QString& message, QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        ++dialogs;
        if (title != QStringLiteral("Saved transfers unavailable")) ++send_calls;
        dialog_message = message;
        return QMessageBox::Ok;
    });
    WalletModel::RpcCallback listing;
    client->setPaymasterAsyncRpcExecutorForTesting([&](const std::string& command, const UniValue&, WalletModel::RpcCallback callback) {
        if (command == "getpaymasterclientsafetystatus")
            callback(PaymasterClientSafetyStatus(), {});
        else if (command == "listdigidollarsendsessions")
            listing = std::move(callback);
        else {
            ++send_calls;
            callback({}, "unexpected mutation");
        }
    });
    form.setWalletModel(gui.walletModel.get());
    QVERIFY(bool(listing));
    QVERIFY(!client->isReady());
    auto* retry = form.findChild<QPushButton*>("loadPersistedPaymasterSession");
    auto* status = form.findChild<QLabel*>("persistedPaymasterSessionsStatus");
    auto* progress = form.findChild<QProgressBar*>("paymasterSessionDiscoveryProgress");
    auto* detail = form.findChild<QLabel*>("paymasterSessionDiscoveryError");
    QVERIFY(retry && status && progress && detail);
    QVERIFY(!progress->isHidden());
    QCOMPARE(progress->minimum(), 0);
    QCOMPARE(progress->maximum(), 0);
    QVERIFY(retry->text().contains("Loading"));
    QVERIFY(!retry->isEnabled());
    client->send("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx", 325);
    QCOMPARE(send_calls, 0);
    if (outcome == QLatin1String("wallet_switch")) {
        auto old = std::move(listing);
        form.setWalletModel(nullptr);
        form.setWalletModel(gui.walletModel.get());
        old(EmptyPaymasterSessionList(), {});
        QVERIFY(!client->isReady());
        auto current = std::move(listing);
        current(EmptyPaymasterSessionList(), {});
        QVERIFY(client->isReady());
        return;
    }
    auto result = EmptyPaymasterSessionList();
    if (outcome == QLatin1String("missing_fields")) result = UniValue{UniValue::VOBJ};
    if (outcome == QLatin1String("count")) result.pushKV("count", 1);
    if (outcome == QLatin1String("cursor")) result.pushKV("next_cursor", "more");
    if (outcome == QLatin1String("bad_entry")) {
        UniValue entries{UniValue::VARR};
        entries.push_back(UniValue{UniValue::VOBJ});
        result.pushKV("sessions", entries);
        result.pushKV("count", 1);
    }
    auto failed = std::move(listing);
    const QString rpc_error = outcome == QLatin1String("reservation_conflict") ? QStringLiteral("PAYMASTER_RESERVATION_SESSION_CONFLICT") :
                              (outcome == QLatin1String("rpc_error") || outcome == QLatin1String("privacy_retry")) ? QStringLiteral("PAYMASTER_SESSION_DATABASE_READ") : QString{};
    failed(result, rpc_error);
    QVERIFY(!client->isReady());
    QVERIFY(status->text().contains("could not be read"));
    QVERIFY(retry->isEnabled());
    QVERIFY(progress->isHidden());
    QVERIFY(!detail->isHidden());
    QCOMPARE(detail->textFormat(), Qt::PlainText);
    QCOMPARE(dialogs, 0); // Startup failure is inline only.
    if (!rpc_error.isEmpty()) QVERIFY(detail->text().contains(rpc_error));
    if (outcome == QLatin1String("reservation_conflict")) QVERIFY(detail->text().contains("Retrying the same data will not fix it"));
    retry->click();
    QVERIFY(bool(listing));
    QVERIFY(!progress->isHidden());
    QVERIFY(!retry->isEnabled());
    auto repeated_failure = std::move(listing);
    if (outcome == QLatin1String("privacy_retry")) client->setPrivacy(true);
    repeated_failure(result, rpc_error);
    if (outcome == QLatin1String("privacy_retry")) {
        QCOMPARE(dialogs, 0);
        QVERIFY(!detail->text().contains(rpc_error));
        QVERIFY(!retry->isEnabled());
        client->setPrivacy(false);
        QVERIFY(detail->text().contains(rpc_error));
    } else {
        QCOMPARE(dialogs, 1); // Even an immediate repeated failure is visible.
        QVERIFY(!dialog_message.isEmpty());
    }
    QVERIFY(!client->isReady());
    retry->click();
    QVERIFY(bool(listing));
    auto success = std::move(listing);
    success(EmptyPaymasterSessionList(), {});
    QVERIFY(client->isReady());
    QVERIFY(progress->isHidden());
    QVERIFY(detail->isHidden());
    QVERIFY(detail->text().isEmpty());
    QVERIFY(form.findChild<QFrame*>("persistedPaymasterSessionsFrame")->isHidden());
    QCOMPARE(send_calls, 0);
}

void PaymasterWidgetTests::paymasterClientReleasedInputsCanBeReservedAgain()
{
    using namespace DigiDollar::Paymaster;
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-reused-session-inputs");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    auto& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);
    struct Cleanup {
        wallet::WalletContext& context;
        const std::shared_ptr<wallet::CWallet>& wallet;
        ~Cleanup() { RemoveWallet(context, wallet, std::nullopt); }
    } cleanup{context, wallet};
    wallet::PaymasterStore store{*wallet};
    const std::string old_id{"00000000-0000-4000-8000-000000000001"};
    const std::string new_id{"00000000-0000-4000-8000-000000000002"};
    const std::string third_id{"00000000-0000-4000-8000-000000000003"};
    const COutPoint input{uint256::ONE, 0};
    const auto now = GetTime();
    PaymentSession old_session, new_session, third_session;
    std::string error;
    QVERIFY(store.CreateOrJoinSession(old_id, uint256::ONE, FeeMode::PAYMASTER, now, old_session, error) == wallet::CreatePaymasterSessionResult::CREATED);
    QVERIFY2(store.ReserveInputs(old_id, {{input, ReservationRole::USER_DD}}, FeeMode::PAYMASTER, now, error), error.c_str());
    QVERIFY2(store.AbandonUnsignedClientSession(old_id, now + 1, error), error.c_str());
    QVERIFY(store.GetSessionByRequestId(old_id, old_session));
    QVERIFY(old_session.state == SessionState::FAILED);
    QCOMPARE(old_session.user_inputs.size(), size_t{1}); // Historical binding is retained.
    QVERIFY(store.CreateOrJoinSession(new_id, uint256::ONE, FeeMode::PAYMASTER, now + 2, new_session, error) == wallet::CreatePaymasterSessionResult::CREATED);
    QVERIFY2(store.ReserveInputs(new_id, {{input, ReservationRole::USER_DD}}, FeeMode::PAYMASTER, now + 2, error), error.c_str());
    QVERIFY(store.GetSessionByRequestId(new_id, new_session));
    QVERIFY(store.CreateOrJoinSession(third_id, uint256::ONE, FeeMode::PAYMASTER, now + 2, third_session, error) == wallet::CreatePaymasterSessionResult::CREATED);

    QString rpc_error;
    UniValue listed;
    try {
        listed = gui.walletModel->executeRpc("listdigidollarsendsessions", UniValue{UniValue::VARR});
    } catch (const UniValue& failure) {
        rpc_error = QString::fromStdString(failure.write());
    }
    QVERIFY2(rpc_error.isEmpty(), qPrintable(rpc_error));
    QCOMPARE(listed.find_value("count").getInt<int>(), 2);
    QCOMPARE(listed.find_value("sessions")[0].find_value("request_id").get_str(), new_id);
    bool live{true};
    QVERIFY2(store.ClientSessionHasLiveReservations(old_session, live, error), error.c_str());
    QVERIFY(!live);
    QVERIFY2(store.ClientSessionHasLiveReservations(new_session, live, error), error.c_str());
    QVERIFY(live);

    // A failed state alone must not excuse signed/pending or broken history.
    auto unsafe_history = old_session;
    unsafe_history.state = SessionState::INPUTS_RESERVED;
    QVERIFY(!store.ClientSessionHasLiveReservations(unsafe_history, live, error));
    unsafe_history = old_session;
    unsafe_history.pending_phase = PendingPhase::PENDING_NETWORK;
    QVERIFY2(store.ClientSessionHasLiveReservations(unsafe_history, live, error), error.c_str());
    QVERIFY(!live);
    unsafe_history = old_session;
    unsafe_history.final_txid = uint256::ONE;
    QVERIFY(!store.ClientSessionHasLiveReservations(unsafe_history, live, error));
    unsafe_history = old_session;
    unsafe_history.attempt_ids.push_back(uint256::ONE); // Missing persisted attempt.
    QVERIFY(!store.ClientSessionHasLiveReservations(unsafe_history, live, error));
    {
        LOCK(wallet->cs_wallet);
        wallet::WalletBatch batch{wallet->GetDatabase()};
        InputReservation reservation;
        QVERIFY(batch.ReadPaymasterReservation(input, reservation));
        const auto original = reservation;
        reservation.request_id = old_id; // Partial identity match is corruption.
        QVERIFY(batch.WritePaymasterReservation(reservation));
        QVERIFY(!store.ClientSessionHasLiveReservations(old_session, live, error));
        QCOMPARE(error, std::string{"PAYMASTER_RESERVATION_SESSION_CONFLICT"});
        reservation = original;
        reservation.request_id = third_id;
        reservation.session_id = third_session.session_id; // This owner never reserved the input.
        QVERIFY(batch.WritePaymasterReservation(reservation));
        QVERIFY(!store.ClientSessionHasLiveReservations(old_session, live, error));
        reservation = original;
        reservation.role = ReservationRole::USER_DGB;
        QVERIFY(batch.WritePaymasterReservation(reservation));
        QVERIFY(!store.ClientSessionHasLiveReservations(old_session, live, error));
        reservation = original;
        reservation.request_id = old_id;
        reservation.session_id = old_session.session_id;
        reservation.authorization_may_exist = true;
        QVERIFY(batch.WritePaymasterReservation(reservation));
        QVERIFY(store.ClientSessionHasLiveReservations(old_session, live, error));
        QVERIFY(live); // Failed but still owned/possibly authorized must stay protected.
        QVERIFY(batch.WritePaymasterReservation(original));
        QVERIFY(wallet->IsLockedCoin(input));
    }

    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    QStringList commands;
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        commands.push_back(QString::fromStdString(command));
        return gui.walletModel->executeRpc(command, params);
    });
    form.setWalletModel(gui.walletModel.get());
    QVERIFY(client->sessionDiscoveryReady());
    QCOMPARE(form.findChild<QComboBox*>("persistedPaymasterSessions")->count(), 2);
    QCOMPARE(commands, QStringList{"listdigidollarsendsessions"}); // Discovery never releases or resumes.
    QVERIFY(store.GetSessionByRequestId(old_id, old_session));
    QVERIFY(old_session.state == SessionState::FAILED);
    QVERIFY(store.GetSessionByRequestId(new_id, new_session));
    QVERIFY(new_session.state == SessionState::INPUTS_RESERVED);
    // A terminal owner can retain its reservations until safe-depth/recovery
    // reconciliation. This must not turn the older unsigned history into a
    // conflict or let this read-only query release the newer owner's input.
    for (const auto state : {SessionState::CONFIRMED, SessionState::CANCELED_SAFE,
                             SessionState::CONFLICTED, SessionState::FAILED}) {
        new_session.state = state;
        {
            LOCK(wallet->cs_wallet);
            QVERIFY(wallet::WalletBatch{wallet->GetDatabase()}.WritePaymasterSession(new_session));
        }
        QVERIFY2(store.ClientSessionHasLiveReservations(old_session, live, error), error.c_str());
        QVERIFY(!live);
        QVERIFY2(store.ClientSessionHasLiveReservations(new_session, live, error), error.c_str());
        QVERIFY(live);
        const auto terminal_list = gui.walletModel->executeRpc("listdigidollarsendsessions", UniValue{UniValue::VARR});
        QCOMPARE(terminal_list.find_value("count").getInt<int>(),
                 state == SessionState::CONFIRMED || state == SessionState::CANCELED_SAFE ? 1 : 2);
        UniValue options{UniValue::VOBJ};
        options.pushKV("active_only", false);
        UniValue params{UniValue::VARR};
        params.push_back(options);
        const auto history = gui.walletModel->executeRpc("listdigidollarsendsessions", params);
        QCOMPARE(history.find_value("count").getInt<int>(), 3);
        LOCK(wallet->cs_wallet);
        InputReservation retained;
        QVERIFY(wallet::WalletBatch{wallet->GetDatabase()}.ReadPaymasterReservation(input, retained));
        QCOMPARE(retained.request_id, new_id);
        QVERIFY(retained.session_id == new_session.session_id);
        QVERIFY(wallet->IsLockedCoin(input));
    }
}

void PaymasterWidgetTests::paymasterClientUncreatedRequestReturnsToCompose_data()
{
    QTest::addColumn<QString>("outcome");
    for (const char* outcome : {"input_selection", "legacy_input_error", "provider_unreachable", "legacy_absence", "read_failure", "unsupported_version"}) {
        QTest::newRow(outcome) << QString::fromLatin1(outcome);
    }
}

void PaymasterWidgetTests::paymasterClientUncreatedRequestReturnsToCompose()
{
    QFETCH(QString, outcome);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-core-uncreated");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    auto& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);
    struct Cleanup {
        wallet::WalletContext& context;
        const std::shared_ptr<wallet::CWallet>& wallet;
        ~Cleanup() { RemoveWallet(context, wallet, std::nullopt); }
    } cleanup{context, wallet};
    auto& database = wallet::GetMockableDatabase(*wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    QStringList request_ids;
    QStringList actions;
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue& params) -> UniValue {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "getpaymasteroffers") return PaymasterPublicOffers();
        if (command == "senddigidollar") {
            if (!params[6].find_value("prepare_only").isTrue() ||
                !params[6].find_value("authorization_commitment").isNull()) {
                throw std::runtime_error("unexpected signing authority");
            }
            request_ids.push_back(QString::fromStdString(params[6].find_value("request_id").get_str()));
            if (outcome == QLatin1String("provider_unreachable")) throw std::runtime_error("PAYMASTER_PROXY_OR_ENDPOINT_UNREACHABLE");
            if (outcome == QLatin1String("legacy_input_error")) throw std::runtime_error("Insufficient confirmed DigiDollar inputs");
            throw std::runtime_error("PAYMASTER_DD_INPUT_SELECTION_FAILED: Insufficient confirmed DD balance");
        }
        if (command == "resolvepaymastersession") {
            actions.push_back(QString::fromStdString(params[1].get_str()));
            if (outcome == QLatin1String("legacy_absence")) throw std::runtime_error("Paymaster session not found");
            if (outcome == QLatin1String("unsupported_version")) throw std::runtime_error("PAYMASTER_UNSUPPORTED_PERSISTED_VERSION");
            if (outcome == QLatin1String("read_failure")) database.m_pass = false;
        }
        try {
            UniValue result = gui.walletModel->executeRpc(command, params);
            database.m_pass = true;
            return result;
        } catch (...) {
            database.m_pass = true;
            throw;
        }
    });
    form.setWalletModel(gui.walletModel.get());
    QVERIFY(client->isReady());
    auto* mode = form.findChild<QComboBox*>("paymasterFeeMode");
    mode->setCurrentIndex(mode->findData("paymaster"));
    auto* address = form.findChild<QLineEdit*>("addressEdit");
    auto* amount = form.findChild<QLineEdit*>("amountEdit");
    const QString recipient{"RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"};
    address->setText(recipient);
    amount->setText("3.25");
    form.setAvailableDigiDollarBalanceForTesting(1000);
    QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection));
    QVERIFY(client->hasCurrentPaymasterOffer());
    const auto before = database.m_records;
    client->send(recipient, 325);
    QCOMPARE(request_ids.size(), 1);
    QCOMPARE(actions, QStringList{"refresh"});
    QVERIFY(!client->isBusy());
    QVERIFY(database.m_records == before);
    QCOMPARE(address->text(), recipient);
    QCOMPARE(amount->text(), QString{"3.25"});
    const bool protected_state = outcome == QLatin1String("legacy_absence") ||
                                 outcome == QLatin1String("read_failure") ||
                                 outcome == QLatin1String("unsupported_version");
    QCOMPARE(address->isReadOnly(), protected_state);
    QCOMPARE(amount->isReadOnly(), protected_state);
    const auto notice = form.findChild<QLabel*>("paymasterTransferNotice")->text();
    if (protected_state) {
        QVERIFY(notice.contains("saved status could not be verified"));
        QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterSessionState", Qt::DirectConnection));
        QCOMPARE(actions, (QStringList{"refresh", "refresh"}));
        QCOMPARE(request_ids.size(), 1);
    } else {
        QVERIFY(notice.contains("No Paymaster transfer was created"));
        if (outcome == QLatin1String("input_selection")) QVERIFY(notice.contains("Insufficient confirmed DD balance"));
        client->send(recipient, 325);
        QCOMPARE(request_ids.size(), 2);
        QVERIFY(request_ids[0] != request_ids[1]);
        QCOMPARE(actions, (QStringList{"refresh", "refresh"}));
        QVERIFY(database.m_records == before);
    }
}

void PaymasterWidgetTests::paymasterClientCoreCancellationRoundTrip_data()
{
    QTest::addColumn<bool>("lost_reply");
    QTest::newRow("reopened") << false;
    QTest::newRow("lost-first-reply") << true;
}

void PaymasterWidgetTests::paymasterClientCoreCancellationRoundTrip()
{
    QFETCH(bool, lost_reply);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    loader->registerRpcs();
    // This suite must also run alone, without RPCNestedTests ending warmup.
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-core-cancel");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    auto& context = *m_node.walletLoader().context();
    AddWallet(context, wallet);
    struct Cleanup {
        wallet::WalletContext& context;
        const std::shared_ptr<wallet::CWallet>& wallet;
        ~Cleanup() { RemoveWallet(context, wallet, std::nullopt); }
    } cleanup{context, wallet};
    wallet::PaymasterStore store{*wallet};
    DigiDollar::Paymaster::PaymentSession session;
    std::string error;
    if (!lost_reply) {
        QVERIFY(store.CreateOrJoinSession("00000000-0000-4000-8000-000000000001", uint256::ONE,
                                          DigiDollar::Paymaster::FeeMode::PAYMASTER, GetTime(), session, error) == wallet::CreatePaymasterSessionResult::CREATED);
    }
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    QStringList actions;
    QString rpc_error;
    form.setDialogHandlerForTesting([](QMessageBox::Icon, const QString&, const QString&, QMessageBox::StandardButtons, QMessageBox::StandardButton) { return QMessageBox::Yes; });
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "getpaymasteroffers") return PaymasterPublicOffers();
        if (command == "senddigidollar") {
            if (store.CreateOrJoinSession(params[6].find_value("request_id").get_str(), uint256::ONE,
                                          DigiDollar::Paymaster::FeeMode::PAYMASTER, GetTime(), session, error) != wallet::CreatePaymasterSessionResult::CREATED) {
                throw std::runtime_error(error);
            }
            throw std::runtime_error("reply lost after durable request creation");
        }
        if (command == "resolvepaymastersession") actions.push_back(QString::fromStdString(params[1].get_str()));
        try {
            return gui.walletModel->executeRpc(command, params);
        } catch (const UniValue& error) {
            rpc_error = QString::fromStdString(error.write());
            throw;
        } catch (const std::exception& error) {
            rpc_error = QString::fromUtf8(error.what());
            throw;
        }
    });
    form.setWalletModel(gui.walletModel.get());
    QVERIFY2(client->isReady(), qPrintable(rpc_error));
    if (lost_reply) {
        auto* mode = form.findChild<QComboBox*>("paymasterFeeMode");
        mode->setCurrentIndex(mode->findData("paymaster"));
        form.findChild<QLineEdit*>("amountEdit")->setText("3.25");
        QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection));
        QVERIFY(client->hasCurrentPaymasterOffer());
        client->send("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx", 325);
    }
    if (lost_reply) {
        // The live failed attempt is now closed automatically, using Core's
        // actual capability checks and a final durable status read-back.
        QCOMPARE(actions, (QStringList{"refresh", "abandon_unsigned", "refresh"}));
        QVERIFY(form.findChild<QLabel*>("paymasterTransferNotice")->text().contains("No payment was sent"));
    } else {
        QCOMPARE(actions, QStringList{"refresh"});
        QVERIFY(QMetaObject::invokeMethod(client, "abandonUnsignedPaymasterSession", Qt::DirectConnection));
    }
    QVERIFY(actions.contains("abandon_unsigned"));
    QVERIFY(store.GetSessionByRequestId(session.request_id, session));
    QVERIFY(session.state == DigiDollar::Paymaster::SessionState::FAILED);
    QVERIFY(!client->isBusy());
    QVERIFY(!form.findChild<QLineEdit*>("addressEdit")->isReadOnly());
    QVERIFY(form.findChild<QLabel*>("paymasterSessionState")->text().contains("canceled"));
}

void PaymasterWidgetTests::paymasterOperatorConfirmationWalletBinding_data()
{
    QTest::addColumn<bool>("pause");
    QTest::addColumn<bool>("switch_wallet");
    QTest::newRow("start") << false << false;
    QTest::newRow("pause") << true << false;
    QTest::newRow("start-wallet-switch") << false << true;
    QTest::newRow("pause-wallet-switch") << true << true;
}

void PaymasterWidgetTests::paymasterOperatorConfirmationWalletBinding()
{
    QFETCH(bool, pause);
    QFETCH(bool, switch_wallet);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    QStringList mutations;
    std::vector<UniValue> stop_params;
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "stoppaymaster") {
            mutations << "stop";
            stop_params.push_back(params);
        }
        if (command == "setpaymasterenabled") mutations << "enable";
        if (command == "startpaymaster") mutations << "start";
        UniValue result{UniValue::VOBJ};
        result.pushKV("enabled", true);
        result.pushKV("running", false);
        return result;
    });
    UniValue snapshot;
    QVERIFY(snapshot.read(R"({"schema_version":1,"unlocked_until":0,"provider":{"running":false,"ready":true,"wallet_locked":false,"service_state":"stopped","transport":{"listener_ready":true},"backup_status":{"required":false}},"diagnostics":[{"code":"PAYMASTER_LOCAL_READY","state":"ready","action":"start","area":"service"}]})"));
    if (pause) {
        auto provider = snapshot.find_value("provider");
        provider.pushKV("running", true);
        provider.pushKV("enabled", true);
        provider.pushKV("autostart", true);
        provider.pushKV("service_state", "active");
        snapshot.pushKV("provider", provider);
    }
    panel->setOperatorStatusForTesting(snapshot);
    auto* action = panel->findChild<QPushButton*>(pause ? "paymasterOperatorPause" : "paymasterOperatorNextAction");
    QVERIFY(action);
    bool reviewed{false};
    QTimer::singleShot(0, panel.get(), [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog) return;
        reviewed = true;
        // Reentrant clicks must not create a second operation/dialog.
        QVERIFY(!action->isEnabled());
        if (switch_wallet) panel->setWalletModel(nullptr);
        dialog->done(QMessageBox::Yes);
    });
    action->click();
    QVERIFY(reviewed);
    if (switch_wallet)
        QVERIFY(mutations.isEmpty());
    else if (pause) {
        QCOMPARE(mutations, QStringList{"stop"});
        QVERIFY(stop_params.front()[0].find_value("persistent").isTrue());
        QVERIFY(stop_params.front()[0].find_value("pause_setup").isTrue());
    } else
        QCOMPARE(mutations, QStringList({"enable", "start"}));
}

void PaymasterWidgetTests::paymasterClientMutationDialogWalletBinding_data()
{
    QTest::addColumn<QString>("action");
    for (const char* action : {"cancel", "recover", "limits"})
        QTest::newRow(action) << QString::fromLatin1(action);
}

void PaymasterWidgetTests::paymasterClientMutationDialogWalletBinding()
{
    QFETCH(QString, action);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-client-dialog-switch");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    int mutations{0};
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "listdigidollarsendsessions") return EmptyPaymasterSessionList();
        if (command != "resolvepaymastersession" || params[1].get_str() != "refresh") ++mutations;
        return action == QLatin1String("recover") ? PaymasterSessionView("PENDING_PROVIDER", "user_psbt", "USER_SIGNED") : PaymasterSessionView("INPUTS_RESERVED", "none", "CANDIDATE");
    });
    form.setWalletModel(gui.walletModel.get());
    bool reviewed{false};
    const auto switch_wallet = [&] {
        reviewed = true;
        form.setWalletModel(nullptr);
        form.setWalletModel(gui.walletModel.get());
    };
    form.setDialogHandlerForTesting([&](QMessageBox::Icon, const QString&, const QString&, QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        switch_wallet();
        return QMessageBox::Yes;
    });
    if (action == QLatin1String("limits")) {
        QTimer::singleShot(0, &form, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            switch_wallet();
            dialog->accept();
        });
        QVERIFY(QMetaObject::invokeMethod(client, "configureClientSafetyPolicy", Qt::DirectConnection));
    } else {
        client->setPaymasterSessionForTesting(action == QLatin1String("recover") ? "PENDING_PROVIDER" : "INPUTS_RESERVED", action == QLatin1String("recover") ? "user_psbt" : "none", true,
                                              "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx", 3.25, action == QLatin1String("recover") ? "USER_SIGNED" : "CANDIDATE");
        QVERIFY(QMetaObject::invokeMethod(client, action == QLatin1String("recover") ? "recoverPaymasterSessionToSelf" : "abandonUnsignedPaymasterSession", Qt::DirectConnection));
    }
    QVERIFY(reviewed);
    QCOMPARE(mutations, 0);
    QVERIFY(!client->isBusy());
    QCOMPARE(form.findChild<QLabel*>("paymasterSessionState")->text(), QStringLiteral("No active session"));
}

void PaymasterWidgetTests::paymasterClientLiveRecoveryProgresses_data()
{
    QTest::addColumn<QString>("outcome");
    for (const char* value : {"authorized", "cancel_review", "rpc_error", "stop", "wallet_close", "unknown_phase", "original_confirmed"})
        QTest::newRow(value) << QString::fromLatin1(value);
}

void PaymasterWidgetTests::paymasterClientLiveRecoveryProgresses()
{
    QFETCH(QString, outcome);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-recovery-progress");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    int reviews{0};
    form.setDialogHandlerForTesting([&](QMessageBox::Icon, const QString& title, const QString&, QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        if (title == QStringLiteral("Confirm exact Paymaster recovery authorization")) {
            ++reviews;
            return outcome == QLatin1String("authorized") ? QMessageBox::Yes : QMessageBox::Cancel;
        }
        return QMessageBox::Yes;
    });
    std::vector<UniValue> recoveries;
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "listdigidollarsendsessions") return EmptyPaymasterSessionList();
        if (command != "resolvepaymastersession") throw std::runtime_error("unexpected recovery RPC");
        if (params[1].get_str() == "refresh") return PaymasterSessionView("PENDING_PROVIDER", "user_psbt", "USER_SIGNED");
        if (params[1].get_str() != "cancel_to_self") throw std::runtime_error("unexpected recovery action");
        recoveries.push_back(params);
        if (outcome == QLatin1String("original_confirmed")) {
            auto result = PaymasterSessionView("CONFIRMED", "final_transaction", "MEMPOOL");
            result.pushKV("result_status", UniValue{UniValue::VNULL});
            result.pushKV("result_sequence", UniValue{UniValue::VNULL});
            return result;
        }
        const auto phase = recoveries.size();
        if (phase == 2 && outcome == QLatin1String("rpc_error")) throw std::runtime_error("recovery transport failed");
        auto result = PaymasterSessionView("PENDING_PROVIDER", "alternative_recovery", "USER_SIGNED");
        auto recovery = result.find_value("recovery");
        recovery.pushKV("phase", outcome == QLatin1String("unknown_phase") ? "future_phase" : phase == 1 ? "capacity_pending" :
                                                                                          phase == 2     ? "request_ready" :
                                                                                          phase == 3     ? "response_validated" :
                                                                                          phase == 4     ? "user_signed" :
                                                                                                           "final_committed");
        if (phase >= 3) {
            for (const char* key : {"recovery_provider_id", "offer_id", "policy_hash", "original_commit_key", "original_template_commitment", "authorization_commitment"})
                recovery.pushKV(key, std::string(64, 'c'));
            recovery.pushKV("privacy_profile", "standard");
            recovery.pushKV("maximum_service_fee_cents", 100);
            recovery.pushKV("service_fee_cents", 2);
            recovery.pushKV("network_fee_satoshis", 1000);
            recovery.pushKV("authorization_accepted", phase >= 4);
            UniValue outputs{UniValue::VARR}, output{UniValue::VOBJ};
            output.pushKV("script_pub_key", "51");
            output.pushKV("amount_cents", 325);
            outputs.push_back(output);
            recovery.pushKV("wallet_returns", outputs);
        }
        result.pushKV("recovery", recovery);
        if (phase >= 5) result.pushKV("broadcast", true);
        return result;
    });
    form.setWalletModel(gui.walletModel.get());
    client->setPaymasterSessionForTesting("PENDING_PROVIDER", "user_psbt", true, "RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx", 3.25, "USER_SIGNED");
    QVERIFY(QMetaObject::invokeMethod(client, "recoverPaymasterSessionToSelf", Qt::DirectConnection));
    QCOMPARE(recoveries.size(), size_t{1});
    const auto poll = [&] { return QMetaObject::invokeMethod(client, "pollPaymasterSession", Qt::DirectConnection); };
    if (outcome == QLatin1String("original_confirmed")) {
        QCOMPARE(form.findChild<QLabel*>("paymasterSessionState")->text(), QStringLiteral("Original payment confirmed"));
        QCOMPARE(reviews, 0);
        QVERIFY(poll());
        QCOMPARE(recoveries.size(), size_t{1});
        return;
    }
    if (outcome == QLatin1String("stop") || outcome == QLatin1String("wallet_close") || outcome == QLatin1String("unknown_phase")) {
        if (outcome == QLatin1String("stop")) QVERIFY(QMetaObject::invokeMethod(client, "cancelPaymasterQuote", Qt::DirectConnection));
        if (outcome == QLatin1String("wallet_close")) form.setWalletModel(nullptr);
        QVERIFY(poll());
        QCOMPARE(recoveries.size(), size_t{1});
        QCOMPARE(reviews, 0);
        return;
    }
    QVERIFY(!form.findChild<QPushButton*>("paymasterSessionPrimaryAction")->isEnabled());
    QVERIFY(poll());
    QCOMPARE(recoveries.size(), size_t{2});
    if (outcome == QLatin1String("rpc_error")) {
        QVERIFY(poll());
        QCOMPARE(recoveries.size(), size_t{2});
        return;
    }
    QVERIFY(poll());
    QCOMPARE(reviews, 1);
    for (size_t i = 0; i < 3; ++i) {
        QCOMPARE(recoveries[i].write(), recoveries[0].write());
        QVERIFY(recoveries[i][2].find_value("prepare_only").isTrue());
        QVERIFY(recoveries[i][2].find_value("recovery_authorization_commitment").isNull());
    }
    if (outcome == QLatin1String("cancel_review")) {
        QVERIFY(poll());
        QCOMPARE(recoveries.size(), size_t{3});
        return;
    }
    QCOMPARE(recoveries.size(), size_t{4});
    QCOMPARE(recoveries[3][2].find_value("recovery_authorization_commitment").get_str(), std::string(64, 'c'));
    QVERIFY(poll());
    QCOMPARE(recoveries.size(), size_t{5});
    QCOMPARE(recoveries[3].write(), recoveries[4].write());
    QVERIFY(poll());
    QCOMPARE(recoveries.size(), size_t{5});
}

void PaymasterWidgetTests::paymasterClientReviewCancellation_data()
{
    QTest::addColumn<QString>("outcome");
    for (const char* outcome : {"canceled", "already_closed", "lost_reply", "refused", "signed_elsewhere", "malformed_cancellation", "malformed_review", "expired_review", "countdown", "countdown_light", "countdown_large", "late_accept", "countdown_wallet_switch", "countdown_privacy", "expired_lost_reply", "expired_signed", "expired_denied", "expired_refused", "expired_status_error", "expiry_rpc_error"}) {
        QTest::newRow(outcome) << QString::fromLatin1(outcome);
    }
}

void PaymasterWidgetTests::paymasterClientReviewCancellation()
{
    QFETCH(QString, outcome);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-review-cancel");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    auto* address = form.findChild<QLineEdit*>("addressEdit");
    auto* amount = form.findChild<QLineEdit*>("amountEdit");
    const QString recipient{"RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx"};
    QString request_id;
    QStringList requests;
    QStringList actions;
    int reviews{0};
    int warnings{0};
    bool closed{false};
    bool dialog_seen{false};
    int first_remaining{-1};
    int last_remaining{-1};
    const bool native_dialog = outcome.startsWith("countdown");
    const bool auto_expiry = outcome.startsWith("expired_") || outcome.startsWith("countdown") || outcome == "late_accept" || outcome == "expiry_rpc_error";
    form.setDialogHandlerForTesting([&](QMessageBox::Icon icon, const QString& title, const QString& message,
                                        QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        if (icon == QMessageBox::Question) {
            if (title != QStringLiteral("Confirm exact Paymaster authorization") || !message.contains("Cancel closes this unsigned request")) {
                ++warnings; // No second confirmation and no unlock/signing dialog.
            }
            ++reviews;
            if (outcome == "late_accept") {
                QTest::qWait(2100);
                return QMessageBox::Yes;
            }
            return QMessageBox::Cancel;
        }
        ++warnings;
        return QMessageBox::Ok;
    });
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "getpaymasteroffers") return PaymasterPublicOffers();
        if (command == "listdigidollarsendsessions") return EmptyPaymasterSessionList();
        if (command == "senddigidollar") {
            if (!params[6].find_value("prepare_only").isTrue() || !params[6].find_value("authorization_commitment").isNull())
                throw std::runtime_error("Cancellation must never authorize payment");
            request_id = QString::fromStdString(params[6].find_value("request_id").get_str());
            requests.push_back(request_id);
            closed = false;
            auto result = PaymasterAuthorizationResult(true, std::string(64, 'c'), "AWAITING_USER_SIGNATURE", "none");
            result.pushKV("request_id", request_id.toStdString());
            if (outcome == QLatin1String("malformed_review")) result.pushKV("user_total_cents", UniValue{});
            if (outcome.startsWith("expired_")) result.pushKV("expires_at", QDateTime::currentSecsSinceEpoch() - 1);
            if (native_dialog || outcome == "late_accept") result.pushKV("expires_at", QDateTime::currentSecsSinceEpoch() + 2);
            if (outcome == "expiry_rpc_error") throw std::runtime_error("PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED");
            return result;
        }
        if (command != "resolvepaymastersession" || params[0].find_value("request_id").get_str() != request_id.toStdString())
            throw std::runtime_error("Unexpected RPC or different request");
        const QString action = QString::fromStdString(params[1].get_str());
        actions.push_back(action);
        if (action == QLatin1String("abandon_unsigned")) {
            if (outcome == QLatin1String("refused") || outcome == "expired_refused") throw std::runtime_error("Cancellation refused");
            closed = true;
            if (outcome == QLatin1String("lost_reply") || outcome == "expired_lost_reply") throw std::runtime_error("Cancellation response lost");
        }
        if (outcome == "expired_status_error") throw std::runtime_error("Status unavailable");
        auto result = PaymasterSessionView("AWAITING_USER_SIGNATURE", "none", "QUOTED");
        if (outcome == QLatin1String("signed_elsewhere") || outcome == "expired_signed") result = PaymasterSessionView("PENDING_PROVIDER", "user_psbt", "USER_SIGNED");
        if (closed || outcome == QLatin1String("already_closed")) {
            result = PaymasterSessionView("FAILED", "none", "REJECTED");
            result.pushKV("requires_attention", false);
            if (outcome == QLatin1String("malformed_cancellation")) result.pushKV("requires_attention", UniValue{});
        }
        if (outcome == "expired_denied") {
            UniValue allowed{UniValue::VARR};
            allowed.push_back("refresh");
            result.pushKV("allowed_actions", allowed);
        }
        auto session = result.find_value("session");
        session.pushKV("request_id", request_id.toStdString());
        result.pushKV("session", session);
        return result;
    });
    form.setWalletModel(gui.walletModel.get());
    form.findChild<QRadioButton*>("feeFundingPaymaster")->setChecked(true);
    address->setText(recipient);
    amount->setText("3.25");
    QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection));
    QVERIFY(client->hasCurrentPaymasterOffer());
    QTimer observer;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    if (native_dialog) {
        QFile css(outcome == "countdown_light" ? ":/css/light" : ":/css/dark");
        QVERIFY(css.open(QIODevice::ReadOnly));
        const QString larger_font = outcome == "countdown_large"
            ? QStringLiteral("\nQMessageBox QLabel, QMessageBox QPushButton { font-size: 16pt; }") : QString{};
        form.setStyleSheet(QString::fromUtf8(css.readAll()) + larger_font);
        if (outcome == "countdown_large") {
            QFont font = form.font();
            font.setPointSize(16);
            form.setFont(font);
        }
        form.setDialogHandlerForTesting({});
        observer.setInterval(50);
        connect(&observer, &QTimer::timeout, &form, [&] {
            auto* dialog = form.findChild<QMessageBox*>("paymasterOfferReviewDialog");
            if (!dialog || !dialog->isVisible()) return;
            if (!dialog_seen) {
                if (outcome == "countdown_large") {
                    auto* label = dialog->findChild<QLabel*>("qt_msgbox_label");
                    QVERIFY(label && label->font().pointSize() >= 16);
                }
                const QString capture_dir = qEnvironmentVariable("DIGIBYTE_QT_TEST_CAPTURE_DIR");
                if (!capture_dir.isEmpty()) dialog->grab().save(capture_dir + "/offer-" + outcome + ".png");
                QVERIFY(dialog->button(QMessageBox::Yes)->isEnabled());
                QVERIFY(!dialog->text().contains("Technical authorization commitment"));
                QVERIFY(dialog->detailedText().contains("Technical authorization commitment"));
            }
            dialog_seen = true;
            const int seconds = dialog->informativeText().section(':', 1).trimmed().section(' ', 0, 0).toInt();
            if (first_remaining < 0) first_remaining = seconds;
            last_remaining = seconds;
            if (outcome == "countdown_wallet_switch") {
                form.setWalletModel(nullptr);
                form.setWalletModel(gui.walletModel.get());
            }
            if (outcome == "countdown_privacy") form.setPrivacy(true);
        });
        connect(&watchdog, &QTimer::timeout, &form, [&] {
            ++warnings;
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
        });
        observer.start();
        watchdog.start(8000);
    }
    client->send(recipient, 325);
    observer.stop();
    watchdog.stop();
    QVERIFY(!client->isBusy());
    QCOMPARE(requests.size(), 1);
    if (native_dialog) {
        QVERIFY(dialog_seen);
        QVERIFY(first_remaining > 0);
    }
    if (outcome == "countdown_wallet_switch" || outcome == "countdown_privacy") {
        QCOMPARE(actions.size(), 0);
        QCOMPARE(warnings, 0);
        QVERIFY(form.findChild<QLabel*>("paymasterTransferNotice")->isHidden());
        return;
    }
    if (auto_expiry) {
        QCOMPARE(warnings, 0);
        auto* notice = form.findChild<QLabel*>("paymasterTransferNotice");
        QVERIFY(!notice->isHidden());
        QVERIFY(notice->text().contains("expired"));
        if (outcome == "expired_signed" || outcome == "expired_denied" || outcome == "expired_refused" || outcome == "expired_status_error") {
            QVERIFY(address->isReadOnly());
            QVERIFY(!notice->text().contains("No payment was sent"));
            QCOMPARE(actions.count("abandon_unsigned"), outcome == "expired_refused" ? 1 : 0);
            auto* primary = form.findChild<QPushButton*>("paymasterSessionPrimaryAction");
            QCOMPARE(primary->text(), QStringLiteral("Check current status"));
            primary->click();
            QCOMPARE(actions.count("abandon_unsigned"), outcome == "expired_refused" ? 1 : 0);
        } else {
            QVERIFY(!address->isReadOnly());
            QVERIFY(!amount->isReadOnly());
            QCOMPARE(address->text(), recipient);
            QCOMPARE(amount->text(), QStringLiteral("3.25"));
            QVERIFY(notice->text().contains("No payment was sent"));
            QVERIFY(form.findChild<QFrame*>("paymasterSessionFrame")->isHidden());
            QCOMPARE(actions.count("abandon_unsigned"), 1);
            if (native_dialog) QVERIFY(last_remaining < first_remaining);
        }
        // Neither expiration nor lost replies creates another send/signature.
        QCOMPARE(requests.size(), 1);
        form.setPrivacy(true);
        QVERIFY(notice->isHidden());
        return;
    }
    if (outcome == QLatin1String("canceled") || outcome == QLatin1String("already_closed")) {
        QCOMPARE(reviews, 1);
        QCOMPARE(warnings, 0);
        QVERIFY(!address->isReadOnly());
        QVERIFY(!amount->isReadOnly());
        QCOMPARE(address->text(), recipient);
        QCOMPARE(amount->text(), QStringLiteral("3.25"));
        QVERIFY(form.findChild<QFrame*>("paymasterSessionFrame")->isHidden());
        QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection));
        QVERIFY(client->hasCurrentPaymasterOffer());
        client->send(recipient, 325);
        QCOMPARE(requests.size(), 2);
        QVERIFY(requests[0] != requests[1]);
        QCOMPARE(reviews, 2);
        QCOMPARE(warnings, 0);
        QCOMPARE(actions.count("abandon_unsigned"), outcome == QLatin1String("canceled") ? 2 : 0);
    } else {
        QVERIFY(address->isReadOnly());
        QVERIFY(amount->isReadOnly());
        QVERIFY(warnings > 0);
        if (outcome == QLatin1String("signed_elsewhere") || outcome.endsWith("review")) {
            QCOMPARE(actions.count("abandon_unsigned"), 0);
        } else {
            QCOMPARE(actions.count("abandon_unsigned"), 1);
        }
        if (outcome == QLatin1String("lost_reply")) {
            // A lost reply is resolved read-only; never repeat the cancellation.
            auto* primary = form.findChild<QPushButton*>("paymasterSessionPrimaryAction");
            primary->click();
            QCOMPARE(primary->text(), QStringLiteral("Start a new transfer"));
            primary->click();
            QVERIFY(!address->isReadOnly());
            QCOMPARE(actions.count("abandon_unsigned"), 1);
            QCOMPARE(requests.size(), 1);
        }
    }
}

void PaymasterWidgetTests::paymasterFeeAmountsAndPercentages()
{
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-fee-comparison");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    client->setPaymasterRpcExecutorForTesting([](const std::string& command, const UniValue&) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "listdigidollarsendsessions") return EmptyPaymasterSessionList();
        throw std::runtime_error("Fee comparison must not mutate Core or request an offer");
    });
    form.setWalletModel(gui.walletModel.get());
    auto* cap = form.findChild<QSpinBox*>("paymasterFeeCap");
    auto* percent = form.findChild<QLabel*>("paymasterFeeCapPercent");
    auto* amount = form.findChild<QLineEdit*>("amountEdit");
    auto* subtract = form.findChild<QCheckBox*>("subtractPaymasterFeeFromAmount");
    QVERIFY(cap && percent && amount && subtract);
    form.findChild<QRadioButton*>("feeFundingPaymaster")->setChecked(true);
    cap->setLocale(QLocale::c());
    QCOMPARE(cap->value(), 100); // Stored/RPC value remains cents.
    QCOMPARE(cap->text(), QStringLiteral("1.00 DD"));
    amount->setText("2.00");
    QVERIFY(percent->text().contains("50.00%"));
    amount->setText("100.00");
    QCOMPARE(cap->value(), 100); // More recipient DD never raises the ceiling.
    QVERIFY(percent->text().contains("1.00%"));
    auto* editor = cap->findChild<QLineEdit*>();
    QVERIFY(editor);
    for (const auto& locale : {QLocale::c(), QLocale(QLocale::German)}) {
        cap->setLocale(locale);
        editor->selectAll();
        QTest::keyClicks(editor, "0.37");
        cap->interpretText();
        QCOMPARE(cap->value(), 37);
        // Invalid precision, grouping or exponential text cannot authorize more.
        for (const QString& invalid : {QStringLiteral("1e3"), QStringLiteral("1,234.56"),
                                       QStringLiteral("0.123"), QStringLiteral("0,37")}) {
            editor->setText(invalid);
            cap->interpretText();
            QCOMPARE(cap->value(), 37);
        }
    }
    amount->setText("2.00");
    QVERIFY(percent->text().contains("18.50%"));
    subtract->setChecked(true);
    QVERIFY(percent->text().contains("exact recipient amount"));
    QVERIFY(!percent->text().contains("18.50%"));
    subtract->setChecked(false);
    client->setPrivacy(true);
    QVERIFY(!percent->text().contains("18.50%"));
    client->setPrivacy(false);
    QVERIFY(percent->text().contains("18.50%"));
    amount->clear();
    QVERIFY(percent->text().contains("Enter a recipient amount"));

    std::unique_ptr<DigiDollarPaymasterWidget> provider{CreatePaymasterWidget(nullptr)};
    auto* rate = provider->findChild<QSpinBox*>("paymasterPolicyFeeBps");
    auto* service_fee_cap = provider->findChild<QSpinBox*>(
        "paymasterPolicyMaximumUserPaidServiceFeeCents");
    auto* example = provider->findChild<QSpinBox*>("paymasterFeeExampleAmount");
    auto* result = provider->findChild<QLabel*>("paymasterFeeExampleResult");
    auto* user_paid = provider->findChild<QCheckBox*>("paymasterPolicyUserPaid");
    auto* summary = provider->findChild<QLabel*>("paymasterPolicySummary");
    QVERIFY(rate && service_fee_cap && example && result && user_paid && summary);
    user_paid->setChecked(true);
    rate->setValue(50);
    service_fee_cap->setValue(100);
    QVERIFY(summary->text().contains("Unsaved changes"));
    auto* save_policy = provider->findChild<QPushButton*>("savePaymasterPolicy");
    QVERIFY(save_policy);
    // This display fixture has no selected wallet; its ancestor page retains
    // the onboarding gate even when the local save control is available.
    QVERIFY(save_policy->isEnabledTo(save_policy->parentWidget()));
    example->setValue(50000);
    QVERIFY(result->text().contains("1.00 DD"));
    QVERIFY(result->text().contains("0.20%"));
    QVERIFY(summary->text().contains("maximum 1.00 DD"));
    const QString saved_summary = summary->text();
    service_fee_cap->setValue(0);
    QVERIFY(result->text().contains("2.50 DD"));
    service_fee_cap->setValue(100);
    example->setValue(100);
    QVERIFY(result->text().contains("0.01 DD"));
    QVERIFY(result->text().contains("1.00%")); // Core rounds up to one cent.
    QCOMPARE(rate->value(), 50);               // Effective percentage does not rewrite tariff.
    QCOMPARE(summary->text(), saved_summary);  // Example creates no policy edit.
    rate->setValue(0);
    QVERIFY(result->text().contains("0.00 DD"));
    rate->setValue(1);
    QVERIFY(result->text().contains("No valid fee")); // Core rejects sub-step rate.
    rate->setValue(10000);
    example->setValue(10000000);
    QVERIFY(result->text().contains("No valid fee")); // Total exceeds Core bound.
    user_paid->setChecked(false);
    QVERIFY(result->text().contains("pricing is inactive"));
    QVERIFY(!example->isEnabled());
}

void PaymasterWidgetTests::paymasterClientPreparationRequiresCurrentOffer_data()
{
    QTest::addColumn<QString>("funding_mode");
    QTest::newRow("paymaster") << QStringLiteral("paymaster");
    QTest::newRow("automatic_without_dgb") << QStringLiteral("auto");
}

void PaymasterWidgetTests::paymasterClientPreparationRequiresCurrentOffer()
{
    QFETCH(QString, funding_mode);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-offer-preparation-gate");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    auto* timer = client->findChild<QTimer*>("paymasterOfferRefreshTimer");
    auto* expiry = client->findChild<QTimer*>("paymasterOfferExpiryTimer");
    auto* prepare = form.findChild<QPushButton*>("sendButton");
    auto* amount = form.findChild<QLineEdit*>("amountEdit");
    auto* mode = form.findChild<QComboBox*>("paymasterFeeMode");
    auto* help = form.findChild<QLabel*>("paymasterOfferCheckHelp");
    QVERIFY(timer && expiry && prepare && amount && mode && help);
    timer->stop(); // Exercise automatic refresh without waiting ten seconds.
    int unexpected{0};
    WalletModel::RpcCallback pending;
    client->setPaymasterAsyncRpcExecutorForTesting([&](const std::string& command, const UniValue&, WalletModel::RpcCallback callback) {
        if (command == "getpaymasterclientsafetystatus") {
            callback(PaymasterClientSafetyStatus(), {});
        } else if (command == "listdigidollarsendsessions") {
            callback(EmptyPaymasterSessionList(), {});
        } else if (command == "getpaymasteroffers" && !pending) {
            pending = std::move(callback);
        } else {
            ++unexpected;
            callback({}, "Unexpected request: no payment should be prepared");
        }
    });
    form.setDialogHandlerForTesting([](QMessageBox::Icon, const QString&, const QString&, QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        return QMessageBox::Ok;
    });
    const QString recipient = QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    form.setWalletModel(gui.walletModel.get());
    form.findChild<QLineEdit*>("addressEdit")->setText(recipient);
    amount->setText("3.25");
    form.setAvailableDigiDollarBalanceForTesting(1000);
    mode->setCurrentIndex(mode->findData(funding_mode));
    form.show();
    const auto tick = [&] { return QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection); };
    const auto reply = [&](const UniValue& result, const QString& error = QString{}) {
        auto callback = std::move(pending);
        pending = {};
        if (callback) callback(result, error);
    };

    QVERIFY(!prepare->isEnabled()); // Not checked yet.
    QVERIFY(!client->hasCurrentPaymasterOffer());
    prepare->click();
    client->send(recipient, 325); // A direct invocation cannot bypass the gate.
    QCOMPARE(unexpected, 0);
    QVERIFY(tick());
    QVERIFY(pending);
    QVERIFY(!prepare->isEnabled()); // Checking.
    reply(UniValue{UniValue::VARR});
    QVERIFY(!prepare->isEnabled()); // Empty directory.
    QVERIFY(help->text().contains(funding_mode == QLatin1String("auto") ? "No spendable DGB" : "automatically when an offer is found"));
    client->send(recipient, 325);
    QCOMPARE(unexpected, 0);

    QVERIFY(tick());
    reply({}, "node unavailable");
    QVERIFY(!prepare->isEnabled());
    QVERIFY(tick());
    reply(UniValue{UniValue::VOBJ});
    QVERIFY(!prepare->isEnabled()); // Malformed replies fail closed.
    QVERIFY(tick());
    reply(PaymasterPublicOffers());
    QVERIFY(client->hasCurrentPaymasterOffer());
    QVERIFY(prepare->isEnabled()); // A later announcement enables preparation.
    QCOMPARE(unexpected, 0);       // Finding an offer never starts a payment itself.
    client->send(recipient, 326);  // Different amount cannot reuse the preview.
    QCOMPARE(unexpected, 0);

    amount->setText("3.26");
    QVERIFY(!prepare->isEnabled());
    QVERIFY(!client->hasCurrentPaymasterOffer());
    amount->setText("3.25");
    QVERIFY(tick());
    amount->setText("3.26");
    reply(PaymasterPublicOffers());
    QVERIFY(!prepare->isEnabled()); // Late result cannot enable edited inputs.
    amount->setText("3.25");
    QVERIFY(tick());
    reply(PaymasterPublicOffers());
    QVERIFY(prepare->isEnabled());
    auto* fee_cap = form.findChild<QSpinBox*>("paymasterFeeCap");
    fee_cap->setValue(fee_cap->value() - 1);
    QVERIFY(!prepare->isEnabled());
    QVERIFY(tick());
    reply(PaymasterPublicOffers());
    QVERIFY(prepare->isEnabled());

    expiry->start(1);
    QTest::qSleep(30); // Expired, but its callback is still queued.
    QVERIFY(!client->hasCurrentPaymasterOffer());
    client->send(recipient, 325);
    QCOMPARE(unexpected, 0);
    QCoreApplication::processEvents();
    QVERIFY(!prepare->isEnabled());
    QVERIFY(tick());
    QVERIFY(pending);
    form.setWalletModel(nullptr);
    reply(PaymasterPublicOffers());
    QVERIFY(!prepare->isEnabled()); // Old-wallet replies cannot enable a send.
    QVERIFY(!client->hasCurrentPaymasterOffer());
    form.setWalletModel(gui.walletModel.get());
    form.setAvailableDigiDollarBalanceForTesting(1000);
    mode->setCurrentIndex(mode->findData(QStringLiteral("auto")));
    QVERIFY(!prepare->isEnabled()); // No own DGB and no current offer.
    mode->setCurrentIndex(mode->findData(QStringLiteral("dgb")));
    QVERIFY(!prepare->isEnabled());
    mode->setCurrentIndex(mode->findData(QStringLiteral("paymaster")));
    QVERIFY(!prepare->isEnabled());
    QCOMPARE(unexpected, 0);
}

void PaymasterWidgetTests::paymasterClientFundingBalanceChanges()
{
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-funding-balances");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    auto* mode = form.findChild<QComboBox*>("paymasterFeeMode");
    auto* send = form.findChild<QPushButton*>("sendButton");
    int mutations{0};
    int dialogs{0};
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue&) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "listdigidollarsendsessions") return EmptyPaymasterSessionList();
        if (command == "getpaymasteroffers") return PaymasterPublicOffers();
        ++mutations;
        return UniValue{};
    });
    form.setDialogHandlerForTesting([&](QMessageBox::Icon, const QString&, const QString&, QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        ++dialogs;
        return QMessageBox::Cancel;
    });
    form.setWalletModel(gui.walletModel.get());
    const QString recipient = QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    form.findChild<QLineEdit*>("addressEdit")->setText(recipient);
    form.findChild<QLineEdit*>("amountEdit")->setText("3.25");
    form.setAvailableDigiDollarBalanceForTesting(1000);
    const auto select = [&](const char* value) { mode->setCurrentIndex(mode->findData(QString::fromLatin1(value))); };
    QVERIFY(!client->hasOwnDgbForFees()); // Balance not loaded yet: fail closed.
    select("dgb");
    QVERIFY(!send->isEnabled());
    QVERIFY(send->toolTip().contains("No spendable DGB"));
    QVERIFY(QMetaObject::invokeMethod(&form, "onSendClicked", Qt::DirectConnection));
    QCOMPARE(dialogs, 0); // A direct call cannot bypass the zero-DGB guard.
    select("auto");
    QVERIFY(!send->isEnabled());
    QCOMPARE(send->text(), QStringLiteral("Send payment"));
    client->send(recipient, 325);
    QCOMPARE(mutations, 0);

    QSignalSpy balances(gui.walletModel.get(), &WalletModel::balanceChanged);
    gui.walletModel->pollBalanceChanged();
    QVERIFY(client->hasOwnDgbForFees());
    QVERIFY(!balances.isEmpty());
    QVERIFY(send->isEnabled()); // The balance signal updates the untouched form.
    QCOMPARE(send->text(), QStringLiteral("Send payment"));
    select("dgb");
    QVERIFY(send->isEnabled());
    QCOMPARE(send->text(), QStringLiteral("Send payment"));
    select("paymaster");
    QVERIFY(!send->isEnabled()); // Own DGB does not enable explicit Paymaster.
    select("auto");
    QVERIFY(send->isEnabled());

    // Empty only the isolated fixture ledger, then use the real balance poll.
    {
        LOCK(wallet->cs_wallet);
        wallet->mapWallet.clear();
        wallet->MarkDirty();
    }
    gui.walletModel->updateTransaction();
    gui.walletModel->pollBalanceChanged();
    QVERIFY(!client->hasOwnDgbForFees());
    QVERIFY(!send->isEnabled());
    QCOMPARE(send->text(), QStringLiteral("Send payment"));
    QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection));
    QVERIFY(send->isEnabled());
    QCOMPARE(send->text(), QStringLiteral("Send payment"));
    select("dgb");
    QVERIFY(!send->isEnabled()); // A provider cannot fund Own DGB mode.
    QVERIFY(QMetaObject::invokeMethod(&form, "onSendClicked", Qt::DirectConnection));
    QCOMPARE(dialogs, 0);
    QCOMPARE(mutations, 0);
    select("auto");
    form.setWalletModel(nullptr);
    QVERIFY(!send->isEnabled());
    gui.walletModel->balanceChanged(gui.walletModel->getCachedBalance());
    QVERIFY(!send->isEnabled()); // Old-wallet signals stay disconnected.
}

void PaymasterWidgetTests::paymasterClientNewTransferClearsOffers_data()
{
    QTest::addColumn<bool>("already_cleared");
    QTest::addColumn<bool>("automatic");
    QTest::newRow("cleared-after-success") << true << false;
    QTest::newRow("filled-entry") << false << false;
    QTest::newRow("automatic-cleared-after-success") << true << true;
    QTest::newRow("automatic-filled-entry") << false << true;
}

void PaymasterWidgetTests::paymasterClientNewTransferClearsOffers()
{
    QFETCH(bool, already_cleared);
    QFETCH(bool, automatic);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    const auto wallet = SetupDescriptorsWallet(m_node, test, "qt-new-transfer-offers");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    int offer_reads{0};
    int unexpected{0};
    client->setPaymasterRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasterclientsafetystatus") return PaymasterClientSafetyStatus();
        if (command == "listdigidollarsendsessions") return EmptyPaymasterSessionList();
        if (command == "getpaymasteroffers") {
            ++offer_reads;
            UniValue offers{UniValue::VARR};
            offers.push_back(PaymasterOffer(
                offer_reads == 1 ? "Previous provider" : "Fresh provider",
                std::string(64, 'b'), "user_paid", 2, params[0].getInt<int64_t>(),
                false, QDateTime::currentSecsSinceEpoch() + 60));
            return offers;
        }
        ++unexpected;
        return UniValue{};
    });
    form.setWalletModel(gui.walletModel.get());
    form.findChild<QRadioButton*>(automatic ? "feeFundingAuto" : "feeFundingPaymaster")->setChecked(true);
    auto* timer = client->findChild<QTimer*>("paymasterOfferRefreshTimer");
    auto* expiry = client->findChild<QTimer*>("paymasterOfferExpiryTimer");
    auto* address = form.findChild<QLineEdit*>("addressEdit");
    auto* amount = form.findChild<QLineEdit*>("amountEdit");
    auto* primary = form.findChild<QPushButton*>("paymasterSessionPrimaryAction");
    auto* table = form.findChild<QTableWidget*>("paymasterOffers");
    auto* cards = form.findChild<QFrame*>("paymasterOfferCardsFrame");
    auto* status = form.findChild<QLabel*>("paymasterOffersStatus");
    QVERIFY(timer && expiry && address && amount && primary && table && cards && status);
    timer->stop();
    const QString recipient = QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    address->setText(recipient);
    amount->setText(QStringLiteral("3.25"));
    form.show();
    QVERIFY(QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(offer_reads, 1);

    client->setPaymasterSessionForTesting(
        QStringLiteral("CONFIRMED"), QStringLiteral("final_transaction"),
        true, recipient, 3.25, QStringLiteral("MEMPOOL"));
    if (already_cleared) {
        // Successful send clears the form while the durable session still
        // protects its history. Clearing it again emits no textChanged signal.
        QVERIFY(QMetaObject::invokeMethod(&form, "onClearClicked", Qt::DirectConnection));
        QVERIFY(address->text().isEmpty());
        QVERIFY(amount->text().isEmpty());
    }
    QVERIFY(primary->isEnabled());
    QCOMPARE(primary->text(), QStringLiteral("Start a new transfer"));
    primary->click();
    QVERIFY(address->text().isEmpty());
    QVERIFY(amount->text().isEmpty());
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(cards->isHidden());
    QVERIFY(!expiry->isActive());
    QVERIFY(!status->text().contains(QStringLiteral("offer found"), Qt::CaseInsensitive));
    QVERIFY(status->text().contains(QStringLiteral("Enter a valid")));
    QVERIFY(form.findChild<QLabel*>("feeFundingSummary")->text().contains(QStringLiteral("Enter a recipient")));
    QVERIFY(!form.findChild<QPushButton*>("sendButton")->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(offer_reads, 1); // An empty new entry must not refresh the old offers.

    address->setText(recipient);
    amount->setText(QStringLiteral("3.50"));
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(offer_reads, 2);
    QCOMPARE(table->rowCount(), 1);
    QVERIFY(table->item(0, 0)->text().contains(QStringLiteral("Fresh provider")));
    QCOMPARE(unexpected, 0); // Starting another entry sends no payment RPC.
}

void PaymasterWidgetTests::paymasterClientOfferAutomaticRefresh()
{
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-auto-offers");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    auto* client = form.findChild<PaymasterSendWidget*>();
    auto* timer = client->findChild<QTimer*>("paymasterOfferRefreshTimer");
    QVERIFY(timer && timer->isActive());
    QCOMPARE(timer->interval(), 10000);
    timer->stop(); // Drive the real timer signal deterministically.
    int reads{0};
    int unexpected{0};
    int dialogs{0};
    WalletModel::RpcCallback pending;
    form.setDialogHandlerForTesting([&](QMessageBox::Icon, const QString&, const QString&, QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        ++dialogs;
        return QMessageBox::Ok;
    });
    client->setPaymasterAsyncRpcExecutorForTesting([&](const std::string& command, const UniValue&, WalletModel::RpcCallback callback) {
        if (command == "getpaymasterclientsafetystatus") {
            callback(PaymasterClientSafetyStatus(), {});
            return;
        }
        if (command == "listdigidollarsendsessions") {
            callback(EmptyPaymasterSessionList(), {});
            return;
        }
        if (command != "getpaymasteroffers" || pending) {
            ++unexpected;
            callback({}, "unexpected RPC or overlapping preview");
            return;
        }
        ++reads;
        pending = std::move(callback);
    });
    const auto tick = [&] { return QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection); };
    const auto reply = [&](const UniValue& result, const QString& error = QString{}) {
        auto callback = std::move(pending);
        pending = {};
        if (callback) callback(result, error);
    };
    UniValue available{UniValue::VARR};
    available.push_back(PaymasterOffer("Provider", std::string(64, 'b'), "user_paid", 2, 325, false, QDateTime::currentSecsSinceEpoch() + 60));
    form.setWalletModel(gui.walletModel.get());
    form.findChild<QRadioButton*>("feeFundingPaymaster")->setChecked(true);
    auto* amount = form.findChild<QLineEdit*>("amountEdit");
    amount->setText("3.25");
    QVERIFY(tick());
    QCOMPARE(reads, 0); // Hidden forms do not poll.
    form.show();
    auto* icon = form.findChild<QLabel*>("paymasterOfferStateIcon");
    auto* animation = client->findChild<QTimer*>("paymasterOfferIconTimer");
    QVERIFY(icon && animation);
    QVERIFY(tick());
    QCOMPARE(reads, 1);
    QVERIFY(pending);
    QVERIFY(animation->isActive());
    QVERIFY(!icon->pixmap(Qt::ReturnByValue).isNull());
    const QImage first_frame = icon->pixmap(Qt::ReturnByValue).toImage();
    QTRY_VERIFY(icon->pixmap(Qt::ReturnByValue).toImage() != first_frame);
    QCOMPARE(reads, 1); // Drawing the spinner never queries Core.
    client->setPrivacy(true);
    QVERIFY(!animation->isActive());
    QVERIFY(icon->isHidden());
    QVERIFY(icon->pixmap(Qt::ReturnByValue).isNull());
    client->setPrivacy(false);
    QVERIFY(animation->isActive());
    form.hide();
    QVERIFY(!animation->isActive());
    form.show();
    QVERIFY(animation->isActive());
    const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_OFFERS_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        for (const QString& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
            QFile css(":/css/" + theme);
            QVERIFY(css.open(QIODevice::ReadOnly));
            form.setStyleSheet(QString::fromUtf8(css.readAll()));
            form.resize(900, 1100);
            QTest::qWait(100); // Repaint one animation frame in the new theme.
            QVERIFY(form.findChild<QFrame*>("paymasterOfferCheckFrame")->grab().save(screenshot + "-checking-" + theme + ".png"));
        }
    }
    QVERIFY(tick());
    QCOMPARE(reads, 1); // Coalesce while the RPC is still pending.
    reply(UniValue{UniValue::VARR});
    QVERIFY(!animation->isActive());
    QCOMPARE(icon->text(), QStringLiteral("—"));
    auto* status = form.findChild<QLabel*>("paymasterOffersStatus");
    auto* table = form.findChild<QTableWidget*>("paymasterOffers");
    QVERIFY(status->text().contains("No public offer"));
    auto* updated = form.findChild<QLabel*>("paymasterOffersUpdated");
    QVERIFY(updated && updated->text().contains("Automatic check every 10 s"));
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(tick());
    reply(available);
    QVERIFY(!animation->isActive());
    QCOMPARE(icon->text(), QStringLiteral("✓"));
    QCOMPARE(status->text(), QStringLiteral("Public Paymaster offer found"));
    QCOMPARE(table->rowCount(), 1); // A late announcement appears without a click.
    QCOMPARE(reads, 2);
    QVERIFY(tick());
    reply({}, "node unavailable");
    QVERIFY(!animation->isActive());
    QCOMPARE(icon->text(), QStringLiteral("!"));
    QCOMPARE(status->property("statusKind").toString(), QStringLiteral("error"));
    QVERIFY(status->text().contains("could not be checked"));
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(dialogs, 0); // Background failures never open repetitive modals.
    QVERIFY(tick());
    reply(UniValue{UniValue::VOBJ});
    QVERIFY(status->text().contains("malformed"));
    QCOMPARE(dialogs, 0);
    QVERIFY(tick());
    amount->setText("4.00");
    QVERIFY(!animation->isActive());
    QCOMPARE(icon->text(), QStringLiteral("↻"));
    reply(available);
    QCOMPARE(table->rowCount(), 0); // Late response for the old amount is discarded.
    amount->setText("3.25");
    const int before = reads;
    client->setPrivacy(true);
    QVERIFY(tick());
    client->setPrivacy(false);
    form.findChild<QRadioButton*>("feeFundingDgb")->setChecked(true);
    QVERIFY(tick());
    form.findChild<QRadioButton*>("feeFundingPaymaster")->setChecked(true);
    amount->clear();
    QVERIFY(tick());
    QCOMPARE(reads, before);
    amount->setText("3.25");
    QVERIFY(tick());
    QVERIFY(pending);
    form.setWalletModel(nullptr);
    QVERIFY(!animation->isActive());
    QVERIFY(icon->isHidden());
    reply(available);
    QCOMPARE(table->rowCount(), 0); // Old wallet results cannot repopulate the view.
    form.setWalletModel(gui.walletModel.get());
    client->setPaymasterSessionForTesting("CREATED", "none", true, {}, 0);
    const int active_reads = reads;
    QVERIFY(tick());
    QCOMPARE(reads, active_reads); // No offer refresh during a payment/session.
    QCOMPARE(unexpected, 0);
    QCOMPARE(dialogs, 0);
}

void PaymasterWidgetTests::paymasterOperatorDelayedStartup_data()
{
    QTest::addColumn<QString>("outcome");
    for (const char* outcome : {"ready", "initial_error", "operator_error", "malformed_operator", "client_status_error", "wallet_close"}) {
        QTest::newRow(outcome) << QString::fromLatin1(outcome);
    }
}

void PaymasterWidgetTests::paymasterOperatorDelayedStartup()
{
    QFETCH(QString, outcome);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto* action = panel->findChild<QPushButton*>("paymasterOperatorNextAction");
    auto* pause = panel->findChild<QPushButton*>("paymasterOperatorPause");
    auto* headline = panel->findChild<QLabel*>("paymasterOperatorHeadline");
    auto* hint = panel->findChild<QLabel*>("paymasterOperatorHint");
    auto* tabs = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    auto* loading = panel->findChild<QWidget*>("paymasterOperatorLoading");
    auto* progress = panel->findChild<QProgressBar*>("paymasterOperatorProgress");
    auto* step_label = panel->findChild<QLabel*>("paymasterOperatorLoadingStep");
    auto* timer = panel->findChild<QTimer*>("paymasterOperatorProgressTimer");
    QVERIFY(action && pause && headline && hint && tabs && loading && progress && step_label && timer);
    QVERIFY(loading->isHidden());
    QVERIFY(!timer->isActive());
    UniValue info;
    QVERIFY(info.read(R"({"wallet_eligible":true,"settings_present":true,"enabled":true,"running":false,"ready":true,
        "wallet_locked":false,"pool_ready":true,"operation_mode":"automatic","autostart":false,"service_state":"stopped",
        "service_queue":{"waiting_requests":0,"waiting_submits":0},"readiness_errors":[],
        "pool":{"entries":8,"reserved":0,"admission_dgb":3,"admission_carriers":3,"operational_dgb":1,"operational_carriers":1,"complete_operational_slots":1},
        "policy":{"funding_models":["user_paid"],"sponsorship_scope":"public","fee_rate_bps":50,"min_amount_cents":100,
        "max_amount_cents":100000,"quote_ttl":60,"maximum_network_fee_dgb_satoshis":20000000}})"));
    info.pushKV("provider_id", std::string(64, 'a'));
    UniValue safety;
    QVERIFY(safety.read(R"({"configured":true,"policy":{
        "user_paid":{"maximum_network_fee_per_transaction_satoshis":20000000,"maximum_reserved_network_fee_satoshis":50000000,
        "maximum_network_fee_per_hour_satoshis":100000000,"maximum_network_fee_per_day_satoshis":500000000,
        "maximum_completed_per_hour":10,"maximum_completed_per_day":100},
        "public_sponsored":{"maximum_network_fee_per_transaction_satoshis":0,"maximum_reserved_network_fee_satoshis":0,
        "maximum_network_fee_per_hour_satoshis":0,"maximum_network_fee_per_day_satoshis":0,"maximum_completed_per_hour":0,"maximum_completed_per_day":0},
        "restricted_sponsored":{"maximum_network_fee_per_transaction_satoshis":0,"maximum_reserved_network_fee_satoshis":0,
        "maximum_network_fee_per_hour_satoshis":0,"maximum_network_fee_per_day_satoshis":0,"maximum_completed_per_hour":0,"maximum_completed_per_day":0},
        "maximum_active_quotes_total":16,"maximum_active_quotes_per_netgroup":4,"maximum_active_quotes_per_recipient":2,
        "maximum_quote_requests_per_netgroup_per_minute":10,"updated_at":1}})"));
    UniValue usage;
    QVERIFY(usage.read(R"({"reserved_network_fee_satoshis":0,"spent_network_fee_last_hour_satoshis":0,"spent_network_fee_last_day_satoshis":0,
        "active_quotes":0,"completed_last_hour":0,"completed_last_day":0,"can_accept_minimum_quote":true,"errors":[]})"));
    for (const auto* name : {"user_paid", "public_sponsored", "restricted_sponsored"})
        safety.pushKV(name, usage);
    const auto liquidity = PaymasterLiquidityStatus("ready", true, true, 0);
    auto provider = info;
    provider.pushKV("liquidity", liquidity);
    UniValue transport;
    QVERIFY(transport.read(R"({"listener_ready":true,"outbound_in_use":0,"outbound_limit":1,"inbound_in_use":0,"inbound_limit":16,"queued":0})"));
    provider.pushKV("transport", transport);
    UniValue snapshot;
    QVERIFY(snapshot.read(R"({"schema_version":1,"unlocked_until":0,"diagnostics":[
        {"code":"PAYMASTER_LOCAL_READY","state":"ready","action":"start","area":"service"}]})"));
    snapshot.pushKV("provider", provider);
    snapshot.pushKV("safety", safety);
    DigiDollarPaymasterWidget::RpcCallback pending;
    QString pending_command;
    QStringList reads;
    panel->setAsyncRpcExecutorForTesting([&](const std::string& command, const UniValue&, DigiDollarPaymasterWidget::RpcCallback callback) {
        QVERIFY(!pending);
        QVERIFY(command.rfind("get", 0) == 0); // Loading never writes a policy or starts service.
        pending_command = QString::fromStdString(command);
        reads.push_back(pending_command);
        pending = std::move(callback);
    });
    panel->refreshStatus();
    QStringList displayed_steps;
    for (int step = 0; pending && step < 10; ++step) {
        if (pending_command == QLatin1String("getpaymasterfinancestatus")) {
            // Capital/finance is a serialized read; it must not erase the
            // already validated operator status or pretend it needs setup.
            QVERIFY(loading->isHidden());
            QVERIFY(!action->isEnabled());
            auto callback = std::move(pending);
            pending = {};
            callback(PaymasterOverviewFinance(), {});
            continue;
        }
        // Keep the callback pending across event processing, just like a slow
        // real wallet RPC. Synchronous snapshot tests cannot observe this gap.
        QCoreApplication::processEvents();
        QVERIFY(!action->isEnabled());
        QVERIFY(!pause->isEnabled());
        QCOMPARE(headline->text(), QStringLiteral("Reading provider status…"));
        QCOMPARE(action->text(), QStringLiteral("Please wait…"));
        QVERIFY(hint->text().contains("automatically"));
        QVERIFY(!loading->isHidden());
        QVERIFY(timer->isActive());
        QCOMPARE(progress->minimum(), 0);
        QCOMPARE(progress->maximum(), 0); // Activity, never an invented percentage.
        const QString displayed = step_label->text().section(" (", 0, 0);
        QVERIFY(!displayed.isEmpty());
        QVERIFY(!displayed.contains("getpaymaster"));
        QVERIFY(!displayed_steps.contains(displayed));
        displayed_steps << displayed;
        QCOMPARE(progress->accessibleDescription(), step_label->text());
        if (step == 0 && outcome == QLatin1String("ready")) {
            const auto text_before = step_label->text();
            QTRY_VERIFY(step_label->text() != text_before); // Visible activity during a delayed reply.
            QCOMPARE(reads.size(), 1);                      // The display timer never polls Core.
            panel->setPrivacy(true);
            QVERIFY(loading->isHidden());
            QVERIFY(step_label->text().isEmpty());
            QVERIFY(!timer->isActive());
            panel->setPrivacy(false);
            QVERIFY(!loading->isHidden());
            QVERIFY(timer->isActive());
            QCOMPARE(reads.size(), 1);
        }
        if (step == 1 && outcome == QLatin1String("ready")) {
            const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_LOADING_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                panel->setObjectName("paymasterWidget");
                panel->resize(1100, 700);
                for (const auto& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                    QFile css(":/css/" + theme);
                    QVERIFY(css.open(QIODevice::ReadOnly));
                    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
                    panel->show();
                    QCoreApplication::processEvents();
                    QVERIFY(loading->isVisible());
                    QVERIFY(panel->grab().save(screenshot + "-" + theme + ".png"));
                }
            }
        }
        const QString command = pending_command;
        auto callback = std::move(pending);
        pending = {};
        if (outcome == QLatin1String("wallet_close") && command == QLatin1String("getpaymasteroperatorinfo")) {
            panel->setWalletModel(nullptr);
            callback(snapshot, {});
            QVERIFY(!pending);
            QVERIFY(!headline->text().contains("Ready to start"));
            QVERIFY(loading->isHidden());
            QVERIFY(step_label->text().isEmpty());
            QVERIFY(!timer->isActive());
            return;
        }
        if (command == QLatin1String("getpaymasterinfo"))
            callback(info, outcome == QLatin1String("initial_error") ? "provider status unavailable" : QString{});
        else if (command == QLatin1String("getpaymasterliquiditystatus"))
            callback(liquidity, {});
        else if (command == QLatin1String("getpaymasterpoolinfo"))
            callback(PaymasterLiquidityPoolStatus(), {});
        else if (command == QLatin1String("getpaymastersafetystatus"))
            callback(safety, {});
        else if (command == QLatin1String("getpaymasterclientsafetystatus"))
            callback(PaymasterClientSafetyStatus(), outcome == QLatin1String("client_status_error") ? "client status unavailable" : QString{});
        else if (command == QLatin1String("getpaymasteroperatorinfo")) {
            callback(outcome == QLatin1String("malformed_operator") ? UniValue{} : snapshot,
                     (outcome == QLatin1String("operator_error") || outcome == QLatin1String("initial_error")) ? "operator status unavailable" : QString{});
        } else
            QFAIL("Unexpected startup RPC");
    }
    QVERIFY(!pending);
    QCOMPARE(reads.count(QStringLiteral("getpaymasteroperatorinfo")), 1);
    QCOMPARE(reads.size(), outcome == QLatin1String("initial_error") || outcome == QLatin1String("operator_error") || outcome == QLatin1String("malformed_operator") ? 1 : 2);
    QVERIFY(loading->isHidden());
    QVERIFY(step_label->text().isEmpty());
    QVERIFY(!timer->isActive());
    QVERIFY(action->isEnabled());
    QCOMPARE(tabs->currentIndex(), 0); // No Settings navigation or setup wizard.
    QVERIFY(!panel->findChild<QWidget*>("paymasterSetupChoice")->isVisible());
    if (outcome == QLatin1String("initial_error") || outcome == QLatin1String("operator_error") || outcome == QLatin1String("malformed_operator")) {
        QCOMPARE(action->text(), QStringLiteral("Refresh status"));
        QVERIFY(!headline->text().contains("Ready to start"));
    } else {
        bool finance_summary{false};
        for (auto* label : panel->findChildren<QLabel*>()) {
            QVERIFY(!label->text().contains("finance ledger has not been initialized"));
            QVERIFY(!label->text().contains("Open finances to load income and costs"));
            finance_summary |= label->text().contains("All time · confirmed");
        }
        QVERIFY(finance_summary);
        QCOMPARE(action->text(), QStringLiteral("Start provider…"));
        QVERIFY(headline->text().contains("Ready to start"));
        QVERIFY(hint->text().contains("do not need to repeat setup"));
    }
    if (outcome == QLatin1String("ready")) {
        const QString ready_headline = headline->text();
        const QString ready_hint = hint->text();
        for (int refresh = 0; refresh < 3; ++refresh) {
            panel->refreshStatus();
            QVERIFY(pending);
            QCoreApplication::processEvents();
            QCOMPARE(headline->text(), ready_headline);
            QCOMPARE(hint->text(), ready_hint);
            QCOMPARE(action->text(), QStringLiteral("Start provider…"));
            QVERIFY(loading->isHidden());
            QVERIFY(timer->isActive()); // Local elapsed-time updates can expose a slow read.
            QVERIFY(!action->isEnabled());
            QVERIFY(!pause->isEnabled());
            const int before = reads.size();
            if (refresh == 0) QTRY_VERIFY_WITH_TIMEOUT(!loading->isHidden(), 3000);
            QCOMPARE(reads.size(), before); // The display timer never polls Core.
            panel->refreshStatus();
            QCOMPARE(reads.size(), before); // No overlapping poll.
            auto callback = std::move(pending);
            pending = {};
            callback(snapshot, refresh == 2 ? QStringLiteral("connection lost") : QString{});
            if (refresh < 2) {
                QVERIFY(pending);
                QCOMPARE(pending_command, QStringLiteral("getpaymasterfinancestatus"));
                auto finance_callback = std::move(pending);
                pending = {};
                finance_callback(PaymasterOverviewFinance(), {});
            }
        }
        QVERIFY(!headline->text().contains("Ready to start"));
        QCOMPARE(action->text(), QStringLiteral("Refresh status"));
        panel->refreshStatus();
        QVERIFY(pending);
        QVERIFY(!loading->isHidden()); // A failed refresh invalidates the cached state.
        QCOMPARE(headline->text(), QStringLiteral("Reading provider status…"));
        auto callback = std::move(pending);
        pending = {};
        callback(snapshot, {});
        QVERIFY(pending);
        auto finance_callback = std::move(pending);
        pending = {};
        finance_callback(PaymasterOverviewFinance(), {});
        QCOMPARE(headline->text(), ready_headline);
        QVERIFY(loading->isHidden());
    }
}

void PaymasterWidgetTests::paymasterConnectionLayout_data()
{
    QTest::addColumn<QString>("theme");
    QTest::addColumn<QSize>("window_size");
    QTest::addColumn<QString>("page_name");
    for (const auto& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
        for (const auto* page : {"paymasterConnectionPage", "paymasterManagementPage", "paymasterAutomationPage", "paymasterSettingsHome"}) {
            const auto name = QString::fromLatin1(page);
            const auto prefix = theme + "_" + name;
            QTest::newRow(qPrintable(prefix + "_narrow")) << theme << QSize(900, 620) << name;
            QTest::newRow(qPrintable(prefix + "_wide")) << theme << QSize(1280, 900) << name;
        }
    }
}

void PaymasterWidgetTests::paymasterConnectionLayout()
{
    QFETCH(QString, theme);
    QFETCH(QSize, window_size);
    QFETCH(QString, page_name);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    panel->setObjectName("paymasterWidget");
    QFile css(":/css/" + theme);
    QVERIFY(css.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    auto* tabs = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    auto* settings = panel->findChild<QStackedWidget*>("paymasterSettingsPages");
    auto* scroll = panel->findChild<QScrollArea*>(page_name);
    auto* prerequisites = panel->findChild<QPushButton*>("paymasterPrerequisitesToggle");
    auto* runtime = panel->findChild<QPushButton*>("paymasterRuntimeSettingsToggle");
    auto* backup = panel->findChild<QGroupBox*>("paymasterOverviewBackupNotice");
    QVERIFY(tabs && settings && scroll && prerequisites && runtime && backup);
    QCOMPARE(settings->count(), 6);
    auto* operation = panel->findChild<QWidget*>("paymasterOverviewPage");
    auto* administration = panel->findChild<QWidget*>("paymasterManagementPage");
    auto* connection = panel->findChild<QWidget*>("paymasterConnectionPage");
    QVERIFY(operation && administration && connection);
    for (const auto* name : {"paymasterOperatorStop", "paymasterOperatorRetire"}) {
        auto* control = panel->findChild<QWidget*>(name);
        QVERIFY(control && panel->findChild<QWidget*>("paymasterLiquidityPage")->isAncestorOf(control));
        QVERIFY(!operation->isAncestorOf(control));
        QVERIFY(!connection->isAncestorOf(control));
    }
    QVERIFY(operation->isAncestorOf(panel->findChild<QPushButton*>("paymasterOperatorPause")));
    QVERIFY(operation->isAncestorOf(panel->findChild<QPushButton*>("paymasterOperatorNextAction")));
    QVERIFY(operation->isAncestorOf(panel->findChild<QPushButton*>("paymasterOperatorStart")));
    QVERIFY(administration->isAncestorOf(backup));
    QVERIFY(connection->isAncestorOf(prerequisites));
    auto* automation = panel->findChild<QWidget*>("paymasterAutomationPage");
    QVERIFY(automation && automation->isAncestorOf(runtime));
    auto* contents = scroll->widget();
    QVERIFY(scroll->widgetResizable());
    QVERIFY(!contents->autoFillBackground());
    QVERIFY(!scroll->viewport()->autoFillBackground());
    auto* column = contents->findChild<QWidget*>("paymasterPageColumn");
    QVERIFY(column && column->layout());
    auto* column_layout = qobject_cast<QVBoxLayout*>(column->layout());
    QVERIFY(column_layout);
    // Exercise every card, including a pending backup reminder, without RPC.
    backup->show();
    for (int i = 0; i < tabs->count(); ++i)
        tabs->widget(i)->setEnabled(true);
    tabs->setCurrentWidget(settings);
    settings->setCurrentWidget(scroll);
    panel->resize(window_size);
    panel->show();
    QVERIFY(QTest::qWaitForWindowExposed(panel.get()));
    QTRY_COMPARE(contents->width(), scroll->viewport()->width());

    const auto verify_layout = [&] {
        QCoreApplication::processEvents();
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QWidget* previous{nullptr};
        for (int i = 0; i < column_layout->count(); ++i) {
            auto* widget = column_layout->itemAt(i)->widget();
            if (!widget || widget->isHidden()) continue;
            QVERIFY2(widget->height() >= widget->minimumSizeHint().height(), qPrintable(widget->objectName()));
            if (previous) QVERIFY2(widget->y() > previous->geometry().bottom(), qPrintable(widget->objectName()));
            if (auto* label = qobject_cast<QLabel*>(widget); label && label->wordWrap()) {
                QVERIFY2(label->height() >= label->heightForWidth(label->width()), qPrintable(label->objectName()));
            }
            previous = widget;
        }
        // The last action must be reachable by scrolling, including after
        // changing the window size or expanding a formerly hidden section.
        QVERIFY(previous);
        scroll->ensureWidgetVisible(previous);
        QCoreApplication::processEvents();
        const QRect last_rect(previous->mapTo(scroll->viewport(), QPoint{}), previous->size());
        QVERIFY(scroll->viewport()->rect().intersects(last_rect));
    };
    verify_layout();
    const int collapsed_height = contents->height();
    if (page_name == "paymasterConnectionPage" || page_name == "paymasterAutomationPage") {
        (page_name == "paymasterConnectionPage" ? prerequisites : runtime)->setChecked(true);
        QCoreApplication::processEvents();
        QVERIFY(contents->height() >= collapsed_height);
        if (page_name == "paymasterAutomationPage")
            QVERIFY(panel->findChild<QWidget*>("paymasterRuntimeSettingsPanel")->isVisibleTo(scroll));
        else
            QVERIFY(prerequisites->isChecked()); // Content may fit after wallet tools moved to their own page.
    }
    verify_layout();
    panel->resize(QSize(1050, 700));
    QTRY_COMPARE(contents->width(), scroll->viewport()->width());
    verify_layout();
    settings->setCurrentIndex(0);
    settings->setCurrentWidget(scroll);
    verify_layout();
    runtime->setChecked(false);
    prerequisites->setChecked(false);
    verify_layout();
    const QColor expected = theme == "dark" ? QColor("#0b2419") : QColor("#eef9f2");
    QCOMPARE(contents->palette().color(QPalette::Window), expected);
    const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_CONNECTION_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        scroll->verticalScrollBar()->setValue(0);
        QVERIFY(panel->grab().save(screenshot + "-" + QTest::currentDataTag() + ".png"));
    }
}

void PaymasterWidgetTests::paymasterOperationControllerRecoversWithoutDuplicateApproval()
{
    using Phase = PaymasterOperationController::Phase;
    PaymasterOperationController controller;
    controller.reset(7);
    QVERIFY(controller.begin(QStringLiteral("Restore"), 7));
    QVERIFY(!controller.begin(QStringLiteral("Duplicate"), 7));
    controller.review(UniValue{UniValue::VOBJ});
    controller.execute();
    controller.accepted();
    UniValue snapshot;
    QVERIFY(snapshot.read(R"({"provider":{"pool_ready":false,"start_requested":false,
        "active_operations":[{"state":"pending_confirmation","error":"","confirmations":0,"required_confirmations":1}]},
        "diagnostics":[]})"));
    QVERIFY(!controller.observe(snapshot, 6, 100));
    QVERIFY(controller.stale(100));
    QVERIFY(controller.observe(snapshot, 7, 100));
    QCOMPARE(controller.phase, Phase::Waiting);
    QCOMPARE(controller.required_confirmations, 1);
    QCOMPARE(controller.completed_confirmations, 0);
    QVERIFY(!controller.begin(QStringLiteral("Duplicate"), 7));
    QVERIFY(!controller.stale(130));
    QVERIFY(controller.stale(131));
    UniValue provider = snapshot.find_value("provider");
    UniValue pending_creation;
    QVERIFY(pending_creation.read(R"([{"state":"pending_creation","error":"","confirmations":0,"required_confirmations":1}])"));
    provider.pushKV("enabled", false);
    snapshot.pushKV("provider", provider);
    QVERIFY(controller.observe(snapshot, 7, 131));
    QCOMPARE(controller.phase, Phase::Waiting); // A sent transaction can confirm while paused.
    provider.pushKV("active_operations", pending_creation);
    snapshot.pushKV("provider", provider);
    QVERIFY(controller.observe(snapshot, 7, 131));
    QCOMPARE(controller.phase, Phase::Blocked);
    QVERIFY(controller.error.contains("PROVIDER_DISABLED"));
    provider.pushKV("enabled", true);
    provider.pushKV("wallet_locked", true);
    snapshot.pushKV("provider", provider);
    QVERIFY(controller.observe(snapshot, 7, 131));
    QCOMPARE(controller.phase, Phase::Blocked);
    QVERIFY(controller.error.contains("WALLET_LOCKED"));
    provider.pushKV("wallet_locked", false);
    provider.pushKV("last_service_error", "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
    snapshot.pushKV("provider", provider);
    QVERIFY(controller.observe(snapshot, 7, 132));
    QCOMPARE(controller.phase, Phase::Blocked);
    QCOMPARE(controller.error, QStringLiteral("PAYMASTER_MAINTENANCE_FEE_EXCEEDED"));
    // A fee error for future refill must not stop an already saved transaction
    // from being displayed as awaiting confirmation.
    UniValue confirming;
    QVERIFY(confirming.read(R"([{"state":"pending_confirmation","error":"","confirmations":0,"required_confirmations":1}])"));
    provider.pushKV("active_operations", confirming);
    snapshot.pushKV("provider", provider);
    QVERIFY(controller.observe(snapshot, 7, 132));
    QCOMPARE(controller.phase, Phase::Waiting);
    QCOMPARE(controller.required_confirmations, 1);
    provider.pushKV("last_service_error", "");
    provider.pushKV("pool_ready", true);
    provider.pushKV("active_operations", UniValue{UniValue::VARR});
    snapshot.pushKV("provider", provider);
    QVERIFY(controller.observe(snapshot, 7, 132));
    QCOMPARE(controller.phase, Phase::Complete);
    QVERIFY(controller.begin(QStringLiteral("Next task"), 7));
    controller.reset(8);
    QVERIFY(!controller.observe(snapshot, 7, 133));
    QCOMPARE(controller.phase, Phase::Idle);
}

void PaymasterWidgetTests::paymasterOperationFundingReviewIsBounded()
{
    UniValue approved;
    QVERIFY(approved.read(R"({"admission_dgb_satoshis_each":10000000,"operational_dgb_satoshis_each":20000000,
        "carrier_cents_each":100,"maximum_fee_satoshis":20000000,"admission_dgb_slots":3,"operational_dgb_slots":1,
        "admission_carrier_slots":3,"operational_carrier_slots":1,"total_output_satoshis":50000000,
        "total_carrier_cents":400,"maximum_total_fee_satoshis":40000000,"missing_admission_dgb_slots":3,
        "missing_operational_dgb_slots":1,"missing_admission_carrier_slots":3,"missing_operational_carrier_slots":1})"));
    QVERIFY(PaymasterOperationController::fundingWithinApproval(approved, approved));
    for (const auto* field : {"total_output_satoshis", "total_carrier_cents", "maximum_total_fee_satoshis",
                              "maximum_fee_satoshis", "operational_carrier_slots", "carrier_cents_each"}) {
        UniValue changed = approved;
        changed.pushKV(field, approved.find_value(field).getInt<int64_t>() + 1);
        QVERIFY(!PaymasterOperationController::fundingWithinApproval(approved, changed));
        changed.pushKV(field, -1);
        QVERIFY(!PaymasterOperationController::fundingWithinApproval(approved, changed));
    }
    UniValue reduced = approved;
    reduced.pushKV("missing_admission_dgb_slots", 0);
    reduced.pushKV("total_output_satoshis", 20000000);
    QVERIFY(PaymasterOperationController::fundingWithinApproval(approved, reduced));
    QVERIFY(!PaymasterOperationController::fundingWithinApproval(UniValue{}, approved));

    UniValue snapshot;
    QVERIFY(snapshot.read(R"({"provider":{"policy":{"funding_models":["user_paid"]},"liquidity":{"policy":{
        "target_admission_dgb":5,"target_operational_dgb":2,"target_admission_carriers":4,"target_operational_carriers":0,
        "automatic_replenishment":false,"paid_maintenance_approved":false,
        "maximum_maintenance_fee_per_transaction_satoshis":9007199254740993,
        "maximum_maintenance_fee_per_hour_satoshis":0,"maximum_maintenance_fee_per_day_satoshis":0}}}})"));
    auto proposal = PaymasterOperationController::restorationPolicy(snapshot);
    QCOMPARE(proposal.find_value("target_operational_carriers").getInt<int>(), 1);
    QCOMPARE(proposal.find_value("target_admission_carriers").getInt<int>(), 4);
    QCOMPARE(proposal.find_value("target_admission_dgb").getInt<int>(), 5);
    QCOMPARE(proposal.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>(), int64_t{9007199254740993});
    QVERIFY(proposal.find_value("automatic_replenishment").isFalse());
    QVERIFY(proposal.find_value("paid_maintenance_approved").isFalse());
    // A proposal neither writes the saved zero nor adds DD reserves to sponsoring.
    QCOMPARE(snapshot.find_value("provider").find_value("liquidity").find_value("policy").find_value("target_operational_carriers").getInt<int>(), 0);
    auto provider = snapshot.find_value("provider");
    UniValue policy; QVERIFY(policy.read(R"({"funding_models":["sponsored"]})"));
    provider.pushKV("policy", policy); snapshot.pushKV("provider", provider);
    proposal = PaymasterOperationController::restorationPolicy(snapshot);
    QCOMPARE(proposal.find_value("target_operational_carriers").getInt<int>(), 0);
}

void PaymasterWidgetTests::paymasterDeferredStartIntentIsWalletScoped()
{
    DigiDollar::Paymaster::Manager manager{true};
    const auto provider = uint256S(std::string(64, 'a'));
    const auto other = uint256S(std::string(64, 'b'));
    QVERIFY(!manager.RequestProviderStart("wallet", provider));
    QVERIFY(manager.TryBeginProviderWork("wallet", provider, false));
    QVERIFY(manager.RequestProviderStart("wallet", provider));
    QVERIFY(manager.HasRequestedProviderStart("wallet", provider));
    QVERIFY(!manager.HasRequestedProviderStart("wallet", other));
    QVERIFY(!manager.HasRequestedProviderStart("other-wallet", provider));
    QVERIFY(manager.CompleteProviderStart("wallet", provider));
    QVERIFY(!manager.HasRequestedProviderStart("wallet", provider));
    manager.EndProviderWork("wallet");
    manager.StopProvider("wallet");
    QVERIFY(manager.TryBeginProviderWork("wallet", provider, false));
    QVERIFY(manager.RequestProviderStart("wallet", provider));
    manager.EndProviderWork("wallet");
    manager.StopProvider("wallet");
    QVERIFY(!manager.HasRequestedProviderStart("wallet", provider));
    QVERIFY(manager.TryBeginProviderWork("wallet", provider, false));
    QVERIFY(manager.RequestProviderStart("wallet", provider));
    manager.EndProviderWork("wallet");
    manager.SetEnabled(false);
    QVERIFY(!manager.HasRequestedProviderStart("wallet", provider));
}

void PaymasterWidgetTests::paymasterFiveDestinationsAndVisibleTasks()
{
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto* tabs = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    auto* settings = panel->findChild<QStackedWidget*>("paymasterSettingsPages");
    auto* navigation = panel->findChild<QListWidget*>("paymasterNavigation");
    auto* selector = panel->findChild<QComboBox*>("paymasterNavigationSelect");
    QVERIFY(tabs && settings && navigation && selector);
    QCOMPARE(navigation->count(), 5);
    QCOMPARE(selector->count(), 5);
    panel->resize(760, 650);
    panel->show();
    QCoreApplication::processEvents();
    QVERIFY(navigation->isHidden());
    QVERIFY(selector->isVisible());
    panel->resize(1200, 750);
    QCoreApplication::processEvents();
    QVERIFY(navigation->isVisible());
    QVERIFY(selector->isHidden());
    QCOMPARE(tabs->count(), 5);
    QCOMPARE(settings->count(), 6);
    QCOMPARE(tabs->widget(0)->property("paymasterPageTitle").toString(), QStringLiteral("Overview"));
    QCOMPARE(tabs->widget(1)->property("paymasterPageTitle").toString(), QStringLiteral("Funds & reserves"));
    QVERIFY(panel->findChild<QPushButton*>("paymasterRestoreTask"));
    QVERIFY(panel->findChild<QPushButton*>("paymasterWithdrawTask"));
    QVERIFY(panel->findChild<QPushButton*>("paymasterReleaseTask"));
    QVERIFY(panel->findChild<QProgressBar*>("paymasterTaskProgress"));
    auto* fee = panel->findChild<QLineEdit*>("paymasterMaintenanceFeePerTransaction");
    QVERIFY(fee);
    QCOMPARE(fee->text(), QStringLiteral("0.50000000"));
    auto* technical = panel->findChild<QWidget*>("paymasterTechnicalDetails");
    QVERIFY(technical);
    QVERIFY(!technical->findChild<QPushButton*>("paymasterOperatorNextAction"));
    QVERIFY(!technical->findChild<QPushButton*>("paymasterOperatorPause"));
}

namespace {
UniValue GuidedOperatorSnapshot()
{
    UniValue info;
    if (!info.read(R"({"wallet_eligible":true,"settings_present":true,"enabled":true,"running":false,"ready":true,
        "wallet_locked":false,"pool_ready":true,"operation_mode":"automatic","autostart":false,"service_state":"stopped",
        "service_queue":{"waiting_requests":0,"waiting_submits":0},"readiness_errors":[],
        "pool":{"entries":8,"reserved":0,"admission_dgb":3,"admission_carriers":3,"operational_dgb":1,"operational_carriers":1,"complete_operational_slots":1},
        "policy":{"funding_models":["user_paid"],"sponsorship_scope":"public","fee_rate_bps":50,"min_amount_cents":100,
        "max_amount_cents":100000,"quote_ttl":60,"maximum_network_fee_dgb_satoshis":20000000}})")) throw std::runtime_error("Invalid operator fixture");
    info.pushKV("provider_id", std::string(64, 'a'));
    UniValue safety;
    if (!safety.read(R"({"configured":true,"policy":{
        "user_paid":{"maximum_network_fee_per_transaction_satoshis":20000000,"maximum_reserved_network_fee_satoshis":50000000,
        "maximum_network_fee_per_hour_satoshis":100000000,"maximum_network_fee_per_day_satoshis":500000000,
        "maximum_completed_per_hour":10,"maximum_completed_per_day":100},
        "public_sponsored":{"maximum_network_fee_per_transaction_satoshis":0,"maximum_reserved_network_fee_satoshis":0,
        "maximum_network_fee_per_hour_satoshis":0,"maximum_network_fee_per_day_satoshis":0,"maximum_completed_per_hour":0,"maximum_completed_per_day":0},
        "restricted_sponsored":{"maximum_network_fee_per_transaction_satoshis":0,"maximum_reserved_network_fee_satoshis":0,
        "maximum_network_fee_per_hour_satoshis":0,"maximum_network_fee_per_day_satoshis":0,"maximum_completed_per_hour":0,"maximum_completed_per_day":0},
        "maximum_active_quotes_total":16,"maximum_active_quotes_per_netgroup":4,"maximum_active_quotes_per_recipient":2,
        "maximum_quote_requests_per_netgroup_per_minute":10,"updated_at":1}})")) throw std::runtime_error("Invalid operator fixture");
    UniValue usage;
    if (!usage.read(R"({"reserved_network_fee_satoshis":0,"spent_network_fee_last_hour_satoshis":0,"spent_network_fee_last_day_satoshis":0,
        "active_quotes":0,"completed_last_hour":0,"completed_last_day":0,"can_accept_minimum_quote":true,"errors":[]})")) throw std::runtime_error("Invalid operator fixture");
    for (const auto* name : {"user_paid", "public_sponsored", "restricted_sponsored"})
        safety.pushKV(name, usage);
    const auto liquidity = PaymasterLiquidityStatus("ready", true, true, 0);
    auto provider = info;
    provider.pushKV("liquidity", liquidity);
    UniValue transport;
    if (!transport.read(R"({"listener_ready":true,"outbound_in_use":0,"outbound_limit":1,"inbound_in_use":0,"inbound_limit":16,"queued":0})")) throw std::runtime_error("Invalid operator fixture");
    provider.pushKV("transport", transport);
    UniValue snapshot;
    if (!snapshot.read(R"({"schema_version":1,"unlocked_until":0,"diagnostics":[
        {"code":"PAYMASTER_LOCAL_READY","state":"ready","action":"start","area":"service"}]})")) throw std::runtime_error("Invalid operator fixture");
    snapshot.pushKV("provider", provider);
    snapshot.pushKV("safety", safety);
    snapshot.pushKV("network", "regtest");
    snapshot.pushKV("wallet", "provider");
    snapshot.pushKV("wallet_generation", "loaded-provider");
    return snapshot;
}
}

void PaymasterWidgetTests::paymasterOperatorWorkTransitions_data()
{
    QTest::addColumn<QString>("theme");
    QTest::newRow("dark") << QStringLiteral("dark");
    QTest::newRow("light") << QStringLiteral("light");
}

void PaymasterWidgetTests::paymasterOperatorWorkTransitions()
{
    QFETCH(QString, theme);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    QFile css(":/css/" + theme);
    QVERIFY(css.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    panel->resize(760, 860);
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    provider.pushKV("running", true);
    provider.pushKV("service_state", "active");
    auto pool = provider.find_value("pool");
    pool.pushKV("reserved", 12); // Historical committed entries are not current work.
    UniValue activity{UniValue::VOBJ};
    for (const auto* key : {"active_payments", "capacity_requests", "reserved_outputs", "pending_confirmations"}) activity.pushKV(key, 0);
    QStringList commands;
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue&) {
        commands << QString::fromStdString(command);
        if (command == "getpaymasteroperatorinfo") return snapshot;
        if (command == "getpaymasterfinancestatus") return PaymasterOverviewFinance();
        throw std::runtime_error("Unexpected process-display RPC");
    });
    auto* headline = panel->findChild<QLabel*>("paymasterOperatorHeadline");
    auto* header = panel->findChild<QLabel*>("paymasterProviderStatus");
    auto* hint = panel->findChild<QLabel*>("paymasterOperatorHint");
    auto* action = panel->findChild<QPushButton*>("paymasterOperatorNextAction");
    auto* pause = panel->findChild<QPushButton*>("paymasterOperatorPause");
    QVERIFY(headline && header && hint && action && pause);
    const auto observe = [&] {
        pool.pushKV("activity", activity);
        provider.pushKV("pool", pool);
        snapshot.pushKV("provider", provider);
        snapshot.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
        commands.clear();
        panel->refreshStatus();
        QCOMPARE(commands, QStringList({"getpaymasteroperatorinfo", "getpaymasterfinancestatus"}));
        QCOMPARE(header->text(), headline->text());
        QVERIFY(pause->isEnabled());
    };
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Provider is running"));
    activity.pushKV("capacity_requests", 1);
    activity.pushKV("reserved_outputs", 2);
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Payment capacity reserved"));
    QCOMPARE(action->text(), QStringLiteral("View payment activity"));
    activity.pushKV("capacity_requests", 0);
    activity.pushKV("active_payments", 1);
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Payment in progress"));
    provider.pushKV("service_state", "waiting_for_readiness");
    provider.pushKV("last_service_error", "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING");
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Payment in progress"));
    provider.pushKV("last_service_error", "PAYMASTER_PROVIDER_SYNCING");
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Waiting for the node"));
    provider.pushKV("service_state", "error");
    provider.pushKV("last_service_error", "PAYMASTER_SUBMIT_BINDING_MISMATCH");
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Status needs review"));
    provider.pushKV("service_state", "active");
    provider.pushKV("last_service_error", "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING");
    provider.pushKV("ready", false);
    UniValue errors{UniValue::VARR};
    errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    provider.pushKV("readiness_errors", errors);
    activity.pushKV("active_payments", 0);
    activity.pushKV("reserved_outputs", 0);
    activity.pushKV("pending_confirmations", 2);
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Waiting for reserve confirmations"));
    QCOMPARE(action->text(), QStringLiteral("View progress"));
    provider.pushKV("ready", true);
    provider.pushKV("readiness_errors", UniValue{UniValue::VARR});
    activity.pushKV("pending_confirmations", 0);
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Provider is running"));
    provider.pushKV("service_state", "manual");
    provider.pushKV("last_service_error", "");
    activity.pushKV("active_payments", 1);
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Payment in progress"));
    QVERIFY(hint->text().contains("manual processing"));
    QVERIFY(!hint->text().contains("continues automatically"));
    provider.pushKV("service_state", "error");
    provider.pushKV("last_service_error", "PAYMASTER_FUTURE_CONFIRMATION_ERROR");
    observe();
    QCOMPARE(headline->text(), QStringLiteral("Status needs review"));

    provider.pushKV("service_state", "replenishing_liquidity");
    provider.pushKV("last_service_error", "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
    activity.pushKV("active_payments", 0);
    UniValue planned;
    QVERIFY(planned.read(R"([{"state":"pending_creation","error":"","confirmations":0,"required_confirmations":1}])"));
    provider.pushKV("active_operations", planned);
    observe();
    auto* task_status = panel->findChild<QLabel*>("paymasterTaskStatus");
    auto* progress = panel->findChild<QProgressBar*>("paymasterTaskProgress");
    auto* review = panel->findChild<QPushButton*>("paymasterContinueTask");
    auto* limits = panel->findChild<QPushButton*>("paymasterMaintenanceLimitsToggle");
    QVERIFY(task_status && progress && review && limits);
    QVERIFY(task_status->text().contains("estimated fee exceeds"));
    QVERIFY(progress->isHidden());
    QVERIFY(!review->isHidden());
    QCOMPARE(review->text(), QStringLiteral("Review refill cost limit"));
    QVERIFY(headline->text() != QStringLiteral("Status needs review"));
    review->click();
    QVERIFY(limits->isChecked());
    for (const auto& command : commands) QVERIFY(command.startsWith(QStringLiteral("get")));
}

void PaymasterWidgetTests::paymasterOperatorPollingPreservesDraftsAndThrottlesFinance()
{
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    provider.pushKV("running", true);
    provider.pushKV("service_state", "active");
    snapshot.pushKV("provider", provider);
    QStringList commands;
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue&) {
        commands << QString::fromStdString(command);
        if (command == "getpaymasteroperatorinfo") return snapshot;
        if (command == "getpaymasterfinancestatus") return PaymasterOverviewFinance();
        throw std::runtime_error("Unexpected polling RPC");
    });
    panel->show();
    panel->refreshStatus();
    auto* timer = panel->findChild<QTimer*>("paymasterSetupStatusTimer");
    auto* fee = panel->findChild<QSpinBox*>("paymasterPolicyFeeBps");
    QVERIFY(timer && fee);
    QCOMPARE(timer->interval(), 2000);
    fee->setValue(84);
    QVERIFY(timer->isActive()); // An offer draft must not freeze the Overview.
    const auto tick = [&] {
        commands.clear();
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(fee->value(), 84);
    };
    tick();
    QCOMPARE(commands, QStringList({"getpaymasteroperatorinfo"}));
    UniValue activity{UniValue::VOBJ};
    for (const auto* key : {"active_payments", "capacity_requests", "reserved_outputs", "pending_confirmations"}) activity.pushKV(key, 0);
    activity.pushKV("active_payments", 1);
    auto pool = provider.find_value("pool");
    pool.pushKV("activity", activity);
    provider.pushKV("pool", pool);
    snapshot.pushKV("provider", provider);
    tick();
    QCOMPARE(commands, QStringList({"getpaymasteroperatorinfo", "getpaymasterfinancestatus"}));
    tick();
    QCOMPARE(commands, QStringList({"getpaymasteroperatorinfo"}));
    commands.clear();
    panel->refreshStatus();
    QCOMPARE(commands, QStringList({"getpaymasteroperatorinfo", "getpaymasterfinancestatus"}));
    provider.pushKV("running", false);
    provider.pushKV("service_state", "stopped");
    snapshot.pushKV("provider", provider);
    tick();
    QCOMPARE(timer->interval(), 30000);
    panel->setPrivacy(true);
    QVERIFY(!timer->isActive());
    tick();
    QVERIFY(commands.isEmpty());
}

void PaymasterWidgetTests::paymasterOperatorBackgroundRefresh_data()
{
    QTest::addColumn<QString>("theme");
    QTest::newRow("dark") << QStringLiteral("dark");
    QTest::newRow("light") << QStringLiteral("light");
}

void PaymasterWidgetTests::paymasterOperatorBackgroundRefresh()
{
    QFETCH(QString, theme);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    panel->setObjectName("paymasterWidget");
    QFile css(":/css/" + theme);
    QVERIFY(css.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    provider.pushKV("running", true);
    provider.pushKV("service_state", "active");
    const auto update_snapshot = [&] {
        snapshot.pushKV("provider", provider);
        snapshot.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, GetTime()));
    };
    update_snapshot();
    QStringList commands;
    UniValue last_params;
    DigiDollarPaymasterWidget::RpcCallback pending;
    panel->setAsyncRpcExecutorForTesting([&](const std::string& command, const UniValue& params, DigiDollarPaymasterWidget::RpcCallback callback) {
        QVERIFY(!pending); // Never overlap reads or mutations.
        commands << QString::fromStdString(command);
        last_params = params;
        pending = std::move(callback);
    });
    const auto reply = [&](const UniValue& result, const QString& error = {}) {
        QVERIFY(pending);
        auto callback = std::move(pending);
        pending = {};
        callback(result, error);
    };
    panel->resize(1100, 850);
    panel->show();
    panel->refreshStatus();
    reply(snapshot);
    reply(PaymasterOverviewFinance());
    auto* timer = panel->findChild<QTimer*>("paymasterSetupStatusTimer");
    auto* pages = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    auto* fee = panel->findChild<QSpinBox*>("paymasterPolicyFeeBps");
    auto* autostart = panel->findChild<QCheckBox*>("paymasterProviderAutostart");
    auto* headline = panel->findChild<QLabel*>("paymasterOperatorHeadline");
    auto* finance = panel->findChild<QLabel*>("paymasterOverviewFinanceStatus");
    QVERIFY(timer && pages && fee && autostart && headline && finance);
    timer->stop();
    fee->setValue(84);
    panel->activateWindow();
    autostart->setFocus();
    QCoreApplication::processEvents();
    const bool focused = autostart->hasFocus();
    const QString confirmed_finance = finance->text();
    QVERIFY(confirmed_finance.contains("All time"));
    struct EnabledProbe : QObject {
        int changes{0};
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::EnabledChange) ++changes;
            return false;
        }
    } probe;
    for (int i = 0; i < pages->count(); ++i) pages->widget(i)->installEventFilter(&probe);
    fee->installEventFilter(&probe);
    const auto tick = [&] {
        commands.clear();
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        timer->stop();
        QCOMPARE(commands, QStringList({"getpaymasteroperatorinfo"}));
    };
    for (int i = 0; i < 3; ++i) {
        tick();
        QVERIFY(pages->currentWidget()->isEnabled() && fee->isEnabled());
        QCOMPARE(finance->text(), confirmed_finance);
        // A timer event during the in-flight read must not enqueue another.
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(commands.size(), 1);
        snapshot.pushKV("observed_at", GetTime() + i);
        reply(snapshot);
        QVERIFY(!pending);
        QCOMPARE(fee->value(), 84);
        QCOMPARE(headline->text(), QStringLiteral("Provider is running"));
        QCOMPARE(finance->text(), confirmed_finance);
        QCOMPARE(probe.changes, 0);
        if (focused) QVERIFY(autostart->hasFocus());
    }
    // Changed capital requires a finance continuation, also without page gates.
    auto pool = provider.find_value("pool");
    pool.pushKV("entries", 9);
    provider.pushKV("pool", pool);
    update_snapshot();
    tick();
    reply(snapshot);
    QCOMPARE(commands.back(), QStringLiteral("getpaymasterfinancestatus"));
    QVERIFY(pages->currentWidget()->isEnabled() && fee->isEnabled());
    QCOMPARE(finance->text(), confirmed_finance);
    reply(PaymasterOverviewFinance());
    QCOMPARE(probe.changes, 0);
    // Explicit user intent is serialized after the read, exactly once. It
    // must not save an unrelated offer draft or execute during the observation.
    tick();
    autostart->click();
    QCOMPARE(commands.size(), 1);
    QVERIFY(!pages->currentWidget()->isEnabled());
    autostart->click();
    QCOMPARE(commands.size(), 1);
    reply(snapshot);
    QCOMPARE(commands.back(), QStringLiteral("setpaymasterruntimesettings"));
    QCOMPARE(last_params[0].size(), size_t{1});
    QVERIFY(last_params[0].find_value("autostart").isTrue());
    UniValue saved{UniValue::VOBJ};
    saved.pushKV("autostart", true);
    saved.pushKV("operation_mode", "automatic");
    saved.pushKV("running", true);
    reply(saved);
    provider.pushKV("autostart", true);
    update_snapshot();
    reply(snapshot);
    reply(PaymasterOverviewFinance());
    QCOMPARE(commands.count("setpaymasterruntimesettings"), 1);
    QCOMPARE(fee->value(), 84);
    QVERIFY(pages->currentWidget()->isEnabled());
    // Lost reads invalidate readiness; passive refresh never hides a failure.
    tick();
    reply({}, QStringLiteral("status response lost"));
    QVERIFY(headline->text().contains("unavailable", Qt::CaseInsensitive));
    panel->refreshStatus();
    reply(snapshot);
    reply(PaymasterOverviewFinance());
    tick();
    panel->setPrivacy(true);
    const QString private_finance = finance->text();
    reply(snapshot);
    QCOMPARE(finance->text(), private_finance);
    QVERIFY(!pending);
    panel->setWalletModel(nullptr);
    QCOMPARE(fee->value(), 50);
    QVERIFY(!pending);
    panel->setPrivacy(false);
    if (pending) {
        auto late = std::move(pending);
        pending = {};
        panel->setWalletModel(nullptr);
        const QString reset_headline = headline->text();
        const QString reset_finance = finance->text();
        late(snapshot, {}); // Discard a completion belonging to the previous load.
        QCOMPARE(headline->text(), reset_headline);
        QCOMPARE(finance->text(), reset_finance);
    }
}

void PaymasterWidgetTests::paymasterOverviewAutostart_data()
{
    QTest::addColumn<QString>("scenario");
    for (const char* name : {"enable", "disable", "running", "lost_saved", "lost_unsaved",
                             "malformed", "wallet_change", "privacy", "processing_draft", "settings_copy"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void PaymasterWidgetTests::paymasterOverviewAutostart()
{
    QFETCH(QString, scenario);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    bool stored = scenario == "disable";
    provider.pushKV("autostart", stored);
    provider.pushKV("running", scenario == "running");
    snapshot.pushKV("provider", provider);
    int writes{0}, reads{0};
    UniValue sent;
    DigiDollarPaymasterWidget::RpcCallback pending;
    panel->setAsyncRpcExecutorForTesting([&](const std::string& command, const UniValue& params, DigiDollarPaymasterWidget::RpcCallback done) {
        if (command == "setpaymasterruntimesettings") {
            ++writes; sent = params; pending = std::move(done);
        } else {
            ++reads;
            provider.pushKV("autostart", stored); snapshot.pushKV("provider", provider);
            if (command == "getpaymasteroperatorinfo") done(snapshot, {});
            else if (command == "getpaymasterinfo") done(provider, {});
            else done({}, QStringLiteral("fixture has no optional page data"));
        }
    });
    panel->setReadinessStatusForTesting(provider);
    panel->setMutationSnapshotsAvailableForTesting(true, true, true);
    panel->setOperatorStatusForTesting(snapshot);
    auto* control = panel->findChild<QCheckBox*>("paymasterProviderAutostart");
    auto* copy = panel->findChild<QCheckBox*>("paymasterSettingsAutostart");
    auto* status = panel->findChild<QLabel*>("paymasterAutostartStatus");
    auto* processing = panel->findChild<QComboBox*>("paymasterOperationMode");
    QVERIFY(control && copy && status && processing);
    if (scenario == "privacy") {
        panel->setPrivacy(true);
        QVERIFY(!control->isEnabled() && !copy->isEnabled());
        QCOMPARE(writes, 0);
        return;
    }
    if (scenario == "processing_draft") processing->setCurrentIndex(processing->findData("manual"));
    (scenario == "settings_copy" ? copy : control)->setChecked(!stored);
    QTRY_COMPARE(writes, 1);
    QVERIFY(pending);
    QCOMPARE(sent.size(), size_t{1});
    QCOMPARE(sent[0].size(), size_t{1});
    QCOMPARE(sent[0].find_value("autostart").get_bool(), !stored);
    QVERIFY(sent[0].find_value("operation_mode").isNull());
    QVERIFY(!control->isEnabled() && !copy->isEnabled());
    UniValue result{UniValue::VOBJ};
    result.pushKV("operation_mode", "automatic");
    result.pushKV("autostart", !stored);
    result.pushKV("running", scenario == "running");
    auto finish = std::move(pending);
    if (scenario == "wallet_change") {
        panel->setWalletModel(nullptr); finish(result, {});
        QCoreApplication::processEvents();
        QVERIFY(!control->isChecked() && !control->isEnabled());
        return;
    }
    if (scenario == "lost_saved") { stored = !stored; finish({}, QStringLiteral("lost reply")); }
    else if (scenario == "lost_unsaved") finish({}, QStringLiteral("write failed"));
    else if (scenario == "malformed") { result.pushKV("autostart", "unknown"); finish(result, {}); }
    else { stored = !stored; finish(result, {}); }
    QTRY_VERIFY(reads > 0);
    QTRY_COMPARE(control->isChecked(), stored);
    QCOMPARE(copy->isChecked(), stored);
    QCOMPARE(writes, 1);
    if (scenario == "processing_draft") QCOMPARE(processing->currentData().toString(), QStringLiteral("manual"));
    if (scenario == "lost_unsaved" || scenario == "malformed") QVERIFY(status->text().contains("not saved"));
}

void PaymasterWidgetTests::paymasterOverviewRefill_data()
{
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<QString>("theme");
    for (const char* name : {"enable", "disable", "approve", "decline", "lost_saved", "lost_unsaved",
                             "wallet_change", "draft_preserved", "blocked", "settings_copy", "stale_revision",
                             "new_policy", "approval_privacy", "approval_wallet"})
        QTest::newRow(name) << QString::fromLatin1(name) << QStringLiteral("dark");
    QTest::newRow("approve_light") << QStringLiteral("approve") << QStringLiteral("light");
}

void PaymasterWidgetTests::paymasterOverviewRefill()
{
    QFETCH(QString, scenario);
    QFETCH(QString, theme);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    panel->setObjectName("paymasterWidget");
    QFile css(":/css/" + theme);
    QVERIFY(css.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    auto liquidity = provider.find_value("liquidity");
    auto policy = liquidity.find_value("policy");
    const bool initially_on = scenario == "disable" || scenario == "blocked";
    policy.pushKV("automatic_replenishment", initially_on);
    const bool needs_review = scenario == "approve" || scenario == "decline" || scenario.startsWith("approval_") || scenario == "new_policy";
    policy.pushKV("paid_maintenance_approved", !needs_review);
    policy.pushKV("updated_at", 100);
    if (scenario == "new_policy") liquidity.pushKV("policy_configured", false);
    liquidity.pushKV("policy", policy);
    UniValue observation;
    QVERIFY(observation.read(R"({"configured":true,"enabled":true,"paid_maintenance_approved":true,"state":"blocked","reason":"PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED","pending_transaction":false})"));
    liquidity.pushKV("automation_status", observation);
    provider.pushKV("liquidity", liquidity);
    snapshot.pushKV("provider", provider);
    int writes{0}, approvals{0};
    UniValue sent;
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "setpaymasterliquiditypolicy") {
            ++writes; sent = params;
            if (scenario == "wallet_change") panel->setWalletModel(nullptr);
            if (scenario == "lost_unsaved" || scenario == "stale_revision") throw std::runtime_error("PAYMASTER_LIQUIDITY_POLICY_CHANGED");
            policy = params[0]; policy.pushKV("updated_at", 101);
            liquidity.pushKV("policy_configured", true);
            liquidity.pushKV("policy", policy);
            provider.pushKV("liquidity", liquidity); snapshot.pushKV("provider", provider);
            if (scenario == "lost_saved") throw std::runtime_error("lost response");
            return policy;
        }
        if (command == "getpaymasterliquiditystatus") return liquidity;
        if (command == "getpaymasteroperatorinfo") return snapshot;
        if (command == "getpaymasterinfo") return provider;
        throw std::runtime_error("optional fixture data unavailable");
    });
    panel->setReadinessStatusForTesting(provider);
    panel->setLiquidityStatusForTesting(liquidity);
    panel->setOperatorStatusForTesting(snapshot);
    panel->setMutationSnapshotsAvailableForTesting(true, true, true);
    auto* control = panel->findChild<QCheckBox*>("paymasterOverviewRefill");
    auto* copy = panel->findChild<QCheckBox*>("paymasterSettingsRefill");
    auto* status = panel->findChild<QLabel*>("paymasterOverviewRefillStatus");
    auto* target = panel->findChild<QSpinBox*>("paymasterOperationalDgbSlots");
    QVERIFY(control && copy && status && target);
    QCOMPARE(control->isChecked(), initially_on);
    if (scenario == "blocked") {
        QVERIFY(control->isChecked() && copy->isChecked());
        QVERIFY(status->text().contains("prerequisite") && status->text().contains("budget"));
        QCOMPARE(writes, 0);
        return;
    }
    if (scenario == "draft_preserved") target->setValue(5);
    QTimer approval;
    connect(&approval, &QTimer::timeout, panel.get(), [&] {
        auto* dialog = panel->findChild<QDialog*>("paymasterRefillApproval");
        if (!dialog || !dialog->isVisible()) return;
        ++approvals; approval.stop();
        QTimer::singleShot(1000, dialog, &QDialog::reject);
        QCOMPARE(dialog->palette().color(QPalette::Window).name(),
                 theme == "dark" ? QStringLiteral("#0b2419") : QStringLiteral("#eef9f2"));
        const auto screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_REFILL_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot + "-" + theme + ".png"));
        if (scenario == "approval_privacy") { panel->setPrivacy(true); return; }
        if (scenario == "approval_wallet") { panel->setWalletModel(nullptr); return; }
        dialog->done(scenario == "decline" ? QDialog::Rejected : QDialog::Accepted);
    });
    approval.start(5);
    (scenario == "settings_copy" ? copy : control)->setChecked(!initially_on);
    if (scenario.startsWith("approval_")) {
        QCOMPARE(approvals, 1); QCOMPARE(writes, 0);
        QVERIFY(!panel->findChild<QDialog*>("paymasterRefillApproval"));
        return;
    }
    if (scenario == "decline") {
        QCOMPARE(approvals, 1); QCOMPARE(writes, 0);
        QVERIFY(!control->isChecked() && !copy->isChecked());
        return;
    }
    QTRY_COMPARE(writes, 1);
    QCOMPARE(sent.size(), size_t{2});
    QCOMPARE(sent[1].getInt<int64_t>(), scenario == "new_policy" ? int64_t{0} : int64_t{100});
    QCOMPARE(sent[0].find_value("target_operational_dgb").getInt<int>(), 1);
    QCOMPARE(sent[0].find_value("automatic_replenishment").get_bool(), !initially_on);
    QVERIFY(sent[0].find_value("paid_maintenance_approved").isTrue());
    QCOMPARE(approvals, needs_review ? 1 : 0);
    if (scenario == "wallet_change") {
        QVERIFY(!control->isEnabled());
        return;
    }
    const bool persisted = scenario != "lost_unsaved" && scenario != "stale_revision";
    QTRY_COMPARE(control->isChecked(), persisted ? !initially_on : initially_on);
    QCOMPARE(copy->isChecked(), control->isChecked());
    QCOMPARE(writes, 1);
    if (scenario == "draft_preserved") {
        QCOMPARE(target->value(), 5);
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->done(QMessageBox::Yes);
        });
        auto* page = panel->findChild<QWidget*>("paymasterLiquidityPage");
        page->setEnabled(true);
        panel->findChild<QPushButton*>("paymasterSaveLiquidityPolicyPrimary")->click();
        QCOMPARE(writes, 2);
        QCOMPARE(sent[1].getInt<int64_t>(), int64_t{101});
        QCOMPARE(sent[0].find_value("target_operational_dgb").getInt<int>(), 5);
        QVERIFY(sent[0].find_value("automatic_replenishment").isTrue());
    }
}

void PaymasterWidgetTests::paymasterCapitalOverview_data()
{
    QTest::addColumn<QString>("scenario");
    for (const char* scenario : {"ready", "surplus_wrong_pool", "pending", "reserved", "sponsored",
                                 "unsaved_targets", "partial_history", "zero_income", "finance_error",
                                 "malformed_finance", "negative_capital", "wrong_provider"})
        QTest::newRow(scenario) << QString::fromLatin1(scenario);
}

void PaymasterWidgetTests::paymasterCapitalOverview()
{
    QFETCH(QString, scenario);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    UniValue snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    auto liquidity = provider.find_value("liquidity");
    auto finance = PaymasterOverviewFinance();
    if (scenario == QLatin1String("surplus_wrong_pool")) {
        liquidity.pushKV("admission_dgb", PaymasterLiquiditySlotStatus(4, 0, 0, 3));
        liquidity.pushKV("operational_dgb", PaymasterLiquiditySlotStatus(0, 0, 1, 1));
    } else if (scenario == QLatin1String("pending")) {
        liquidity.pushKV("operational_dgb", PaymasterLiquiditySlotStatus(0, 1, 0, 1));
    } else if (scenario == QLatin1String("reserved")) {
        auto slot = PaymasterLiquiditySlotStatus(0, 0, 0, 1);
        slot.pushKV("reserved", 1);
        liquidity.pushKV("operational_dgb", slot);
    } else if (scenario == QLatin1String("sponsored")) {
        auto policy = provider.find_value("policy");
        UniValue models{UniValue::VARR};
        models.push_back("sponsored");
        policy.pushKV("funding_models", models);
        provider.pushKV("policy", policy);
        auto targets = liquidity.find_value("policy");
        targets.pushKV("target_admission_carriers", 0);
        targets.pushKV("target_operational_carriers", 0);
        liquidity.pushKV("policy", targets);
        liquidity.pushKV("admission_carriers", PaymasterLiquiditySlotStatus(0, 0, 0, 0));
        liquidity.pushKV("operational_carriers", PaymasterLiquiditySlotStatus(0, 0, 0, 0));
    }
    provider.pushKV("liquidity", liquidity);
    snapshot.pushKV("provider", provider);
    if (scenario == QLatin1String("partial_history")) finance.pushKV("history_partially_reconstructable", true);
    if (scenario == QLatin1String("zero_income")) {
        UniValue summaries, totals;
        QVERIFY(totals.read(R"({"service_fee_income_cents":0,"dgb_operating_cost_satoshis":0,"successful_transfers":0})"));
        summaries.setObject();
        summaries.pushKV("all", totals);
        finance.pushKV("period_summaries", summaries);
    }
    if (scenario == QLatin1String("malformed_finance")) finance = UniValue{UniValue::VOBJ};
    if (scenario == QLatin1String("negative_capital")) {
        auto capital = finance.find_value("pool_capital");
        capital.pushKV("dgb_available_satoshis", -1);
        finance.pushKV("pool_capital", capital);
    }
    if (scenario == QLatin1String("wrong_provider")) finance.pushKV("provider_id", std::string(64, 'b'));
    QStringList commands;
    UniValue finance_params;
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        commands << QString::fromStdString(command);
        if (command == "getpaymasteroperatorinfo") return snapshot;
        if (command == "getpaymasterfinancestatus") {
            finance_params = params;
            if (scenario == QLatin1String("finance_error")) throw std::runtime_error("injected read failure");
            return finance;
        }
        throw std::runtime_error("Unexpected dashboard RPC");
    });
    panel->refreshStatus();
    QCOMPARE(commands, QStringList({"getpaymasteroperatorinfo", "getpaymasterfinancestatus"}));
    QCOMPARE(finance_params[0].find_value("period").get_str(), std::string("all"));
    QVERIFY(finance_params[0].find_value("include_events").isFalse());
    const auto metric = [&](const char* name) { return panel->findChild<QLabel*>(QStringLiteral("paymasterCapital_") + name); };
    auto* status = panel->findChild<QLabel*>("paymasterOverviewFinanceStatus");
    auto* pool_status = panel->findChild<QLabel*>("paymasterOverviewLiquidityStatus");
    QVERIFY(status && pool_status && metric("operational_dgb") && metric("dgb_available"));
    auto* income_card = panel->findChild<QWidget*>("paymasterOverviewFinanceCard");
    auto* capital_card = panel->findChild<QWidget*>("paymasterOverviewLiquidityCard");
    auto* payout_hint = panel->findChild<QLabel*>("paymasterEarningsWithdrawalHint");
    QVERIFY(income_card && capital_card && payout_hint);
    auto* funds = panel->findChild<QWidget*>("paymasterLiquidityPage");
    QVERIFY(funds && funds->isAncestorOf(payout_hint));
    QVERIFY(income_card->isAncestorOf(metric("carrier_earned")));
    QVERIFY(income_card->isAncestorOf(metric("carrier_withdrawable")));
    QVERIFY(capital_card->isAncestorOf(metric("carrier_base")));

    if (scenario == QLatin1String("finance_error") || scenario == QLatin1String("malformed_finance") ||
        scenario == QLatin1String("negative_capital") || scenario == QLatin1String("wrong_provider")) {
        QCOMPARE(status->property("statusKind").toString(), QStringLiteral("action"));
        QVERIFY(status->text().contains("unavailable"));
        QVERIFY(!metric("dgb_available")->text().contains("5"));
    } else {
        QCOMPARE(status->property("statusKind").toString(), scenario == QLatin1String("partial_history") ? QStringLiteral("action") : QStringLiteral("neutral"));
        QVERIFY(status->text().contains("All time · confirmed"));
        QVERIFY(metric("dgb_available")->text().contains("Available: 5 DGB"));
        QVERIFY(metric("dgb_reserved")->text().contains("In use: 1 DGB"));
        QVERIFY(metric("dgb_pending")->text().contains("Awaiting confirmation: 0.5 DGB"));
        QVERIFY(metric("income")->text().contains(" DD"));
        QVERIFY(metric("costs")->text().contains(" DGB"));
        QVERIFY(metric("carrier_base")->text().contains("4.00 DD"));
        QVERIFY(metric("carrier_earned")->text().contains("0.25 DD"));
        QVERIFY(metric("carrier_withdrawable")->text().contains("0.20 DD"));
        QCOMPARE(metric("maintenance_pending")->property("statusKind").toString(), QStringLiteral("waiting"));
    }
    if (scenario == QLatin1String("surplus_wrong_pool")) {
        QVERIFY(pool_status->property("statusKind").toString() != QLatin1String("ready"));
        QCOMPARE(metric("operational_dgb")->property("statusKind").toString(), QStringLiteral("error"));
    } else if (scenario == QLatin1String("pending") || scenario == QLatin1String("reserved")) {
        QVERIFY(pool_status->property("statusKind").toString() != QLatin1String("ready"));
        QCOMPARE(metric("operational_dgb")->property("statusKind").toString(), QStringLiteral("waiting"));
    } else {
        QCOMPARE(pool_status->property("statusKind").toString(), QStringLiteral("ready"));
    }
    if (scenario == QLatin1String("sponsored"))
        QCOMPARE(metric("operational_carriers")->property("statusKind").toString(), QStringLiteral("neutral"));
    if (scenario == QLatin1String("unsaved_targets")) {
        auto* target = panel->findChild<QSpinBox*>("paymasterOperationalDgbSlots");
        QVERIFY(target);
        target->setValue(16);
        QVERIFY(metric("operational_dgb")->text().contains("1 / 1 ready"));
        QCOMPARE(pool_status->property("statusKind").toString(), QStringLiteral("ready"));
        QCOMPARE(commands.size(), 2); // Editing is not an authorization or RPC.
    }
}

void PaymasterWidgetTests::paymasterOverviewFinanceWalletAndPrivacyBinding()
{
    for (bool close_wallet : {false, true}) {
        std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
        DigiDollarPaymasterWidget::RpcCallback pending;
        QString command;
        panel->setAsyncRpcExecutorForTesting([&](const std::string& method, const UniValue&, DigiDollarPaymasterWidget::RpcCallback callback) {
            QVERIFY(!pending);
            command = QString::fromStdString(method);
            pending = std::move(callback);
        });
        panel->refreshStatus();
        QCOMPARE(command, QStringLiteral("getpaymasteroperatorinfo"));
        auto provider_callback = std::move(pending);
        pending = {};
        provider_callback(GuidedOperatorSnapshot(), {});
        QVERIFY(pending);
        QCOMPARE(command, QStringLiteral("getpaymasterfinancestatus"));
        if (close_wallet) panel->setWalletModel(nullptr);
        else panel->setPrivacy(true);
        auto finance_callback = std::move(pending);
        pending = {};
        finance_callback(PaymasterOverviewFinance(), {});
        QVERIFY(!pending);
        auto* capital = panel->findChild<QWidget*>("paymasterOverviewCapitalDetails");
        auto* status = panel->findChild<QLabel*>("paymasterOverviewFinanceStatus");
        QVERIFY(capital && status);
        QVERIFY(!status->text().contains("1.25"));
        if (close_wallet) {
            for (auto* label : capital->findChildren<QLabel*>()) QVERIFY(!label->text().contains("5"));
        } else {
            QVERIFY(capital->isHidden());
            QVERIFY(panel->findChild<QWidget*>("paymasterOverviewFinanceDetails")->isHidden());
        }
    }
}

void PaymasterWidgetTests::paymasterStopAndRelease_data()
{
    QTest::addColumn<QString>("decision");
    QTest::addColumn<bool>("retire");
    for (const char* decision : {"cancel_stop", "stop_only", "cancel_release", "release", "blocked", "bad_receipt", "privacy", "wallet_change"})
        QTest::newRow(decision) << QString::fromLatin1(decision) << false;
    for (const char* decision : {"cancel_stop", "cancel_release", "release", "blocked", "bad_receipt", "privacy", "wallet_change", "stop_error", "bad_stop",
                                "balance_error", "bad_dgb", "bad_dd", "backup_cancelled", "empty_pool"})
        QTest::newRow(qPrintable(QStringLiteral("retire_") + decision)) << QString::fromLatin1(decision) << true;
}

void PaymasterWidgetTests::paymasterStopAndRelease()
{
    QFETCH(QString, decision);
    QFETCH(bool, retire);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    UniValue backup_status;
    QVERIFY(backup_status.read(R"({"required":true,"reminder_updated_at":1,"last_successful_backup_at":0,"external_backup_acknowledged_at":0})"));
    provider.pushKV("backup_status", backup_status);
    snapshot.pushKV("provider", provider);
    QStringList writes;
    int previews{0}, executions{0}, balance_reads{0}, backup_requests{0};
    panel->setProviderBackupRequestHandler([&] { ++backup_requests; }); // cancelled file dialog: no acknowledgement
    panel->setRpcExecutorForTesting([&](const std::string& method, const UniValue& params) {
        if (method == "getpaymasteroperatorinfo") return snapshot;
        if (method == "getpaymasterfinancestatus") return PaymasterOverviewFinance();
        if (method == "stoppaymaster") {
            if (decision == "stop_error") throw std::runtime_error("Provider busy");
            if (!params[0].find_value("persistent").isTrue() || !params[0].find_value("pause_setup").isTrue())
                throw std::runtime_error("Stop must be persistent");
            writes << QStringLiteral("stop");
            auto provider = snapshot.find_value("provider");
            provider.pushKV("enabled", false);
            provider.pushKV("running", false);
            provider.pushKV("autostart", false);
            snapshot.pushKV("provider", provider);
            UniValue result{UniValue::VOBJ}; result.pushKV("running", decision == "bad_stop"); return result;
        }
        if (method == "releasepaymastercapital") {
            const bool execute = params.size() > 0 && params[0].find_value("execute").isTrue();
            execute ? ++executions : ++previews;
            if (decision == QLatin1String("blocked")) throw std::runtime_error("PAYMASTER_CAPITAL_RESERVED");
            if (execute && params[0].find_value("plan_id").get_str() != std::string(64, 'c'))
                throw std::runtime_error("Unreviewed release plan");
            if (execute) writes << QStringLiteral("release");
            UniValue result{UniValue::VOBJ};
            result.pushKV("executed", execute);
            result.pushKV("plan_id", std::string(64, execute && decision == QLatin1String("bad_receipt") ? 'd' : 'c'));
            result.pushKV("pool_entries", decision == "empty_pool" ? 0 : 8);
            result.pushKV("dgb_satoshis", decision == "empty_pool" ? 0 : 100000000);
            result.pushKV("dd_cents", decision == "empty_pool" ? 0 : 425);
            result.pushKV("network_fee_satoshis", 0);
            return result;
        }
        if (method == "getbalances") {
            ++balance_reads;
            if (decision == "balance_error") throw std::runtime_error("Balance unavailable");
            UniValue result;
            result.read(R"({"mine":{"trusted":12.00000001,"untrusted_pending":0.5,"immature":3,"used":0.2}})");
            if (decision == "bad_dgb") result.read(R"({"mine":{"trusted":-1,"untrusted_pending":0,"immature":0}})");
            return result;
        }
        if (method == "getdigidollarbalance") {
            if (params.size() != 2 || params[0].get_str() != "" || params[1].getInt<int>() != 0)
                throw std::runtime_error("Pending DD must be included");
            UniValue result;
            result.read(R"({"confirmed":1234,"unconfirmed":50,"total":1284})");
            if (decision == "bad_dd") result.pushKV("total", 1283);
            return result;
        }
        throw std::runtime_error("Unexpected stop RPC");
    });
    panel->refreshStatus();
    auto* button = panel->findChild<QPushButton*>(retire ? "paymasterOperatorRetire" : "paymasterOperatorStop");
    auto* status = panel->findChild<QLabel*>("paymasterStopStatus");
    QVERIFY(button && status && button->isEnabled());
    bool default_checked_correctly{false}, reviewed{false};
    QTimer answer;
    connect(&answer, &QTimer::timeout, panel.get(), [&] {
        if (auto* dialog = panel->findChild<QDialog*>("paymasterStopDialog"); dialog && dialog->isVisible()) {
            auto* release = dialog->findChild<QCheckBox*>("paymasterStopReleaseAll");
            if (retire) {
                QVERIFY(!release);
                auto* notice = dialog->findChild<QLabel*>("paymasterRetirementReleaseNotice");
                QVERIFY(notice && notice->text().contains("approve the exact release separately"));
                default_checked_correctly = true;
            } else {
                QVERIFY(release && release->isEnabled());
                default_checked_correctly = !release->isChecked();
                release->click();
                QVERIFY(release->isChecked());
                release->click();
                QVERIFY(!release->isChecked());
                release->setChecked(decision != QLatin1String("stop_only"));
            }
            dialog->done(decision == QLatin1String("cancel_stop") ? QDialog::Rejected : QDialog::Accepted);
        } else if (auto* review = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            QVERIFY(review->text().contains(decision == "empty_pool" ? "0.00 DD" : "4.25 DD"));
            reviewed = true;
            if (decision == QLatin1String("privacy")) panel->setPrivacy(true);
            if (decision == QLatin1String("wallet_change")) panel->setWalletModel(nullptr);
            review->done(decision == QLatin1String("cancel_release") ? QMessageBox::No : QMessageBox::Yes);
        }
    });
    answer.start(10);
    button->click();
    answer.stop();
    QVERIFY(default_checked_correctly);
    QCOMPARE(writes.count(QStringLiteral("stop")), decision == "cancel_stop" || decision == "stop_error" ? 0 : 1);
    const bool approved = QStringList{"release", "bad_receipt", "balance_error", "bad_dgb", "bad_dd", "backup_cancelled", "empty_pool"}.contains(decision);
    QCOMPARE(executions, approved ? 1 : 0);
    const bool stop_confirmed = !QStringList{"cancel_stop", "stop_only", "stop_error", "bad_stop"}.contains(decision);
    QCOMPARE(previews, stop_confirmed ? 1 : 0);
    if (approved) QVERIFY(reviewed);
    if (decision == QLatin1String("release")) QVERIFY(status->text().contains(retire ? "Review the remaining funds" : "all reviewed pool capital"));
    if (decision == QLatin1String("bad_receipt")) QVERIFY(status->text().contains("unclear"));
    if (decision == QLatin1String("blocked")) QVERIFY(status->text().contains("capital was not released"));
    if (decision == "stop_error" || decision == "bad_stop") QVERIFY(status->text().contains("Stop not confirmed"));
    const bool balance_review = retire && approved && decision != "bad_receipt";
    QCOMPARE(balance_reads, balance_review ? 1 : 0);
    auto* summary = panel->findChild<QGroupBox*>("paymasterRetirementSummary");
    auto* balances = panel->findChild<QLabel*>("paymasterRetirementBalances");
    QVERIFY(summary && balances);
    QCOMPARE(summary->isHidden(), !balance_review);
    if (balance_review) {
        if (decision == "balance_error" || decision == "bad_dgb" || decision == "bad_dd") {
            QVERIFY(balances->text().contains("could not be verified"));
            QVERIFY(status->text().contains("incomplete"));
        } else {
            QVERIFY(balances->text().contains("12.00000001 DGB"));
            QVERIFY(balances->text().contains("12.34 DD"));
            QVERIFY(balances->text().contains("0.50 DD"));
            QVERIFY(balances->text().contains("DGB excluded by address-reuse avoidance"));
        }
    }
    if (decision == "backup_cancelled") {
        auto* backup = panel->findChild<QPushButton*>("paymasterRetirementBackup");
        QVERIFY(backup && backup->isEnabled());
        backup->click();
        QCOMPARE(backup_requests, 1);
        QVERIFY(panel->findChild<QLabel*>("paymasterRetirementBackupStatus")->text().contains("still recommended"));
        QCOMPARE(writes, QStringList({"stop", "release"}));
    }
    if (retire && decision == "release") {
        QVERIFY(status->text().contains("Provider stopped and pool capital released"));
        auto resumed = GuidedOperatorSnapshot();
        auto running = resumed.find_value("provider");
        running.pushKV("running", true);
        running.pushKV("service_state", "active");
        resumed.pushKV("provider", running);
        resumed.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(running, 0, 100));
        panel->setOperatorStatusForTesting(resumed);
        QVERIFY(status->isHidden());
        QVERIFY(status->text().isEmpty());
        QVERIFY(summary->isHidden());
        panel->setPrivacy(true);
        panel->setPrivacy(false);
        QVERIFY(status->isHidden()); // Toggling privacy must not resurrect it.
        QVERIFY(status->text().isEmpty());
    }

}

void PaymasterWidgetTests::paymasterHomeStartIsSeparateFromSettings()
{
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    int calls{0};
    panel->setRpcExecutorForTesting([&](const std::string&, const UniValue&) {
        ++calls;
        return UniValue{UniValue::VOBJ};
    });
    auto* start = panel->findChild<QPushButton*>("paymasterOperatorStart");
    auto* next = panel->findChild<QPushButton*>("paymasterOperatorNextAction");
    auto* overview = panel->findChild<QWidget*>("paymasterOverviewPage");
    QVERIFY(start && next && overview && overview->isAncestorOf(start));
    QVERIFY(!start->isEnabled());
    auto snapshot = GuidedOperatorSnapshot();
    panel->setOperatorStatusForTesting(snapshot);
    QVERIFY(start->isEnabled());
    QVERIFY(next->isHidden()); // No duplicate Start button.
    UniValue diagnostics;
    QVERIFY(diagnostics.read(R"([{"code":"PAYMASTER_BACKUP_REQUIRED","state":"action_required","action":"backup_wallet","area":"wallet"}])"));
    snapshot.pushKV("diagnostics", diagnostics);
    panel->setOperatorStatusForTesting(snapshot);
    QVERIFY(start->isEnabled()); // Reminder is not a readiness gate.
    QVERIFY(next->isHidden()); // Start remains the single primary action.
    QVERIFY(panel->findChild<QPushButton*>("paymasterOverviewBackupNow"));
    QTimer answer;
    connect(&answer, &QTimer::timeout, panel.get(), [&] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            dialog->done(QMessageBox::Cancel);
    });
    answer.start(10);
    start->click();
    answer.stop();
    QCOMPARE(calls, 0);
    for (const char* scenario : {"locked", "not_ready", "no_identity", "running", "unknown"}) {
        auto current = snapshot;
        auto provider = current.find_value("provider");
        if (std::string(scenario) == "locked") provider.pushKV("wallet_locked", true);
        if (std::string(scenario) == "not_ready") provider.pushKV("ready", false);
        if (std::string(scenario) == "no_identity") provider.pushKV("provider_id", "");
        if (std::string(scenario) == "running") {
            provider.pushKV("running", true);
            provider.pushKV("service_state", "active");
        }
        if (std::string(scenario) == "unknown") current.pushKV("schema_version", 999);
        current.pushKV("provider", provider);
        panel->setOperatorStatusForTesting(current);
        if (std::string(scenario) == "running") {
            auto* headline = panel->findChild<QLabel*>("paymasterOperatorHeadline");
            QVERIFY(headline && headline->text().contains("running", Qt::CaseInsensitive));
            QVERIFY(!headline->text().contains("backup", Qt::CaseInsensitive));
            QVERIFY(!panel->findChild<QPushButton*>("paymasterOperatorPause")->isHidden());
            const auto screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_PAGES_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                panel->setRpcExecutorForTesting([current](const std::string& command, const UniValue&) {
                    if (command == "getpaymasteroperatorinfo") return current;
                    if (command == "getpaymasterfinancestatus") return PaymasterOverviewFinance();
                    if (command == "getpaymasterinfo") return current.find_value("provider");
                    if (command == "getpaymasterliquiditystatus") return current.find_value("provider").find_value("liquidity");
                    if (command == "getpaymastersafetystatus") return current.find_value("safety");
                    return UniValue{UniValue::VOBJ};
                });
                panel->refreshStatus();
                panel->setReadinessStatusForTesting(provider);
                panel->setLiquidityStatusForTesting(provider.find_value("liquidity"));
                QTimer::singleShot(0, [] {
                    if (auto* confirm = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) confirm->done(QMessageBox::Yes);
                });
                panel->findChild<QPushButton*>("paymasterChooseExpertSetup")->click();
                auto* pages = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
                panel->setObjectName("paymasterWidget");
                panel->resize(820, 720);
                for (const auto* theme : {"dark", "light"}) {
                    QFile css(QString(":/css/") + theme);
                    QVERIFY(css.open(QIODevice::ReadOnly));
                    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
                    for (const auto* page : {"paymasterOverviewPage", "paymasterLiquidityPage"}) {
                        pages->setCurrentWidget(panel->findChild<QWidget*>(page));
                        panel->show();
                        QVERIFY(QTest::qWaitForWindowExposed(panel.get()));
                        QCoreApplication::processEvents();
                        QVERIFY(panel->grab().save(screenshot + "-" + theme + "-" + page + ".png"));
                    }
                }
                pages->setCurrentWidget(overview);
                panel->hide();
            }
        }
        QVERIFY2(!start->isEnabled(), scenario);
        start->click();
        QCOMPARE(calls, 0);
    }
    panel->setOperatorStatusForTesting(snapshot);
    panel->setPrivacy(true);
    QVERIFY(!start->isEnabled());
    QCOMPARE(calls, 0);
}

void PaymasterWidgetTests::paymasterRetirementLateResults_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<bool>("wallet_change");
    for (const char* phase : {"stop", "preview", "execute", "dgb", "dd"}) {
        for (bool wallet_change : {false, true})
            QTest::newRow(qPrintable(QString::fromLatin1(phase) + (wallet_change ? "_wallet" : "_privacy")))
                << QString::fromLatin1(phase) << wallet_change;
    }
}

void PaymasterWidgetTests::paymasterRetirementLateResults()
{
    QFETCH(QString, phase);
    QFETCH(bool, wallet_change);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    DigiDollarPaymasterWidget::RpcCallback pending;
    QString command;
    UniValue parameters;
    QStringList calls;
    panel->setAsyncRpcExecutorForTesting([&](const std::string& method, const UniValue& params, DigiDollarPaymasterWidget::RpcCallback callback) {
        QVERIFY(!pending);
        command = QString::fromStdString(method);
        parameters = params;
        calls << command;
        pending = std::move(callback);
    });
    const auto reply = [&](const UniValue& result) {
        QVERIFY(pending);
        auto callback = std::move(pending); pending = {};
        callback(result, {});
    };
    panel->refreshStatus();
    QCOMPARE(command, QStringLiteral("getpaymasteroperatorinfo"));
    reply(GuidedOperatorSnapshot());
    QCOMPARE(command, QStringLiteral("getpaymasterfinancestatus"));
    reply(PaymasterOverviewFinance());
    QVERIFY(!pending);
    auto* retire = panel->findChild<QPushButton*>("paymasterOperatorRetire");
    QVERIFY(retire && retire->isEnabled());
    QTimer answer;
    connect(&answer, &QTimer::timeout, panel.get(), [&] {
        if (auto* dialog = panel->findChild<QDialog*>("paymasterStopDialog"); dialog && dialog->isVisible())
            dialog->accept();
        else if (auto* review = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            review->done(QMessageBox::Yes);
    });
    answer.start(10);
    retire->click();
    for (const char* stage : {"stop", "preview", "execute", "dgb", "dd"}) {
        QVERIFY(pending);
        auto* tabs = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
        QVERIFY(tabs && tabs->isEnabled());
        const auto calls_before_clicks = calls;
        for (auto* button : tabs->findChildren<QPushButton*>()) {
            QVERIFY(!button->isEnabled());
            button->click();
        }
        QCOMPARE(calls, calls_before_clicks);
        UniValue result{UniValue::VOBJ};
        const QString current = QString::fromLatin1(stage);
        if (current == "stop") {
            QCOMPARE(command, QStringLiteral("stoppaymaster"));
            result.pushKV("running", false);
        } else if (current == "preview" || current == "execute") {
            QCOMPARE(command, QStringLiteral("releasepaymastercapital"));
            if (current == "execute") {
                QVERIFY(parameters[0].find_value("execute").isTrue());
                QCOMPARE(parameters[0].find_value("plan_id").get_str(), std::string(64, 'c'));
            }
            result.pushKV("executed", current == "execute");
            result.pushKV("plan_id", std::string(64, 'c'));
            result.pushKV("pool_entries", 0);
            result.pushKV("dgb_satoshis", 0);
            result.pushKV("dd_cents", 0);
            result.pushKV("network_fee_satoshis", 0);
        } else if (current == "dgb") {
            QCOMPARE(command, QStringLiteral("getbalances"));
            QVERIFY(result.read(R"({"mine":{"trusted":12.3456,"untrusted_pending":0,"immature":0}})"));
        } else {
            QCOMPARE(command, QStringLiteral("getdigidollarbalance"));
            QVERIFY(result.read(R"({"confirmed":2345,"unconfirmed":0,"total":2345})"));
        }
        if (current == phase) {
            if (wallet_change) panel->setWalletModel(nullptr);
            else panel->setPrivacy(true);
            const auto count = calls.size();
            reply(result);
            QVERIFY(!pending);
            QCOMPARE(calls.size(), count); // no follow-on mutation, balance read or backup
            QVERIFY(panel->findChild<QGroupBox*>("paymasterRetirementSummary")->isHidden());
            QVERIFY(panel->findChild<QLabel*>("paymasterRetirementBalances")->text().isEmpty());
            QVERIFY(panel->findChild<QLabel*>("paymasterStopStatus")->isHidden());
            answer.stop();
            return;
        }
        reply(result);
    }
    QFAIL("Requested retirement boundary was not exercised");
}

void PaymasterWidgetTests::paymasterGuidedRestoreHasOneApproval_data()
{
    QTest::addColumn<QString>("decision");
    for (const char* decision : {"approve", "cancel", "privacy", "wallet_change",
             "repair_approve", "repair_cancel", "repair_privacy", "repair_wallet_change",
             "repair_changed_settings", "repair_changed_cost", "repair_lost_reply", "repair_write_failed", "repair_retry"})
        QTest::newRow(decision) << QString::fromLatin1(decision);
}

void PaymasterWidgetTests::paymasterGuidedRestoreHasOneApproval()
{
    QFETCH(QString, decision);
    const bool repair = decision.startsWith("repair_");
    const QString outcome = repair ? decision.mid(7) : decision;
    const bool approved = outcome != "cancel" && outcome != "privacy" && outcome != "wallet_change";
    const bool completes = outcome == "approve" || outcome == "lost_reply" || outcome == "retry";
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    UniValue snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    provider.pushKV("pool_ready", false);
    provider.pushKV("active_operations", UniValue{UniValue::VARR});
    if (repair) {
        provider.pushKV("enabled", false);
        provider.pushKV("ready", false);
        auto liquidity = provider.find_value("liquidity");
        auto policy = liquidity.find_value("policy");
        policy.pushKV("target_operational_carriers", 0);
        liquidity.pushKV("policy", policy);
        liquidity.pushKV("targets_satisfy_provider_policy", false);
        liquidity.pushKV("maintenance_state", "waiting_for_target_configuration");
        provider.pushKV("liquidity", liquidity);
        UniValue diagnostics;
        diagnostics.read(R"([{"code":"PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE","state":"action_required","severity":"warning","area":"liquidity","action":"review_liquidity"}])");
        snapshot.pushKV("diagnostics", diagnostics);
    }
    snapshot.pushKV("provider", provider);
    const auto saved_policy = provider.find_value("liquidity").find_value("policy");
    int previews{0}, executions{0}, confirmations{0}, writes{0}, starts{0}, enables{0};
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasteroperatorinfo") return snapshot;
        if (command == "setpaymasterliquiditypolicy") {
            ++writes;
            UniValue expected = saved_policy;
            expected.pushKV("target_operational_carriers", 1);
            for (const auto& key : params[0].getKeys()) {
                if (params[0].find_value(key).write() != expected.find_value(key).write())
                    throw std::runtime_error("Restore changed an unrelated setting");
            }
            if (outcome == "write_failed") throw std::runtime_error("Write failed");
            auto liquidity = provider.find_value("liquidity");
            liquidity.pushKV("policy", params[0]);
            liquidity.pushKV("targets_satisfy_provider_policy", true);
            provider.pushKV("liquidity", liquidity);
            snapshot.pushKV("provider", provider);
            if (outcome == "lost_reply") throw std::runtime_error("Reply lost after durable write");
            return params[0];
        }
        if (command == "setpaymasterenabled") {
            ++enables;
            provider.pushKV("enabled", true);
            snapshot.pushKV("provider", provider);
            UniValue result{UniValue::VOBJ}; result.pushKV("enabled", true); return result;
        }
        if (command == "startpaymaster") {
            ++starts;
            if (!params[0].find_value("wait_for_readiness").isTrue()) throw std::runtime_error("Missing reviewed deferred start");
            UniValue result{UniValue::VOBJ}; result.pushKV("start_requested", true); return result;
        }
        if (command != "preparepaymasterpool") throw std::runtime_error("Unexpected guided operation");
        if (params[0].find_value("operational_carrier_slots").getInt<int>() != 1)
            throw std::runtime_error("PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
        const bool execute = params[0].find_value("execute").isTrue();
        execute ? ++executions : ++previews;
        if (outcome == "retry" && previews == 1)
            throw std::runtime_error("PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
        UniValue result;
        result.read(R"({"accepted":false,"cancelled":false,"executed":false,"preparation":[],
            "maximum_fee_satoshis":50000000,"maximum_total_fee_satoshis":50000000,
            "admission_dgb_slots":3,"operational_dgb_slots":1,"admission_carrier_slots":3,"operational_carrier_slots":1,
            "missing_admission_dgb_slots":0,"missing_operational_dgb_slots":0,
            "missing_admission_carrier_slots":0,"missing_operational_carrier_slots":1,
            "admission_dgb_satoshis_each":10000000,"operational_dgb_satoshis_each":20000000,"carrier_cents_each":100,
            "total_output_satoshis":0,"total_carrier_cents":100})");
        const std::string plan(64, writes ? 'd' : 'c');
        result.pushKV("plan_id", plan);
        if (writes && outcome == "changed_cost") {
            result.pushKV("missing_admission_dgb_slots", 1);
            result.pushKV("total_output_satoshis", 10000000);
        }
        result.pushKV("accepted", execute);
        if (execute) {
            if (params[0].find_value("plan_id").get_str() != plan ||
                params[0].find_value("maximum_fee_satoshis").getInt<int64_t>() != 50000000)
                throw std::runtime_error("Reviewed scope changed");
            UniValue pending;
            pending.read(R"([{"state":"pending_creation","error":"","confirmations":0,"required_confirmations":1}])");
            provider.pushKV("active_operations", pending);
            snapshot.pushKV("provider", provider);
        }
        return result;
    });
    panel->refreshStatus();
    auto* restore = panel->findChild<QPushButton*>("paymasterRestoreTask");
    auto* status = panel->findChild<QLabel*>("paymasterTaskStatus");
    QVERIFY(restore && status);
    QVERIFY(restore->isEnabled());
    auto* primary = panel->findChild<QPushButton*>("paymasterOperatorNextAction");
    QVERIFY(primary);
    if (repair) {
        QVERIFY(primary->text().contains("Restore reserves and start"));
        QVERIFY(restore->isHidden());
        QVERIFY(panel->findChild<QPushButton*>("paymasterOverviewLiquidityAction")->isHidden());
    }
    QTimer response;
    response.setInterval(0);
    connect(&response, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog) return;
        ++confirmations;
        const QString review = dialog->text();
        const bool amounts_visible = review.contains("1.00 DD");
        QVERIFY(review.contains("0.50000000 DGB"));
        if (repair) {
            QVERIFY(review.contains(QString::fromUtf8("Payment reserves: 0 → 1")));
            QVERIFY(review.contains("Unchanged maintenance limits"));
            QVERIFY(review.contains("Start the provider automatically"));
            QCOMPARE(writes, 0);
        }
        if (outcome == "changed_settings") {
            auto changed = provider.find_value("policy"); changed.pushKV("fee_rate_bps", 75);
            provider.pushKV("policy", changed); snapshot.pushKV("provider", provider);
        }
        if (outcome == "privacy") panel->setPrivacy(true);
        if (outcome == "wallet_change") panel->setWalletModel(nullptr);
        dialog->button(approved ? QMessageBox::Yes : QMessageBox::Cancel)->click();
        response.stop();
        QVERIFY2(amounts_visible, qPrintable(dialog->text()));
    });
    response.start();
    QTimer::singleShot(5000, panel.get(), [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    (repair ? primary : restore)->click();
    if (outcome == "retry") {
        auto* next = panel->findChild<QPushButton*>("paymasterContinueTask");
        QVERIFY(next && next->isEnabled());
        QVERIFY(next->text().contains("Restore required reserves"));
        QVERIFY(status->text().contains("too low to receive service fees"));
        QVERIFY(!status->text().contains("PAYMASTER_"));
        next->click();
    }
    QTRY_COMPARE(confirmations, 1);
    QCOMPARE(previews, outcome == "retry" ? 3 : repair && approved && outcome != "changed_settings" && outcome != "write_failed" ? 2 : 1);
    QCOMPARE(writes, repair && approved && outcome != "changed_settings" ? 1 : 0);
    QCOMPARE(executions, completes ? 1 : 0);
    QCOMPARE(starts, repair && completes ? 1 : 0);
    QCOMPARE(enables, repair && completes ? 1 : 0);
    if (outcome == "changed_settings") QVERIFY(status->text().contains("settings changed during review"));
    if (outcome == "changed_cost") QVERIFY(status->text().contains("funding requirement changed"));
    if (outcome == "write_failed") QVERIFY(status->text().contains("settings do not match"));
    if (completes) {
        QVERIFY(status->text().contains("No confirmation is pending"));
        QVERIFY(!status->text().contains("4/5"));
        QVERIFY(!restore->isEnabled());
        restore->click();
        QCOMPARE(executions, 1);
        provider.pushKV("pool_ready", true);
        provider.pushKV("active_operations", UniValue{UniValue::VARR});
        snapshot.pushKV("provider", provider);
        panel->refreshStatus();
        QVERIFY(status->text().contains("Task complete"));
        QVERIFY(restore->isEnabled());
    }
}

void PaymasterWidgetTests::paymasterGuidedCapitalTasks_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("decision");
    for (const char* mode : {"all_excess", "release_slot", "rebalancepaymasterpool"})
        for (const char* decision : {"approve", "cancel", "lost_reply"})
            QTest::newRow((std::string(mode) + "-" + decision).c_str()) << QString::fromLatin1(mode) << QString::fromLatin1(decision);
    for (const char* decision : {"below_minimum", "zero_earnings", "one_cent_short", "minimum_error", "unknown_error"})
        QTest::newRow(decision) << QStringLiteral("all_excess") << QString::fromLatin1(decision);
    for (const char* decision : {"fresh_selection", "selection_reordered", "empty_pool", "invalid_pool", "pool_read_error",
                                 "slot_unavailable_preview", "slot_unavailable_execute", "plan_changed", "target_zero"})
        QTest::newRow(decision) << QStringLiteral("release_slot") << QString::fromLatin1(decision);
    QTest::newRow("release-slot-wallet-selection") << QStringLiteral("release_slot") << QStringLiteral("wallet_selection");
}

void PaymasterWidgetTests::paymasterGuidedCapitalTasks()
{
    QFETCH(QString, mode);
    QFETCH(QString, decision);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    UniValue snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    provider.pushKV("active_operations", UniValue{UniValue::VARR});
    auto liquidity = provider.find_value("liquidity");
    const bool below_minimum = decision == QLatin1String("below_minimum") || decision == QLatin1String("zero_earnings") || decision == QLatin1String("one_cent_short");
    const qint64 earnings = decision == QLatin1String("below_minimum") ? 6 : decision == QLatin1String("zero_earnings") ? 0 : decision == QLatin1String("one_cent_short") ? 99 : 100;
    liquidity.pushKV("carrier_withdrawable_excess_cents", earnings);
    provider.pushKV("liquidity", liquidity);
    snapshot.pushKV("provider", provider);
    int previews{0}, executions{0}, approvals{0}, pauses{0}, selections{0}, pool_reads{0};
    auto current_pool = PaymasterLiquidityPoolStatus();
    UniValue current_entries{UniValue::VARR};
    auto current_reserve = current_pool.find_value("pool")[0];
    const bool replaced = decision == "fresh_selection" || decision == "selection_reordered";
    if (replaced) current_reserve.pushKV("txid", std::string(64, 'b'));
    if (decision == "empty_pool") current_reserve.pushKV("state", "reserved");
    current_entries.push_back(current_reserve);
    current_pool.pushKV("pool", current_entries);
    const QString release_error = decision == "plan_changed" ? QStringLiteral("PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED") :
        decision == "target_zero" ? QStringLiteral("PAYMASTER_OPERATIONAL_CARRIER_TARGET_ALREADY_ZERO") :
        QStringLiteral("PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE");
    const bool blocked_preview = decision == "slot_unavailable_preview" || decision == "target_zero";
    const bool blocked_execute = decision == "slot_unavailable_execute" || decision == "plan_changed";
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasteroperatorinfo") return snapshot;
        if (command == "getpaymasterpoolinfo") {
            ++pool_reads;
            if (decision == "pool_read_error") throw std::runtime_error("Reserve read unavailable");
            if (decision == "invalid_pool") return UniValue{UniValue::VOBJ};
            return current_pool;
        }
        if (command == "stoppaymaster") {
            ++pauses;
            if (!params[0].find_value("persistent").isTrue() || !params[0].find_value("pause_setup").isTrue())
                throw std::runtime_error("Capital release did not pause safely");
            provider.pushKV("running", false);
            provider.pushKV("enabled", false);
            snapshot.pushKV("provider", provider);
            UniValue result{UniValue::VOBJ}; result.pushKV("running", false); return result;
        }
        if (command == "rebalancepaymasterpool") {
            const auto& options = params[0];
            const bool execute = options.find_value("execute").isTrue();
            execute ? ++executions : ++previews;
            if (!options.find_value("dgb_only").isTrue() ||
                options.find_value("maximum_fee_satoshis").getInt<int64_t>() <= 0 ||
                (execute && options.find_value("plan_id").get_str() != std::string(64, 'e')))
                throw std::runtime_error("DGB release lost its exact scope or plan");
            UniValue result{UniValue::VOBJ};
            result.pushKV("executed", execute);
            result.pushKV("plan_id", std::string(64, 'e'));
            result.pushKV("maximum_network_fee_satoshis", options.find_value("maximum_fee_satoshis"));
            result.pushKV("retired_admission_dgb_slots", 1);
            result.pushKV("retired_operational_dgb_slots", 0);
            result.pushKV("retired_admission_carrier_slots", 0);
            result.pushKV("retired_operational_carrier_slots", 0);
            result.pushKV("retired_dgb_satoshis", 10000000);
            result.pushKV("retired_carrier_cents", 0);
            if (execute && decision == "lost_reply") throw std::runtime_error("Connection lost after acceptance");
            return result;
        }
        if (command != "withdrawpaymastercarrier") throw std::runtime_error("Unexpected capital RPC");
        const bool execute = params[0].find_value("execute").isTrue();
        execute ? ++executions : ++previews;
        if (mode == "release_slot" && !execute &&
            params[0].find_value("txid").get_str() != current_reserve.find_value("txid").get_str())
            throw std::runtime_error("The review used a cached or different reserve");
        if ((!execute && blocked_preview) || (execute && blocked_execute))
            throw std::runtime_error(release_error.toStdString());
        if (decision == QLatin1String("minimum_error"))
            throw std::runtime_error("PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED: Amount below minimum DigiDollar output. Minimum: 100 cents");
        if (decision == QLatin1String("unknown_error"))
            throw std::runtime_error("PAYMASTER_TEST_WITHDRAWAL_FAILURE");
        if (execute && params[0].find_value("plan_id").get_str() != std::string(64, 'e'))
            throw std::runtime_error("Plan was not bound");
        UniValue result{UniValue::VOBJ};
        result.pushKV("executed", execute);
        result.pushKV("mode", mode.toStdString());
        result.pushKV("plan_id", std::string(64, 'e'));
        result.pushKV("source_carriers", 1);
        result.pushKV("expires_at", QDateTime::currentSecsSinceEpoch() + 600);
        result.pushKV("withdrawable_excess_cents", 100);
        result.pushKV("retained_carrier_cents", 100);
        result.pushKV("estimated_network_fee_satoshis", 1000);
        result.pushKV("operational_carrier_target", 0);
        if (execute && mode == QLatin1String("all_excess")) {
            result.pushKV("txid", std::string(64, 'f'));
            UniValue work;
            work.read(R"([{"kind":"withdrawal","state":"pending_confirmation","confirmations":0,"required_confirmations":1}])");
            provider.pushKV("active_operations", work);
            snapshot.pushKV("provider", provider);
        }
        if (execute && decision == QLatin1String("lost_reply")) throw std::runtime_error("Connection lost after acceptance");
        return result;
    });
    panel->refreshStatus();
    // A non-empty old selector must never bypass the current pool read.
    panel->setLiquidityPoolForTesting(PaymasterLiquidityPoolStatus());
    auto* action = panel->findChild<QPushButton*>(mode == QLatin1String("all_excess") ? "paymasterWithdrawTask" : mode == QLatin1String("rebalancepaymasterpool") ? "paymasterFinanceReviewDgb" : "paymasterReleaseTask");
    QVERIFY(action);
    auto* hint = panel->findChild<QLabel*>("paymasterEarningsWithdrawalHint");
    auto* expert_preview = panel->findChild<QPushButton*>("paymasterPreviewCarrierExcess");
    QVERIFY(hint && expert_preview);
    if (below_minimum) {
        QVERIFY(!action->isEnabled());
        QVERIFY(!expert_preview->isEnabled());
        QVERIFY(!hint->isHidden());
        QVERIFY(hint->text().contains(QString::number(earnings / 100.0, 'f', 2) + " DD"));
        QVERIFY(hint->text().contains("Minimum payout: 1.00 DD"));
        action->click();
        QCOMPARE(previews, 0);
        QCOMPARE(executions, 0);
        panel->setPrivacy(true);
        QVERIFY(hint->isHidden());
        QVERIFY(hint->text().isEmpty());
        QVERIFY(action->toolTip().isEmpty());
        panel->setPrivacy(false);
        panel->setWalletModel(nullptr);
        QVERIFY(hint->isHidden());
        QVERIFY(!hint->text().contains("0.06 DD"));
        return;
    }
    QVERIFY(action->isEnabled());
    if (decision == QLatin1String("minimum_error") || decision == QLatin1String("unknown_error")) {
        action->click();
        auto* status = panel->findChild<QLabel*>("paymasterTaskStatus");
        QVERIFY(status);
        if (decision == QLatin1String("minimum_error")) {
            QVERIFY(status->text().contains("at least 1.00 DD"));
            QVERIFY(status->text().contains("No withdrawal was submitted"));
        } else {
            QVERIFY(!status->text().contains("PAYMASTER_TEST_WITHDRAWAL_FAILURE"));
            auto* details = panel->findChild<QLabel*>("paymasterTaskErrorDetails");
            QVERIFY(details && details->isHidden());
            QVERIFY(details->text().contains("PAYMASTER_TEST_WITHDRAWAL_FAILURE"));
        }
        QCOMPARE(previews, 1);
        QCOMPARE(executions, 0);
        QCOMPARE(pauses, 0);
        return;
    }
    QTimer response;
    response.setInterval(0);
    connect(&response, &QTimer::timeout, [&] {
        if (auto* choose = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            ++selections;
            if (decision == "selection_reordered") {
                auto* reserves = panel->findChild<QComboBox*>("paymasterReleaseCarrierSelection");
                if (reserves) {
                    reserves->clear();
                    reserves->addItem("A different reserve", QString(QString(64, 'c') + ":7"));
                }
            }
            if (decision == QLatin1String("wallet_selection")) {
                response.stop();
                panel->setWalletModel(nullptr);
            }
            choose->accept();
            return;
        }
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            response.stop();
            ++approvals;
            dialog->button(decision == QLatin1String("cancel") ? QMessageBox::Cancel : QMessageBox::Yes)->click();
        }
    });
    response.start();
    action->click();
    if (decision == "empty_pool" || decision == "invalid_pool" || decision == "pool_read_error" || blocked_preview || blocked_execute) {
        auto* status = panel->findChild<QLabel*>("paymasterTaskStatus");
        auto* details = panel->findChild<QLabel*>("paymasterTaskErrorDetails");
        auto* more = panel->findChild<QPushButton*>("paymasterTaskErrorToggle");
        auto* retry = panel->findChild<QPushButton*>("paymasterContinueTask");
        QVERIFY(status && details && more && retry);
        QTRY_VERIFY(!more->isHidden());
        response.stop();
        QVERIFY(!status->text().contains("PAYMASTER_"));
        QVERIFY(details->isHidden());
        if (blocked_preview || blocked_execute || decision == "empty_pool") {
            QVERIFY(details->text().contains(release_error));
            QCOMPARE(retry->text(), QStringLiteral("Refresh reserves"));
        }
        QCOMPARE(previews, blocked_preview || blocked_execute ? 1 : 0);
        QCOMPARE(executions, blocked_execute ? 1 : 0);
        QCOMPARE(pauses, blocked_execute ? 1 : 0);
        QCOMPARE(approvals, blocked_execute ? 1 : 0);
        QVERIFY(pool_reads >= 1);
        if (decision == "slot_unavailable_preview") {
            const QString capture = qEnvironmentVariable("DIGIBYTE_PAYMASTER_RELEASE_SCREENSHOT");
            if (!capture.isEmpty()) {
                panel->setObjectName("paymasterWidget");
                QFile css(":/css/dark");
                QVERIFY(css.open(QIODevice::ReadOnly));
                panel->setStyleSheet(QString::fromUtf8(css.readAll()));
                panel->resize(1000, 700);
                panel->show();
                QVERIFY(QTest::qWaitForWindowExposed(panel.get()));
                QVERIFY(panel->grab().save(capture));
            }
            more->click();
            QVERIFY(!details->isHidden());
            const int reads_before = pool_reads;
            retry->click();
            QTRY_VERIFY(pool_reads > reads_before);
            QCOMPARE(previews, 1);
            QCOMPARE(executions, 0);
            QCOMPARE(pauses, 0);
            panel->setPrivacy(true);
            QVERIFY(details->text().isEmpty());
        } else {
            panel->setWalletModel(nullptr);
            QVERIFY(details->text().isEmpty());
        }
        return;
    }
    if (decision == QLatin1String("wallet_selection")) {
        QTRY_COMPARE(selections, 1);
        QCOMPARE(previews, 0);
        QCOMPARE(executions, 0);
        QCOMPARE(pauses, 0);
        return;
    }
    QTRY_COMPARE(approvals, 1);
    QCOMPARE(previews, 1);
    QCOMPARE(executions, decision == QLatin1String("cancel") ? 0 : 1);
    QCOMPARE(pauses, mode == QLatin1String("release_slot") && decision != QLatin1String("cancel") ? 1 : 0);
    if (decision != QLatin1String("cancel") && mode == QLatin1String("all_excess")) {
        auto* status = panel->findChild<QLabel*>("paymasterTaskStatus");
        QVERIFY(status->text().contains("0/1"));
        QVERIFY(!action->isEnabled());
        panel->refreshStatus();
        QCOMPARE(executions, 1);
        provider.pushKV("active_operations", UniValue{UniValue::VARR});
        snapshot.pushKV("provider", provider);
        panel->refreshStatus();
        QVERIFY(status->text().contains("Task complete"));
    }
}

void PaymasterWidgetTests::paymasterGuidedTaskLayout_data()
{
    QTest::addColumn<QString>("theme");
    QTest::addColumn<int>("font_size");
    for (const char* theme : {"dark", "light"})
        for (int size : {11, 17})
            QTest::newRow((std::string(theme) + "-" + std::to_string(size)).c_str()) << QString::fromLatin1(theme) << size;
}

void PaymasterWidgetTests::paymasterGuidedTaskLayout()
{
    QFETCH(QString, theme);
    QFETCH(int, font_size);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    panel->setObjectName("paymasterWidget");
    QFile css(":/css/" + theme);
    QVERIFY(css.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    QFont font = panel->font(); font.setPointSize(font_size); panel->setFont(font);
    UniValue snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    UniValue work;
    QVERIFY(work.read(R"([{"kind":"withdrawal","state":"pending_confirmation","confirmations":0,"required_confirmations":1}])"));
    provider.pushKV("active_operations", work);
    snapshot.pushKV("provider", provider);
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue&) {
        return command == "getpaymasterfinancestatus" ? PaymasterOverviewFinance() : snapshot;
    });
    panel->refreshStatus();
    panel->resize(760, 620);
    panel->show();
    QVERIFY(QTest::qWaitForWindowExposed(panel.get()));
    auto* scroll = panel->findChild<QScrollArea*>("paymasterOverviewPage");
    auto* status = panel->findChild<QLabel*>("paymasterTaskStatus");
    auto* action = panel->findChild<QPushButton*>("paymasterOperatorNextAction");
    QVERIFY(scroll && status && action);
    QCoreApplication::processEvents();
    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    QVERIFY(status->text().contains("Withdraw earnings"));
    QVERIFY(status->text().contains("0/1"));
    QVERIFY(status->height() >= status->heightForWidth(status->width()));
    // Task progress stays outside the scrolling page and visible across destinations.
    QVERIFY(panel->rect().contains(QRect(status->mapTo(panel.get(), QPoint{}), status->size())));
    action->setFocus();
    QTest::keyClick(action, Qt::Key_Tab);
    QVERIFY(QApplication::focusWidget() && QApplication::focusWidget() != action);
    const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_TASK_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(panel->grab().save(screenshot + "-" + QTest::currentDataTag() + ".png"));
    auto* budget_details = panel->findChild<QWidget*>("paymasterOverviewBudgetDetails");
    auto* spent = panel->findChild<QLabel*>("paymasterBudget_user_paid_day_spent");
    auto* limit = panel->findChild<QLabel*>("paymasterBudget_user_paid_day_limit");
    auto* reserved = panel->findChild<QLabel*>("paymasterBudget_user_paid_reserved");
    QVERIFY(budget_details && spent && limit && reserved);
    QVERIFY(!budget_details->isHidden());
    QVERIFY(spent->text().endsWith(" DGB"));
    QVERIFY(limit->text().endsWith(" DGB"));
    QVERIFY(reserved->text().endsWith(" DGB"));
    QVERIFY(reserved->alignment().testFlag(Qt::AlignRight));
    scroll->ensureWidgetVisible(budget_details);
    QCoreApplication::processEvents();
    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    for (auto* label : budget_details->findChildren<QLabel*>()) {
        if (!label->isVisible()) continue;
        if (label->wordWrap()) QVERIFY(label->height() >= label->heightForWidth(label->width()));
        else QVERIFY(label->width() >= label->sizeHint().width());
    }
    if (!screenshot.isEmpty()) QVERIFY(panel->grab().save(screenshot + "-budget-" + QTest::currentDataTag() + ".png"));
    auto* dashboard = panel->findChild<QWidget*>("paymasterOverviewDashboard");
    QVERIFY(dashboard);
    panel->resize(1200, 900);
    QCoreApplication::processEvents();
    if (!screenshot.isEmpty()) QVERIFY(dashboard->grab().save(screenshot + "-cards-" + QTest::currentDataTag() + ".png"));
    panel->setWalletModel(nullptr);
    auto broken = GuidedOperatorSnapshot();
    auto stopped = broken.find_value("provider");
    stopped.pushKV("enabled", false); stopped.pushKV("ready", false); stopped.pushKV("pool_ready", false);
    stopped.pushKV("active_operations", UniValue{UniValue::VARR});
    auto liquidity = stopped.find_value("liquidity");
    auto policy = liquidity.find_value("policy"); policy.pushKV("target_operational_carriers", 0);
    liquidity.pushKV("policy", policy); liquidity.pushKV("targets_satisfy_provider_policy", false);
    stopped.pushKV("liquidity", liquidity); broken.pushKV("provider", stopped);
    UniValue diagnostics;
    QVERIFY(diagnostics.read(R"([{"code":"PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE","state":"action_required","severity":"warning","area":"liquidity","action":"review_liquidity"}])"));
    broken.pushKV("diagnostics", diagnostics);
    panel->setRpcExecutorForTesting([&](const std::string&, const UniValue&) { return broken; });
    // This layout fixture supplies its wallet through the test RPC transport.
    // A real nullptr wallet intentionally disables the entire production panel.
    panel->setEnabled(true);
    panel->refreshStatus();
    scroll->verticalScrollBar()->setValue(0);
    QCoreApplication::processEvents();
    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    QVERIFY(action->isVisible() && action->isEnabled());
    QVERIFY(action->text().contains("Restore reserves and start"));
    QVERIFY(panel->findChild<QPushButton*>("paymasterOverviewLiquidityAction")->isHidden());
    QVERIFY(panel->findChild<QPushButton*>("paymasterRestoreTask")->isHidden());
    if (!screenshot.isEmpty()) QVERIFY(panel->grab().save(screenshot + "-repair-" + QTest::currentDataTag() + ".png"));
    auto* settings = panel->findChild<QStackedWidget*>("paymasterSettingsPages");
    auto* tabs = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    QVERIFY(tabs && settings->isEnabled());
    tabs->setCurrentWidget(settings);
    tabs->setCurrentWidget(panel->findChild<QWidget*>("paymasterLiquidityPage"));
    QVERIFY(tabs->currentWidget()->isVisible());
    panel->setPrivacy(true);
    QVERIFY(budget_details->isHidden());
    QVERIFY(spent->text().isEmpty());
    QVERIFY(limit->text().isEmpty());
    QVERIFY(reserved->text().isEmpty());
}


void PaymasterWidgetTests::paymasterNodeConnectionEditor_data()
{
    QTest::addColumn<QString>("scenario");
    for (const char* value : {"unchanged", "override", "all_locked_dark", "all_locked_light", "save", "edit_after_preview", "core_conflict", "wallet_change", "privacy", "close_pending"})
        QTest::newRow(value) << QString::fromLatin1(value);
}

void PaymasterWidgetTests::paymasterNodeConnectionEditor()
{
    QFETCH(QString, scenario);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    if (scenario.startsWith("all_locked_")) {
        QFile css(QStringLiteral(":/css/") + scenario.section('_', -1));
        QVERIFY(css.open(QIODevice::ReadOnly));
        panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    }
    auto snapshot = GuidedOperatorSnapshot();
    UniValue settings;
    QVERIFY(settings.read(R"({"paymasterbind":["127.0.0.1:18450"],"paymasterendpoint":"127.0.0.1:18450",
        "digidollar":1,"paymaster":1,"txindex":1,"v2transport":1,"prune":0,"maxconnections":125,
        "paymastermaxoutbound":1,"paymastermaxinbound":16})"));
    snapshot.pushKV("node_settings", settings);
    if (scenario == "override" || scenario.startsWith("all_locked_")) {
        UniValue overrides{UniValue::VOBJ}; overrides.pushKV("paymasterbind", "command_line");
        overrides.pushKV("paymasterendpoint", "command_line");
        if (scenario.startsWith("all_locked_"))
            for (const auto& key : settings.getKeys()) overrides.pushKV(key, "command_line");
        snapshot.pushKV("node_setting_overrides", overrides);
    }
    UniValue node{UniValue::VOBJ}, fields{UniValue::VOBJ};
    for (const auto& key : settings.getKeys()) {
        const bool locked = snapshot.find_value("node_setting_overrides").find_value(key).isStr();
        UniValue field{UniValue::VOBJ};
        field.pushKV("value", settings.find_value(key));
        field.pushKV("configured_value", settings.find_value(key));
        field.pushKV("source", locked ? "command_line" : "default");
        field.pushKV("editable", !locked);
        field.pushKV("reason", locked ? "command_line" : "");
        field.pushKV("restart_required", false);
        fields.pushKV(key, field);
    }
    node.pushKV("network", "regtest"); node.pushKV("target", "node.conf"); node.pushKV("fields", fields);
    int previews{0}, writes{0};
    panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
        if (command == "getpaymasteroperatorinfo") return snapshot;
        if (command == "getpaymasternodeconfig") return node;
        if (command == "preparepaymasternodeconfig") {
            ++previews;
            if (params[0].getKeys().size() != 1 || params[0].find_value("maxconnections").getInt<int>() != 130)
                throw std::runtime_error("Unchanged settings were submitted");
            if (scenario == "core_conflict") throw std::runtime_error("PAYMASTER_CONFIG_OVERRIDE: maxconnections (command line)");
            if (scenario == "wallet_change") panel->setWalletModel(nullptr);
            if (scenario == "privacy") panel->setPrivacy(true);
            if (scenario == "close_pending") {
                if (auto* editor = panel->findChild<QWidget*>("paymasterNodeConfigEditor")) delete editor;
            }
            UniValue result{UniValue::VOBJ}; result.pushKV("plan_id", std::string(64, 'c')); result.pushKV("settings", params[0]); return result;
        }
        if (command == "applypaymasternodeconfig") {
            ++writes;
            if (params[0].find_value("plan_id").get_str() != std::string(64, 'c') ||
                params[0].find_value("settings").find_value("maxconnections").getInt<int>() != 130)
                throw std::runtime_error("Unreviewed configuration");
            UniValue result{UniValue::VOBJ}; result.pushKV("applied", true); result.pushKV("backup", "node.conf.bak"); return result;
        }
        return UniValue{UniValue::VOBJ};
    });
    panel->refreshStatus();
    int opened{0};
    QTimer interact;
    interact.setInterval(0);
    connect(&interact, &QTimer::timeout, [&] {
        auto* dialog = panel->findChild<QWidget*>("paymasterNodeConfigEditor");
        if (!dialog) return;
        interact.stop(); ++opened;
        auto* bind = dialog->findChild<QPlainTextEdit*>("paymasterNode_paymasterbind");
        auto* total = dialog->findChild<QSpinBox*>("paymasterNode_maxconnections");
        auto* preview = dialog->findChild<QPushButton*>("paymasterNodePreview");
        auto* save = dialog->findChild<QPushButton*>("paymasterNodeSave");
        auto* status = dialog->findChild<QLabel*>("paymasterNodeConfigStatus");
        QVERIFY(preview && save && status);
        if (scenario.startsWith("all_locked_")) {
            QVERIFY(!bind && !total);
            QVERIFY(preview->isHidden() && save->isHidden());
            QCOMPARE(dialog->findChild<QLabel*>("paymasterNode_digidollar")->text(), QStringLiteral("Yes"));
            QCOMPARE(dialog->findChild<QLabel*>("paymasterNode_maxconnections")->text(), QStringLiteral("125"));
            auto* scroll = panel->findChild<QScrollArea*>("paymasterConnectionPage");
            QVERIFY(scroll && scroll->isAncestorOf(dialog));
            const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_NODE_SCREENSHOT");
            if (!screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot + "-" + scenario + ".png"));
            QCOMPARE(previews, 0);
            return;
        }
        QVERIFY(total);
        if (scenario == "override") {
            QVERIFY(!bind);
            auto* fixed = dialog->findChild<QLabel*>("paymasterNode_paymasterbind");
            QVERIFY(fixed && fixed->isEnabled());
            QCOMPARE(fixed->text(), QStringLiteral("127.0.0.1:18450"));
        } else {
            QVERIFY(bind);
            QCOMPARE(bind->toPlainText(), QStringLiteral("127.0.0.1:18450"));
        }
        QVERIFY(!save->isEnabled());
        if (scenario == "override") {
            const QString screenshot = qEnvironmentVariable("DIGIBYTE_PAYMASTER_NODE_SCREENSHOT");
            if (!screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot + "-override.png"));
            QVERIFY(dialog->findChild<QLabel*>("paymasterNodeSourceHelp")->text().contains("BAT/shortcut"));
        }
        if (scenario != "unchanged" && scenario != "override") total->setValue(130);
        preview->click();
        if (scenario == "wallet_change" || scenario == "privacy" || scenario == "close_pending") return;
        if (scenario == "unchanged" || scenario == "override") {
            QCOMPARE(previews, 0);
            QVERIFY(status->text().contains("No settings changed"));
        } else if (scenario == "core_conflict") {
            QVERIFY(status->text().contains("start argument"));
            QVERIFY(!save->isEnabled());
            QCOMPARE(total->value(), 130);
        } else {
            QVERIFY(save->isEnabled());
            if (scenario == "edit_after_preview") {
                total->setValue(140);
                QVERIFY(!save->isEnabled());
            } else {
                save->click();
                QVERIFY(status->text().contains("Configuration saved"));
            }
        }

    });
    interact.start();
    QTimer::singleShot(5000, panel.get(), [&] {
        if (auto* dialog = panel->findChild<QDialog*>("paymasterNodeConnectionDialog")) dialog->reject();
    });
    auto* button = panel->findChild<QPushButton*>("paymasterReviewConnection");
    QVERIFY(button); button->click();
    QTRY_COMPARE(opened, 1);
    QCOMPARE(writes, scenario == "save" ? 1 : 0);
}

void PaymasterWidgetTests::paymasterProviderCommandLocksPages()
{
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    DigiDollarPaymasterWidget::RpcCallback pending;
    QStringList commands;
    panel->setAsyncRpcExecutorForTesting([&](const std::string& command, const UniValue&, DigiDollarPaymasterWidget::RpcCallback callback) {
        QVERIFY(!pending);
        commands << QString::fromStdString(command);
        pending = std::move(callback);
    });
    auto* tabs = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    QVERIFY(tabs);
    const auto assert_locked = [&] {
        QVERIFY(tabs->isEnabled());
        QVERIFY(panel->findChild<QListWidget*>("paymasterNavigation")->isEnabled());
        const auto before = commands;
        for (auto* button : tabs->findChildren<QPushButton*>()) {
            QVERIFY2(!button->isEnabled(), qPrintable(button->objectName()));
            button->click();
        }
        QCOMPARE(commands, before);
    };
    const auto reply = [&](const UniValue& result, const QString& error = {}) {
        QVERIFY(pending);
        auto callback = std::move(pending);
        pending = {};
        callback(result, error);
    };
    panel->refreshStatus();
    assert_locked();
    reply(GuidedOperatorSnapshot());
    QCOMPARE(commands.back(), QStringLiteral("getpaymasterfinancestatus"));
    assert_locked(); // Still locked across the serialized follow-up read.
    reply(PaymasterOverviewFinance());
    QVERIFY(tabs->isEnabled());
    auto* connection = panel->findChild<QPushButton*>("paymasterReviewConnection");
    QVERIFY(connection && connection->isEnabled());

    panel->resize(1100, 800);
    panel->show();
    QCoreApplication::processEvents();

    struct VisibilityProbe : QObject {
        int changes{0};
        int paints{0};
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Hide || event->type() == QEvent::Show ||
                event->type() == QEvent::HideToParent || event->type() == QEvent::ShowToParent) ++changes;
            if (event->type() == QEvent::Paint) ++paints;
            return false;
        }
    } probe;
    auto* budgets = panel->findChild<QWidget*>("paymasterOverviewBudgetDetails");
    auto* pause = panel->findChild<QPushButton*>("paymasterOperatorPause");
    QVERIFY(budgets);
    budgets->installEventFilter(&probe);
    panel->findChild<QLabel*>("paymasterOperatorHeadline")->installEventFilter(&probe);
    if (pause) pause->installEventFilter(&probe);
    panel->refreshStatus();
    assert_locked();
    QCOMPARE(probe.changes, 0); // A valid snapshot must not be hidden and rebuilt.
    QVERIFY(tabs->updatesEnabled());
    // The old paint freeze left a blank page when a slow read overlapped
    // navigation or a window expose. Exercise real paint events before reply.
    auto* navigation = panel->findChild<QListWidget*>("paymasterNavigation");
    navigation->setCurrentRow(2);
    QCoreApplication::processEvents();
    const int previous_paints = probe.paints;
    navigation->setCurrentRow(0);
    panel->resize(1050, 760);
    QCoreApplication::processEvents();
    QVERIFY(probe.paints > previous_paints);
    QVERIFY(tabs->currentWidget()->isVisible());
    QVERIFY(tabs->updatesEnabled());
    probe.changes = 0;
    assert_locked();
    reply(GuidedOperatorSnapshot());
    QCOMPARE(probe.changes, 0);
    assert_locked();
    reply(PaymasterOverviewFinance());
    QCOMPARE(probe.changes, 0);
    QVERIFY(tabs->isEnabled() && tabs->updatesEnabled());
    panel->refreshStatus();
    assert_locked();
    reply(UniValue{}, QStringLiteral("injected status failure"));
    QVERIFY(tabs->isEnabled()); // Errors must not leave the page locked.
    QVERIFY(tabs->updatesEnabled());

    panel->refreshStatus();
    assert_locked();
    auto late = std::move(pending);
    pending = {};
    panel->setWalletModel(nullptr);
    // The fake transport represents a newly selected wallet; nullptr itself
    // deliberately disables the whole widget in production.
    panel->setEnabled(true);
    panel->setAsyncRpcExecutorForTesting([&](const std::string& command, const UniValue&, DigiDollarPaymasterWidget::RpcCallback callback) {
        QVERIFY(!pending);
        commands << QString::fromStdString(command);
        pending = std::move(callback);
    });
    panel->refreshStatus();
    assert_locked();
    late(GuidedOperatorSnapshot(), {});
    assert_locked(); // Previous wallet must not unlock the new wallet's call.
    reply(GuidedOperatorSnapshot());
    assert_locked();
    reply(PaymasterOverviewFinance());
    QVERIFY(tabs->isEnabled());
}

void PaymasterWidgetTests::paymasterPreparationFeeRecovery()
{
    for (const QString outcome : {QStringLiteral("preview"), QStringLiteral("preview_error"), QStringLiteral("decline"), QStringLiteral("signed_race"), QStringLiteral("wallet_change")}) {
        std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
        auto snapshot = GuidedOperatorSnapshot();
        UniValue step;
        QVERIFY(step.read(R"({"plan_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","asset":"dd_carrier","state":"pending_creation","error":"PAYMASTER_POOL_FEE_LIMIT","maximum_fee_satoshis":20000000})"));
        UniValue steps{UniValue::VARR}; steps.push_back(step);
        auto provider = snapshot.find_value("provider");
        provider.pushKV("preparation", steps);
        provider.pushKV("ready", false);
        provider.pushKV("last_service_error", "PAYMASTER_POOL_PREPARATION_PENDING");
        snapshot.pushKV("provider", provider);
        snapshot.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
        int cancels{0}, previews{0}, executions{0}, approvals{0};
        panel->setRpcExecutorForTesting([&](const std::string& command, const UniValue& params) {
            if (command == "getpaymasteroperatorinfo") return snapshot;
            if (command == "getpaymasterfinancestatus") return PaymasterOverviewFinance();
            if (command != "preparepaymasterpool") throw std::runtime_error("Unexpected recovery mutation");
            if (params[0].find_value("cancel").isTrue()) {
                ++cancels;
                if (params[0].find_value("plan_id").get_str() != std::string(64, 'a')) throw std::runtime_error("Wrong cancelled plan");
                auto returned = step;
                returned.pushKV("state", outcome == "signed_race" ? "pending_confirmation" : "cancelled");
                if (outcome == "signed_race") returned.pushKV("txid", std::string(64, 'b'));
                UniValue records{UniValue::VARR}; records.push_back(returned);
                UniValue result{UniValue::VOBJ}; result.pushKV("plan_id", std::string(64, 'a'));
                result.pushKV("cancelled", outcome != "signed_race"); result.pushKV("preparation", records);
                return result;
            }
            if (params[0].find_value("execute").isTrue()) ++executions;
            ++previews;
            if (params[0].find_value("maximum_fee_satoshis").getInt<int64_t>() != 30000000) throw std::runtime_error("Wrong replacement fee");
            if (outcome == "preview_error") throw std::runtime_error("injected preview unavailable");
            UniValue result;
            result.read(R"({"accepted":false,"cancelled":false,"executed":false,"preparation":[],
                "maximum_fee_satoshis":30000000,"maximum_total_fee_satoshis":30000000,
                "admission_dgb_slots":3,"operational_dgb_slots":1,"admission_carrier_slots":3,"operational_carrier_slots":1,
                "missing_admission_dgb_slots":0,"missing_operational_dgb_slots":0,
                "missing_admission_carrier_slots":0,"missing_operational_carrier_slots":1,
                "admission_dgb_satoshis_each":10000000,"operational_dgb_satoshis_each":20000000,"carrier_cents_each":100,
                "total_output_satoshis":0,"total_carrier_cents":100})");
            result.pushKV("plan_id", std::string(64, 'c'));
            return result;
        });
        panel->refreshStatus();
        auto pool = PaymasterLiquidityPoolStatus(); pool.pushKV("preparation", steps);
        panel->setLiquidityPoolForTesting(pool);
        panel->setMutationSnapshotsAvailableForTesting(true, true, true);
        panel->setOperatorStatusForTesting(snapshot);
        auto* next = panel->findChild<QPushButton*>("paymasterOperatorNextAction");
        QVERIFY(next && next->text().contains("Review setup fee"));
        bool opened{false};
        QTimer answer;
        connect(&answer, &QTimer::timeout, panel.get(), [&] {
            if (auto* review = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                ++approvals;
                review->done(QMessageBox::No);
                return;
            }
            auto* dialog = panel->findChild<QDialog*>("paymasterReviewPreparationFee");
            if (!dialog || !dialog->isVisible()) return;
            opened = true;
            auto* fee = dialog->findChild<QLineEdit*>("paymasterReplacementSetupFee");
            QVERIFY(fee && fee->text() == "0.20000000");
            fee->setText("0.30000000");
            if (outcome == "wallet_change") panel->setWalletModel(nullptr);
            outcome == "decline" ? dialog->reject() : dialog->accept();
        });
        answer.start(5);
        next->click();
        QVERIFY(opened);
        QTest::qWait(30);
        QCOMPARE(cancels, outcome == "preview" || outcome == "preview_error" || outcome == "signed_race" ? 1 : 0);
        QCOMPARE(previews, outcome == "preview" || outcome == "preview_error" ? 1 : 0);
        QCOMPARE(approvals, outcome == "preview" ? 1 : 0);
        QCOMPARE(executions, 0);
    }
}

namespace {
UniValue PaymasterStartWithoutReserves()
{
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    provider.pushKV("start_requested", true);
    provider.pushKV("ready", false);
    provider.pushKV("pool_ready", false);
    provider.pushKV("service_state", "waiting_for_readiness");
    provider.pushKV("active_operations", UniValue{UniValue::VARR});
    provider.pushKV("preparation", UniValue{UniValue::VARR});
    auto pool = provider.find_value("pool");
    pool.pushKV("admission_carriers", 0);
    pool.pushKV("operational_carriers", 0);
    pool.pushKV("complete_operational_slots", 0);
    provider.pushKV("pool", pool);
    auto liquidity = provider.find_value("liquidity");
    auto policy = liquidity.find_value("policy");
    policy.pushKV("automatic_replenishment", false);
    policy.pushKV("paid_maintenance_approved", false);
    liquidity.pushKV("policy", policy);
    liquidity.pushKV("maintenance_state", "waiting_for_maintenance_approval");
    liquidity.pushKV("admission_carriers", PaymasterLiquiditySlotStatus(0, 0, 3, 3));
    liquidity.pushKV("operational_carriers", PaymasterLiquiditySlotStatus(0, 0, 1, 1));
    provider.pushKV("liquidity", liquidity);
    UniValue errors{UniValue::VARR};
    errors.push_back("PAYMASTER_ADMISSION_CARRIERS_MISSING");
    errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    provider.pushKV("readiness_errors", errors);
    snapshot.pushKV("provider", provider);
    snapshot.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
    return snapshot;
}
}

void PaymasterWidgetTests::paymasterStartIntentNeedsReserveApproval()
{
    using Phase = PaymasterOperationController::Phase;
    auto snapshot = PaymasterStartWithoutReserves();
    const auto observe = [](const UniValue& current) {
        PaymasterOperationController controller;
        controller.reset(1);
        controller.observe(current, 1, 100);
        return controller;
    };
    auto controller = observe(snapshot);
    QCOMPARE(controller.phase, Phase::Blocked);
    QVERIFY(controller.startNeedsReserveApproval());
    QCOMPARE(controller.error, QStringLiteral("PAYMASTER_POOL_PREPARATION_REQUIRED"));
    QVERIFY(controller.begin(QStringLiteral("Restore and start"), 1));
    QVERIFY(!controller.begin(QStringLiteral("Duplicate"), 1));
    for (const auto* key : {"pending", "reserved"}) {
        auto current = snapshot;
        auto provider = current.find_value("provider");
        auto liquidity = provider.find_value("liquidity");
        auto counts = liquidity.find_value("operational_carriers");
        counts.pushKV(key, 1);
        liquidity.pushKV("operational_carriers", counts);
        provider.pushKV("liquidity", liquidity);
        current.pushKV("provider", provider);
        auto waiting = observe(current);
        QVERIFY(!waiting.startNeedsReserveApproval());
        QCOMPARE(waiting.phase, Phase::Waiting);
        QVERIFY(!waiting.begin(QStringLiteral("Duplicate"), 1));
    }
    for (const auto* state : {"pending_creation", "pending_confirmation", "conflict", "unknown"}) {
        auto current = snapshot;
        auto provider = current.find_value("provider");
        UniValue step{UniValue::VOBJ}, work{UniValue::VARR};
        step.pushKV("state", state); work.push_back(step);
        // Either journal alone is sufficient to prevent a new preparation.
        for (const auto* journal : {"preparation", "active_operations"}) {
            auto working = provider; working.pushKV(journal, work);
            current.pushKV("provider", working);
            QVERIFY(!observe(current).startNeedsReserveApproval());
        }
    }
    for (const auto* guard : {"enabled", "wallet_locked"}) {
        auto current = snapshot;
        auto provider = current.find_value("provider");
        provider.pushKV(guard, std::string{guard} == "wallet_locked");
        current.pushKV("provider", provider);
        QCOMPARE(observe(current).error, std::string{guard} == "enabled"
            ? QStringLiteral("PAYMASTER_PROVIDER_DISABLED") : QStringLiteral("PAYMASTER_WALLET_LOCKED"));
    }
    auto provider = snapshot.find_value("provider");
    provider.pushKV("liquidity", UniValue{UniValue::VOBJ});
    snapshot.pushKV("provider", provider);
    QVERIFY(!observe(snapshot).startNeedsReserveApproval()); // Incomplete data cannot authorize a new task.
}

void PaymasterWidgetTests::paymasterPreparationContinuesAfterRefresh_data()
{
    QTest::addColumn<QString>("scenario");
    for (const char* name : {"fee_recovery", "start_only", "decline", "wallet_change", "privacy"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void PaymasterWidgetTests::paymasterPreparationContinuesAfterRefresh()
{
    QFETCH(QString, scenario);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto snapshot = PaymasterStartWithoutReserves();
    auto provider = snapshot.find_value("provider");
    UniValue step;
    QVERIFY(step.read(R"({"plan_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "kind":"preparation","asset":"dd_carrier","state":"pending_creation","error":"PAYMASTER_POOL_FEE_LIMIT",
        "maximum_fee_satoshis":20000000,"diagnostic":"estimated fee 0.3703 DGB exceeds approved setup limit 0.20 DGB"})"));
    UniValue steps{UniValue::VARR}; steps.push_back(step);
    if (scenario != "start_only") {
        provider.pushKV("preparation", steps);
        provider.pushKV("active_operations", steps);
        snapshot.pushKV("provider", provider);
        snapshot.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
    }
    DigiDollarPaymasterWidget::RpcCallback pending;
    std::string command;
    UniValue params;
    int cancellations{0}, previews{0}, executions{0}, starts{0}, approvals{0};
    panel->setAsyncRpcExecutorForTesting([&](const std::string& name, const UniValue& args, DigiDollarPaymasterWidget::RpcCallback callback) {
        QVERIFY(!pending);
        command = name; params = args; pending = std::move(callback);
        if (name == "preparepaymasterpool") {
            if (args[0].find_value("cancel").isTrue()) ++cancellations;
            else if (args[0].find_value("execute").isTrue()) ++executions;
            else ++previews;
        } else if (name == "startpaymaster") ++starts;
        else QVERIFY(name == "getpaymasteroperatorinfo" || name == "getpaymasterfinancestatus");
    });
    const auto reply = [&](const UniValue& value) {
        QVERIFY(pending);
        auto callback = std::move(pending); pending = {};
        callback(value, {});
    };
    const auto finish_refresh = [&] {
        QCOMPARE(command, std::string{"getpaymasteroperatorinfo"});
        reply(snapshot);
        if (pending) {
            QCOMPARE(command, std::string{"getpaymasterfinancestatus"});
            reply(PaymasterOverviewFinance());
        }
    };
    panel->refreshStatus(); finish_refresh();
    auto* next = panel->findChild<QPushButton*>("paymasterOperatorNextAction");
    auto* status = panel->findChild<QLabel*>("paymasterTaskStatus");
    auto* progress = panel->findChild<QProgressBar*>("paymasterTaskProgress");
    QVERIFY(next && next->isEnabled() && status && progress);
    if (scenario == "start_only") {
        QVERIFY(status->text().contains("No reserve transaction is pending"));
        QVERIFY(progress->isHidden());
        QVERIFY(next->text().contains("Restore reserves and start"));
    } else {
        QVERIFY(next->text().contains("Review setup fee"));
        QTimer::singleShot(0, panel.get(), [&] {
            auto* dialog = panel->findChild<QDialog*>("paymasterReviewPreparationFee");
            QVERIFY(dialog);
            auto* fee = dialog->findChild<QLineEdit*>("paymasterReplacementSetupFee");
            QVERIFY(fee); fee->setText("0.50000000");
            QVERIFY(dialog->findChild<QCheckBox*>()->isChecked());
            dialog->accept();
        });
    }
    next->click();
    QCOMPARE(command, std::string{"preparepaymasterpool"});
    if (scenario != "start_only") {
        QVERIFY(params[0].find_value("cancel").isTrue());
        QCOMPARE(params[0].find_value("plan_id").get_str(), std::string(64, 'a'));
        UniValue cancelled{UniValue::VOBJ};
        cancelled.pushKV("plan_id", std::string(64, 'a'));
        cancelled.pushKV("cancelled", true);
        cancelled.pushKV("preparation", UniValue{UniValue::VARR});
        provider.pushKV("preparation", UniValue{UniValue::VARR});
        provider.pushKV("active_operations", UniValue{UniValue::VARR});
        snapshot.pushKV("provider", provider);
        snapshot.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
        reply(cancelled);
        // The next preview must already be in the same serialized RPC chain.
        QVERIFY(pending);
        QCOMPARE(command, std::string{"preparepaymasterpool"});
    }
    QCOMPARE(previews, 1);
    QVERIFY(!params[0].find_value("execute").isTrue());
    const int64_t ceiling{50000000};
    QCOMPARE(params[0].find_value("maximum_fee_satoshis").getInt<int64_t>(), ceiling);
    UniValue preview;
    QVERIFY(preview.read(R"({"accepted":false,"cancelled":false,"executed":false,"preparation":[],
        "admission_dgb_slots":3,"operational_dgb_slots":1,"admission_carrier_slots":3,"operational_carrier_slots":1,
        "missing_admission_dgb_slots":0,"missing_operational_dgb_slots":0,
        "missing_admission_carrier_slots":3,"missing_operational_carrier_slots":1,
        "admission_dgb_satoshis_each":10000000,"operational_dgb_satoshis_each":20000000,"carrier_cents_each":100,
        "total_output_satoshis":0,"total_carrier_cents":400})"));
    preview.pushKV("plan_id", std::string(64, 'c'));
    preview.pushKV("maximum_fee_satoshis", ceiling);
    preview.pushKV("maximum_total_fee_satoshis", ceiling);
    reply(preview);
    // Beat the queued approval dialog with a real delayed status read.
    panel->refreshStatus();
    QVERIFY(pending);
    QTest::qWait(30);
    QVERIFY(!QApplication::activeModalWidget());
    QCOMPARE(executions, 0);
    if (scenario == "wallet_change" || scenario == "privacy") {
        scenario == "privacy" ? panel->setPrivacy(true) : panel->setWalletModel(nullptr);
        reply(snapshot);
        QTest::qWait(30);
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(executions, 0); QCOMPARE(starts, 0);
        return;
    }
    finish_refresh();
    QTimer answer;
    answer.setInterval(5);
    connect(&answer, &QTimer::timeout, panel.get(), [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog) return;
        ++approvals;
        QVERIFY(dialog->text().contains("4.00 DD"));
        QVERIFY(dialog->text().contains("Start the provider automatically"));
        dialog->done(scenario == "decline" ? QMessageBox::No : QMessageBox::Yes);
        answer.stop();
    });
    answer.start();
    QTRY_COMPARE(approvals, 1);
    if (scenario == "decline") {
        QCOMPARE(executions, 0); QCOMPARE(starts, 0);
        panel->refreshStatus(); finish_refresh();
        QVERIFY(status->text().contains("No reserve transaction is pending"));
        QVERIFY(progress->isHidden());
        return;
    }
    QCOMPARE(executions, 1);
    QCOMPARE(command, std::string{"preparepaymasterpool"});
    QVERIFY(params[0].find_value("execute").isTrue());
    QCOMPARE(params[0].find_value("plan_id").get_str(), std::string(64, 'c'));
    QCOMPARE(params[0].find_value("maximum_fee_satoshis").getInt<int64_t>(), ceiling);
    next->click(); QCOMPARE(executions, 1);
    auto accepted = preview; accepted.pushKV("accepted", true);
    reply(accepted);
    QCOMPARE(command, std::string{"startpaymaster"});
    QVERIFY(params[0].find_value("wait_for_readiness").isTrue());
    UniValue start{UniValue::VOBJ}; start.pushKV("start_requested", true);
    reply(start);
    step.pushKV("plan_id", std::string(64, 'c'));
    step.pushKV("txid", std::string(64, 'd'));
    step.pushKV("state", "pending_confirmation"); step.pushKV("error", "");
    step.pushKV("maximum_fee_satoshis", ceiling);
    step.pushKV("confirmations", 0); step.pushKV("required_confirmations", 1);
    steps.clear(); steps.setArray(); steps.push_back(step);
    provider.pushKV("active_operations", steps); provider.pushKV("preparation", steps);
    auto liquidity = provider.find_value("liquidity");
    const auto saved_policy = liquidity.find_value("policy");
    liquidity.pushKV("maintenance_state", "waiting_for_liquidity_confirmation");
    liquidity.pushKV("admission_carriers", PaymasterLiquiditySlotStatus(0, 3, 0, 3));
    liquidity.pushKV("operational_carriers", PaymasterLiquiditySlotStatus(0, 1, 0, 1));
    provider.pushKV("liquidity", liquidity);
    snapshot.pushKV("provider", provider);
    snapshot.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
    finish_refresh();
    QVERIFY(status->text().contains("0/1"));
    QVERIFY(!progress->isHidden());
    provider.pushKV("pool_ready", true); provider.pushKV("ready", true);
    provider.pushKV("running", true); provider.pushKV("start_requested", false);
    provider.pushKV("service_state", "active");
    provider.pushKV("active_operations", UniValue{UniValue::VARR});
    provider.pushKV("preparation", UniValue{UniValue::VARR});
    provider.pushKV("readiness_errors", UniValue{UniValue::VARR});
    const auto ready_provider = GuidedOperatorSnapshot().find_value("provider");
    provider.pushKV("pool", ready_provider.find_value("pool"));
    liquidity = ready_provider.find_value("liquidity");
    liquidity.pushKV("policy", saved_policy);
    provider.pushKV("liquidity", liquidity);
    snapshot.pushKV("provider", provider);
    snapshot.pushKV("diagnostics", DigiDollar::Paymaster::OperatorDiagnostics(provider, 0, 100));
    panel->refreshStatus(); finish_refresh();
    QVERIFY(status->text().contains("5/5"));
    QVERIFY(progress->isHidden());
    QCOMPARE(cancellations, scenario == "start_only" ? 0 : 1);
    QCOMPARE(previews, 1); QCOMPARE(executions, 1); QCOMPARE(starts, 1);
}

void PaymasterWidgetTests::paymasterReservePresets_data()
{
    QTest::addColumn<QString>("theme");
    QTest::newRow("dark") << QStringLiteral("dark");
    QTest::newRow("light") << QStringLiteral("light");
}

void PaymasterWidgetTests::paymasterReserveReduction_data()
{
    QTest::addColumn<QString>("theme");
    QTest::addColumn<QString>("decision");
    for (const auto* theme : {"dark", "light"}) {
        for (const auto* decision : {"cancel", "approve", "blocked", "changed", "bad_receipt", "lost_reply", "wallet_change", "privacy",
                                    "increased_fee", "bad_fee", "fee_limit", "wallet_limit", "busy", "busy_preview", "busy_timeout", "busy_changed", "busy_wallet", "busy_privacy"})
            QTest::newRow(qPrintable(QString::fromLatin1(theme) + '-' + decision)) << QString::fromLatin1(theme) << QString::fromLatin1(decision);
    }
}

void PaymasterWidgetTests::paymasterReserveReduction()
{
    QFETCH(QString, theme);
    QFETCH(QString, decision);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    panel->setObjectName("paymasterWidget");
    QFile css(":/css/" + theme);
    QVERIFY(css.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    auto liquidity = provider.find_value("liquidity");
    auto saved = liquidity.find_value("policy");
    saved.pushKV("paid_maintenance_approved", false);
    saved.pushKV("updated_at", 100);
    int available{3};
    const auto update = [&](const UniValue& policy) {
        liquidity.pushKV("policy", policy);
        liquidity.pushKV("policy_configured", true);
        for (const auto& pair : {std::pair{"admission_dgb", "target_admission_dgb"}, {"operational_dgb", "target_operational_dgb"},
                                {"admission_carriers", "target_admission_carriers"}, {"operational_carriers", "target_operational_carriers"}}) {
            auto slot = liquidity.find_value(pair.first);
            const int ready = QString::fromLatin1(pair.first).startsWith("admission") ? available * 3 : available;
            slot.pushKV("ready", ready);
            slot.pushKV("target", policy.find_value(pair.second));
            slot.pushKV("counted_toward_target", ready);
            liquidity.pushKV(pair.first, slot);
        }
        auto pool = provider.find_value("pool");
        pool.pushKV("complete_operational_slots", available);
        pool.pushKV("admission_dgb", available * 3); pool.pushKV("admission_carriers", available * 3);
        pool.pushKV("operational_dgb", available); pool.pushKV("operational_carriers", available);
        provider.pushKV("pool", pool);
        provider.pushKV("liquidity", liquidity);
        snapshot.pushKV("provider", provider);
    };
    for (const auto* key : {"target_admission_dgb", "target_admission_carriers"}) saved.pushKV(key, 9);
    for (const auto* key : {"target_operational_dgb", "target_operational_carriers"}) saved.pushKV(key, 3);
    update(saved);
    int saves{0}, previews{0}, executions{0};
    bool exact_binding{true};
    panel->setRpcExecutorForTesting([&](const std::string& method, const UniValue& params) {
        if (method == "getpaymasteroperatorinfo") return snapshot;
        if (method == "getpaymasterliquiditystatus") return liquidity;
        if (method == "getpaymasterfinancestatus") {
            auto finance = PaymasterOverviewFinance();
            auto capital = finance.find_value("pool_capital");
            capital.pushKV("dgb_available_satoshis", available == 3 ? 150000000 : 50000000);
            capital.pushKV("carrier_base_cents", available == 3 ? 1200 : 400);
            capital.pushKV("dgb_reserved_satoshis", 0); capital.pushKV("dgb_pending_satoshis", 0);
            capital.pushKV("pending_maintenance_transactions", 0);
            finance.pushKV("pool_capital", capital);
            return finance;
        }
        if (method == "setpaymasterliquiditypolicy") {
            ++saves;
            saved = params[0]; saved.pushKV("updated_at", 101); update(saved);
            return saved;
        }
        if (method == "rebalancepaymasterpool") {
            const auto& options = params[0];
            const bool execute = options.find_value("execute").isTrue();
            execute ? ++executions : ++previews;
            const int64_t proposed_fee = decision == "increased_fee" || (decision == "fee_limit" && previews > 1) ? 75000000 : 50000000;
            exact_binding &= options.find_value("expected_liquidity_updated_at").getInt<int>() == 101 &&
                options.find_value("admission_dgb_slots").getInt<int>() == 3 && options.find_value("operational_dgb_slots").getInt<int>() == 1 &&
                options.find_value("admission_carrier_slots").getInt<int>() == 3 && options.find_value("operational_carrier_slots").getInt<int>() == 1 &&
                options.find_value("dgb_only").isFalse() && options.find_value("maximum_fee_satoshis").getInt<int64_t>() == (execute ? proposed_fee : 50000000) &&
                options.find_value("recommend_fee").isTrue() == !execute;
            if (decision == "blocked") throw std::runtime_error("PAYMASTER_REBALANCE_ACTIVE_RESERVATIONS");
            if (execute && decision == "changed") throw std::runtime_error("PAYMASTER_LIQUIDITY_POLICY_CHANGED");
            if (!execute && decision == "busy_preview" && previews == 1) throw std::runtime_error("PAYMASTER_PROVIDER_BUSY");
            if (execute && decision.startsWith("busy") && decision != "busy_preview" && (executions == 1 || decision == "busy_timeout")) {
                if (decision == "busy_wallet") QTimer::singleShot(50, panel.get(), [&] { panel->setWalletModel(nullptr); });
                if (decision == "busy_privacy") QTimer::singleShot(50, panel.get(), [&] { panel->setPrivacy(true); });
                throw std::runtime_error("PAYMASTER_PROVIDER_BUSY");
            }
            if (execute && decision == "busy_changed") throw std::runtime_error("PAYMASTER_POOL_PLAN_CHANGED");
            if (execute && decision == "fee_limit" && executions == 1) throw std::runtime_error("PAYMASTER_RETIREMENT_FEE_LIMIT");
            if (execute && decision == "wallet_limit") throw std::runtime_error("PAYMASTER_RETIREMENT_WALLET_FEE_LIMIT");
            UniValue result;
            result.read(R"({"executed":false,"retired_admission_dgb_slots":6,"retired_operational_dgb_slots":2,
                "retired_admission_carrier_slots":6,"retired_operational_carrier_slots":2,"retired_dgb_satoshis":100000000,
                "retired_carrier_cents":800,"maximum_network_fee_satoshis":50000000,"maximum_total_fee_satoshis":100000000})");
            result.pushKV("plan_id", std::string(64, 'c'));
            result.pushKV("maximum_network_fee_satoshis", proposed_fee);
            result.pushKV("maximum_total_fee_satoshis", 2 * proposed_fee);
            result.pushKV("estimated_dgb_fee_satoshis", 100000);
            result.pushKV("estimated_dd_fee_satoshis", decision == "bad_fee" ? 90000000 : proposed_fee == 75000000 ? 60000000 : 12000000);
            result.pushKV("wallet_maximum_fee_satoshis", 1000000000);
            if (execute) {
                if (options.find_value("plan_id").get_str() != std::string(64, 'c')) throw std::runtime_error("Wrong release plan");
                available = 1; update(saved);
                if (decision == "lost_reply") throw std::runtime_error("Lost response after execution");
                result.pushKV("executed", true);
                result.pushKV("dd_txid", std::string(64, 'd')); result.pushKV("dgb_txid", std::string(64, 'e'));
                result.pushKV("dd_network_fee_satoshis", 12000000); result.pushKV("network_fee_satoshis", 100000);
                if (decision == "bad_receipt") result.pushKV("plan_id", std::string(64, 'f'));
            }
            return result;
        }
        throw std::runtime_error("Unexpected reserve reduction RPC");
    });
    panel->refreshStatus();
    auto* preset = panel->findChild<QComboBox*>("paymasterReservePreset");
    auto* save = panel->findChild<QPushButton*>("paymasterSaveLiquidityPolicyPrimary");
    auto* state = panel->findChild<QLabel*>("paymasterReserveState");
    auto* overview_target = panel->findChild<QLabel*>("paymasterCapital_refill_target");
    auto* overview_available = panel->findChild<QLabel*>("paymasterCapital_complete_slots");
    auto* release = panel->findChild<QPushButton*>("paymasterReviewExcessReserves");
    QVERIFY(preset && save && state && overview_target && overview_available && release);
    auto* pages = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    pages->setCurrentWidget(panel->findChild<QWidget*>("paymasterLiquidityPage"));
    panel->resize(1000, 800);
    panel->show();
    preset->setCurrentIndex(preset->findData(1));
    bool reviewed{false};
    int approvals{0};
    QTimer answer;
    connect(&answer, &QTimer::timeout, panel.get(), [&] {
        if (auto* review = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            ++approvals;
            const bool higher = decision == "increased_fee" || (decision == "fee_limit" && approvals > 1);
            reviewed = review->text().contains("8.00 DD") && review->text().contains(higher ? "1.50000000 DGB total" : "1.00000000 DGB total") &&
                review->text().contains("Estimated fees:") && review->text().contains("one-time release fees") &&
                review->text().contains("Cancel keeps the smaller target");
            if (const auto path = qEnvironmentVariable("DIGIBYTE_PAYMASTER_RELEASE_REVIEW_SCREENSHOT"); !path.isEmpty() && decision == "increased_fee")
                review->grab().save(path + "-" + theme + ".png");
            if (decision == "wallet_change") panel->setWalletModel(nullptr);
            if (decision == "privacy") panel->setPrivacy(true);
            review->done(decision == "cancel" ? QMessageBox::No : QMessageBox::Yes);
        }
    });
    answer.start(10);
    save->click();
    QTRY_COMPARE(previews, decision == "busy_preview" ? 2 : 1);
    if (decision != "blocked" && decision != "bad_fee") QTRY_VERIFY(reviewed);
    if (decision == "busy" || decision == "busy_changed") QTRY_COMPARE(executions, 2);
    if (decision == "busy_timeout") QTRY_COMPARE(executions, 4);
    if (decision == "busy_wallet" || decision == "busy_privacy") QTest::qWait(900);
    QCoreApplication::processEvents();
    answer.stop();
    QCOMPARE(saves, 1);
    QVERIFY(exact_binding);
    const bool executed = decision != "cancel" && decision != "blocked" && decision != "bad_fee" && decision != "wallet_change" && decision != "privacy";
    QCOMPARE(executions, decision == "busy_timeout" ? 4 : (decision == "busy" || decision == "busy_changed") ? 2 : executed ? 1 : 0);
    QCOMPARE(approvals, decision == "blocked" || decision == "bad_fee" ? 0 : 1);
    if (decision != "wallet_change" && decision != "privacy" && decision != "busy_wallet" && decision != "busy_privacy") {
        QCOMPARE(preset->currentData().toInt(), 1);
        QVERIFY(overview_target->text().contains("1 payment(s)"));
        QVERIFY(state->text().contains("Saved refill target: 1 payment(s)"));
        QVERIFY(overview_available->text().contains(QString::number(available)));
        QCOMPARE(release->isHidden(), available == 1);
        if (available == 3) QVERIFY(state->text().contains("Extra reserves remain"));
    }
    if (decision == "fee_limit" || decision == "busy_timeout" || decision == "busy_changed") {
        auto* status = panel->findChild<QLabel*>("paymasterTaskStatus");
        auto* next = panel->findChild<QPushButton*>("paymasterContinueTask");
        QVERIFY(status && next);
        QVERIFY(status->text().contains("No reserves were released by this attempt"));
        QVERIFY(next->text().contains("Recalculate fees"));
        if (decision == "fee_limit") {
            answer.start(10);
            next->click();
            QTRY_COMPARE(executions, 2);
            answer.stop();
            QVERIFY(reviewed);
            QCOMPARE(approvals, 2);
            QCOMPARE(previews, 2);
            QCOMPARE(saves, 1);
            QVERIFY(exact_binding);
        }
    }
    if (decision == "wallet_limit") {
        auto* status = panel->findChild<QLabel*>("paymasterTaskStatus");
        auto* next = panel->findChild<QPushButton*>("paymasterContinueTask");
        QVERIFY(status && next);
        QVERIFY(status->text().contains("wallet-wide transaction limit"));
        QCOMPARE(next->text(), QStringLiteral("Check current status"));
        next->click();
        QCOMPARE(executions, 1);
        QCOMPARE(saves, 1);
    }
    if (decision == "cancel") {
        if (const auto path = qEnvironmentVariable("DIGIBYTE_PAYMASTER_REDUCTION_SCREENSHOT"); !path.isEmpty()) {
            panel->findChild<QGroupBox*>("paymasterAutomaticLiquidityPolicy")->grab().save(path + "-funds-" + theme + ".png");
            pages->setCurrentWidget(panel->findChild<QWidget*>("paymasterOverviewPage"));
            panel->findChild<QWidget*>("paymasterOverviewLiquidityCard")->grab().save(path + "-overview-" + theme + ".png");
        }
        answer.start(10);
        release->click();
        QTRY_COMPARE(previews, 2);
        QCoreApplication::processEvents();
        answer.stop();
        QCOMPARE(executions, 0);
        QCOMPARE(saves, 1); // A later release review does not save any settings.
    }
}

void PaymasterWidgetTests::paymasterReservePresets()
{
    QFETCH(QString, theme);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    panel->setObjectName(QStringLiteral("paymasterWidget"));
    QFile css(":/css/" + theme);
    QVERIFY(css.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    auto snapshot = GuidedOperatorSnapshot();
    auto provider = snapshot.find_value("provider");
    auto liquidity = provider.find_value("liquidity");
    auto saved = liquidity.find_value("policy");
    saved.pushKV("target_admission_dgb", 9);
    saved.pushKV("target_operational_dgb", 3);
    saved.pushKV("target_admission_carriers", 9);
    saved.pushKV("target_operational_carriers", 3);
    saved.pushKV("paid_maintenance_approved", true);
    saved.pushKV("maximum_maintenance_fee_per_transaction_satoshis", 12345678);
    saved.pushKV("updated_at", 100);
    liquidity.pushKV("policy", saved);
    liquidity.pushKV("policy_configured", true);
    provider.pushKV("liquidity", liquidity);
    snapshot.pushKV("provider", provider);
    int writes{0};
    panel->setRpcExecutorForTesting([&](const std::string& method, const UniValue&) {
        if (method == "getpaymasteroperatorinfo") return snapshot;
        if (method == "getpaymasterfinancestatus") return PaymasterOverviewFinance();
        ++writes;
        return UniValue{};
    });
    panel->refreshStatus();
    auto* pages = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    auto* capital = panel->findChild<QWidget*>("paymasterLiquidityPage");
    auto* preset = panel->findChild<QComboBox*>("paymasterReservePreset");
    auto* manual = panel->findChild<QWidget*>("paymasterManualReserveTargets");
    auto* summary = panel->findChild<QLabel*>("paymasterLiquidityTargetSummary");
    auto* suggestion = panel->findChild<QLabel*>("paymasterRefillSuggestion");
    auto* admission = panel->findChild<QSpinBox*>("paymasterAdmissionDgbSlots");
    auto* operational = panel->findChild<QSpinBox*>("paymasterOperationalDgbSlots");
    auto* carriers = panel->findChild<QSpinBox*>("paymasterOperationalCarrierSlots");
    auto* fee = panel->findChild<QLineEdit*>("paymasterMaintenanceFeePerTransaction");
    auto* hour = panel->findChild<QLineEdit*>("paymasterMaintenanceFeePerHour");
    auto* day = panel->findChild<QLineEdit*>("paymasterMaintenanceFeePerDay");
    auto* approved = panel->findChild<QCheckBox*>("paymasterPaidMaintenanceApproved");
    auto* discard = panel->findChild<QPushButton*>("paymasterDiscardLiquidity");
    auto* save = panel->findChild<QPushButton*>("paymasterSaveLiquidityPolicyPrimary");
    QVERIFY(pages && capital && preset && manual && summary && suggestion && admission && operational && carriers && fee && hour && day && approved && discard && save);
    pages->setCurrentWidget(capital);
    panel->resize(560, 640);
    panel->show();
    QCOMPARE(preset->currentData().toInt(), 0);
    QVERIFY(manual->isVisibleTo(capital));
    QCOMPARE(fee->text(), QStringLiteral("0.12345678"));
    QVERIFY(suggestion->text().contains("below this suggestion"));
    preset->setCurrentIndex(preset->findData(3));
    QCOMPARE(admission->value(), 3);
    QCOMPARE(operational->value(), 3);
    QCOMPARE(carriers->value(), 3);
    QVERIFY(!manual->isVisibleTo(capital));
    QVERIFY(summary->text().contains("3 payment(s)"));
    QVERIFY(summary->text().contains("6.00 DD"));
    QCOMPARE(fee->text(), QStringLiteral("0.75000000"));
    QCOMPARE(hour->text(), QStringLiteral("3.00000000"));
    QCOMPARE(day->text(), QStringLiteral("15.00000000"));
    QCoreApplication::processEvents();
    if (const auto path = qEnvironmentVariable("DIGIBYTE_PAYMASTER_RESERVE_SCREENSHOT"); !path.isEmpty())
        panel->findChild<QGroupBox*>("paymasterAutomaticLiquidityPolicy")->grab().save(path + "-" + theme + ".png");
    QVERIFY(approved->isChecked()); // A proposal preserves the saved choice.
    QCOMPARE(writes, 0);
    panel->refreshStatus();
    QCOMPARE(preset->currentData().toInt(), 3);
    QCOMPARE(fee->text(), QStringLiteral("0.75000000"));
    auto* offer_fee = panel->findChild<QSpinBox*>("paymasterPolicyMaximumNetworkFee");
    auto* offer_user_paid = panel->findChild<QCheckBox*>("paymasterPolicyUserPaid");
    QVERIFY(offer_fee && offer_user_paid);
    offer_fee->setValue(COIN);
    offer_user_paid->setChecked(false);
    // Independent offer drafts must not change the reserve preset, its capital
    // review, or the funding model used by a reserve-only approval.
    QCOMPARE(preset->currentData().toInt(), 3);
    QVERIFY(summary->text().contains("0.9 DGB + 6.00 DD"));
    bool review_seen{false};
    QTimer::singleShot(0, [&] {
        if (auto* review = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            review_seen = review->text().contains("0.75") && review->text().contains("15") && review->text().contains("0.90000000");
            review->done(QMessageBox::No);
        }
    });
    save->click();
    QVERIFY(review_seen);
    QCOMPARE(writes, 0); // Cancelling a higher cost approval never saves.
    discard->click();
    QCOMPARE(preset->currentData().toInt(), 0);
    QCOMPARE(admission->value(), 9);
    QCOMPARE(fee->text(), QStringLiteral("0.12345678"));
    preset->setCurrentIndex(preset->findData(6));
    QCOMPARE(carriers->value(), 6);
    QCOMPARE(fee->text(), QStringLiteral("1.00000000"));
    QCOMPARE(hour->text(), QStringLiteral("4.00000000"));
    QCOMPARE(day->text(), QStringLiteral("20.00000000"));
    preset->setCurrentIndex(preset->findData(0));
    QVERIFY(manual->isVisibleTo(capital));
    carriers->setValue(2);
    QVERIFY(summary->text().contains("2 payment(s)"));
    QCOMPARE(operational->value(), 6); // Manual pairs are never coerced.
    QCOMPARE(preset->currentData().toInt(), 0);
    QTest::keyClick(preset, Qt::Key_Home);
    QCOMPARE(preset->currentData().toInt(), 1);
    QCOMPARE(operational->value(), 1);
    QCOMPARE(fee->text(), QStringLiteral("0.50000000"));
    QVERIFY(!manual->isVisibleTo(capital));
    QCOMPARE(writes, 0);
}

void PaymasterWidgetTests::paymasterRefillPresetsCoverCarrierFees()
{
    BasicTestingSetup test{ChainType::REGTEST};
    DigiDollarWallet calculator;
    CKey key;
    key.MakeNewKey(true);
    const auto script = DigiDollar::CreateDigiDollarP2TR(XOnlyPubKey(key.GetPubKey()), 100);
    for (int capacity : {1, 3, 6}) {
        // Price the actual DD output format with 1-DD input fragmentation,
        // DD/DGB change, metadata and one fee input. This is read-only sizing,
        // not signing or authorizing a transaction.
        const int carriers = 3 + capacity;
        CMutableTransaction transaction;
        transaction.SetDigiDollarType(DD_TX_TRANSFER);
        transaction.vin.resize(carriers + 2);
        for (int i = 0; i < carriers + 1; ++i)
            transaction.vout.emplace_back(1000000, script);
        transaction.vout.emplace_back(COIN, CScript() << OP_1 << std::vector<unsigned char>(32, 1));
        CScript metadata = CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(2);
        for (int i = 0; i < carriers + 1; ++i)
            metadata << CScriptNum(100);
        transaction.vout.emplace_back(0, metadata);
        const auto proposed = DigiDollar::Paymaster::SetupLiquidityPreset(true, capacity);
        const auto limit = proposed.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>();
        QVERIFY(calculator.CalculateTransactionFee(transaction) > 10000000);
        QVERIFY(calculator.CalculateTransactionFee(transaction) < limit);
        transaction.vin.resize(64);
        QVERIFY(calculator.CalculateTransactionFee(transaction) > limit);
        // Exceptional fragmentation cannot raise the finite proposal.
        QCOMPARE(proposed.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>(), limit);
    }
}

void PaymasterWidgetTests::paymasterLiquiditySaveAcrossRefresh_data()
{
    QTest::addColumn<QString>("scenario");
    for (const char* name : {"approve", "unapproved", "decline", "wallet_change", "privacy",
                             "lost_reply", "malformed_reply", "write_error", "read_error"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void PaymasterWidgetTests::paymasterLiquiditySaveAcrossRefresh()
{
    QFETCH(QString, scenario);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto snapshot = PaymasterStartWithoutReserves();
    auto provider = snapshot.find_value("provider");
    auto liquidity = provider.find_value("liquidity");
    auto saved = liquidity.find_value("policy");
    saved.pushKV("updated_at", 100);
    liquidity.pushKV("policy", saved);
    provider.pushKV("liquidity", liquidity);
    snapshot.pushKV("provider", provider);
    DigiDollarPaymasterWidget::RpcCallback pending;
    std::string command;
    UniValue params;
    int writes{0}, reads{0};
    panel->setAsyncRpcExecutorForTesting([&](const std::string& name, const UniValue& args, DigiDollarPaymasterWidget::RpcCallback callback) {
        QVERIFY(!pending);
        command = name; params = args; pending = std::move(callback);
        if (name == "setpaymasterliquiditypolicy") ++writes;
        else {
            QVERIFY(name == "getpaymasteroperatorinfo" || name == "getpaymasterfinancestatus" || name == "getpaymasterliquiditystatus");
            ++reads;
        }
    });
    const auto reply = [&](const UniValue& value, const QString& error = {}) {
        QVERIFY(pending);
        auto callback = std::move(pending); pending = {};
        callback(value, error);
    };
    const auto refresh_reply = [&] {
        QCOMPARE(command, std::string{"getpaymasteroperatorinfo"});
        reply(snapshot);
        if (pending) {
            QCOMPARE(command, std::string{"getpaymasterfinancestatus"});
            reply(PaymasterOverviewFinance());
        }
    };
    panel->refreshStatus(); refresh_reply();
    auto* automatic = panel->findChild<QCheckBox*>("paymasterAutomaticReplenishment");
    auto* approved = panel->findChild<QCheckBox*>("paymasterPaidMaintenanceApproved");
    auto* limits = panel->findChild<QPushButton*>("paymasterMaintenanceLimitsToggle");
    auto* fee = panel->findChild<QLineEdit*>("paymasterMaintenanceFeePerTransaction");
    auto* hour = panel->findChild<QLineEdit*>("paymasterMaintenanceFeePerHour");
    auto* day = panel->findChild<QLineEdit*>("paymasterMaintenanceFeePerDay");
    auto* save = panel->findChild<QPushButton*>("paymasterSaveLiquidityPolicyPrimary");
    auto* status = panel->findChild<QLabel*>("paymasterLiquidityPolicyStatus");
    auto* targets = panel->findChild<QLabel*>("paymasterLiquidityTargetSaveStatus");
    QVERIFY(automatic && approved && limits && fee && hour && day && save && status && targets);
    QVERIFY(!automatic->isChecked() && !approved->isChecked());
    QVERIFY(!save->isEnabled());
    automatic->click();
    QVERIFY(automatic->isChecked());
    QVERIFY(limits->isChecked());
    QVERIFY(!approved->isChecked()); // Showing the limits does not approve spending.
    QVERIFY(status->text().contains("permission for refill transactions"));
    if (scenario != "unapproved") approved->click();
    fee->setText("0.30000000"); hour->setText("0.70000000"); day->setText("2.40000000");
    panel->refreshStatus();
    QVERIFY(!save->isEnabled());
    refresh_reply(); // Old saved values must not overwrite the pending edit.
    QVERIFY(automatic->isChecked());
    QCOMPARE(approved->isChecked(), scenario != "unapproved");
    QCOMPARE(fee->text(), QStringLiteral("0.30000000"));
    QVERIFY(save->isEnabled());
    QVERIFY(targets->text().contains("Not saved"));
    const auto reads_before_review = reads;
    bool reviewed{false}, review_locked{false}, refresh_blocked{false};
    if (scenario != "unapproved") QTimer::singleShot(0, panel.get(), [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog) return;
        reviewed = true;
        review_locked = !save->isEnabled() && !automatic->isEnabled();
        panel->refreshStatus();
        refresh_blocked = !pending && reads == reads_before_review;
        if (scenario == "wallet_change") panel->setWalletModel(nullptr);
        if (scenario == "privacy") panel->setPrivacy(true);
        // A broken lock must fail the test without leaving a nested dialog open.
        dialog->done(scenario == "decline" || !review_locked ? QMessageBox::Cancel : QMessageBox::Yes);
    });
    save->click();
    QCOMPARE(reviewed, scenario != "unapproved");
    if (reviewed) { QVERIFY(review_locked); QVERIFY(refresh_blocked); }
    if (scenario == "decline" || scenario == "wallet_change" || scenario == "privacy") {
        QCOMPARE(writes, 0);
        QVERIFY(!pending);
        if (scenario == "decline") {
            QVERIFY(save->isEnabled());
            QVERIFY(automatic->isChecked() && approved->isChecked());
            QVERIFY(targets->text().contains("Not saved"));
        }
        return;
    }
    QCOMPARE(command, std::string{"setpaymasterliquiditypolicy"});
    QCOMPARE(writes, 1);
    QVERIFY(params[0].find_value("automatic_replenishment").isTrue());
    QCOMPARE(params[0].find_value("paid_maintenance_approved").get_bool(), scenario != "unapproved");
    QCOMPARE(params[0].find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>(), int64_t{30000000});
    QCOMPARE(params[0].find_value("maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>(), int64_t{70000000});
    QCOMPARE(params[0].find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>(), int64_t{240000000});
    save->click(); QCOMPARE(writes, 1);
    const bool failed = scenario == "write_error" || scenario == "read_error";
    if (!failed) {
        saved = params[0]; saved.pushKV("updated_at", 101);
        liquidity.pushKV("policy", saved);
        provider.pushKV("liquidity", liquidity);
        snapshot.pushKV("provider", provider);
    }
    if (scenario == "lost_reply" || failed) reply(UniValue{}, "injected lost or failed save");
    else if (scenario == "malformed_reply") reply(UniValue{UniValue::VOBJ});
    else reply(saved);
    if (scenario == "lost_reply" || scenario == "malformed_reply" || failed) {
        QCOMPARE(command, std::string{"getpaymasterliquiditystatus"});
        QCOMPARE(writes, 1); // Reconcile by reading; never repeat the write automatically.
        if (scenario == "read_error") reply(UniValue{}, "injected status unavailable");
        else reply(liquidity);
    }
    if (failed) {
        QVERIFY(!pending);
        QVERIFY(automatic->isChecked() && approved->isChecked());
        QCOMPARE(fee->text(), QStringLiteral("0.30000000"));
        QVERIFY(save->isEnabled());
        QVERIFY(status->text().contains("Your edits have been kept"));
        QVERIFY(status->text().contains("injected"));
        QVERIFY(targets->text().contains("Not saved"));
        return;
    }
    refresh_reply();
    QVERIFY(automatic->isChecked());
    QCOMPARE(approved->isChecked(), scenario != "unapproved");
    QVERIFY(!save->isEnabled());
    QVERIFY(targets->text().contains("Saved"));
    QVERIFY(status->text().contains(scenario == "unapproved" ? "paid refill is not approved" : "automatic refill is on"));
    // A fresh view must load the canonical policy, not defaults from its form.
    std::unique_ptr<DigiDollarPaymasterWidget> reopened{CreatePaymasterWidget(nullptr)};
    reopened->setRpcExecutorForTesting([&](const std::string& name, const UniValue&) {
        if (name == "getpaymasteroperatorinfo") return snapshot;
        if (name == "getpaymasterfinancestatus") return PaymasterOverviewFinance();
        throw std::runtime_error("Opening settings must not mutate the wallet");
    });
    reopened->refreshStatus();
    QVERIFY(reopened->findChild<QCheckBox*>("paymasterAutomaticReplenishment")->isChecked());
    QCOMPARE(reopened->findChild<QCheckBox*>("paymasterPaidMaintenanceApproved")->isChecked(), scenario != "unapproved");
    QCOMPARE(reopened->findChild<QLineEdit*>("paymasterMaintenanceFeePerTransaction")->text(), QStringLiteral("0.30000000"));
    QCOMPARE(writes, 1);
}


void PaymasterWidgetTests::paymasterOfferPolicyTypedValues_data()
{
    QTest::addColumn<bool>("german");
    QTest::addColumn<QString>("outcome");
    for (bool german : {false, true}) {
        for (const char* outcome : {"saved", "stale_readback", "rpc_error", "invalid", "partial_draft"}) {
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(german ? "de" : "en", outcome)))
                << german << QString::fromLatin1(outcome);
        }
    }
}

void PaymasterWidgetTests::paymasterOfferPolicyTypedValues()
{
    QFETCH(bool, german);
    QFETCH(QString, outcome);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    auto snapshot = GuidedOperatorSnapshot();
    auto old_provider = snapshot.find_value("provider");
    auto old_policy = old_provider.find_value("policy");
    old_policy.pushKV("maximum_user_paid_service_fee_cents", 0);
    old_policy.pushKV("policy_hash", std::string(64, 'a'));
    old_provider.pushKV("policy", old_policy);
    snapshot.pushKV("provider", old_provider);
    UniValue written;
    int writes{0};
    panel->setRpcExecutorForTesting([&](const std::string& method, const UniValue& params) {
        if (method == "getpaymasteroperatorinfo") return snapshot;
        if (method == "setpaymasterpolicy") {
            ++writes;
            written = params[0];
            if (outcome == "rpc_error") throw std::runtime_error("injected policy write failure");
            auto persisted = written;
            persisted.pushKV("policy_hash", std::string(64, 'b'));
            if (outcome != "stale_readback") {
                auto provider = snapshot.find_value("provider");
                provider.pushKV("policy", persisted);
                snapshot.pushKV("provider", provider);
            }
            return persisted;
        }
        return UniValue{UniValue::VOBJ};
    });
    panel->refreshStatus();
    auto* save = panel->findChild<QPushButton*>("savePaymasterPolicy");
    auto* summary = panel->findChild<QLabel*>("paymasterPolicySummary");
    auto* status = panel->findChild<QLabel*>("paymasterProviderStatus");
    QVERIFY(save && summary && status);
    const QLocale locale = german ? QLocale(QLocale::German) : QLocale::c();
    if (outcome == "partial_draft") {
        auto* amount = panel->findChild<QSpinBox*>("paymasterPolicyMinimumCents");
        QVERIFY(amount);
        amount->setLocale(locale);
        auto* editor = amount->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->selectAll();
        // A below-minimum intermediate value leaves the numeric value at
        // 100 cents. Text editing alone must mark it as a protected draft.
        QTest::keyClicks(editor, "0");
        const QString partial = editor->text();
        QVERIFY2(!amount->hasAcceptableInput(), qPrintable(partial));
        QCOMPARE(amount->value(), 100);
        panel->refreshStatus();
        QCOMPARE(editor->text(), partial);
        QVERIFY(summary->text().contains("Unsaved changes"));
        save->click();
        QCOMPARE(writes, 0);
        QCOMPARE(editor->text(), partial);
        return;
    }
    const std::vector<std::pair<const char*, int>> fields{
        {"paymasterPolicyFeeBps", 80},
        {"paymasterPolicyMaximumUserPaidServiceFeeCents", 37},
        {"paymasterPolicyMinimumCents", 125},
        {"paymasterPolicyMaximumCents", 234567},
        {"paymasterPolicyMaximumNetworkFee", 12345678},
    };
    for (const auto& [name, value] : fields) {
        auto* field = panel->findChild<QSpinBox*>(name);
        QVERIFY(field);
        field->setLocale(locale);
        auto* editor = field->findChild<QLineEdit*>();
        QVERIFY(editor);
        const bool dgb = QString::fromLatin1(name).endsWith("MaximumNetworkFee");
        QString text = QString::number(value / (dgb ? 100000000.0 : 100.0), 'f', dgb ? 8 : 2);
        editor->selectAll();
        QTest::keyClicks(editor, text);
        QVERIFY2(field->hasAcceptableInput(), qPrintable(QString::fromLatin1(name) + ": " + editor->text()));
        // Do not manually call interpretText: Save and normal keyboard focus
        // handling must commit the exact typed limits.
    }
    // Background refresh while the form is dirty must preserve all edits.
    panel->refreshStatus();
    auto* fee = panel->findChild<QSpinBox*>("paymasterPolicyFeeBps");
    if (outcome == "invalid") {
        auto* editor = fee->findChild<QLineEdit*>();
        const QStringList invalid{"0.801 %", "-0.80 %", "1e3 %", "100.01 %", "0,80 %"};
        for (const QString& text : invalid) {
            editor->setText(text);
            QFocusEvent focus_out(QEvent::FocusOut);
            QApplication::sendEvent(fee, &focus_out);
            save->click();
            QCOMPARE(writes, 0);
            QVERIFY(status->text().contains("not saved"));
            QCOMPARE(editor->text(), text);
            QCOMPARE(fee->value(), 80);
        }
        return;
    }
    save->click();
    QCOMPARE(writes, 1);
    const std::vector<std::pair<const char*, int>> expected{
        {"fee_rate_bps", 80}, {"maximum_user_paid_service_fee_cents", 37},
        {"min_amount_cents", 125}, {"max_amount_cents", 234567},
        {"maximum_network_fee_dgb_satoshis", 12345678},
    };
    for (const auto& [name, value] : expected) QCOMPARE(written.find_value(name).getInt<int>(), value);
    for (const auto& [name, value] : fields) QCOMPARE(panel->findChild<QSpinBox*>(name)->value(), value);
    if (outcome == "rpc_error") {
        QVERIFY(summary->text().contains("Unsaved changes"));
        QVERIFY(panel->findChild<QLabel*>("paymasterPolicySaveStatus")->text().contains("could not be confirmed"));
    } else if (outcome == "stale_readback") {
        QVERIFY(summary->text().contains("waiting for the updated policy"));
        auto provider = snapshot.find_value("provider");
        written.pushKV("policy_hash", std::string(64, 'b'));
        provider.pushKV("policy", written);
        snapshot.pushKV("provider", provider);
        panel->refreshStatus();
        QVERIFY(summary->text().contains("currently saved"));
    } else {
        QVERIFY(summary->text().contains("currently saved"));
    }
}

void PaymasterWidgetTests::paymasterOfferFormAlignment_data()
{
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("width");
    for (bool dark : {false, true}) {
        for (int width : {640, 1200}) {
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
        }
    }
}

void PaymasterWidgetTests::paymasterOfferFormAlignment()
{
    QFETCH(bool, dark);
    QFETCH(int, width);
    std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
    panel->setObjectName("paymasterWidget");
    QFile css(dark ? ":/css/dark" : ":/css/light");
    QVERIFY(css.open(QIODevice::ReadOnly));
    panel->setStyleSheet(QString::fromUtf8(css.readAll()));
    panel->setOperatorStatusForTesting(GuidedOperatorSnapshot());
    auto* offer = panel->findChild<QWidget*>("paymasterPolicyFeeBps");
    auto* settings = panel->findChild<QStackedWidget*>("paymasterSettingsPages");
    auto* pages = panel->findChild<QStackedWidget*>("paymasterOperatorPages");
    QVERIFY(offer && settings && pages);
    QWidget* page = offer;
    while (page && settings->indexOf(page) < 0) page = page->parentWidget();
    QVERIFY(page);
    pages->setCurrentWidget(settings);
    settings->setCurrentWidget(page);
    pages->setEnabled(true);
    settings->setEnabled(true);
    page->setEnabled(true);
    panel->resize(width, 900);
    panel->show();
    QTest::qWait(20);
    const QList<QString> names{"paymasterDisplayName", "paymasterPolicyFeeBps",
        "paymasterPolicyMaximumUserPaidServiceFeeCents", "paymasterFeeExampleAmount",
        "paymasterPolicyMinimumCents", "paymasterPolicyMaximumCents"};
    int left{-1};
    int right{-1};
    for (const auto& name : names) {
        auto* field = panel->findChild<QWidget*>(name);
        QVERIFY(field && field->isVisible());
        const QPoint start = field->mapTo(panel.get(), QPoint{});
        if (left < 0) { left = start.x(); right = start.x() + field->width(); }
        QCOMPARE(start.x(), left);
        QCOMPARE(start.x() + field->width(), right);
    }
    QVERIFY(panel->width() <= width);
    const QString screenshot_dir = qEnvironmentVariable("DIGIBYTE_QT_TEST_SCREENSHOT_DIR");
    if (!screenshot_dir.isEmpty()) {
        auto* scroll = qobject_cast<QScrollArea*>(page);
        QVERIFY(scroll && scroll->widget());
        QVERIFY(scroll->widget()->grab().save(screenshot_dir + QStringLiteral("/offer-%1-%2.png")
            .arg(dark ? "dark" : "light").arg(width)));
    }
}


void PaymasterWidgetTests::paymasterClientOfferCards_data()
{
    QTest::addColumn<QString>("theme");
    QTest::addColumn<int>("width");
    for (const auto& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
        for (int width : {720, 1200}) {
            QTest::newRow(qPrintable(theme + QString::number(width))) << theme << width;
        }
    }
}

void PaymasterWidgetTests::paymasterClientOfferCards()
{
    QFETCH(QString, theme);
    QFETCH(int, width);
    TestChain100Setup test;
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    m_node.setContext(&test.m_node);
    auto wallet = SetupDescriptorsWallet(m_node, test, "qt-offer-cards");
    DigiDollarMiniGUI gui(m_node);
    gui.initModelForWallet(m_node, wallet);
    DigiDollarSendWidget form(gui.platformStyle.get());
    form.setObjectName("digiDollarTab"); // Apply the containing tab's shared styles.
    QFile css(":/css/" + theme);
    QVERIFY(css.open(QIODevice::ReadOnly));
    form.setStyleSheet(QString::fromUtf8(css.readAll()));
    auto* client = form.findChild<PaymasterSendWidget*>();
    auto* timer = client->findChild<QTimer*>("paymasterOfferRefreshTimer");
    timer->stop();
    const int64_t expires = QDateTime::currentSecsSinceEpoch() + 600;
    UniValue offers{UniValue::VARR};
    offers.push_back(PaymasterOffer("Higher fee", std::string(64, 'a'), "user_paid", 5, 325, false, expires, 150));
    offers.push_back(PaymasterOffer("<b>Lowest & cost</b>", std::string(64, 'b'), "user_paid", 2, 325, false, expires));
    auto unavailable = PaymasterOffer("Earlier failed provider", std::string(64, 'c'), "sponsored", 0, 325, false, expires);
    unavailable.pushKV("recommendation_deprioritized", true);
    offers.push_back(unavailable);
    UniValue sent;
    WalletModel::RpcCallback pending_send;
    client->setPaymasterAsyncRpcExecutorForTesting([&](const std::string& method, const UniValue& params, WalletModel::RpcCallback callback) {
        if (method == "getpaymasterclientsafetystatus") callback(PaymasterClientSafetyStatus(), {});
        else if (method == "listdigidollarsendsessions") callback(EmptyPaymasterSessionList(), {});
        else if (method == "getpaymasteroffers") callback(offers, {});
        else if (method == "senddigidollar") {
            sent = params;
            pending_send = std::move(callback);
        } else QFAIL("Unexpected RPC in offer-card test");
    });
    form.setWalletModel(gui.walletModel.get());
    form.setAvailableDigiDollarBalanceForTesting(1000);
    form.findChild<QRadioButton*>("feeFundingPaymaster")->setChecked(true);
    form.findChild<QLineEdit*>("amountEdit")->setText("3.25");
    const QString recipient = QStringLiteral("RD3HXjF4ibdKEAHNwmv4AnwHWKsb2PgXsiMN5mtm5ao3XJmKLATx");
    form.findChild<QLineEdit*>("addressEdit")->setText(recipient);
    form.setDialogHandlerForTesting([](QMessageBox::Icon, const QString&, const QString&, QMessageBox::StandardButtons, QMessageBox::StandardButton) { return QMessageBox::Yes; });
    const auto refresh = [&] { return QMetaObject::invokeMethod(client, "refreshPaymasterOffers", Qt::DirectConnection); };
    QVERIFY(refresh());
    auto* cards = form.findChild<QFrame*>("paymasterOfferCardsFrame");
    QVERIFY(cards && !cards->isHidden());
    QCOMPARE(form.findChild<QFrame*>("paymasterOfferCard0")->property("providerId").toString(), QString(64, 'b'));
    QVERIFY(form.findChild<QRadioButton*>("paymasterOfferChoice0")->isChecked());
    QVERIFY(form.findChild<QLabel*>("paymasterRecommendedOffer"));
    auto* first = form.findChild<QFrame*>("paymasterOfferCard0");
    QStringList texts;
    for (auto* label : first->findChildren<QLabel*>()) {
        QCOMPARE(label->textFormat(), Qt::PlainText);
        texts.push_back(label->text());
    }
    QVERIFY(texts.contains(QStringLiteral("<b>Lowest & cost</b>")));
    QVERIFY(texts.contains(QStringLiteral("Service fee: 0.02 $DD (≈ 0.62%)")));
    QVERIFY(texts.contains(QStringLiteral("Total from your wallet: 3.27 $DD")));
    QCOMPARE(form.findChild<QFrame*>("paymasterOfferCard2")->property("providerId").toString(), QString(64, 'c'));
    auto* manual_choice = form.findChild<QRadioButton*>("paymasterOfferChoice1");
    manual_choice->setFocus();
    QTest::keyClick(manual_choice, Qt::Key_Space);
    QVERIFY(form.findChild<QFrame*>("paymasterOfferCard1")->property("feeSelected").toBool());
    // A new cheaper provider changes the recommendation, preserving the
    // operator's explicit choice of another still-valid offer.
    offers.push_back(PaymasterOffer("New sponsor", std::string(64, 'e'), "sponsored", 0, 325, false, expires));
    QVERIFY(refresh());
    QCOMPARE(form.findChild<QFrame*>("paymasterOfferCard0")->property("providerId").toString(), QString(64, 'e'));
    QVERIFY(form.findChild<QRadioButton*>("paymasterOfferChoice2")->isChecked());
    client->setPrivacy(true);
    QVERIFY(cards->isHidden());
    QVERIFY(!form.findChild<QRadioButton*>("paymasterOfferChoice0"));
    client->setPrivacy(false);
    QVERIFY(form.findChild<QRadioButton*>("paymasterOfferChoice2")->isChecked());
    form.resize(width, 1600);
    form.show();
    QTest::qWait(20);
    QVERIFY(cards->width() <= form.width());
    const QString screenshots = qEnvironmentVariable("DIGIBYTE_QT_TEST_SCREENSHOT_DIR");
    if (!screenshots.isEmpty()) {
        QVERIFY(form.findChild<QFrame*>("feeFrame")->grab().save(screenshots + "/offer-cards-" + theme + "-" + QString::number(width) + ".png"));
    }
    // Editing the amount invalidates both the cost preview and selection.
    auto* amount = form.findChild<QLineEdit*>("amountEdit");
    amount->setText("3.26");
    QVERIFY(!form.findChild<QRadioButton*>("paymasterOfferChoice0"));
    QVERIFY(!client->hasCurrentPaymasterOffer());
    amount->setText("3.25");
    QVERIFY(refresh());
    QVERIFY(form.findChild<QRadioButton*>("paymasterOfferChoice0")->isChecked());
    // Select a higher fee explicitly and verify the immutable RPC template.
    form.findChild<QRadioButton*>("paymasterOfferChoice2")->click();
    auto* send = form.findChild<QPushButton*>("sendButton");
    QCOMPARE(send->text(), QStringLiteral("Send payment"));
    QVERIFY(send->isEnabled());
    send->click();
    QVERIFY(pending_send);
    QCOMPARE(sent[6].find_value("preferred_provider_id").get_str(), std::string(64, 'a'));
    QCOMPARE(sent[6].find_value("preferred_offer_id").get_str(), std::string(64, 'd'));
    QVERIFY(sent[6].find_value("prepare_only").isTrue());
    auto unavailable_callback = std::move(pending_send);
    unavailable_callback({}, QStringLiteral("PAYMASTER_SELECTED_OFFER_UNAVAILABLE"));
    QVERIFY(!client->hasCurrentPaymasterOffer());
    QVERIFY(!form.findChild<QLineEdit*>("addressEdit")->isReadOnly());
    QVERIFY(!form.findChild<QRadioButton*>("paymasterOfferChoice0"));
    // A subsequent explicit send uses a fresh preview; no silent substitute.
    QVERIFY(refresh());
    form.findChild<QRadioButton*>("paymasterOfferChoice2")->click();
    send->click();
    QVERIFY(pending_send);
    // Wallet closure erases cards and rejects the delayed old-wallet reply.
    form.setWalletModel(nullptr);
    auto callback = std::move(pending_send);
    callback(PaymasterSessionView("PENDING_PROVIDER", "user_psbt", "USER_SIGNED"), {});
    QVERIFY(!form.findChild<QRadioButton*>("paymasterOfferChoice0"));
    QVERIFY(!client->hasCurrentPaymasterOffer());
}

void PaymasterWidgetTests::paymasterAppNumberFormat()
{
    struct RestoreLocale {
        QLocale saved;
        ~RestoreLocale() { QLocale::setDefault(saved); }
    } restore;
    for (const auto& locale : {QLocale::c(), QLocale(QLocale::German)}) {
        QLocale::setDefault(locale);
        PaymasterAmountSpinBox amount(nullptr);
        QCOMPARE(amount.locale(), QLocale::c());
        amount.setValue(1234567);
        QCOMPARE(amount.text(), QStringLiteral("12345.67 DD"));
        QCOMPARE(PaymasterFormatDD(1234567), QString::fromUtf8("12\xE2\x80\x89" "345.67"));
        QCOMPARE(PaymasterFormatDGB(1234567890000LL), QString::fromUtf8("12\xE2\x80\x89" "345.67890000"));
        auto* editor = amount.findChild<QLineEdit*>();
        editor->setText("1,000.00 DD");
        QVERIFY(!amount.hasAcceptableInput());
        amount.interpretText();
        QCOMPARE(amount.value(), 1234567);
        std::unique_ptr<DigiDollarPaymasterWidget> panel{CreatePaymasterWidget(nullptr)};
        for (auto* spin : panel->findChildren<QSpinBox*>()) QCOMPARE(spin->locale(), QLocale::c());
        for (auto* spin : panel->findChildren<QDoubleSpinBox*>()) QCOMPARE(spin->locale(), QLocale::c());
        auto* rate = panel->findChild<QSpinBox*>("paymasterPolicyFeeBps");
        rate->setValue(80);
        QVERIFY(rate->text().contains("0.80"));
        QVERIFY(!rate->text().contains(','));
    }
}

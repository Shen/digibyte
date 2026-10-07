// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterwallet.h>

#include <interfaces/node.h>

#include <QPointer>
#include <QThread>
#include <QUrl>

#include <exception>
#include <utility>

std::shared_ptr<WalletModel::UnlockContext> PaymasterQt::RequestUnlock(WalletModel& model)
{
    const bool was_locked = model.getEncryptionStatus() == WalletModel::Locked;
    if (was_locked) Q_EMIT model.requireUnlock();
    const bool valid = model.getEncryptionStatus() != WalletModel::Locked;
    return std::make_shared<WalletModel::UnlockContext>(model.walletShared(), valid, was_locked);
}

void PaymasterQt::ExecuteSigningRpcAsync(WalletModel& model, std::string command, UniValue params,
                                       std::function<bool()> is_current, WalletModel::RpcCallback callback)
{
    const auto wallet = model.walletShared();
    const QByteArray encoded_name = QUrl::toPercentEncoding(model.getWalletName());
    const std::string uri = "/wallet/" + std::string(encoded_name.constData(), encoded_name.length());
    interfaces::Node* const node = &model.node();
    QPointer<WalletModel> guard{&model};
    // Merely asking IsLocked can wait behind Paymaster maintenance. Do not
    // take that lock on Qt before launching an otherwise asynchronous RPC.
    QThread* preflight = QThread::create([guard, wallet, node, uri, command = std::move(command),
                                         params = std::move(params), is_current = std::move(is_current),
                                         callback = std::move(callback)]() mutable {
        const bool was_locked = wallet->isLocked();
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, wallet, node, uri, was_locked, command = std::move(command),
                                          params = std::move(params), is_current = std::move(is_current),
                                          callback = std::move(callback)]() mutable {
            if (!guard || !is_current()) return;
            if (was_locked) Q_EMIT guard->requireUnlock();
            // The normal modal unlock dialog may switch/close the wallet.
            // Even then, release temporary authority using the retained wallet.
            const bool current = guard && is_current();
            QThread* signing = QThread::create([guard, wallet, node, uri, was_locked, current,
                                               command = std::move(command), params = std::move(params),
                                               callback = std::move(callback)]() mutable {
                UniValue result;
                QString error;
                {
                    const bool valid = !wallet->isLocked();
                    WalletModel::UnlockContext unlock(wallet, valid, was_locked);
                    if (current) {
                        if (!valid) {
                            error = QStringLiteral("PAYMASTER_WALLET_UNLOCK_CANCELLED");
                        } else {
                            try {
                                result = node->executeRpc(command, params, uri);
                            } catch (const UniValue& rpc_error) {
                                const auto& message = rpc_error.find_value("message");
                                error = QString::fromStdString(message.isStr() ? message.get_str() : rpc_error.write());
                            } catch (const std::exception& exception) {
                                error = QString::fromUtf8(exception.what());
                            } catch (...) {
                                error = QStringLiteral("Unknown RPC error");
                            }
                        }
                    }
                    // Relock on this worker, before any UI result/error dialog.
                }
                if (!current || !guard) return;
                QMetaObject::invokeMethod(guard, [guard, callback = std::move(callback),
                                                  result = std::move(result), error = std::move(error)]() mutable {
                    if (guard && callback) callback(std::move(result), std::move(error));
                }, Qt::QueuedConnection);
            });
            QObject::connect(signing, &QThread::finished, signing, &QObject::deleteLater);
            signing->start();
        }, Qt::QueuedConnection);
    });
    QObject::connect(preflight, &QThread::finished, preflight, &QObject::deleteLater);
    preflight->start();
}

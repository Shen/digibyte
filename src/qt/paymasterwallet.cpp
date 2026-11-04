// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterwallet.h>
#include <qt/paymasterasync.h>

#include <interfaces/node.h>

#include <QPointer>
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
    ExecuteSigningRpcAsyncDetailed(model, std::move(command), std::move(params), std::move(is_current),
        [callback = std::move(callback)](UniValue result, WalletModel::RpcError error) mutable {
            if (callback) callback(std::move(result), error.failed ? std::move(error.message) : QString{});
        });
}

void PaymasterQt::ExecuteSigningRpcAsyncDetailed(WalletModel& model, std::string command, UniValue params,
                                                std::function<bool()> is_current, WalletModel::DetailedRpcCallback callback)
{
    const auto wallet = model.walletShared();
    const QByteArray encoded_name = QUrl::toPercentEncoding(model.getWalletName());
    const std::string uri = "/wallet/" + std::string(encoded_name.constData(), encoded_name.length());
    interfaces::Node* const node = &model.node();
    QPointer<WalletModel> guard{&model};
    QPointer<WorkQueue> queue{&model.asyncWorkQueue()};
    // Hold one mutation slot across preflight, the unlock dialog and signing.
    // Merely checking IsLocked can wait, so it too stays off the Qt thread.
    queue->submit({WorkQueue::Kind::MUTATION, false}, {},
        [queue, guard, wallet, node, uri, command = std::move(command), params = std::move(params), is_current]
        (WorkQueue::Completion complete) mutable {
            queue->run([wallet] { return RpcReply{UniValue{wallet->isLocked()}, {}}; },
                [queue, guard, wallet, node, uri, command = std::move(command), params = std::move(params),
                 is_current, complete = std::move(complete)](RpcReply preflight) mutable {
                    if (preflight.error.failed) { complete(std::move(preflight)); return; }
                    if (!guard || !is_current()) {
                        complete({{}, {true, 0, QStringLiteral("PAYMASTER_OPERATION_CANCELED"), {}}});
                        return;
                    }
                    const bool was_locked = preflight.value.get_bool();
                    if (was_locked) Q_EMIT guard->requireUnlock();
                    // A modal dialog can unload the model. No node access may
                    // begin afterwards; the retained wallet still owns relocking.
                    if (!queue || queue->stopping()) {
                        if (was_locked) wallet->lock();
                        return;
                    }
                    const bool current = guard && is_current();
                    queue->run([wallet, node, uri, was_locked, current,
                                command = std::move(command), params = std::move(params)]() mutable {
                        const bool valid = !wallet->isLocked();
                        WalletModel::UnlockContext unlock(wallet, valid, was_locked);
                        if (!current || !valid) {
                            RpcReply reply;
                            reply.error.failed = true;
                            reply.error.message = QStringLiteral("PAYMASTER_WALLET_UNLOCK_CANCELLED");
                            reply.error.data = UniValue{UniValue::VOBJ};
                            reply.error.data.pushKV("paymaster_code", "PAYMASTER_WALLET_UNLOCK_CANCELLED");
                            reply.error.data.pushKV("disposition", "user_decision");
                            return reply;
                        }
                        return RpcReply{node->executeRpc(command, params, uri), {}};
                    }, std::move(complete));
                });
        },
        [guard, is_current, callback = std::move(callback)](RpcReply reply) mutable {
            if (guard && is_current() && callback) callback(std::move(reply.value), std::move(reply.error));
        });
}

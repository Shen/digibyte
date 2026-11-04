// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_WALLETASYNC_H
#define DIGIBYTE_QT_WALLETASYNC_H

#include <univalue.h>
#include <QObject>
#include <QString>
#include <functional>
#include <list>
#include <memory>
#include <vector>

class QThread;

namespace WalletAsync {
struct RpcError {
    bool failed{false};
    int code{0};
    QString message;
    UniValue data;
    static RpcError fromRpc(const UniValue& error);
};

struct RpcReply {
    UniValue value;
    RpcError error;
};

/** One queue per wallet. All scheduling and delivery happen on its Qt thread;
 * blocking work uses the existing QThread workers. Shutdown discards queued
 * work and joins started workers before their node/wallet dependencies die. */
class WorkQueue final : public QObject {
public:
    enum class Kind { READ, MUTATION };
    using Completion = std::function<void(RpcReply)>;
    using Start = std::function<void(Completion)>;
    explicit WorkQueue(QObject* parent = nullptr) : QObject(parent) {}
    ~WorkQueue() override;
    void submit(Kind kind, std::string key, Start start, Completion completion);
    void run(std::function<RpcReply()> work, Completion completion);
    void track(QThread* worker);
    void stop();
    bool stopping() const { return m_stopping; }

private:
    struct Job {
        Kind kind;
        std::string key;
        uint64_t generation;
        Start start;
        std::vector<Completion> callbacks;
        bool running{false};
        bool completed{false};
    };
    void pump();
    void complete(const std::shared_ptr<Job>& job, RpcReply reply);
    std::list<std::shared_ptr<Job>> m_jobs;
    std::vector<QThread*> m_workers;
    uint64_t m_generation{0};
    size_t m_reads{0};
    bool m_mutating{false};
    bool m_stopping{false};
};
} // namespace WalletAsync
#endif // DIGIBYTE_QT_WALLETASYNC_H

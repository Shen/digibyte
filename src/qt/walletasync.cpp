// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/walletasync.h>

#include <QPointer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <exception>

namespace WalletAsync {
RpcError RpcError::fromRpc(const UniValue& error)
{
    RpcError result;
    result.failed = true;
    const auto& message = error.find_value("message");
    const auto& code = error.find_value("code");
    result.message = QString::fromStdString(message.isStr() ? message.get_str() : error.write());
    if (result.message.isEmpty()) result.message = QStringLiteral("RPC failed");
    if (code.isNum()) {
        try { result.code = code.getInt<int>(); } catch (const std::exception&) {}
    }
    result.data = error.find_value("data");
    return result;
}

WorkQueue::~WorkQueue() { stop(); }

void WorkQueue::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping) return;
    m_stopping = true;
    m_jobs.clear();
    // A UI cancellation never interrupts or replays a mutation already started.
    for (auto* worker : m_workers) {
        disconnect(worker, nullptr, this, nullptr);
        worker->wait();
        delete worker;
    }
    m_workers.clear();
}

void WorkQueue::submit(Kind kind, std::string key, Start start, Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping) return;
    if (kind == Kind::READ && !key.empty()) {
        for (const auto& job : m_jobs) {
            if (job->kind == kind && job->key == key && job->generation == m_generation && !job->completed) {
                job->callbacks.push_back(std::move(completion));
                return;
            }
        }
    }
    // A read requested after a mutation was queued must not attach itself to
    // an older in-flight snapshot with identical parameters.
    if (kind == Kind::MUTATION) ++m_generation;
    auto job = std::make_shared<Job>(Job{kind, std::move(key), m_generation,
                                       std::move(start), {std::move(completion)}});
    m_jobs.push_back(job);
    pump();
}

void WorkQueue::pump()
{
    if (m_stopping) return;
    // Copy the launch list: a test/validation failure may complete immediately.
    std::vector<std::shared_ptr<Job>> launch;
    for (const auto& job : m_jobs) {
        if (job->running || job->completed) continue;
        if (job->kind == Kind::READ) {
            if (m_reads == 2) continue;
            ++m_reads;
        } else {
            if (m_mutating) continue;
            m_mutating = true;
        }
        job->running = true;
        launch.push_back(job);
    }
    QPointer<WorkQueue> guard{this};
    for (const auto& job : launch) {
        if (!guard || guard->m_stopping) return;
        job->start([guard, job](RpcReply reply) {
            if (guard && !guard->m_stopping) guard->complete(job, std::move(reply));
        });
    }
}

void WorkQueue::complete(const std::shared_ptr<Job>& job, RpcReply reply)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (job->completed) return;
    job->completed = true;
    if (job->kind == Kind::READ) --m_reads;
    else { m_mutating = false; ++m_generation; }
    m_jobs.remove(job);
    QPointer<WorkQueue> guard{this};
    // Callers can close their wallet in a callback. Do not dereference this
    // again unless the QObject still exists.
    for (auto& callback : job->callbacks) {
        if (!guard || guard->m_stopping) return;
        if (callback) callback(reply);
    }
    if (guard && !guard->m_stopping) guard->pump();
}

void WorkQueue::run(std::function<RpcReply()> work, Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping) return;
    QPointer<WorkQueue> guard{this};
    QThread* worker = QThread::create([guard, work = std::move(work), completion = std::move(completion)]() mutable {
        RpcReply reply;
        try {
            reply = work();
        } catch (const UniValue& error) {
            reply.error = RpcError::fromRpc(error);
        } catch (const std::exception& error) {
            reply.error.failed = true;
            reply.error.message = QString::fromUtf8(error.what());
        } catch (...) {
            reply.error.failed = true;
            reply.error.message = QStringLiteral("Unknown RPC error");
        }
        if (reply.error.failed && reply.error.message.isEmpty()) reply.error.message = QStringLiteral("RPC failed");
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, completion = std::move(completion), reply = std::move(reply)]() mutable {
            if (guard && !guard->m_stopping && completion) completion(std::move(reply));
        }, Qt::QueuedConnection);
    });
    track(worker);
    worker->start();
}

void WorkQueue::track(QThread* worker)
{
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!m_stopping);
    m_workers.push_back(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        const auto found = std::find(m_workers.begin(), m_workers.end(), worker);
        if (found == m_workers.end()) return;
        m_workers.erase(found);
        worker->deleteLater();
    });
}
} // namespace WalletAsync

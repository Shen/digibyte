// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <qt/modaloverlay.h>
#include <qt/forms/ui_modaloverlay.h>

#include <chainparams.h>
#include <clientversion.h>
#include <qt/guiutil.h>

#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QResizeEvent>

ModalOverlay::ModalOverlay(bool enable_wallet, QWidget* parent)
    : QWidget(parent),
      ui(new Ui::ModalOverlay),
      bestHeaderDate(QDateTime())
{
    ui->setupUi(this);
    ui->formLayout->setHorizontalSpacing(18);
    ui->formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    ui->formLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    ui->newestBlockDate->setText(tr("No blocks processed yet"));
    showSyncStage(tr("Waiting for header information…"));
    connect(ui->closeButton, &QPushButton::clicked, this, &ModalOverlay::closeClicked);
    if (parent) {
        parent->installEventFilter(this);
        raise();
    }

    blockProcessTime.clear();
    setVisible(false);
    if (!enable_wallet) {
        ui->infoText->setVisible(false);
        ui->infoTextStrong->setText(tr("%1 is currently syncing.  It will download headers and blocks from peers and validate them until reaching the tip of the block chain.").arg(QString::fromStdString(CLIENT_NAME)));
    }

    m_animation.setTargetObject(this);
    m_animation.setPropertyName("pos");
    m_animation.setDuration(300 /* ms */);
    m_animation.setEasingCurve(QEasingCurve::OutQuad);
}

ModalOverlay::~ModalOverlay()
{
    delete ui;
}

bool ModalOverlay::eventFilter(QObject * obj, QEvent * ev) {
    if (obj == parent()) {
        if (ev->type() == QEvent::Resize) {
            QResizeEvent * rev = static_cast<QResizeEvent*>(ev);
            resize(rev->size());
            if (!layerIsVisible)
                setGeometry(0, height(), width(), height());

            if (m_animation.endValue().toPoint().y() > 0) {
                m_animation.setEndValue(QPoint(0, height()));
            }
        }
        else if (ev->type() == QEvent::ChildAdded) {
            raise();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

//! Tracks parent widget changes
bool ModalOverlay::event(QEvent* ev) {
    if (ev->type() == QEvent::ParentAboutToChange) {
        if (parent()) parent()->removeEventFilter(this);
    }
    else if (ev->type() == QEvent::ParentChange) {
        if (parent()) {
            parent()->installEventFilter(this);
            raise();
        }
    }
    return QWidget::event(ev);
}

void ModalOverlay::setKnownBestHeight(int count, const QDateTime& blockDate, bool presync)
{
    if (!presync && count > bestHeaderHeight) {
        bestHeaderHeight = count;
        bestHeaderDate = blockDate;
        const auto headers_left = blockDate.secsTo(QDateTime::currentDateTime()) / Params().GetConsensus().nPowTargetSpacing;
        // Routine header notifications during block download must not reset
        // the samples used to estimate processing speed and remaining time.
        if (headers_left >= HEADER_HEIGHT_DELTA_SYNC || blockProcessTime.isEmpty() || blockProcessTime.front().second >= 1.0) {
            UpdateHeaderSyncLabel();
        }
    }
    if (presync) {
        UpdateHeaderPresyncLabel(count, blockDate);
    }
}

void ModalOverlay::tipUpdate(int count, const QDateTime& blockDate, double nVerificationProgress)
{
    QDateTime currentDate = QDateTime::currentDateTime();

    ui->newestBlockDate->setText(count > 0 ? blockDate.toString() : tr("No blocks processed yet"));

    // Before the first header notification (including genesis-only startup),
    // transaction statistics cannot describe synchronization completion.
    if (!bestHeaderDate.isValid() || bestHeaderHeight < count) {
        showSyncStage(tr("Waiting for header information…"));
        return;
    }
    const auto headers_left = bestHeaderDate.secsTo(currentDate) / Params().GetConsensus().nPowTargetSpacing;
    if (headers_left >= HEADER_HEIGHT_DELTA_SYNC) {
        UpdateHeaderSyncLabel();
        return;
    }

    // Regtest has no transaction-rate baseline; its estimate can already be
    // 100% with only genesis processed. Show actual known block counts instead.
    if (nVerificationProgress >= 1.0 && count < bestHeaderHeight) {
        showSyncStage(tr("Blocks processed: %1 of %2").arg(count).arg(bestHeaderHeight));
        ui->numberOfBlocksLeft->setText(QString::number(bestHeaderHeight - count));
        return;
    }
    ui->numberOfBlocksLeft->setText(QString::number(bestHeaderHeight - count));
    ui->progressIncreasePerH->setText(tr("Calculating…"));
    ui->expectedTimeLeft->setText(tr("Calculating…"));

    // keep a vector of samples of verification progress at height
    blockProcessTime.push_front(qMakePair(currentDate.toMSecsSinceEpoch(), nVerificationProgress));

    // show progress speed if we have more than one sample
    if (blockProcessTime.size() >= 2) {
        double progressDelta = 0;
        double progressPerHour = 0;
        qint64 timeDelta = 0;
        qint64 remainingMSecs = 0;
        double remainingProgress = 1.0 - nVerificationProgress;
        for (int i = 1; i < blockProcessTime.size(); i++) {
            QPair<qint64, double> sample = blockProcessTime[i];

            // take first sample after 500 seconds or last available one
            if (sample.first < (currentDate.toMSecsSinceEpoch() - 500 * 1000) || i == blockProcessTime.size() - 1) {
                progressDelta = blockProcessTime[0].second - sample.second;
                timeDelta = blockProcessTime[0].first - sample.first;
                progressPerHour = (progressDelta > 0 && timeDelta > 0) ? progressDelta / (double)timeDelta * 1000 * 3600 : 0;
                remainingMSecs = (progressDelta > 0 && timeDelta > 0) ? remainingProgress / progressDelta * timeDelta : -1;
                break;
            }
        }
        // show progress increase per hour
        ui->progressIncreasePerH->setText(QString::number(progressPerHour * 100, 'f', 2)+"%");

        // show expected remaining time
        if(remainingMSecs >= 0) {
            ui->expectedTimeLeft->setText(GUIUtil::formatNiceTimeOffset(remainingMSecs / 1000.0));
        } else {
            ui->expectedTimeLeft->setText(tr("Calculating…"));
        }

        static const int MAX_SAMPLES = 5000;
        if (blockProcessTime.count() > MAX_SAMPLES) {
            blockProcessTime.remove(MAX_SAMPLES, blockProcessTime.count() - MAX_SAMPLES);
        }
    }

    ui->percentageProgress->setText(QString::number(nVerificationProgress * 100, 'f', 2) + "%");
}

void ModalOverlay::showSyncStage(const QString& progress)
{
    blockProcessTime.clear();
    ui->numberOfBlocksLeft->setText(tr("Not known yet"));
    ui->percentageProgress->setText(progress);
    ui->progressIncreasePerH->setText(QStringLiteral("—"));
    ui->expectedTimeLeft->setText(tr("Not enough information yet"));
}

void ModalOverlay::UpdateHeaderSyncLabel()
{
    if (bestHeaderHeight <= 0 || !bestHeaderDate.isValid()) {
        showSyncStage(tr("Waiting for header information…"));
    } else {
        showSyncStage(tr("Synchronizing headers: %1 processed").arg(bestHeaderHeight));
    }
}

void ModalOverlay::UpdateHeaderPresyncLabel(int height, const QDateTime& /*blockDate*/)
{
    showSyncStage(tr("Pre-synchronizing headers: %1 processed").arg(height));
}


void ModalOverlay::toggleVisibility()
{
    showHide(layerIsVisible, true);
    if (!layerIsVisible)
        userClosed = true;
}

void ModalOverlay::showHide(bool hide, bool userRequested)
{
    if ( (layerIsVisible && !hide) || (!layerIsVisible && hide) || (!hide && userClosed && !userRequested))
        return;

    Q_EMIT triggered(hide);

    if (!isVisible() && !hide)
        setVisible(true);

    m_animation.setStartValue(QPoint(0, hide ? 0 : height()));
    // The eventFilter() updates the endValue if it is required for QEvent::Resize.
    m_animation.setEndValue(QPoint(0, hide ? height() : 0));
    m_animation.start(QAbstractAnimation::KeepWhenStopped);
    layerIsVisible = !hide;
}

void ModalOverlay::closeClicked()
{
    showHide(true);
    userClosed = true;
}
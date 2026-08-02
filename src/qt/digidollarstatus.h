// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_DIGIDOLLARSTATUS_H
#define DIGIBYTE_QT_DIGIDOLLARSTATUS_H

#include <QLabel>
#include <QString>
#include <QStyle>
#include <QVariant>
#include <QWidget>

/**
 * Shared semantic states for persistent DigiDollar messages.
 *
 * A status must never rely on colour alone. Callers keep a readable symbol and
 * action-oriented text in the label while this helper attaches the common
 * dynamic properties consumed by both the light and dark DigiDollar themes.
 */
namespace DigiDollarStatus {

enum class Kind {
    INFO,
    WAITING,
    SUCCESS,
    ACTION,
    ERR,
};

inline QString KindName(Kind kind)
{
    switch (kind) {
    case Kind::INFO:
        return QStringLiteral("info");
    case Kind::WAITING:
        return QStringLiteral("waiting");
    case Kind::SUCCESS:
        return QStringLiteral("ready");
    case Kind::ACTION:
        return QStringLiteral("action");
    case Kind::ERR:
        return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

inline void Repolish(QWidget* widget)
{
    if (!widget || !widget->style()) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

/** Apply the full card treatment to a frame or a standalone message label. */
inline void SetBanner(QWidget* widget, Kind kind)
{
    if (!widget) return;
    const QString name = KindName(kind);
    const bool changed =
        widget->property("digidollarRole").toString() != QStringLiteral("statusBanner") ||
        widget->property("statusKind").toString() != name;
    widget->setProperty("digidollarRole", QStringLiteral("statusBanner"));
    widget->setProperty("statusKind", name);
    if (changed) Repolish(widget);
}

/** Apply status text colour without turning a compact field hint into a card. */
inline void SetText(QLabel* label, Kind kind)
{
    if (!label) return;
    const QString name = KindName(kind);
    const bool changed =
        label->property("digidollarRole").toString() != QStringLiteral("statusText") ||
        label->property("statusKind").toString() != name;
    label->setProperty("digidollarRole", QStringLiteral("statusText"));
    label->setProperty("statusKind", name);
    if (changed) Repolish(label);
}

} // namespace DigiDollarStatus

#endif // DIGIBYTE_QT_DIGIDOLLARSTATUS_H

// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_PAYMASTERAMOUNT_H
#define DIGIBYTE_QT_PAYMASTERAMOUNT_H

#include <digidollar/amount.h>
#include <qt/digibyteunits.h>

#include <QLocale>
#include <QSpinBox>
#include <QWheelEvent>

/** App-standard monetary display: decimal point and locale-independent thin
 * space grouping, with integer units throughout. Unit::uDGB has the same two
 * decimal places as DD cents; only its numeric formatter is reused here. */
inline QString PaymasterFormatDD(qint64 cents)
{
    return DigiByteUnits::format(DigiByteUnits::Unit::uDGB, cents);
}

inline QString PaymasterFormatDGB(qint64 satoshis)
{
    return DigiByteUnits::format(DigiByteUnits::Unit::DGB, satoshis);
}

/** Display DD while retaining integer-cent values/signals and RPC limits.
 * The decimal point follows the app's monetary format. Grouping and extra precision are
 * rejected instead of silently interpreting or rounding an authorization. */
class PaymasterAmountSpinBox final : public QSpinBox
{
public:
    explicit PaymasterAmountSpinBox(QWidget* parent) : QSpinBox(parent)
    {
        setLocale(QLocale::c());
        setRange(0, DigiDollar::MAX_DD_RPC_AMOUNT_CENTS);
        setSuffix(QStringLiteral(" DD"));
    }

protected:
    QString textFromValue(int value) const override
    {
        return QString::fromStdString(DigiDollar::FormatDDAmountDollars(value));
    }

    int valueFromText(const QString& text) const override
    {
        const auto parsed = parse(text);
        return parsed.ok() && parsed.cents >= minimum() ? static_cast<int>(parsed.cents) : value();
    }

    QValidator::State validate(QString& text, int&) const override
    {
        const QString number = numberText(text);
        if (number.isEmpty()) return QValidator::Intermediate;
        const bool trailing_decimal = number.endsWith(QLatin1Char('.'));
        const auto parsed = DigiDollar::ParseDDAmount(
            (trailing_decimal ? number + QLatin1Char('0') : number).toStdString(),
            DigiDollar::DDAmountUnit::DOLLARS, maximum());
        if (!parsed.ok()) return QValidator::Invalid;
        return trailing_decimal || parsed.cents < minimum() ? QValidator::Intermediate : QValidator::Acceptable;
    }

    // QSpinBox's default fixup removes grouping characters. For a spending
    // ceiling an invalid input must never be repaired into a larger amount.
    void fixup(QString&) const override {}

    void wheelEvent(QWheelEvent* event) override { event->ignore(); }

private:
    QString numberText(QString text) const
    {
        text = text.trimmed();
        if (text.endsWith(suffix())) text.chop(suffix().size());
        return text.trimmed();
    }

    DigiDollar::DDAmountParseResult parse(const QString& text) const
    {
        return DigiDollar::ParseDDAmount(numberText(text).toStdString(),
                                         DigiDollar::DDAmountUnit::DOLLARS, maximum());
    }
};

/** Effective percentage of the recipient amount, rounded for display only.
 * All inputs are integer cents; this never changes a fee or spending limit. */
inline QString PaymasterEffectivePercent(qint64 fee_cents, qint64 recipient_cents)
{
    if (fee_cents < 0 || fee_cents > DigiDollar::MAX_DD_RPC_AMOUNT_CENTS ||
        recipient_cents <= 0 || recipient_cents > DigiDollar::MAX_DD_RPC_AMOUNT_CENTS) return {};
    const qint64 hundredths = (fee_cents * 10000 + recipient_cents / 2) / recipient_cents;
    return QString::number(hundredths / 100) + QLatin1Char('.') +
           QStringLiteral("%1").arg(hundredths % 100, 2, 10, QLatin1Char('0'));
}

#endif // DIGIBYTE_QT_PAYMASTERAMOUNT_H

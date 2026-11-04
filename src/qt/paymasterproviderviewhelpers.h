// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_PAYMASTERPROVIDERVIEWHELPERS_H
#define DIGIBYTE_QT_PAYMASTERPROVIDERVIEWHELPERS_H
#include <paymaster/setup.h>
#include <qt/paymasterproviderview.h>
#include <qt/paymasteroperation.h>
#include <qt/paymasteramount.h>

#include <qt/digidollaroverviewwidget.h>
#include <qt/digidollarreceivewidget.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollarmintwidget.h>
#include <qt/optionsmodel.h>
#include <qt/digidollarredeemwidget.h>
#include <qt/digidollarpositionswidget.h>
#include <qt/digidollartransactionswidget.h>
#include <qt/walletmodel.h>
#include <qt/paymasterwallet.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFocusEvent>
#include <QFormLayout>
#include <QElapsedTimer>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QSignalBlocker>
#include <QLocale>
#include <QMessageBox>
#include <QMap>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>
#include <QWizard>
#include <QWizardPage>

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <interfaces/node.h>
#include <netbase.h>
#include <node/context.h>
#include <paymaster/directory.h>
#include <paymaster/types.h>
#include <util/strencodings.h>
#include <validation.h>
#include <versionbits.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <utility>


namespace PaymasterQt::ProviderViews {

constexpr size_t MAX_PAYMASTER_RPC_ARRAY_ENTRIES{128};
constexpr size_t MAX_PAYMASTER_RPC_TEXT_BYTES{1024};
constexpr size_t MAX_PAYMASTER_POOL_ENTRIES{8192};
constexpr size_t MAX_PAYMASTER_FINANCE_DAYS{366};
constexpr size_t MAX_PAYMASTER_FINANCE_PAGE_EVENTS{250};
constexpr qint64 DEFAULT_PAYMASTER_PREPARATION_FEE{50000000};

enum class PaymasterSetupMode {
    UNDECIDED,
    GUIDED,
    EXPERT,
    EXISTING,
};

QString PaymasterSetupModeSettingsKey(const WalletModel* model);

/** Read-only fee calculation: the example amount is never saved to policy. */
class PaymasterFeeExample final : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(PaymasterFeeExample)

public:
    explicit PaymasterFeeExample(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        m_amount = new PaymasterAmountSpinBox(this);
        m_amount->setObjectName("paymasterFeeExampleAmount");
        m_amount->setMinimum(100);
        m_amount->setValue(200);
        m_result = new QLabel(this);
        m_result->setObjectName("paymasterFeeExampleResult");
        m_result->setTextFormat(Qt::PlainText);
        m_result->setWordWrap(true);
        m_result->setToolTip(tr("Example only; this does not change your payment range or budgets. Core rounds the service fee up to a whole DD cent, so the effective percentage can exceed the advertised rate for small payments."));
        layout->addWidget(m_result);
        connect(m_amount, qOverload<int>(&QSpinBox::valueChanged), this, [this] { updateResult(); });
    }

    void addToForm(QFormLayout* form)
    {
        auto* label = new QLabel(tr("Example recipient amount:"), form->parentWidget());
        m_amount->setAccessibleName(label->text());
        label->setBuddy(m_amount);
        form->addRow(label, m_amount);
        form->addRow(QString{}, this);
    }

    void setFeeRate(int basis_points, bool user_paid,
                    qint64 maximum_service_fee_cents = 0)
    {
        m_basis_points = basis_points;
        m_user_paid = user_paid;
        m_maximum_service_fee_cents = maximum_service_fee_cents;
        updateResult();
    }

private:
    void updateResult()
    {
        m_amount->setEnabled(m_user_paid);
        if (!m_user_paid) {
            m_result->setText(tr("User-paid pricing is inactive. Sponsored transfers charge no DD service fee."));
            return;
        }
        const auto fee = DigiDollar::Paymaster::ComputePaymasterFee(
            DigiDollar::Paymaster::DDCents{m_amount->value()}, m_basis_points,
            DigiDollar::Paymaster::DDCents{m_maximum_service_fee_cents});
        if (!fee) {
            m_result->setText(tr("No valid fee for this example. Check the rate (0.10% steps) and the maximum total amount."));
            return;
        }
        m_result->setText(tr("Service fee: %1 DD · effective ≈ %2% · total: %3 DD")
                              .arg(PaymasterFormatDD(fee->value),
                                   PaymasterEffectivePercent(fee->value, m_amount->value()),
                                   PaymasterFormatDD(m_amount->value() + fee->value)));
    }

    PaymasterAmountSpinBox* m_amount;
    QLabel* m_result;
    int m_basis_points{0};
    qint64 m_maximum_service_fee_cents{0};
    bool m_user_paid{false};
};

class NoWheelSpinBox final : public QSpinBox
{
public:
    explicit NoWheelSpinBox(QWidget* parent) : QSpinBox(parent) { setLocale(QLocale::c()); }

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelDoubleSpinBox final : public QDoubleSpinBox
{
public:
    explicit NoWheelDoubleSpinBox(QWidget* parent) : QDoubleSpinBox(parent) { setLocale(QLocale::c()); }

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelComboBox final : public QComboBox
{
public:
    explicit NoWheelComboBox(QWidget* parent) : QComboBox(parent) {}

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

/** Scaled display with exact integer RPC values. Qt's default QSpinBox
 * validator only accepts whole display values, so parsing and validation must
 * share the same app-standard decimal representation. */
class NoWheelScaledSpinBox final : public QSpinBox
{
public:
    NoWheelScaledSpinBox(int scale, int decimals, QWidget* parent)
        : QSpinBox(parent), m_scale(scale), m_decimals(decimals)
    {
        setLocale(QLocale::c());
    }

protected:
    QString textFromValue(int value) const override
    {
        return QString::number(value / m_scale) + QLatin1Char('.') +
               QString::number(value % m_scale).rightJustified(m_decimals, QLatin1Char('0'));
    }

    int valueFromText(const QString& text) const override
    {
        const auto parsed = parse(text);
        return parsed && *parsed >= minimum() ? *parsed : value();
    }

    QValidator::State validate(QString& text, int&) const override
    {
        const QString number = numberText(text);
        if (number.isEmpty() || number == QLatin1Char('.')) return QValidator::Intermediate;
        const auto parsed = parse(text);
        if (!parsed) return QValidator::Invalid;
        return *parsed < minimum() ? QValidator::Intermediate : QValidator::Acceptable;
    }

    // Invalid precision or grouping must not be repaired into another limit.
    void fixup(QString&) const override {}

    void focusOutEvent(QFocusEvent* event) override
    {
        const QString draft = lineEdit()->text();
        const bool invalid = !hasAcceptableInput();
        QSpinBox::focusOutEvent(event);
        // Clicking Save changes focus before its clicked handler runs. Keep
        // malformed text available for that handler to reject and explain.
        if (invalid) lineEdit()->setText(draft);
    }

    void wheelEvent(QWheelEvent* event) override { event->ignore(); }

private:
    QString numberText(QString text) const
    {
        text = text.trimmed();
        if (!suffix().isEmpty() && text.endsWith(suffix())) text.chop(suffix().size());
        return text.trimmed();
    }

    std::optional<int> parse(const QString& text) const
    {
        const QString number = numberText(text);
        if (number.startsWith(QLatin1Char('-'))) return std::nullopt;
        const QStringList parts = number.split(QLatin1Char('.'));
        if (parts.size() > 2 || (parts.size() == 1 && parts.front().isEmpty())) return std::nullopt;
        bool ok{false};
        for (const QChar digit : parts.front()) {
            if (digit < QLatin1Char('0') || digit > QLatin1Char('9')) return std::nullopt;
        }
        const qint64 whole = parts.front().isEmpty() ? 0 : parts.front().toLongLong(&ok);
        if ((!parts.front().isEmpty() && !ok) || whole < 0 || whole > maximum() / m_scale) return std::nullopt;
        const QString fraction = parts.size() == 2 ? parts.back() : QString{};
        if (fraction.size() > m_decimals) return std::nullopt;
        qint64 fractional{0};
        for (const QChar digit : fraction) {
            if (digit < QLatin1Char('0') || digit > QLatin1Char('9')) return std::nullopt;
            fractional = fractional * 10 + digit.digitValue();
        }
        for (int i = fraction.size(); i < m_decimals; ++i) fractional *= 10;
        const qint64 scaled = whole * m_scale + fractional;
        if (scaled > maximum()) return std::nullopt;
        return static_cast<int>(scaled);
    }

    const int m_scale;
    const int m_decimals;
};

/** Keep independently framed sections on the same label/field columns. */
void AlignPaymasterFormLabels(const QList<QFormLayout*>& forms);

class DgbAmountLineEdit final : public QLineEdit
{
public:
    explicit DgbAmountLineEdit(qint64 satoshis, QWidget* parent)
        : QLineEdit(parent)
    {
        setMaxLength(24);
        setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9]{1,11}([.][0-9]{0,8})?")), this));
        setSatoshis(satoshis);
    }

    bool satoshis(qint64& value) const
    {
        QString amount = text().trimmed();
        const QStringList parts = amount.split('.');
        if (parts.isEmpty() || parts.size() > 2) return false;
        bool whole_ok{false};
        const qint64 whole = parts.at(0).toLongLong(&whole_ok);
        if (!whole_ok || whole < 0 || whole > 92233720368LL) return false;
        QString fraction = parts.size() == 2 ? parts.at(1) : QString{};
        if (fraction.size() > 8) return false;
        fraction = fraction.leftJustified(8, QLatin1Char('0'));
        bool fraction_ok{true};
        const qint64 fractional = fraction.isEmpty() ? 0 : fraction.toLongLong(&fraction_ok);
        if (!fraction_ok) return false;
        if (whole > (std::numeric_limits<qint64>::max() - fractional) / 100000000) return false;
        value = whole * COIN + fractional;
        return MoneyRange(value);
    }

    void setSatoshis(qint64 satoshis)
    {
        const qint64 whole = satoshis / COIN;
        const qint64 fractional = satoshis % COIN;
        setText(QStringLiteral("%1.%2")
                    .arg(whole)
                    .arg(fractional, 8, 10, QLatin1Char('0')));
    }
};

/** Shared draft editor for operator settings and the setup wizard. Selecting
 * a preset changes this form only; the owning workflow obtains approval. */
class PaymasterReserveEditor final : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(PaymasterReserveEditor)

public:
    PaymasterReserveEditor(QWidget* parent, const QString& prefix,
                           std::array<QSpinBox*, 4> targets,
                           std::array<DgbAmountLineEdit*, 3> fees,
                           std::function<bool()> user_paid,
                           std::function<qint64()> payment_fee)
        : QWidget(parent), m_targets(targets), m_fees(fees),
          m_user_paid(std::move(user_paid)), m_payment_fee(std::move(payment_fee))
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* form = new QFormLayout;
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        m_preset = new NoWheelComboBox(this);
        m_preset->setObjectName(prefix + "ReservePreset");
        m_preset->addItem(tr("Small — reserves for 1 payment"), 1);
        m_preset->addItem(tr("Standard — reserves for 3 payments"), 3);
        m_preset->addItem(tr("Higher capacity — reserves for 6 payments"), 6);
        m_preset->addItem(tr("Manual — choose reserve counts"), 0);
        form->addRow(tr("Automatic refill target:"), m_preset);
        layout->addLayout(form);
        m_summary = new QLabel(this);
        m_summary->setObjectName(prefix + "ReservePresetSummary");
        m_summary->setTextFormat(Qt::PlainText);
        m_summary->setWordWrap(true);
        layout->addWidget(m_summary);
        m_manual = new QWidget(this);
        m_manual->setObjectName(prefix + "ManualReserveTargets");
        auto* manual_form = new QFormLayout(m_manual);
        manual_form->setContentsMargins(0, 0, 0, 0);
        manual_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        manual_form->addRow(tr("DGB capacity-check reserves:"), targets[0]);
        manual_form->addRow(tr("DGB payment reserves:"), targets[1]);
        manual_form->addRow(tr("DD capacity-check reserves:"), targets[2]);
        manual_form->addRow(tr("DD payment reserves:"), targets[3]);
        auto* explanation = new QLabel(tr("Capacity checks use three separate confirmed reserves. Extra capacity-check reserves bind more capital without increasing parallel payments. Each user-paid payment needs both a DGB reserve and a DD reserve; the smaller payment count determines capacity."), m_manual);
        explanation->setWordWrap(true);
        manual_form->addRow(explanation);
        layout->addWidget(m_manual);
        m_fee_summary = new QLabel(this);
        m_fee_summary->setObjectName(prefix + "RefillSuggestion");
        m_fee_summary->setTextFormat(Qt::PlainText);
        m_fee_summary->setWordWrap(true);
        layout->addWidget(m_fee_summary);
        m_suggest_fees = new QPushButton(tr("Use suggested refill limits"), this);
        m_suggest_fees->setObjectName(prefix + "SuggestRefillLimits");
        m_suggest_fees->setToolTip(tr("Replace the three refill fee limits in this draft. Saving and approving them is a separate step."));
        layout->addWidget(m_suggest_fees, 0, Qt::AlignLeft);
        connect(m_suggest_fees, &QPushButton::clicked, this, [this] {
            applyFees(suggestion());
            synchronize();
        });
        connect(m_preset, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            const int capacity = m_preset->currentData().toInt();
            m_manual_selected = capacity == 0;
            if (capacity)
                applyPreset(capacity);
            else
                synchronize();
        });
        for (auto* target : m_targets) {
            connect(target, qOverload<int>(&QSpinBox::valueChanged), this, [this] { synchronize(); });
        }
        for (auto* fee : m_fees) {
            connect(fee, &QLineEdit::textChanged, this, [this] { synchronize(); });
        }
        synchronize(true);
    }

    void applyPreset(int capacity)
    {
        const auto values = DigiDollar::Paymaster::SetupLiquidityPreset(m_user_paid(), capacity);
        m_applying = true;
        m_manual_selected = false;
        const std::array<const char*, 4> keys{"target_admission_dgb", "target_operational_dgb", "target_admission_carriers", "target_operational_carriers"};
        for (size_t i = 0; i < keys.size(); ++i)
            m_targets[i]->setValue(values.find_value(keys[i]).getInt<int>());
        applyFees(values);
        m_applying = false;
        synchronize(true);
    }

    QLabel* summaryLabel() const { return m_summary; }

    void addManualHelp(QWidget* widget)
    {
        static_cast<QFormLayout*>(m_manual->layout())->addRow(widget);
    }

    void synchronize(bool classify_saved = false)
    {
        if (m_applying) return;
        if (classify_saved) m_manual_selected = false;
        const bool paid = m_user_paid();
        const int operational = m_targets[1]->value();
        const bool matched = !m_manual_selected && m_targets[0]->value() == 3 &&
                             m_targets[2]->value() == (paid ? 3 : 0) && m_targets[3]->value() == (paid ? operational : 0) &&
                             (operational == 1 || operational == 3 || operational == 6);
        const QSignalBlocker blocker(m_preset);
        m_preset->setCurrentIndex(m_preset->findData(matched ? operational : 0));
        m_manual->setVisible(!matched);
        const int capacity = paid ? std::min(operational, m_targets[3]->value()) : operational;
        const qint64 admission_capital = 10000000LL * m_targets[0]->value();
        const qint64 payment_value = std::max<qint64>(10000000, m_payment_fee());
        const bool capital_fits = payment_value <= (MAX_MONEY - admission_capital) / operational;
        const QString dgb_capital = capital_fits ? compactDgb(admission_capital + payment_value * operational) : tr("exceeds the supported amount range");
        QString summary = tr("Refill target: keep reserves for %1 payment(s).\nMinimum target capital: %2 DGB + %3 DD, before fees. Existing usable reserves count toward this target.")
                              .arg(capacity)
                              .arg(dgb_capital)
                              .arg(PaymasterFormatDD(100LL * (m_targets[2]->value() + m_targets[3]->value())));
        summary += QLatin1Char('\n') + tr("Payments require confirmed reserves and sufficient budgets.");
        if (paid && (m_targets[2]->value() < 3 || m_targets[3]->value() < 1))
            summary += QLatin1Char('\n') + tr("User-paid service needs at least three DD capacity checks and one DD payment reserve.");
        m_summary->setText(summary);
        m_summary->setToolTip(tr("This target controls refill, not the maximum number of payments. Lowering it offers a separate review of excess reserves before capital is released."));
        const auto values = suggestion();
        const std::array<const char*, 3> keys{"maximum_maintenance_fee_per_transaction_satoshis", "maximum_maintenance_fee_per_hour_satoshis", "maximum_maintenance_fee_per_day_satoshis"};
        bool below{false}, same{true};
        for (size_t i = 0; i < keys.size(); ++i) {
            qint64 current{0};
            const bool valid = m_fees[i]->satoshis(current);
            const auto proposed = values.find_value(keys[i]).getInt<int64_t>();
            below |= !valid || current < proposed;
            same &= valid && current == proposed;
        }
        QString text = tr("Suggested refill limits: %1 DGB per transaction / %2 DGB per hour / %3 DGB per day.")
                           .arg(compactDgb(values.find_value(keys[0]).getInt<int64_t>()))
                           .arg(compactDgb(values.find_value(keys[1]).getInt<int64_t>()))
                           .arg(compactDgb(values.find_value(keys[2]).getInt<int64_t>()));
        if (below) text += QLatin1Char('\n') + tr("The displayed limits are below this suggestion. A refill may pause for cost approval; limits never increase automatically.");
        m_fee_summary->setText(text);
        m_fee_summary->setVisible(!same);
        m_suggest_fees->setVisible(!same);
    }

private:
    static QString compactDgb(qint64 value)
    {
        QString text = PaymasterFormatDGB(value);
        while (text.endsWith(QLatin1Char('0')))
            text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
        return text;
    }

    UniValue suggestion() const
    {
        // Manual admission surpluses also add transaction outputs.
        const int size = std::max({m_targets[1]->value(), m_targets[3]->value(),
                                   m_targets[0]->value() - 2, m_targets[2]->value() - 2});
        return DigiDollar::Paymaster::SetupLiquidityPreset(m_user_paid(), size);
    }

    void applyFees(const UniValue& values)
    {
        const std::array<const char*, 3> keys{"maximum_maintenance_fee_per_transaction_satoshis", "maximum_maintenance_fee_per_hour_satoshis", "maximum_maintenance_fee_per_day_satoshis"};
        for (size_t i = 0; i < keys.size(); ++i)
            m_fees[i]->setSatoshis(values.find_value(keys[i]).getInt<int64_t>());
    }

    std::array<QSpinBox*, 4> m_targets;
    std::array<DgbAmountLineEdit*, 3> m_fees;
    std::function<bool()> m_user_paid;
    std::function<qint64()> m_payment_fee;
    QComboBox* m_preset;
    QWidget* m_manual;
    QLabel* m_summary;
    QLabel* m_fee_summary;
    QPushButton* m_suggest_fees;
    bool m_manual_selected{false};
    bool m_applying{false};
};

class PaymasterDisplayNameValidator final : public QValidator
{
public:
    explicit PaymasterDisplayNameValidator(QObject* parent)
        : QValidator(parent)
    {
    }

    State validate(QString& input, int&) const override
    {
        return DigiDollar::Paymaster::IsValidPaymasterDisplayName(
                   input.toStdString())
            ? Acceptable
            : Invalid;
    }
};

class PaymasterSetupWizard final : public QWizard
{
public:
    explicit PaymasterSetupWizard(QWidget* parent) : QWizard(parent) {}

    void setCloseBlocked(bool blocked) { m_close_blocked = blocked; }

public:
    void reject() override
    {
        if (!m_close_blocked) QWizard::reject();
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (m_close_blocked) {
            event->ignore();
            return;
        }
        QWizard::closeEvent(event);
    }

private:
    bool m_close_blocked{false};
};

class CompletableWizardPage : public QWizardPage
{
public:
    explicit CompletableWizardPage(QWidget* parent = nullptr) : QWizardPage(parent) {}

    bool isComplete() const override { return m_complete; }

    void setComplete(bool complete)
    {
        if (m_complete == complete) return;
        m_complete = complete;
        Q_EMIT completeChanged();
    }

private:
    bool m_complete{false};
};

class ValidatedWizardPage final : public CompletableWizardPage
{
public:
    explicit ValidatedWizardPage(QWidget* parent = nullptr) : CompletableWizardPage(parent) {}

    void setValidator(std::function<bool()> validator)
    {
        m_validator = std::move(validator);
    }

    bool validatePage() override
    {
        return !m_validator || m_validator();
    }

private:
    std::function<bool()> m_validator;
};

void ConfigureScrollArea(QScrollArea* scroll, QWidget* contents);

QWidget* CreatePaymasterPageColumn(QWidget* page, QVBoxLayout*& layout);

void AddPaymasterPageHeading(QVBoxLayout* layout, QWidget* parent,
                             const QString& title, const QString& description,
                             const QString& object_prefix);

QPushButton* AddPaymasterDisclosure(QVBoxLayout* layout, QWidget* parent,
                                    QWidget* panel, const QString& show_text,
                                    const QString& hide_text,
                                    const QString& object_name,
                                    bool initially_open = false);

QFrame* CreatePaymasterStatusCard(QWidget* parent, const QString& title,
                                  QLabel*& status, QPushButton*& action);

class PaymasterResponsiveCards final : public QWidget
{
public:
    explicit PaymasterResponsiveCards(QWidget* parent = nullptr, int max_columns = 2)
        : QWidget(parent), m_layout(new QGridLayout(this)), m_max_columns(max_columns)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setHorizontalSpacing(12);
        m_layout->setVerticalSpacing(12);
    }

    void addCard(QWidget* card)
    {
        m_cards.push_back(card);
        relayout();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        relayout();
    }

private:
    void relayout()
    {
        const int columns = m_max_columns == 3 && width() >= 990 ? 3 : width() > 0 && width() < 760 ? 1 :
                                                                                                      2;
        for (QWidget* card : m_cards) m_layout->removeWidget(card);
        for (int index = 0; index < m_cards.size(); ++index) {
            m_layout->addWidget(m_cards.at(index), index / columns,
                                index % columns);
        }
        for (int column = 0; column < m_max_columns; ++column) {
            m_layout->setColumnStretch(column, column < columns ? 1 : 0);
        }

        // A responsive grid changes its row count after the page receives its
        // final width. Keep that new minimum height visible to the surrounding
        // page layout; otherwise Qt can retain the old, shorter allocation and
        // the cards paint over the widget that follows the grid.
        m_layout->invalidate();
        const int required_height = m_layout->minimumSize().height();
        if (minimumHeight() != required_height) {
            setMinimumHeight(required_height);
            updateGeometry();
        }
    }

    QGridLayout* const m_layout;
    QList<QWidget*> m_cards;
    const int m_max_columns;
};

bool UseDarkPaymasterWizardTheme(const WalletModel* model, const QWidget* widget);

using GuidedPaymasterSafetyLimits = DigiDollar::Paymaster::SetupSafetyLimits;
GuidedPaymasterSafetyLimits GuidedSafetyLimits(bool conservative, qint64 advertised_network_fee);

bool GetInt64Field(const UniValue& object, const char* name, qint64& value);

bool HasInt64Fields(const UniValue& object,
                    std::initializer_list<const char*> names);

bool GetIntField(const UniValue& object, const char* name, int& value);

bool HasIntFields(const UniValue& object,
                  std::initializer_list<const char*> names);

bool IsStringArray(const UniValue& value);

bool IsEnumString(const UniValue& value,
                  std::initializer_list<const char*> allowed);

bool IsBoundedOptionalString(const UniValue& value, size_t maximum,
                             bool allow_empty = false);

bool IsCompleteProviderPolicy(const UniValue& policy);

bool IsCompleteFundingSafety(const UniValue& limits);

bool IsCompleteProviderSafetyPolicy(const UniValue& policy);

bool IsCompleteLiquidityPolicy(const UniValue& policy);

bool IsHex256Field(const UniValue& object, const char* name);

bool IsCompleteLiquiditySlotStatus(const UniValue& status);

bool IsCompleteLiquidityStatus(const UniValue& result);

bool IsCompleteProviderPoolInfo(const UniValue& result);

bool SameNumericField(const UniValue& expected, const UniValue& actual,
                      const char* name);

bool SameNumericFields(const UniValue& expected, const UniValue& actual,
                       std::initializer_list<const char*> names);

bool SameBooleanField(const UniValue& expected, const UniValue& actual,
                      const char* name);

bool SameStringField(const UniValue& expected, const UniValue& actual,
                     const char* name);

bool SameStringSet(const UniValue& expected, const UniValue& actual,
                   const char* name);

bool IsNonNullHex256(const UniValue& value);

bool IsHex256Field(const UniValue& object, const char* name);

bool IsExplicitlyStoppedProvider(const UniValue& provider);

bool IsExactProviderPolicyAcknowledgement(const UniValue& requested,
                                          const UniValue& persisted);

bool IsExactFundingSafetyAcknowledgement(const UniValue& requested,
                                         const UniValue& persisted);

bool IsExactProviderSafetyAcknowledgement(const UniValue& requested,
                                          const UniValue& persisted);

bool IsExactLiquidityPolicyAcknowledgement(const UniValue& requested,
                                           const UniValue& persisted);

bool IsCompletePoolPreparationResult(const UniValue& result);

bool IsCompletePoolRebalanceResult(const UniValue& result);

bool IsBoundedReserveReductionResult(const UniValue& result, const UniValue& options, const UniValue& reviewed = {});

bool IsReserveReductionFeeProposal(const UniValue& result);

bool IsCompleteProviderPool(const UniValue& pool);

bool IsCompleteProviderRuntimeStatus(const UniValue& result);

bool IsCompleteProviderInfoSnapshot(const UniValue& result);

bool IsCompleteBackupAcknowledgement(const UniValue& result);

bool IsCompleteFinanceSummary(const UniValue& summary);

bool IsCompleteFinanceStatus(const UniValue& result,
                             const QString& expected_period);

// The overview requests no event history. Core deliberately omits events and
// daily_totals in that mode, so validate the fields we actually display.
bool IsCompleteOverviewFinance(const UniValue& result);

bool IsCompleteCarrierWithdrawalResult(const UniValue& result,
                                       const QString& expected_mode,
                                       bool expected_execution,
                                       const QString& expected_plan);

bool IsCompleteProviderStartResult(const UniValue& result);

bool IsCompleteClientSafetyPolicy(const UniValue& policy);

bool IsCompleteClientSafetyStatus(const UniValue& result);

QString PaymasterWizardStyleSheet(bool dark_theme);
} // namespace PaymasterQt::ProviderViews
#endif

// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollartab.h>

#include <qt/digidollaroverviewwidget.h>
#include <qt/digidollarreceivewidget.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollarmintwidget.h>
#include <qt/optionsmodel.h>
#include <qt/digidollarredeemwidget.h>
#include <qt/digidollarpositionswidget.h>
#include <qt/digidollartransactionswidget.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>

#include <QTabWidget>
#include <QApplication>
#include <QTabBar>
#include <QVBoxLayout>
#include <QTimer>
#include <QLabel>
#include <QStackedWidget>
#include <QFont>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QTextStream>
#include <QWizard>
#include <QWizardPage>
#include <QWheelEvent>

#include <chainparams.h>
#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <paymaster/directory.h>
#include <validation.h>
#include <versionbits.h>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <memory>

namespace {
enum class PaymasterSetupMode {
    UNDECIDED,
    GUIDED,
    EXPERT,
    EXISTING,
};

QString PaymasterSetupModeSettingsKey(const WalletModel* model)
{
    if (!model) return {};
    const QByteArray wallet_identity =
        QByteArray::number(static_cast<int>(Params().GetChainType())) + '\0' +
        model->getWalletName().toUtf8();
    const QByteArray wallet_id = QCryptographicHash::hash(
        wallet_identity, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("Paymaster/Wallet/%1/SetupMode")
        .arg(QString::fromLatin1(wallet_id));
}

class NoWheelSpinBox final : public QSpinBox
{
public:
    explicit NoWheelSpinBox(QWidget* parent) : QSpinBox(parent) {}

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelDoubleSpinBox final : public QDoubleSpinBox
{
public:
    explicit NoWheelDoubleSpinBox(QWidget* parent) : QDoubleSpinBox(parent) {}

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

class NoWheelScaledSpinBox final : public QSpinBox
{
public:
    NoWheelScaledSpinBox(double scale, int decimals, QWidget* parent)
        : QSpinBox(parent), m_scale(scale), m_decimals(decimals)
    {
    }

protected:
    QString textFromValue(int value) const override
    {
        return locale().toString(value / m_scale, 'f', m_decimals);
    }

    int valueFromText(const QString& text) const override
    {
        QString number = text;
        number.remove(suffix());
        bool ok{false};
        const double value = locale().toDouble(number.trimmed(), &ok);
        if (!ok) return minimum();
        return qBound(minimum(), qRound(value * m_scale), maximum());
    }

    void wheelEvent(QWheelEvent* event) override { event->ignore(); }

private:
    const double m_scale;
    const int m_decimals;
};

class DgbAmountLineEdit final : public QLineEdit
{
public:
    explicit DgbAmountLineEdit(qint64 satoshis, QWidget* parent)
        : QLineEdit(parent)
    {
        setMaxLength(24);
        setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9]{1,11}([.,][0-9]{0,8})?")), this));
        setSatoshis(satoshis);
    }

    bool satoshis(qint64& value) const
    {
        QString amount = text().trimmed();
        amount.replace(',', '.');
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
        value = whole * 100000000 + fractional;
        return true;
    }

    void setSatoshis(qint64 satoshis)
    {
        setText(QString::number(satoshis / 100000000.0, 'f', 8));
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

void ConfigurePaymasterScrollArea(QScrollArea* scroll, QWidget* contents)
{
    // Call this after QScrollArea::setWidget(). Qt deliberately enables
    // autoFillBackground on an inserted widget, which would otherwise restore
    // the native Windows window colour on top of the active QSS theme.
    scroll->setFrameShape(QFrame::NoFrame);
    // Do not fill the viewport from the native Windows palette. The Paymaster
    // pages are themed through QSS; forcing QPalette::Window here otherwise
    // leaves a bright surface behind dark-theme controls.
    scroll->setAttribute(Qt::WA_StyledBackground, true);
    scroll->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    contents->setAttribute(Qt::WA_StyledBackground, true);
    scroll->viewport()->setAutoFillBackground(false);
    contents->setAutoFillBackground(false);
}

QWidget* CreatePaymasterPageColumn(QWidget* page, QVBoxLayout*& layout)
{
    auto* shell = new QHBoxLayout(page);
    shell->setContentsMargins(18, 14, 18, 18);
    shell->setSpacing(0);
    auto* column = new QWidget(page);
    column->setObjectName(QStringLiteral("paymasterPageColumn"));
    column->setMaximumWidth(1100);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    shell->addStretch(1);
    shell->addWidget(column, 8);
    shell->addStretch(1);
    return column;
}

void AddPaymasterPageHeading(QVBoxLayout* layout, QWidget* parent,
                             const QString& title, const QString& description,
                             const QString& object_prefix)
{
    auto* heading = new QLabel(title, parent);
    heading->setObjectName(object_prefix + QStringLiteral("Heading"));
    heading->setProperty("paymasterRole", QStringLiteral("pageHeading"));
    auto* help = new QLabel(description, parent);
    help->setObjectName(object_prefix + QStringLiteral("Introduction"));
    help->setProperty("paymasterRole", QStringLiteral("pageIntroduction"));
    help->setWordWrap(true);
    layout->addWidget(heading);
    layout->addWidget(help);
}

QPushButton* AddPaymasterDisclosure(QVBoxLayout* layout, QWidget* parent,
                                    QWidget* panel, const QString& show_text,
                                    const QString& hide_text,
                                    const QString& object_name,
                                    bool initially_open = false)
{
    auto* button = new QPushButton(initially_open ? hide_text : show_text, parent);
    button->setObjectName(object_name);
    button->setProperty("paymasterRole", QStringLiteral("disclosure"));
    button->setCheckable(true);
    button->setChecked(initially_open);
    button->setAccessibleName(show_text);
    panel->setVisible(initially_open);
    layout->addWidget(button, 0, Qt::AlignLeft);
    layout->addWidget(panel);
    QObject::connect(button, &QPushButton::toggled, panel,
                     [button, panel, show_text, hide_text](bool visible) {
                         panel->setVisible(visible);
                         button->setText(visible ? hide_text : show_text);
                     });
    return button;
}

QFrame* CreatePaymasterStatusCard(QWidget* parent, const QString& title,
                                  QLabel*& status, QPushButton*& action)
{
    auto* card = new QFrame(parent);
    card->setProperty("paymasterRole", QStringLiteral("statusCard"));
    card->setMinimumHeight(142);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(16, 14, 16, 14);
    card_layout->setSpacing(8);
    auto* heading = new QLabel(title, card);
    heading->setProperty("paymasterRole", QStringLiteral("cardHeading"));
    status = new QLabel(QObject::tr("Checking…"), card);
    status->setProperty("paymasterRole", QStringLiteral("statusText"));
    status->setWordWrap(true);
    action = new QPushButton(QObject::tr("Review"), card);
    action->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    action->setCursor(Qt::PointingHandCursor);
    card_layout->addWidget(heading);
    card_layout->addWidget(status);
    card_layout->addStretch();
    card_layout->addWidget(action, 0, Qt::AlignLeft);
    return card;
}

class PaymasterResponsiveCards final : public QWidget
{
public:
    explicit PaymasterResponsiveCards(QWidget* parent = nullptr)
        : QWidget(parent), m_layout(new QGridLayout(this))
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
        const int columns = width() > 0 && width() < 760 ? 1 : 2;
        for (QWidget* card : m_cards) m_layout->removeWidget(card);
        for (int index = 0; index < m_cards.size(); ++index) {
            m_layout->addWidget(m_cards.at(index), index / columns,
                                index % columns);
        }
        for (int column = 0; column < 2; ++column) {
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
};

bool UseDarkPaymasterWizardTheme(const WalletModel* model, const QWidget* widget)
{
    if (model && model->getOptionsModel()) {
        const QString theme = model->getOptionsModel()->data(
            model->getOptionsModel()->index(OptionsModel::Theme), Qt::EditRole).toString();
        return theme.isEmpty() || theme == QLatin1String("dark");
    }
    const QPalette palette = widget ? widget->palette() : QPalette{};
    return palette.color(QPalette::Window).lightness() < 128;
}

QString PaymasterWizardStyleSheet(bool dark_theme)
{
    if (dark_theme) {
        return QStringLiteral(
            "QWizard#PaymasterSetupWizard,"
            "QWizard#PaymasterSetupWizard QWizardPage {"
            "  background-color: #0b2419;"
            "  color: #ffffff;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel {"
            "  background-color: transparent;"
            "  color: #ffffff;"
            "}"
            "QWizard#PaymasterSetupWizard QFrame#paymasterSetupFieldHelp,"
            "QWizard#PaymasterSetupWizard QFrame#paymasterSetupLiquidityFieldHelp {"
            "  background-color: #123a29;"
            "  border: 1px solid #42d884;"
            "  border-left: 5px solid #42d884;"
            "  border-radius: 5px;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpTitle,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpTitle {"
            "  color: #ffffff;"
            "  font-size: 11pt;"
            "  font-weight: bold;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpText,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpText {"
            "  color: #c6ddcf;"
            "  font-size: 10pt;"
            "  font-weight: normal;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityPreviewDetails,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupProgressResult {"
            "  background-color: #123a29;"
            "  border: 1px solid #42d884;"
            "  border-radius: 5px;"
            "  color: #ffffff;"
            "  padding: 10px;"
            "}"
            "QWizard#PaymasterSetupWizard QLineEdit,"
            "QWizard#PaymasterSetupWizard QSpinBox,"
            "QWizard#PaymasterSetupWizard QDoubleSpinBox,"
            "QWizard#PaymasterSetupWizard QComboBox {"
            "  background-color: #113a29;"
            "  color: #ffffff;"
            "  border: 2px solid #42d884;"
            "  border-radius: 4px;"
            "  padding: 5px;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator {"
            "  width: 16px;"
            "  height: 16px;"
            "  background-color: #09261b;"
            "  border: 2px solid #42d884;"
            "  border-radius: 3px;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator:checked {"
            "  background-color: #1f9d57;"
            "  border-color: #74e5a7;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator:unchecked:hover {"
            "  background-color: #164934;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-button {"
            "  subcontrol-origin: border;"
            "  subcontrol-position: top right;"
            "  width: 24px;"
            "  background-color: #dff5e7;"
            "  border-left: 1px solid #42d884;"
            "  border-bottom: 1px solid #8bc9a6;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-button {"
            "  subcontrol-origin: border;"
            "  subcontrol-position: bottom right;"
            "  width: 24px;"
            "  background-color: #dff5e7;"
            "  border-left: 1px solid #42d884;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-arrow {"
            "  image: url(:/icons/spin_up);"
            "  width: 9px;"
            "  height: 6px;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-arrow {"
            "  image: url(:/icons/spin_down);"
            "  width: 9px;"
            "  height: 6px;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton {"
            "  background-color: #16804f;"
            "  color: #ffffff;"
            "  border: 2px solid #16804f;"
            "  border-radius: 4px;"
            "  padding: 7px 14px;"
            "  font-weight: bold;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:hover {"
            "  background-color: #11683e;"
            "  border-color: #42d884;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:focus,"
            "QWizard#PaymasterSetupWizard QLineEdit:focus,"
            "QWizard#PaymasterSetupWizard QSpinBox:focus,"
            "QWizard#PaymasterSetupWizard QDoubleSpinBox:focus,"
            "QWizard#PaymasterSetupWizard QComboBox:focus {"
            "  border-color: #f4d35e;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:disabled {"
            "  background-color: #294b3b;"
            "  border-color: #294b3b;"
            "  color: #9ab5a7;"
            "}");
    }
    return QStringLiteral(
        "QWizard#PaymasterSetupWizard,"
        "QWizard#PaymasterSetupWizard QWizardPage {"
        "  background-color: #eef9f2;"
        "  color: #123f2b;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel {"
        "  background-color: transparent;"
        "  color: #123f2b;"
        "}"
        "QWizard#PaymasterSetupWizard QFrame#paymasterSetupFieldHelp,"
        "QWizard#PaymasterSetupWizard QFrame#paymasterSetupLiquidityFieldHelp {"
        "  background-color: #ffffff;"
        "  border: 1px solid #9bcbb0;"
        "  border-left: 5px solid #1f9d57;"
        "  border-radius: 5px;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpTitle,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpTitle {"
        "  color: #123f2b;"
        "  font-size: 11pt;"
        "  font-weight: bold;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpText,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpText {"
        "  color: #416553;"
        "  font-size: 10pt;"
        "  font-weight: normal;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityPreviewDetails,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupProgressResult {"
        "  background-color: #ffffff;"
        "  border: 1px solid #9bcbb0;"
        "  border-radius: 5px;"
        "  color: #123f2b;"
        "  padding: 10px;"
        "}"
        "QWizard#PaymasterSetupWizard QLineEdit,"
        "QWizard#PaymasterSetupWizard QSpinBox,"
        "QWizard#PaymasterSetupWizard QDoubleSpinBox,"
        "QWizard#PaymasterSetupWizard QComboBox {"
        "  background-color: #ffffff;"
        "  color: #123f2b;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 5px;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator {"
        "  width: 16px;"
        "  height: 16px;"
        "  background-color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 3px;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator:checked {"
        "  background-color: #1f9d57;"
        "  border-color: #146c3a;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator:unchecked:hover {"
        "  background-color: #e8f7ed;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-button {"
        "  subcontrol-origin: border;"
        "  subcontrol-position: top right;"
        "  width: 24px;"
        "  background-color: #e8f7ed;"
        "  border-left: 1px solid #1f9d57;"
        "  border-bottom: 1px solid #9bcbb0;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-button {"
        "  subcontrol-origin: border;"
        "  subcontrol-position: bottom right;"
        "  width: 24px;"
        "  background-color: #e8f7ed;"
        "  border-left: 1px solid #1f9d57;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-arrow {"
        "  image: url(:/icons/spin_up);"
        "  width: 9px;"
        "  height: 6px;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-arrow {"
        "  image: url(:/icons/spin_down);"
        "  width: 9px;"
        "  height: 6px;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton {"
        "  background-color: #146c3a;"
        "  color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 7px 14px;"
        "  font-weight: bold;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:hover {"
        "  background-color: #0f5a30;"
        "  border-color: #0f5a30;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:focus,"
        "QWizard#PaymasterSetupWizard QLineEdit:focus,"
        "QWizard#PaymasterSetupWizard QSpinBox:focus,"
        "QWizard#PaymasterSetupWizard QDoubleSpinBox:focus,"
        "QWizard#PaymasterSetupWizard QComboBox:focus {"
        "  border-color: #8a5a00;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:disabled {"
        "  background-color: #b9d5c3;"
        "  border-color: #b9d5c3;"
        "  color: #60776a;"
        "}");
}
} // namespace

/** Wallet-scoped Paymaster operator console. Protocol and persistence remain
 * in Core; this widget is only an asynchronous presentation layer. */
class DigiDollarPaymasterWidget final : public QWidget
{
    struct FundingSafetyControls {
        QLineEdit* per_transaction{nullptr};
        QLineEdit* reserved{nullptr};
        QLineEdit* per_hour{nullptr};
        QLineEdit* per_day{nullptr};
        QSpinBox* completed_per_hour{nullptr};
        QSpinBox* completed_per_day{nullptr};
        QLabel* mode{nullptr};
    };

    struct FundingSafetyValues {
        qint64 per_transaction{0};
        qint64 reserved{0};
        qint64 per_hour{0};
        qint64 per_day{0};
        int completed_per_hour{0};
        int completed_per_day{0};

        bool allZero() const
        {
            return per_transaction == 0 && reserved == 0 && per_hour == 0 &&
                   per_day == 0 && completed_per_hour == 0 &&
                   completed_per_day == 0;
        }

        bool hasZero() const
        {
            return per_transaction == 0 || reserved == 0 || per_hour == 0 ||
                   per_day == 0 || completed_per_hour == 0 ||
                   completed_per_day == 0;
        }

    };

    struct LiquidityPolicyValues {
        bool automatic_replenishment{true};
        bool paid_maintenance_approved{false};
        int admission_dgb{3};
        int operational_dgb{1};
        int admission_carriers{0};
        int operational_carriers{0};
        qint64 fee_per_transaction{0};
        qint64 fee_per_hour{0};
        qint64 fee_per_day{0};
    };

public:
    explicit DigiDollarPaymasterWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(16, 14, 16, 16);
        outer->setSpacing(12);

        auto* operator_header = new QFrame(this);
        operator_header->setObjectName("paymasterOperatorHeader");
        operator_header->setProperty("paymasterRole", QStringLiteral("operatorHeader"));
        auto* operator_header_layout = new QVBoxLayout(operator_header);
        operator_header_layout->setContentsMargins(18, 14, 18, 14);
        operator_header_layout->setSpacing(4);
        auto* heading = new QLabel(tr("Paymaster provider"), operator_header);
        heading->setObjectName("paymasterOperatorHeading");
        heading->setProperty("paymasterRole", QStringLiteral("operatorHeading"));
        QFont heading_font = heading->font();
        heading_font.setPointSize(18);
        heading_font.setBold(true);
        heading->setFont(heading_font);
        operator_header_layout->addWidget(heading);

        m_wallet = new QLabel(tr("No wallet selected"), operator_header);
        m_wallet->setObjectName("paymasterSelectedWallet");
        m_wallet->setProperty("paymasterRole", QStringLiteral("walletHeading"));
        m_status = new QLabel(tr("Loading provider status…"), operator_header);
        m_status->setObjectName("paymasterProviderStatus");
        m_status->setProperty("paymasterRole", QStringLiteral("globalStatus"));
        m_status->setWordWrap(true);
        operator_header_layout->addWidget(m_wallet);
        operator_header_layout->addWidget(m_status);
        outer->addWidget(operator_header);

        m_tabs = new QTabWidget(this);
        m_tabs->setObjectName("paymasterOperatorTabs");
        outer->addWidget(m_tabs);

        auto* overview_scroll = new QScrollArea(m_tabs);
        overview_scroll->setObjectName("paymasterOverviewPage");
        overview_scroll->setWidgetResizable(true);
        auto* overview = new QWidget(overview_scroll);
        overview->setObjectName("paymasterOverviewContents");
        QVBoxLayout* overview_layout{nullptr};
        auto* overview_column = CreatePaymasterPageColumn(overview, overview_layout);

        m_setup_choice = new QGroupBox(tr("Set up this Paymaster wallet"), overview);
        m_setup_choice->setObjectName("paymasterSetupChoice");
        auto* setup_choice_layout = new QVBoxLayout(m_setup_choice);
        auto* setup_choice_intro = new QLabel(tr(
            "This is the first time Paymaster operator controls have been opened for this wallet. "
            "Choose how you want to configure the provider. No provider setting is changed until "
            "you explicitly save it, and no funds move without a separate transaction preview and confirmation."),
            m_setup_choice);
        setup_choice_intro->setObjectName("paymasterSetupChoiceIntroduction");
        setup_choice_intro->setWordWrap(true);
        setup_choice_layout->addWidget(setup_choice_intro);

        auto* guided_group = new QGroupBox(tr("Guided setup — recommended"), m_setup_choice);
        auto* guided_layout = new QVBoxLayout(guided_group);
        auto* guided_help = new QLabel(tr(
            "Explains each decision, checks the current provider prerequisites and prepares "
            "recommended policy, safety and liquidity values for your review."), guided_group);
        guided_help->setWordWrap(true);
        m_guided_setup = new QPushButton(tr("Start guided setup…"), guided_group);
        m_guided_setup->setObjectName("paymasterChooseGuidedSetup");
        m_guided_setup->setAccessibleDescription(tr(
            "Open the recommended step-by-step Paymaster provider setup assistant."));
        guided_layout->addWidget(guided_help);
        guided_layout->addWidget(m_guided_setup, 0, Qt::AlignLeft);
        setup_choice_layout->addWidget(guided_group);

        auto* expert_group = new QGroupBox(tr("Manual setup — experts"), m_setup_choice);
        auto* expert_layout = new QVBoxLayout(expert_group);
        auto* expert_help = new QLabel(tr(
            "Unlock all configuration pages immediately. Choose this only if you already understand "
            "provider identities, funding models, finite safety budgets and pool preparation."), expert_group);
        expert_help->setWordWrap(true);
        m_expert_setup = new QPushButton(tr("Use manual expert setup"), expert_group);
        m_expert_setup->setObjectName("paymasterChooseExpertSetup");
        m_expert_setup->setAccessibleDescription(tr(
            "Unlock the advanced Paymaster configuration pages without running the assistant."));
        expert_layout->addWidget(expert_help);
        expert_layout->addWidget(m_expert_setup, 0, Qt::AlignLeft);
        setup_choice_layout->addWidget(expert_group);
        setup_choice_layout->addStretch();
        overview_layout->addWidget(m_setup_choice);

        m_setup_content = new QWidget(overview);
        m_setup_content->setObjectName("paymasterConfiguredOverview");
        auto* configured_layout = new QVBoxLayout(m_setup_content);
        configured_layout->setContentsMargins(0, 0, 0, 0);

        auto* introduction = new QGroupBox(tr("Your provider at a glance"), overview_column);
        introduction->setObjectName("paymasterOverviewIntroduction");
        introduction->setProperty("paymasterRole", QStringLiteral("heroCard"));
        auto* introduction_layout = new QVBoxLayout(introduction);
        auto* introduction_text = new QLabel(tr(
            "Help other wallets send DigiDollar when they do not hold DGB. "
            "This wallet supplies the network fee within the limits shown below."), introduction);
        introduction_text->setObjectName("paymasterIntroduction");
        introduction_text->setWordWrap(true);
        introduction_layout->addWidget(introduction_text);
        auto* responsibility = new QLabel(tr(
            "Core validates every payment locally. Your configured spending limits remain the final financial safeguard."), introduction);
        responsibility->setObjectName("paymasterResponsibilityNotice");
        responsibility->setProperty("paymasterRole", QStringLiteral("mutedText"));
        responsibility->setWordWrap(true);
        introduction_layout->addWidget(responsibility);
        m_reopen_wizard = new QPushButton(tr("Review setup with assistant…"), introduction);
        m_reopen_wizard->setObjectName("paymasterSetupWizard");
        m_reopen_wizard->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        introduction_layout->addWidget(m_reopen_wizard, 0, Qt::AlignLeft);
        configured_layout->addWidget(introduction);

        m_overview_backup_notice = new QGroupBox(
            tr("Protect this provider identity"), overview_column);
        m_overview_backup_notice->setObjectName(
            "paymasterOverviewBackupNotice");
        m_overview_backup_notice->setProperty(
            "paymasterRole", QStringLiteral("notice"));
        m_overview_backup_notice->setProperty(
            "statusKind", QStringLiteral("waiting"));
        auto* overview_backup_layout = new QVBoxLayout(
            m_overview_backup_notice);
        auto* overview_backup_text = new QLabel(tr(
            "This provider identity, its private key, configuration, pool state and finance history are stored in this wallet. "
            "Create a new full-wallet backup and keep it offline and confidential. A seed or descriptor export alone is not a complete Paymaster backup."),
            m_overview_backup_notice);
        overview_backup_text->setWordWrap(true);
        overview_backup_layout->addWidget(overview_backup_text);
        m_overview_backup_provider_id = new QLabel(
            m_overview_backup_notice);
        m_overview_backup_provider_id->setObjectName(
            "paymasterOverviewBackupProviderId");
        m_overview_backup_provider_id->setWordWrap(true);
        m_overview_backup_provider_id->setTextInteractionFlags(
            Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        overview_backup_layout->addWidget(m_overview_backup_provider_id);
        auto* overview_backup_actions = new QHBoxLayout();
        m_overview_backup_now = new QPushButton(
            tr("Back up provider wallet now…"), m_overview_backup_notice);
        m_overview_backup_now->setObjectName("paymasterOverviewBackupNow");
        m_overview_backup_now->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        m_overview_backup_external = new QPushButton(
            tr("I use another full-wallet backup method…"),
            m_overview_backup_notice);
        m_overview_backup_external->setObjectName(
            "paymasterOverviewExternalBackup");
        overview_backup_actions->addWidget(m_overview_backup_now);
        overview_backup_actions->addWidget(m_overview_backup_external);
        overview_backup_actions->addStretch();
        overview_backup_layout->addLayout(overview_backup_actions);
        m_overview_backup_notice->hide();
        configured_layout->addWidget(m_overview_backup_notice);

        m_external_prerequisites_card = new QGroupBox(
            tr("External prerequisites"), overview_column);
        m_external_prerequisites_card->setObjectName("paymasterOverviewNextStep");
        m_external_prerequisites_card->setProperty(
            "paymasterRole", QStringLiteral("nextStepCard"));
        m_external_prerequisites_card->setProperty(
            "statusKind", QStringLiteral("waiting"));
        auto* readiness_layout = new QVBoxLayout(m_external_prerequisites_card);
        m_next_step = new QLabel(tr("Checking what is required next…"),
                                 m_external_prerequisites_card);
        m_next_step->setObjectName("paymasterNextStep");
        m_next_step->setProperty("paymasterRole", QStringLiteral("statusText"));
        m_next_step->setWordWrap(true);
        QFont next_step_font = m_next_step->font();
        next_step_font.setBold(true);
        m_next_step->setFont(next_step_font);
        m_readiness_summary = new QLabel(m_external_prerequisites_card);
        m_readiness_summary->setObjectName("paymasterReadinessSummary");
        m_readiness_summary->setWordWrap(true);
        readiness_layout->addWidget(m_next_step);
        readiness_layout->addWidget(m_readiness_summary);
        auto* prerequisite_grid = new QGridLayout();
        prerequisite_grid->setColumnStretch(1, 1);
        const auto add_prerequisite = [this, prerequisite_grid](
                                          int row, const QString& title,
                                          QLabel*& value,
                                          const QString& object_name) {
            auto* heading = new QLabel(title, m_external_prerequisites_card);
            heading->setProperty("paymasterRole", QStringLiteral("mutedText"));
            value = new QLabel(m_external_prerequisites_card);
            value->setObjectName(object_name);
            value->setWordWrap(true);
            prerequisite_grid->addWidget(heading, row, 0, Qt::AlignTop);
            prerequisite_grid->addWidget(value, row, 1);
        };
        add_prerequisite(0, tr("Blockchain synchronization"),
                         m_external_blockchain_status,
                         QStringLiteral("paymasterExternalBlockchainStatus"));
        add_prerequisite(1, tr("Transaction index"),
                         m_external_txindex_status,
                         QStringLiteral("paymasterExternalTxIndexStatus"));
        add_prerequisite(2, tr("Transactions ready"),
                         m_external_broadcast_status,
                         QStringLiteral("paymasterExternalBroadcastStatus"));
        add_prerequisite(3, tr("DigiDollar activation"),
                         m_external_activation_status,
                         QStringLiteral("paymasterExternalActivationStatus"));
        add_prerequisite(4, tr("Oracle price"), m_external_oracle_status,
                         QStringLiteral("paymasterExternalOracleStatus"));
        m_external_other_heading = new QLabel(
            tr("Additional provider requirement"),
            m_external_prerequisites_card);
        m_external_other_heading->setProperty(
            "paymasterRole", QStringLiteral("mutedText"));
        m_external_other_status = new QLabel(m_external_prerequisites_card);
        m_external_other_status->setObjectName(
            "paymasterExternalOtherStatus");
        m_external_other_status->setWordWrap(true);
        prerequisite_grid->addWidget(m_external_other_heading, 5, 0,
                                     Qt::AlignTop);
        prerequisite_grid->addWidget(m_external_other_status, 5, 1);
        m_external_other_heading->hide();
        m_external_other_status->hide();
        readiness_layout->addLayout(prerequisite_grid);
        m_external_oracle_note = new QLabel(tr(
            "Informational: Paymaster transfers do not require an Oracle price. "
            "Minting, redemption and DigiDollar system displays remain limited until one is available."),
            m_external_prerequisites_card);
        m_external_oracle_note->setObjectName("paymasterExternalOracleNote");
        m_external_oracle_note->setProperty("paymasterRole", QStringLiteral("mutedText"));
        m_external_oracle_note->setWordWrap(true);
        readiness_layout->addWidget(m_external_oracle_note);
        configured_layout->addWidget(m_external_prerequisites_card);

        auto* dashboard = new PaymasterResponsiveCards(overview_column);
        dashboard->setObjectName("paymasterOverviewDashboard");
        auto* offer_card = CreatePaymasterStatusCard(
            dashboard, tr("Offer"), m_overview_offer_status,
            m_overview_offer_action);
        offer_card->setObjectName("paymasterOverviewOfferCard");
        auto* safety_card = CreatePaymasterStatusCard(
            dashboard, tr("Spending limits"), m_overview_safety_status,
            m_overview_safety_action);
        safety_card->setObjectName("paymasterOverviewSafetyCard");
        auto* liquidity_card = CreatePaymasterStatusCard(
            dashboard, tr("Liquidity"), m_overview_liquidity_status,
            m_overview_liquidity_action);
        liquidity_card->setObjectName("paymasterOverviewLiquidityCard");
        auto* operation_card = CreatePaymasterStatusCard(
            dashboard, tr("Provider operation"), m_overview_operation_status,
            m_overview_operation_action);
        operation_card->setObjectName("paymasterOverviewOperationCard");
        auto* finance_card = CreatePaymasterStatusCard(
            dashboard, tr("Finances"), m_overview_finance_status,
            m_overview_finance_action);
        finance_card->setObjectName("paymasterOverviewFinanceCard");
        m_overview_liquidity_status->setObjectName(
            "paymasterOverviewLiquidityStatus");
        m_overview_operation_status->setObjectName(
            "paymasterOverviewOperationStatus");
        m_overview_liquidity_action->setObjectName(
            "paymasterOverviewLiquidityAction");
        m_overview_operation_action->setObjectName(
            "paymasterOverviewOperationAction");
        dashboard->addCard(offer_card);
        dashboard->addCard(safety_card);
        dashboard->addCard(liquidity_card);
        dashboard->addCard(finance_card);
        dashboard->addCard(operation_card);
        configured_layout->addWidget(dashboard);

        m_liquidity_maintenance_card = new QGroupBox(
            tr("Automatic liquidity maintenance"), overview_column);
        m_liquidity_maintenance_card->setObjectName(
            "paymasterLiquidityMaintenanceCard");
        m_liquidity_maintenance_card->setProperty(
            "paymasterRole", QStringLiteral("maintenanceCard"));
        auto* maintenance_layout = new QVBoxLayout(
            m_liquidity_maintenance_card);
        m_liquidity_maintenance_state = new QLabel(
            tr("Liquidity maintenance status has not been loaded yet."),
            m_liquidity_maintenance_card);
        m_liquidity_maintenance_state->setObjectName(
            "paymasterLiquidityMaintenanceState");
        m_liquidity_maintenance_state->setProperty(
            "paymasterRole", QStringLiteral("statusText"));
        m_liquidity_maintenance_state->setWordWrap(true);
        QFont maintenance_state_font = m_liquidity_maintenance_state->font();
        maintenance_state_font.setBold(true);
        m_liquidity_maintenance_state->setFont(maintenance_state_font);
        m_liquidity_maintenance_next_step = new QLabel(
            m_liquidity_maintenance_card);
        m_liquidity_maintenance_next_step->setObjectName(
            "paymasterLiquidityMaintenanceNextStep");
        m_liquidity_maintenance_next_step->setProperty(
            "paymasterRole", QStringLiteral("mutedText"));
        m_liquidity_maintenance_next_step->setWordWrap(true);
        m_liquidity_maintenance_cost = new QLabel(
            m_liquidity_maintenance_card);
        m_liquidity_maintenance_cost->setObjectName(
            "paymasterLiquidityMaintenanceCost");
        m_liquidity_maintenance_cost->setProperty(
            "paymasterRole", QStringLiteral("mutedText"));
        m_liquidity_maintenance_cost->setWordWrap(true);
        m_approve_liquidity_maintenance = new QPushButton(
            tr("Review and approve maintenance limits…"),
            m_liquidity_maintenance_card);
        m_approve_liquidity_maintenance->setObjectName(
            "paymasterApproveLiquidityMaintenance");
        m_approve_liquidity_maintenance->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        maintenance_layout->addWidget(m_liquidity_maintenance_state);
        maintenance_layout->addWidget(m_liquidity_maintenance_next_step);
        maintenance_layout->addWidget(m_liquidity_maintenance_cost);
        maintenance_layout->addWidget(m_approve_liquidity_maintenance,
                                      0, Qt::AlignLeft);
        configured_layout->addWidget(m_liquidity_maintenance_card);

        auto* details_toggle = new QPushButton(tr("Show technical details"), overview);
        details_toggle->setObjectName("paymasterTechnicalDetailsToggle");
        details_toggle->setCheckable(true);
        details_toggle->setFlat(true);
        configured_layout->addWidget(details_toggle, 0, Qt::AlignLeft);

        auto* details = new QWidget(overview);
        details->setObjectName("paymasterTechnicalDetails");
        auto* details_layout = new QVBoxLayout(details);
        details_layout->setContentsMargins(0, 0, 0, 0);
        m_identity = new QLabel(tr("Identity: not configured"), overview);
        m_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_pool = new QLabel(tr("Liquidity pool: not configured"), overview);
        m_readiness = new QPlainTextEdit(overview);
        m_readiness->setObjectName("paymasterReadinessCodes");
        m_readiness->setReadOnly(true);
        m_readiness->setMaximumHeight(130);
        m_readiness->setPlaceholderText(tr("No technical readiness information available"));
        details_layout->addWidget(m_identity);
        details_layout->addWidget(m_pool);
        details_layout->addWidget(m_readiness);
        details->setVisible(false);
        configured_layout->addWidget(details);

        auto* runtime_buttons = new QHBoxLayout();
        auto* refresh = new QPushButton(tr("Refresh"), overview);
        refresh->setObjectName("paymasterOverviewRefresh");
        m_start = new QPushButton(tr("Start provider"), overview);
        m_start->setObjectName("paymasterStartProvider");
        m_stop = new QPushButton(tr("Stop provider"), overview);
        m_stop->setObjectName("paymasterStopProvider");
        m_start->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        m_stop->setProperty("paymasterRole", QStringLiteral("dangerAction"));
        runtime_buttons->addWidget(refresh);
        runtime_buttons->addStretch();
        runtime_buttons->addWidget(m_start);
        runtime_buttons->addWidget(m_stop);
        configured_layout->addLayout(runtime_buttons);
        configured_layout->addStretch();
        overview_layout->addWidget(m_setup_content);
        overview_scroll->setWidget(overview);
        ConfigurePaymasterScrollArea(overview_scroll, overview);
        m_tabs->addTab(overview_scroll, tr("Overview"));

        auto* configuration_scroll = new QScrollArea(m_tabs);
        configuration_scroll->setObjectName("paymasterConfigurationPage");
        configuration_scroll->setWidgetResizable(true);
        auto* configuration = new QWidget(configuration_scroll);
        configuration->setObjectName("paymasterConfigurationContents");
        QVBoxLayout* configuration_layout{nullptr};
        auto* configuration_column = CreatePaymasterPageColumn(
            configuration, configuration_layout);
        AddPaymasterPageHeading(
            configuration_layout, configuration_column, tr("Offer and identity"),
            tr("Choose what this provider offers. These settings do not move funds; liquidity is managed separately."),
            QStringLiteral("paymasterConfiguration"));

        auto* identity_group = new QGroupBox(tr("Provider identity"), configuration_column);
        identity_group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* identity_form = new QFormLayout(identity_group);
        identity_form->setHorizontalSpacing(18);
        identity_form->setVerticalSpacing(10);
        identity_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto* identity_help = new QLabel(tr(
            "The identity lets clients verify that offers come from the same provider. It is stored in this wallet and should normally be created only once."), identity_group);
        identity_help->setWordWrap(true);
        identity_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        m_display_name = new QLineEdit(identity_group);
        m_display_name->setObjectName("paymasterDisplayName");
        m_display_name->setMaxLength(32);
        m_display_name->setPlaceholderText(tr("Name shown to clients"));
        m_create_identity = new QPushButton(tr("Create provider identity"), identity_group);
        auto* create_identity = m_create_identity;
        create_identity->setObjectName("paymasterCreateIdentity");
        create_identity->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        m_offer_identity_status = new QLabel(
            tr("Action required · no provider identity has been created yet."),
            identity_group);
        m_offer_identity_status->setObjectName("paymasterOfferIdentityStatus");
        m_offer_identity_status->setProperty("paymasterRole", QStringLiteral("statusText"));
        m_offer_identity_status->setWordWrap(true);
        m_offer_identity_id = new QLabel(tr("Not created yet"), identity_group);
        m_offer_identity_id->setObjectName("paymasterOfferIdentityId");
        m_offer_identity_id->setProperty("paymasterRole", QStringLiteral("mutedText"));
        m_offer_identity_id->setWordWrap(true);
        m_offer_identity_id->setTextInteractionFlags(
            Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_offer_identity_id->setAccessibleName(tr("Full provider identity"));
        identity_form->addRow(identity_help);
        identity_form->addRow(tr("Status:"), m_offer_identity_status);
        identity_form->addRow(tr("Provider ID:"), m_offer_identity_id);
        identity_form->addRow(tr("Display name:"), m_display_name);
        identity_form->addRow(create_identity);
        configuration_layout->addWidget(identity_group);

        auto* policy_group = new QGroupBox(tr("Services offered"), configuration_column);
        policy_group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* policy_form = new QFormLayout(policy_group);
        policy_form->setHorizontalSpacing(18);
        policy_form->setVerticalSpacing(10);
        policy_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto* policy_help = new QLabel(tr(
            "The policy defines which transfers you are willing to support. Clients still verify every quote and transaction locally."), policy_group);
        policy_help->setWordWrap(true);
        policy_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        policy_form->addRow(policy_help);
        m_sponsored = new QCheckBox(tr("Sponsored · provider pays the DGB fee"), policy_group);
        m_sponsored->setObjectName("paymasterPolicySponsored");
        m_sponsored->setProperty("paymasterRole", QStringLiteral("choiceCard"));
        m_user_paid = new QCheckBox(tr("User paid · client pays a DD service fee"), policy_group);
        m_user_paid->setObjectName("paymasterPolicyUserPaid");
        m_user_paid->setProperty("paymasterRole", QStringLiteral("choiceCard"));
        m_user_paid->setChecked(true);
        auto* user_paid_help = new QLabel(tr(
            "The client pays a service fee in DigiDollar; your provider wallet supplies the DGB network fee."), policy_group);
        user_paid_help->setWordWrap(true);
        user_paid_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        auto* sponsored_help = new QLabel(tr(
            "Your provider pays the DGB network fee without charging a DigiDollar service fee. Use strict safety budgets to cap the cost."), policy_group);
        sponsored_help->setWordWrap(true);
        sponsored_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        auto* models = new PaymasterResponsiveCards(policy_group);
        models->setObjectName("paymasterOfferModelCards");
        auto* user_paid_card = new QFrame(models);
        user_paid_card->setObjectName("paymasterUserPaidOfferCard");
        user_paid_card->setProperty("paymasterRole", QStringLiteral("choicePanel"));
        auto* user_paid_layout = new QVBoxLayout(user_paid_card);
        user_paid_layout->setContentsMargins(14, 12, 14, 12);
        user_paid_layout->addWidget(m_user_paid);
        user_paid_layout->addWidget(user_paid_help);
        user_paid_layout->addStretch();
        auto* sponsored_card = new QFrame(models);
        sponsored_card->setObjectName("paymasterSponsoredOfferCard");
        sponsored_card->setProperty("paymasterRole", QStringLiteral("choicePanel"));
        auto* sponsored_layout = new QVBoxLayout(sponsored_card);
        sponsored_layout->setContentsMargins(14, 12, 14, 12);
        sponsored_layout->addWidget(m_sponsored);
        sponsored_layout->addWidget(sponsored_help);
        sponsored_layout->addStretch();
        models->addCard(user_paid_card);
        models->addCard(sponsored_card);
        const auto update_model_card = [](QFrame* card, bool selected) {
            card->setProperty("selected", selected);
            if (card->style()) {
                card->style()->unpolish(card);
                card->style()->polish(card);
            }
        };
        connect(m_user_paid, &QCheckBox::toggled, user_paid_card,
                [user_paid_card, update_model_card](bool checked) {
                    update_model_card(user_paid_card, checked);
                });
        connect(m_sponsored, &QCheckBox::toggled, sponsored_card,
                [sponsored_card, update_model_card](bool checked) {
                    update_model_card(sponsored_card, checked);
                });
        update_model_card(user_paid_card, m_user_paid->isChecked());
        update_model_card(sponsored_card, m_sponsored->isChecked());
        m_scope = new NoWheelComboBox(policy_group);
        m_scope->setObjectName("paymasterPolicySponsorshipScope");
        m_scope->addItem(tr("Public — available to discovered clients"), QStringLiteral("public"));
        m_scope->addItem(tr("Restricted — invitation only"), QStringLiteral("restricted"));
        m_scope->setToolTip(tr(
            "Public sponsorship can be offered together with user-paid service. Restricted sponsorship is a sponsored-only invitation mode."));
        m_fee_bps = scaledSpin(policy_group, 0, 10000, 50, 100.0, 2);
        m_fee_bps->setObjectName("paymasterPolicyFeeBps");
        m_fee_bps->setSingleStep(10);
        m_fee_bps->setSuffix(tr(" %"));
        m_fee_bps->setToolTip(tr(
            "Service fee for user-paid transfers. 100 basis points equal 1%. Sponsored transfers always charge zero DigiDollar service fee."));
        m_min_amount = scaledSpin(policy_group, 1, 10000000, 100, 100.0, 2);
        m_min_amount->setObjectName("paymasterPolicyMinimumCents");
        m_min_amount->setSuffix(tr(" DD"));
        m_min_amount->setToolTip(tr("Smallest DigiDollar payment this provider will accept."));
        m_max_amount = scaledSpin(policy_group, 1, 10000000, 100000, 100.0, 2);
        m_max_amount->setObjectName("paymasterPolicyMaximumCents");
        m_max_amount->setSuffix(tr(" DD"));
        m_max_amount->setToolTip(tr("Largest DigiDollar payment this provider will accept."));
        m_quote_ttl = spin(policy_group, 1, 60, 60);
        m_quote_ttl->setObjectName("paymasterPolicyQuoteLifetime");
        m_quote_ttl->setSuffix(tr(" seconds"));
        m_quote_ttl->setToolTip(tr("How long a client may accept a quote before it expires."));
        m_network_fee = scaledSpin(policy_group, 1, 2000000000, 20000000,
                                   100000000.0, 8);
        m_network_fee->setObjectName("paymasterPolicyMaximumNetworkFee");
        m_network_fee->setSuffix(tr(" DGB"));
        m_network_fee->setToolTip(tr(
            "Absolute DGB network-fee ceiling for one Paymaster transaction. The separate safety policy can impose a lower effective limit."));
        policy_form->addRow(tr("Payment models:"), models);
        m_funding_model_status = new QLabel(policy_group);
        m_funding_model_status->setObjectName("paymasterFundingModelExplanation");
        m_funding_model_status->setWordWrap(true);
        policy_form->addRow(QString(), m_funding_model_status);
        policy_form->addRow(tr("Who may use sponsorship:"), m_scope);
        policy_form->addRow(tr("User-paid service fee:"), m_fee_bps);
        policy_form->addRow(tr("Smallest payment:"), m_min_amount);
        policy_form->addRow(tr("Largest payment:"), m_max_amount);
        policy_form->addRow(tr("Quote validity:"), m_quote_ttl);
        policy_form->addRow(tr("Network-fee ceiling per transfer:"), m_network_fee);
        auto* limits_help = new QLabel(tr(
            "The payment range filters requests before a quote is created. Quote validity limits how long resources remain offered to one client. The network-fee ceiling is an absolute per-transfer guard; lower wallet safety limits still take precedence."), policy_group);
        limits_help->setObjectName("paymasterPolicyLimitsExplanation");
        limits_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        limits_help->setWordWrap(true);
        policy_form->addRow(QString(), limits_help);
        m_policy_summary = new QLabel(policy_group);
        m_policy_summary->setObjectName("paymasterPolicySummary");
        m_policy_summary->setProperty("paymasterRole", QStringLiteral("summaryText"));
        m_policy_summary->setWordWrap(true);
        policy_form->addRow(tr("Policy summary:"), m_policy_summary);
        auto* save_policy = new QPushButton(tr("Save policy"), policy_group);
        save_policy->setObjectName("savePaymasterPolicy");
        save_policy->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        m_enable = new QPushButton(tr("Enable provider configuration"), policy_group);
        m_enable->setObjectName("paymasterEnableProvider");
        m_enable->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        auto* restore_policy_defaults = new QPushButton(
            tr("Restore recommended defaults"), policy_group);
        restore_policy_defaults->setObjectName("paymasterRestorePolicyDefaults");
        restore_policy_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        restore_policy_defaults->setToolTip(tr(
            "Reset only the unsaved policy form. This does not change the provider identity, safety limits or liquidity."));
        auto* policy_buttons = new QHBoxLayout();
        policy_buttons->addWidget(restore_policy_defaults);
        policy_buttons->addStretch();
        policy_buttons->addWidget(save_policy);
        policy_buttons->addWidget(m_enable);
        policy_form->addRow(policy_buttons);
        m_enable_status = new QLabel(
            tr("Provider configuration is currently disabled for this wallet."),
            policy_group);
        m_enable_status->setObjectName("paymasterEnableStatus");
        m_enable_status->setWordWrap(true);
        policy_form->addRow(QString(), m_enable_status);
        configuration_layout->addWidget(policy_group);
        configuration_layout->addStretch();
        configuration_scroll->setWidget(configuration);
        ConfigurePaymasterScrollArea(configuration_scroll, configuration);
        m_configuration_page = configuration_scroll;
        m_tabs->addTab(configuration_scroll, tr("Offer"));

        auto* safety_scroll = new QScrollArea(m_tabs);
        safety_scroll->setObjectName("paymasterSafetyPolicyPage");
        safety_scroll->setWidgetResizable(true);
        auto* safety = new QWidget(safety_scroll);
        safety->setObjectName("paymasterSafetyPolicyContents");
        QVBoxLayout* safety_layout{nullptr};
        auto* safety_column = CreatePaymasterPageColumn(safety, safety_layout);
        AddPaymasterPageHeading(
            safety_layout, safety_column, tr("Provider spending limits"),
            tr("Set the maximum DGB this provider may spend. These wallet-local limits are the final brake even when a remote client sends repeated requests."),
            QStringLiteral("paymasterSafetyPolicy"));
        auto* safety_warning = new QLabel(tr(
            "Final spending brake · every active model needs finite private wallet limits per transfer, hour and day."), safety_column);
        safety_warning->setObjectName("paymasterSafetyPolicyWarning");
        safety_warning->setProperty("paymasterRole", QStringLiteral("notice"));
        safety_warning->setWordWrap(true);
        safety_layout->addWidget(safety_warning);
        auto* safety_instructions = new QLabel(tr(
            "Configure the models you actually offer, then save the provider safety policy. All six limits for an active model must be finite and greater than zero. Setting all six values to zero safely disables an unused model; zero never means unlimited. Start with the recommended values and lower them if the potential DGB cost is too high."), safety);
        safety_instructions->setObjectName("paymasterSafetyPolicyInstructions");
        safety_instructions->setWordWrap(true);
        safety_layout->addWidget(safety_instructions);

        auto* provider_safety = new QGroupBox(tr("Budgets by payment model"), safety_column);
        provider_safety->setProperty("paymasterRole", QStringLiteral("card"));
        auto* provider_safety_layout = new QVBoxLayout(provider_safety);
        m_provider_safety_status = new QLabel(
            tr("Provider safety policy: not configured (provider not ready)"),
            provider_safety);
        m_provider_safety_status->setObjectName("paymasterProviderSafetyStatus");
        m_provider_safety_status->setWordWrap(true);
        provider_safety_layout->addWidget(m_provider_safety_status);
        auto* provider_limits_help = new QLabel(tr(
            "Each tab controls one payment model. Per-transaction limits stop one costly transfer; reserved limits cap simultaneous open quotes; hourly and daily limits cap cumulative spending and completed transfers."), provider_safety);
        provider_limits_help->setObjectName("paymasterProviderSafetyExplanation");
        provider_limits_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        provider_limits_help->setWordWrap(true);
        provider_safety_layout->addWidget(provider_limits_help);
        auto* funding_tabs = new QTabWidget(provider_safety);
        funding_tabs->setObjectName("paymasterFundingSafetyModels");
        m_user_paid_safety = createFundingSafetyControls(
            funding_tabs, tr("User paid"), QStringLiteral("paymasterSafetyUserPaid"),
            /*recommended_defaults=*/true);
        m_public_sponsored_safety = createFundingSafetyControls(
            funding_tabs, tr("Public sponsored"), QStringLiteral("paymasterSafetyPublicSponsored"),
            /*recommended_defaults=*/false);
        m_restricted_sponsored_safety = createFundingSafetyControls(
            funding_tabs, tr("Restricted sponsored"), QStringLiteral("paymasterSafetyRestrictedSponsored"),
            /*recommended_defaults=*/false);
        provider_safety_layout->addWidget(funding_tabs);

        auto* quote_limits = new QGroupBox(tr("Quote and request limits"), provider_safety);
        auto* quote_form = new QFormLayout(quote_limits);
        auto* quote_help = new QLabel(tr(
            "These limits reduce resource exhaustion. Total limits protect the wallet, netgroup limits restrict groups of related network addresses, and recipient buckets stop repeated quotes aimed at the same destination. Reaching a limit rejects new work but does not invalidate an already authorized transfer."), quote_limits);
        quote_help->setObjectName("paymasterQuoteLimitsExplanation");
        quote_help->setWordWrap(true);
        quote_form->addRow(quote_help);
        m_max_active_quotes_total = spin(quote_limits, 1, 1000000, 16);
        m_max_active_quotes_total->setObjectName("paymasterSafetyMaxActiveQuotesTotal");
        m_max_active_quotes_per_netgroup = spin(quote_limits, 1, 1000000, 4);
        m_max_active_quotes_per_netgroup->setObjectName("paymasterSafetyMaxActiveQuotesPerNetgroup");
        m_max_active_quotes_per_recipient = spin(quote_limits, 1, 1000000, 2);
        m_max_active_quotes_per_recipient->setObjectName("paymasterSafetyMaxActiveQuotesPerRecipient");
        m_max_quote_requests_per_netgroup = spin(quote_limits, 1, 1000000, 10);
        m_max_quote_requests_per_netgroup->setObjectName("paymasterSafetyMaxQuoteRequestsPerNetgroupMinute");
        quote_form->addRow(tr("Active quotes (total):"), m_max_active_quotes_total);
        quote_form->addRow(tr("Active quotes per netgroup:"), m_max_active_quotes_per_netgroup);
        quote_form->addRow(tr("Active quotes per recipient bucket:"), m_max_active_quotes_per_recipient);
        quote_form->addRow(tr("Quote requests per netgroup/minute:"), m_max_quote_requests_per_netgroup);
        auto* restore_quote_defaults = new QPushButton(
            tr("Restore recommended quote limits"), quote_limits);
        restore_quote_defaults->setObjectName("paymasterRestoreQuoteSafetyDefaults");
        restore_quote_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        quote_form->addRow(restore_quote_defaults);
        // Technical anti-abuse limits are available through the advanced
        // disclosure below; the normal view stays focused on financial risk.

        auto* save_provider_safety = new QPushButton(tr("Save provider safety policy"), provider_safety);
        save_provider_safety->setObjectName("savePaymasterProviderSafetyPolicy");
        save_provider_safety->setProperty("paymasterRole", QStringLiteral("primaryAction"));
        provider_safety_layout->addWidget(save_provider_safety);
        auto* usage_group = new QGroupBox(tr("Current budget usage"), provider_safety);
        usage_group->setProperty("paymasterRole", QStringLiteral("technicalCard"));
        auto* usage_layout = new QVBoxLayout(usage_group);
        auto* usage_help = new QLabel(tr(
            "This read-only status shows what is currently reserved or spent. Saving new limits does not reset existing accounting."), usage_group);
        usage_help->setWordWrap(true);
        usage_layout->addWidget(usage_help);
        m_provider_safety_usage = new QLabel(usage_group);
        m_provider_safety_usage->setObjectName("paymasterProviderSafetyUsage");
        m_provider_safety_usage->setWordWrap(true);
        m_provider_safety_usage->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_provider_safety_usage->setText(tr("Budget usage is not available yet."));
        usage_layout->addWidget(m_provider_safety_usage);
        safety_layout->addWidget(provider_safety);

        auto* advanced_safety = new QWidget(safety_column);
        advanced_safety->setObjectName("paymasterAdvancedSafetyPanel");
        auto* advanced_safety_layout = new QVBoxLayout(advanced_safety);
        advanced_safety_layout->setContentsMargins(0, 0, 0, 0);
        advanced_safety_layout->addWidget(safety_instructions);
        advanced_safety_layout->addWidget(usage_group);
        advanced_safety_layout->addWidget(quote_limits);
        AddPaymasterDisclosure(
            safety_layout, safety_column, advanced_safety,
            tr("Show advanced protection limits"),
            tr("Hide advanced protection limits"),
            QStringLiteral("paymasterAdvancedSafetyToggle"));

        auto* client_safety = new QGroupBox(tr("Client service-fee limits"), safety);
        client_safety->setObjectName("paymasterClientSafetyGroup");
        auto* client_safety_form = new QFormLayout(client_safety);
        auto* client_safety_help = new QLabel(tr(
            "These limits protect this wallet when it uses another Paymaster. They cap the DigiDollar service fee for one transfer and across a rolling day. The effective per-transfer limit is always the lowest applicable wallet, request and signed-offer limit."), client_safety);
        client_safety_help->setObjectName("paymasterClientSafetyExplanation");
        client_safety_help->setWordWrap(true);
        client_safety_form->addRow(client_safety_help);
        m_client_fee_per_transaction = spin(client_safety, 0, 10000000, 100);
        m_client_fee_per_transaction->setObjectName("paymasterClientSafetyMaxFeePerTransaction");
        m_client_fee_per_day = spin(client_safety, 0, 10000000, 1000);
        m_client_fee_per_day->setObjectName("paymasterClientSafetyMaxFeePerDay");
        m_client_safety_status = new QLabel(
            tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"),
            client_safety);
        m_client_safety_status->setObjectName("paymasterClientSafetyStatus");
        m_client_safety_status->setWordWrap(true);
        m_client_safety_mode = new QLabel(client_safety);
        m_client_safety_mode->setObjectName("paymasterClientSafetyMode");
        m_client_safety_mode->setWordWrap(true);
        auto* save_client_safety = new QPushButton(tr("Save client safety policy"), client_safety);
        save_client_safety->setObjectName("savePaymasterClientSafetyPolicy");
        auto* restore_client_defaults = new QPushButton(
            tr("Restore recommended client limits"), client_safety);
        restore_client_defaults->setObjectName("paymasterRestoreClientSafetyDefaults");
        client_safety_form->addRow(tr("Maximum service fee per transfer (cents):"), m_client_fee_per_transaction);
        client_safety_form->addRow(tr("Maximum service fees per rolling day (cents):"), m_client_fee_per_day);
        client_safety_form->addRow(m_client_safety_mode);
        client_safety_form->addRow(m_client_safety_status);
        auto* client_buttons = new QHBoxLayout();
        client_buttons->addWidget(restore_client_defaults);
        client_buttons->addStretch();
        client_buttons->addWidget(save_client_safety);
        client_safety_form->addRow(client_buttons);
        // Client-side DD fee protection belongs to Send $DD. Keep these
        // widgets alive for compatibility with the existing wallet status
        // refresh path, but never expose them in the provider console.
        client_safety->setVisible(false);
        safety_layout->addStretch();
        safety_scroll->setWidget(safety);
        ConfigurePaymasterScrollArea(safety_scroll, safety);
        m_safety_page = safety_scroll;
        m_tabs->addTab(safety_scroll, tr("Spending limits"));

        auto* liquidity_scroll = new QScrollArea(m_tabs);
        liquidity_scroll->setObjectName("paymasterLiquidityPage");
        liquidity_scroll->setWidgetResizable(true);
        auto* liquidity = new QWidget(liquidity_scroll);
        liquidity->setObjectName("paymasterLiquidityContents");
        QVBoxLayout* liquidity_layout{nullptr};
        auto* liquidity_column = CreatePaymasterPageColumn(liquidity, liquidity_layout);
        AddPaymasterPageHeading(
            liquidity_layout, liquidity_column, tr("Provider liquidity"),
            tr("These wallet-owned reserves let clients verify this provider and let Core pay the network fee without selecting ordinary wallet funds for every request."),
            QStringLiteral("paymasterLiquidity"));

        auto* capacity_cards = new PaymasterResponsiveCards(liquidity_column);
        capacity_cards->setObjectName("paymasterLiquidityCapacityCards");
        capacity_cards->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto* dgb_capacity_card = CreatePaymasterStatusCard(
            capacity_cards, tr("DGB network-fee reserve"),
            m_liquidity_dgb_capacity_status, m_liquidity_dgb_capacity_action);
        auto* carrier_capacity_card = CreatePaymasterStatusCard(
            capacity_cards, tr("DigiDollar service-fee reserve"),
            m_liquidity_carrier_capacity_status,
            m_liquidity_carrier_capacity_action);
        // Both old Review buttons opened the same advanced panel and implied
        // two separate workflows. Capacity is one pool-management task, so the
        // normal view has one clearly named entry point instead.
        m_liquidity_dgb_capacity_action->hide();
        m_liquidity_carrier_capacity_action->hide();
        dgb_capacity_card->setMinimumHeight(0);
        carrier_capacity_card->setMinimumHeight(0);
        dgb_capacity_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        carrier_capacity_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        capacity_cards->addCard(dgb_capacity_card);
        capacity_cards->addCard(carrier_capacity_card);
        liquidity_layout->addWidget(capacity_cards);

        auto* capacity_explanation = new QLabel(tr(
            "Why two counts? Before making an offer, Core proves to a client that this provider has dedicated funds. Separate payment capacity is then kept ready for accepted transfers. “Ready” is available now; “target” is the amount Core maintains. These funds always remain under this wallet's control."),
            liquidity_column);
        capacity_explanation->setObjectName("paymasterLiquidityCapacityExplanation");
        capacity_explanation->setProperty("paymasterRole", QStringLiteral("mutedText"));
        capacity_explanation->setWordWrap(true);
        liquidity_layout->addWidget(capacity_explanation);
        auto* liquidity_steps = new QLabel(tr(
            "First-time setup: 1. Restore or choose target slot counts. 2. Preview pool preparation; no funds move during a preview. 3. Review the exact DGB and DD amounts below. 4. Execute the reviewed preparation. 5. Wait for the new outputs to confirm, then check Overview for readiness."), liquidity);
        liquidity_steps->setObjectName("paymasterLiquiditySetupSteps");
        liquidity_steps->setWordWrap(true);
        liquidity_layout->addWidget(liquidity_steps);

        auto* automatic_maintenance = new QGroupBox(
            tr("Keep reserves ready automatically"), liquidity_column);
        automatic_maintenance->setProperty("paymasterRole", QStringLiteral("card"));
        automatic_maintenance->setObjectName(
            "paymasterAutomaticLiquidityPolicy");
        auto* automatic_maintenance_layout = new QFormLayout(
            automatic_maintenance);
        auto* automatic_maintenance_help = new QLabel(tr(
            "After a completed transfer, Core first reuses the wallet-owned DGB change and DigiDollar carrier. "
            "If that is not enough, Core may create a refill transaction only when you have explicitly approved finite fee limits below."),
            automatic_maintenance);
        automatic_maintenance_help->setWordWrap(true);
        automatic_maintenance_layout->addRow(automatic_maintenance_help);
        m_automatic_replenishment = new QCheckBox(
            tr("Automatically refill missing reserves while this provider is running"),
            automatic_maintenance);
        m_automatic_replenishment->setObjectName(
            "paymasterAutomaticReplenishment");
        m_automatic_replenishment->setChecked(true);
        m_paid_maintenance_approved = new QCheckBox(tr(
            "Permit paid maintenance within the finite limits below"),
            automatic_maintenance);
        m_paid_maintenance_approved->setObjectName(
            "paymasterPaidMaintenanceApproved");
        m_maintenance_fee_per_transaction = integerField(
            automatic_maintenance, 10000000);
        m_maintenance_fee_per_transaction->setObjectName(
            "paymasterMaintenanceFeePerTransaction");
        m_maintenance_fee_per_hour = integerField(
            automatic_maintenance, 50000000);
        m_maintenance_fee_per_hour->setObjectName(
            "paymasterMaintenanceFeePerHour");
        m_maintenance_fee_per_day = integerField(
            automatic_maintenance, 200000000);
        m_maintenance_fee_per_day->setObjectName(
            "paymasterMaintenanceFeePerDay");
        m_liquidity_policy_status = new QLabel(tr(
            "Liquidity policy has not been loaded yet."), automatic_maintenance);
        m_liquidity_policy_status->setObjectName(
            "paymasterLiquidityPolicyStatus");
        m_liquidity_policy_status->setWordWrap(true);
        automatic_maintenance_layout->addRow(m_automatic_replenishment);
        automatic_maintenance_layout->addRow(m_liquidity_policy_status);
        m_maintenance_limits_toggle = new QPushButton(
            tr("Set optional paid-refill limits"), automatic_maintenance);
        m_maintenance_limits_toggle->setObjectName(
            "paymasterMaintenanceLimitsToggle");
        m_maintenance_limits_toggle->setProperty(
            "paymasterRole", QStringLiteral("disclosure"));
        m_maintenance_limits_toggle->setCheckable(true);
        auto* maintenance_limits_panel = new QWidget(automatic_maintenance);
        maintenance_limits_panel->setObjectName("paymasterMaintenanceLimitsPanel");
        auto* maintenance_limits_form = new QFormLayout(maintenance_limits_panel);
        maintenance_limits_form->setContentsMargins(0, 6, 0, 0);
        maintenance_limits_form->addRow(m_paid_maintenance_approved);
        maintenance_limits_form->addRow(
            tr("Maximum fee per transaction (satoshis):"),
            m_maintenance_fee_per_transaction);
        maintenance_limits_form->addRow(
            tr("Maximum fees per rolling hour (satoshis):"),
            m_maintenance_fee_per_hour);
        maintenance_limits_form->addRow(
            tr("Maximum fees per rolling day (satoshis):"),
            m_maintenance_fee_per_day);
        maintenance_limits_panel->setVisible(false);
        automatic_maintenance_layout->addRow(m_maintenance_limits_toggle);
        automatic_maintenance_layout->addRow(maintenance_limits_panel);
        m_save_liquidity_policy_primary = new QPushButton(
            tr("Save liquidity settings"), automatic_maintenance);
        m_save_liquidity_policy_primary->setObjectName(
            "paymasterSaveLiquidityPolicyPrimary");
        m_save_liquidity_policy_primary->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        m_save_liquidity_policy_primary->setToolTip(tr(
            "Save the automatic-refill setting, finite maintenance limits and displayed pool targets. This does not create a transaction or start the provider."));
        automatic_maintenance_layout->addRow(m_save_liquidity_policy_primary);
        connect(m_maintenance_limits_toggle, &QPushButton::toggled,
                maintenance_limits_panel,
                [this, maintenance_limits_panel](bool visible) {
                    maintenance_limits_panel->setVisible(visible);
                    m_maintenance_limits_toggle->setText(
                        visible ? tr("Hide paid-refill limits")
                                : tr("Set optional paid-refill limits"));
                });
        liquidity_layout->addWidget(automatic_maintenance);

        auto* targets = new QGroupBox(tr("Pool targets"), liquidity);
        auto* targets_layout = new QVBoxLayout(targets);
        auto* pool_explanation = new QLabel(tr(
            "Admission slots are separate, confirmed outputs used to demonstrate provider capacity. Operational slots are the outputs reserved for real transfers. DGB slots fund network fees. DD carrier slots are required only for user-paid service because they carry the provider's DigiDollar fee flow; sponsored-only providers can leave both carrier targets at zero."), targets);
        pool_explanation->setObjectName("paymasterLiquiditySlotExplanation");
        pool_explanation->setWordWrap(true);
        targets_layout->addWidget(pool_explanation);
        m_liquidity_current_status = new QLabel(
            tr("Current confirmed pool: status not loaded yet."), targets);
        m_liquidity_current_status->setObjectName("paymasterLiquidityCurrentStatus");
        m_liquidity_current_status->setWordWrap(true);
        targets_layout->addWidget(m_liquidity_current_status);
        auto* pool_form = new QFormLayout();
        m_admission_dgb = spin(liquidity, 3, 16, 3);
        m_admission_dgb->setObjectName("paymasterAdmissionDgbSlots");
        m_operational_dgb = spin(liquidity, 1, 16, 1);
        m_operational_dgb->setObjectName("paymasterOperationalDgbSlots");
        m_admission_carriers = spin(liquidity, 0, 16, 3);
        m_admission_carriers->setObjectName("paymasterAdmissionCarrierSlots");
        m_operational_carriers = spin(liquidity, 0, 16, 1);
        m_operational_carriers->setObjectName("paymasterOperationalCarrierSlots");
        m_admission_dgb->setToolTip(tr("At least three confirmed admission DGB slots are required."));
        m_operational_dgb->setToolTip(tr("Each operational DGB slot can support one concurrent transfer."));
        m_admission_carriers->setToolTip(tr("User-paid providers require at least three admission DD carriers; sponsored-only providers need none."));
        m_operational_carriers->setToolTip(tr("User-paid transfers require an operational DD carrier paired with operational DGB liquidity."));
        pool_form->addRow(tr("Admission DGB slots:"), m_admission_dgb);
        pool_form->addRow(tr("Operational DGB slots:"), m_operational_dgb);
        pool_form->addRow(tr("Admission carrier slots:"), m_admission_carriers);
        pool_form->addRow(tr("Operational carrier slots:"), m_operational_carriers);
        targets_layout->addLayout(pool_form);
        m_liquidity_summary = new QLabel(targets);
        m_liquidity_summary->setObjectName("paymasterLiquidityTargetSummary");
        m_liquidity_summary->setWordWrap(true);
        targets_layout->addWidget(m_liquidity_summary);
        m_liquidity_target_save_status = new QLabel(targets);
        m_liquidity_target_save_status->setObjectName(
            "paymasterLiquidityTargetSaveStatus");
        m_liquidity_target_save_status->setProperty(
            "paymasterRole", QStringLiteral("statusText"));
        m_liquidity_target_save_status->setWordWrap(true);
        targets_layout->addWidget(m_liquidity_target_save_status);
        m_save_liquidity_policy = new QPushButton(
            tr("Save liquidity targets and refill policy"), targets);
        m_save_liquidity_policy->setObjectName(
            "paymasterSaveLiquidityPolicy");
        m_save_liquidity_policy->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        m_save_liquidity_policy->setToolTip(tr(
            "Save the displayed slot targets, automatic-refill setting and finite maintenance limits. This does not create a transaction or start the provider."));
        auto* restore_liquidity_defaults = new QPushButton(
            tr("Restore recommended liquidity defaults"), targets);
        restore_liquidity_defaults->setObjectName("paymasterRestoreLiquidityDefaults");
        restore_liquidity_defaults->setToolTip(tr(
            "Reset the displayed targets and finite maintenance limits, and disable paid maintenance approval. This does not save the policy or create, spend or retire any wallet output."));
        auto* liquidity_target_actions = new QHBoxLayout();
        liquidity_target_actions->addWidget(restore_liquidity_defaults);
        liquidity_target_actions->addStretch();
        liquidity_target_actions->addWidget(m_save_liquidity_policy);
        targets_layout->addLayout(liquidity_target_actions);
        liquidity_layout->addWidget(targets);

        auto* maintenance_status_group = new QGroupBox(
            tr("Current replenishment status"), liquidity);
        maintenance_status_group->setObjectName(
            "paymasterLiquidityMaintenanceStatusGroup");
        auto* maintenance_status_layout = new QVBoxLayout(
            maintenance_status_group);
        m_liquidity_slot_status = new QLabel(
            tr("Ready, pending and missing target counts have not been loaded yet."),
            maintenance_status_group);
        m_liquidity_slot_status->setObjectName("paymasterLiquiditySlotStatus");
        m_liquidity_slot_status->setWordWrap(true);
        m_liquidity_recycling_status = new QLabel(
            tr("No recyclable successor information is available yet."),
            maintenance_status_group);
        m_liquidity_recycling_status->setObjectName(
            "paymasterLiquidityRecyclingStatus");
        m_liquidity_recycling_status->setWordWrap(true);
        m_liquidity_budget_status = new QLabel(
            tr("Maintenance budget use has not been loaded yet."),
            maintenance_status_group);
        m_liquidity_budget_status->setObjectName(
            "paymasterLiquidityBudgetStatus");
        m_liquidity_budget_status->setWordWrap(true);
        maintenance_status_layout->addWidget(m_liquidity_slot_status);
        maintenance_status_layout->addWidget(m_liquidity_recycling_status);
        maintenance_status_layout->addWidget(m_liquidity_budget_status);

        auto* carrier_management = new QGroupBox(
            tr("Carrier value and withdrawal"), liquidity);
        carrier_management->setObjectName("paymasterCarrierManagement");
        auto* carrier_management_layout = new QVBoxLayout(carrier_management);
        auto* carrier_management_help = new QLabel(tr(
            "Every active carrier keeps exactly 1.00 DD as its reusable base. User-paid "
            "service fees can accumulate above that base. Previewing excess withdrawal "
            "keeps every base intact. Releasing a whole operational carrier instead "
            "reduces its target and is allowed only while the provider is stopped."),
            carrier_management);
        carrier_management_help->setWordWrap(true);
        m_carrier_value_status = new QLabel(
            tr("Carrier base and withdrawable excess have not been loaded yet."),
            carrier_management);
        m_carrier_value_status->setObjectName("paymasterCarrierValueStatus");
        m_carrier_value_status->setWordWrap(true);
        auto* excess_buttons = new QHBoxLayout();
        m_preview_carrier_excess = new QPushButton(
            tr("Preview bundled excess withdrawal"), carrier_management);
        m_preview_carrier_excess->setObjectName(
            "paymasterPreviewCarrierExcess");
        m_preview_carrier_excess->setEnabled(false);
        m_execute_carrier_excess = new QPushButton(
            tr("Execute reviewed excess withdrawal…"), carrier_management);
        m_execute_carrier_excess->setObjectName(
            "paymasterExecuteCarrierExcess");
        m_execute_carrier_excess->setEnabled(false);
        excess_buttons->addWidget(m_preview_carrier_excess);
        excess_buttons->addWidget(m_execute_carrier_excess);
        auto* release_form = new QFormLayout();
        m_release_carrier_select = new NoWheelComboBox(carrier_management);
        m_release_carrier_select->setObjectName(
            "paymasterReleaseCarrierSelection");
        m_release_carrier_select->setAccessibleName(tr(
            "Confirmed operational carrier slot to release"));
        release_form->addRow(tr("Operational carrier to release:"),
                             m_release_carrier_select);
        auto* release_buttons = new QHBoxLayout();
        m_preview_carrier_release = new QPushButton(
            tr("Preview full slot release"), carrier_management);
        m_preview_carrier_release->setObjectName(
            "paymasterPreviewCarrierRelease");
        m_preview_carrier_release->setEnabled(false);
        m_execute_carrier_release = new QPushButton(
            tr("Execute reviewed slot release…"), carrier_management);
        m_execute_carrier_release->setObjectName(
            "paymasterExecuteCarrierRelease");
        m_execute_carrier_release->setEnabled(false);
        release_buttons->addWidget(m_preview_carrier_release);
        release_buttons->addWidget(m_execute_carrier_release);
        m_carrier_withdrawal_status = new QLabel(
            tr("No carrier withdrawal has been previewed."), carrier_management);
        m_carrier_withdrawal_status->setObjectName(
            "paymasterCarrierWithdrawalStatus");
        m_carrier_withdrawal_status->setWordWrap(true);
        carrier_management_layout->addWidget(carrier_management_help);
        carrier_management_layout->addWidget(m_carrier_value_status);
        carrier_management_layout->addLayout(excess_buttons);
        carrier_management_layout->addLayout(release_form);
        carrier_management_layout->addLayout(release_buttons);
        carrier_management_layout->addWidget(m_carrier_withdrawal_status);
        liquidity_layout->addWidget(carrier_management);

        auto* preparation = new QGroupBox(tr("Create missing pool liquidity"), liquidity);
        auto* preparation_layout = new QVBoxLayout(preparation);
        auto* preparation_help = new QLabel(tr(
            "Preparation is additive: existing live pool entries count toward the targets, so only missing outputs are proposed. Always preview the current targets before executing."), preparation);
        preparation_help->setWordWrap(true);
        preparation_layout->addWidget(preparation_help);
        m_prepare_preview = new QPushButton(tr("Preview pool preparation"), liquidity);
        auto* prepare_preview = m_prepare_preview;
        m_prepare_execute = new QPushButton(tr("Execute reviewed preparation…"), liquidity);
        auto* prepare_execute = m_prepare_execute;
        prepare_preview->setObjectName("paymasterPreviewPoolPreparation");
        prepare_execute->setObjectName("paymasterExecutePoolPreparation");
        auto* preparation_buttons = new QHBoxLayout();
        preparation_buttons->addWidget(prepare_preview);
        preparation_buttons->addWidget(prepare_execute);
        preparation_layout->addLayout(preparation_buttons);
        liquidity_layout->addWidget(preparation);

        auto* retirement = new QGroupBox(tr("Return excess available liquidity"), liquidity);
        auto* retirement_layout = new QVBoxLayout(retirement);
        auto* retirement_help = new QLabel(tr(
            "Retirement is for deliberately lowering an already prepared pool. It returns only available excess outputs to ordinary wallet liquidity; active reservations are never touched. New operators normally do not need this step."), retirement);
        retirement_help->setObjectName("paymasterLiquidityRetirementExplanation");
        retirement_help->setWordWrap(true);
        retirement_layout->addWidget(retirement_help);
        m_rebalance_preview = new QPushButton(tr("Preview excess retirement"), liquidity);
        auto* rebalance_preview = m_rebalance_preview;
        m_rebalance_execute = new QPushButton(tr("Execute reviewed retirement…"), liquidity);
        auto* rebalance_execute = m_rebalance_execute;
        rebalance_preview->setObjectName("paymasterPreviewPoolRetirement");
        rebalance_execute->setObjectName("paymasterExecutePoolRetirement");
        auto* retirement_buttons = new QHBoxLayout();
        retirement_buttons->addWidget(rebalance_preview);
        retirement_buttons->addWidget(rebalance_execute);
        retirement_layout->addLayout(retirement_buttons);
        liquidity_layout->addWidget(retirement);

        auto* result_group = new QGroupBox(tr("Latest preview or execution result"), liquidity);
        auto* result_layout = new QVBoxLayout(result_group);
        auto* result_help = new QLabel(tr(
            "A preview is side-effect free. Execution buttons remain guarded by confirmation and use the target values currently shown above."), result_group);
        result_help->setWordWrap(true);
        result_layout->addWidget(result_help);
        m_liquidity_preview_status = new QLabel(result_group);
        m_liquidity_preview_status->setObjectName("paymasterLiquidityPreviewStatus");
        m_liquidity_preview_status->setWordWrap(true);
        result_layout->addWidget(m_liquidity_preview_status);
        m_liquidity_output = new QPlainTextEdit(result_group);
        m_liquidity_output->setObjectName("paymasterLiquidityResult");
        m_liquidity_output->setReadOnly(true);
        m_liquidity_output->setMaximumHeight(260);
        m_liquidity_output->setPlaceholderText(tr("Choose Preview pool preparation to calculate the first setup transaction."));
        m_liquidity_output->viewport()->setBackgroundRole(QPalette::Window);
        m_liquidity_output->viewport()->setAutoFillBackground(true);
        result_layout->addWidget(m_liquidity_output);
        liquidity_layout->addWidget(result_group);

        auto* advanced_liquidity = new QWidget(liquidity_column);
        advanced_liquidity->setObjectName("paymasterAdvancedLiquidityPanel");
        auto* advanced_liquidity_layout = new QVBoxLayout(advanced_liquidity);
        advanced_liquidity_layout->setContentsMargins(0, 0, 0, 0);
        advanced_liquidity_layout->setSpacing(12);
        advanced_liquidity_layout->addWidget(maintenance_status_group);
        advanced_liquidity_layout->addWidget(liquidity_steps);
        advanced_liquidity_layout->addWidget(targets);
        advanced_liquidity_layout->addWidget(carrier_management);
        advanced_liquidity_layout->addWidget(preparation);
        advanced_liquidity_layout->addWidget(retirement);
        advanced_liquidity_layout->addWidget(result_group);
        m_liquidity_advanced_toggle = AddPaymasterDisclosure(
            liquidity_layout, liquidity_column, advanced_liquidity,
            tr("Change liquidity settings"),
            tr("Hide liquidity settings"),
            QStringLiteral("paymasterAdvancedLiquidityToggle"));
        liquidity_layout->addStretch();
        liquidity_scroll->setWidget(liquidity);
        ConfigurePaymasterScrollArea(liquidity_scroll, liquidity);
        m_liquidity_page = liquidity_scroll;
        m_tabs->addTab(liquidity_scroll, tr("Liquidity"));

        auto* finance_scroll = new QScrollArea(m_tabs);
        finance_scroll->setObjectName("paymasterFinancesPage");
        finance_scroll->setWidgetResizable(true);
        auto* finance = new QWidget(finance_scroll);
        finance->setObjectName("paymasterFinancesContents");
        QVBoxLayout* finance_layout{nullptr};
        auto* finance_column = CreatePaymasterPageColumn(finance, finance_layout);
        AddPaymasterPageHeading(
            finance_layout, finance_column, tr("Provider finances"),
            tr("Track DigiDollar service-fee income, DigiByte operating costs and wallet-owned pool capital without treating reserved liquidity as an expense."),
            QStringLiteral("paymasterFinances"));

        auto* finance_toolbar = new QGroupBox(
            tr("Reporting period"), finance_column);
        finance_toolbar->setProperty(
            "paymasterRole", QStringLiteral("card"));
        auto* finance_toolbar_layout = new QHBoxLayout(finance_toolbar);
        auto* finance_period_help = new QLabel(tr(
            "The four summary cards use one consistent wallet snapshot. The selected period controls the detailed booking list and CSV export."),
            finance_toolbar);
        finance_period_help->setWordWrap(true);
        finance_toolbar_layout->addWidget(finance_period_help, 1);
        m_finance_period_select = new NoWheelComboBox(finance_toolbar);
        m_finance_period_select->setObjectName("paymasterFinancePeriod");
        m_finance_period_select->addItem(tr("Today"), QStringLiteral("today"));
        m_finance_period_select->addItem(tr("Last 7 days"), QStringLiteral("7d"));
        m_finance_period_select->addItem(tr("Last 30 days"), QStringLiteral("30d"));
        m_finance_period_select->addItem(tr("All recorded history"), QStringLiteral("all"));
        m_finance_period_select->setCurrentIndex(2);
        m_finance_refresh = new QPushButton(tr("Refresh"), finance_toolbar);
        m_finance_refresh->setObjectName("paymasterFinanceRefresh");
        m_finance_export = new QPushButton(tr("Export CSV…"), finance_toolbar);
        m_finance_export->setObjectName("paymasterFinanceExport");
        m_finance_export->setEnabled(false);
        finance_toolbar_layout->addWidget(m_finance_period_select);
        finance_toolbar_layout->addWidget(m_finance_refresh);
        finance_toolbar_layout->addWidget(m_finance_export);
        finance_layout->addWidget(finance_toolbar);

        // Legacy provider commits may predate the manifest-bound finance
        // ledger. Keep that coverage boundary next to the headline totals,
        // rather than hiding it in booking details where a zero income value
        // could otherwise be mistaken for a complete historical statement.
        m_finance_history_notice = new QGroupBox(
            tr("Recorded-history coverage"), finance_column);
        m_finance_history_notice->setObjectName(
            "paymasterFinanceHistoryNotice");
        m_finance_history_notice->setProperty(
            "paymasterRole", QStringLiteral("notice"));
        m_finance_history_notice->setProperty(
            "statusKind", QStringLiteral("waiting"));
        auto* finance_history_notice_layout =
            new QVBoxLayout(m_finance_history_notice);
        m_finance_history_notice_text = new QLabel(
            m_finance_history_notice);
        m_finance_history_notice_text->setObjectName(
            "paymasterFinanceHistoryNoticeText");
        m_finance_history_notice_text->setWordWrap(true);
        finance_history_notice_layout->addWidget(
            m_finance_history_notice_text);
        m_finance_history_notice->hide();
        finance_layout->addWidget(m_finance_history_notice);

        auto* finance_period_cards = new PaymasterResponsiveCards(finance_column);
        finance_period_cards->setObjectName("paymasterFinancePeriodCards");
        const std::array<QString, 4> period_titles{
            tr("Today"), tr("7 days"), tr("30 days"), tr("Total")};
        for (size_t index = 0; index < period_titles.size(); ++index) {
            auto* card = new QFrame(finance_period_cards);
            card->setProperty("paymasterRole", QStringLiteral("statusCard"));
            card->setMinimumHeight(154);
            auto* card_layout = new QVBoxLayout(card);
            card_layout->setContentsMargins(16, 14, 16, 14);
            auto* title = new QLabel(period_titles.at(index), card);
            title->setProperty("paymasterRole", QStringLiteral("cardHeading"));
            m_finance_period_income.at(index) = new QLabel(
                tr("Service fees: — DD"), card);
            m_finance_period_income.at(index)->setObjectName(
                QStringLiteral("paymasterFinancePeriodIncome%1").arg(index));
            m_finance_period_income.at(index)->setProperty(
                "paymasterRole", QStringLiteral("statusText"));
            m_finance_period_cost.at(index) = new QLabel(
                tr("Operating costs: — DGB"), card);
            m_finance_period_cost.at(index)->setObjectName(
                QStringLiteral("paymasterFinancePeriodCost%1").arg(index));
            m_finance_period_transfers.at(index) = new QLabel(
                tr("Successful transfers: —"), card);
            m_finance_period_transfers.at(index)->setObjectName(
                QStringLiteral("paymasterFinancePeriodTransfers%1").arg(index));
            card_layout->addWidget(title);
            card_layout->addWidget(m_finance_period_income.at(index));
            card_layout->addWidget(m_finance_period_cost.at(index));
            card_layout->addWidget(m_finance_period_transfers.at(index));
            card_layout->addStretch();
            finance_period_cards->addCard(card);
        }
        finance_layout->addWidget(finance_period_cards);

        auto* finance_result_group = new QGroupBox(
            tr("Current-price result estimate"), finance_column);
        finance_result_group->setProperty(
            "paymasterRole", QStringLiteral("card"));
        auto* finance_result_layout = new QVBoxLayout(finance_result_group);
        m_finance_result_estimate = new QLabel(
            tr("Load finance data to calculate the current estimate."),
            finance_result_group);
        m_finance_result_estimate->setObjectName(
            "paymasterFinanceResultEstimate");
        m_finance_result_estimate->setWordWrap(true);
        m_finance_model_breakdown = new QLabel(finance_result_group);
        m_finance_model_breakdown->setObjectName(
            "paymasterFinanceModelBreakdown");
        m_finance_model_breakdown->setWordWrap(true);
        auto* finance_valuation_note = new QLabel(tr(
            "Native DD income and DGB costs are the accounting source of truth. Any USD result uses only the current Oracle price and is not a historical exchange-rate calculation."),
            finance_result_group);
        finance_valuation_note->setProperty(
            "paymasterRole", QStringLiteral("mutedText"));
        finance_valuation_note->setWordWrap(true);
        finance_result_layout->addWidget(m_finance_result_estimate);
        finance_result_layout->addWidget(m_finance_model_breakdown);
        finance_result_layout->addWidget(finance_valuation_note);
        finance_layout->addWidget(finance_result_group);

        auto* finance_pool_group = new QGroupBox(
            tr("Wallet-owned pool capital"), finance_column);
        finance_pool_group->setProperty(
            "paymasterRole", QStringLiteral("card"));
        auto* finance_pool_layout = new QVBoxLayout(finance_pool_group);
        auto* finance_pool_help = new QLabel(tr(
            "Prepared DGB and the 1.00 DD base of each carrier remain your wallet's operating capital. They are shown separately from income and expenses."),
            finance_pool_group);
        finance_pool_help->setWordWrap(true);
        m_finance_pool_dgb = new QLabel(
            tr("DGB capacity has not been loaded yet."), finance_pool_group);
        m_finance_pool_dgb->setObjectName("paymasterFinancePoolDgb");
        m_finance_pool_dgb->setWordWrap(true);
        m_finance_pool_carrier = new QLabel(
            tr("DD carrier capital has not been loaded yet."),
            finance_pool_group);
        m_finance_pool_carrier->setObjectName("paymasterFinancePoolCarrier");
        m_finance_pool_carrier->setWordWrap(true);
        m_finance_pending_maintenance = new QLabel(finance_pool_group);
        m_finance_pending_maintenance->setObjectName(
            "paymasterFinancePendingMaintenance");
        m_finance_pending_maintenance->setWordWrap(true);
        finance_pool_layout->addWidget(finance_pool_help);
        finance_pool_layout->addWidget(m_finance_pool_dgb);
        finance_pool_layout->addWidget(m_finance_pool_carrier);
        finance_pool_layout->addWidget(m_finance_pending_maintenance);
        finance_layout->addWidget(finance_pool_group);

        m_finance_backup_notice = new QGroupBox(
            tr("Provider-wallet backup recommended"), finance_column);
        m_finance_backup_notice->setObjectName("paymasterFinanceBackupNotice");
        m_finance_backup_notice->setProperty(
            "paymasterRole", QStringLiteral("notice"));
        m_finance_backup_notice->setProperty(
            "statusKind", QStringLiteral("waiting"));
        auto* finance_backup_layout = new QVBoxLayout(m_finance_backup_notice);
        m_finance_backup_text = new QLabel(m_finance_backup_notice);
        m_finance_backup_text->setWordWrap(true);
        m_finance_backup_provider_id = new QLabel(m_finance_backup_notice);
        m_finance_backup_provider_id->setObjectName(
            "paymasterFinanceBackupProviderId");
        m_finance_backup_provider_id->setWordWrap(true);
        m_finance_backup_provider_id->setTextInteractionFlags(
            Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        auto* finance_backup_actions = new QHBoxLayout();
        m_finance_backup_now = new QPushButton(
            tr("Back up provider wallet now…"), m_finance_backup_notice);
        m_finance_backup_now->setObjectName("paymasterFinanceBackupNow");
        m_finance_backup_now->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        m_finance_backup_external = new QPushButton(
            tr("Acknowledge another full-wallet backup…"),
            m_finance_backup_notice);
        m_finance_backup_external->setObjectName(
            "paymasterFinanceExternalBackup");
        finance_backup_actions->addWidget(m_finance_backup_now);
        finance_backup_actions->addWidget(m_finance_backup_external);
        finance_backup_actions->addStretch();
        finance_backup_layout->addWidget(m_finance_backup_text);
        finance_backup_layout->addWidget(m_finance_backup_provider_id);
        finance_backup_layout->addLayout(finance_backup_actions);
        m_finance_backup_notice->hide();
        finance_layout->addWidget(m_finance_backup_notice);

        auto* finance_actions_group = new QGroupBox(
            tr("Pool finance actions"), finance_column);
        finance_actions_group->setProperty(
            "paymasterRole", QStringLiteral("card"));
        auto* finance_actions_layout = new QGridLayout(finance_actions_group);
        auto* finance_actions_help = new QLabel(tr(
            "These shortcuts open the existing preview-first liquidity controls. No funds move until you review a fresh plan and separately confirm its execution."),
            finance_actions_group);
        finance_actions_help->setWordWrap(true);
        finance_actions_layout->addWidget(finance_actions_help, 0, 0, 1, 2);
        m_finance_withdraw_fees = new QPushButton(
            tr("Withdraw DD service fees"), finance_actions_group);
        m_finance_release_carrier = new QPushButton(
            tr("Release carrier capital"), finance_actions_group);
        m_finance_review_dgb = new QPushButton(
            tr("Review excess DGB liquidity"), finance_actions_group);
        m_finance_add_liquidity = new QPushButton(
            tr("Add provider liquidity"), finance_actions_group);
        m_finance_withdraw_fees->setObjectName(
            "paymasterFinanceWithdrawFees");
        m_finance_release_carrier->setObjectName(
            "paymasterFinanceReleaseCarrier");
        m_finance_review_dgb->setObjectName(
            "paymasterFinanceReviewDgb");
        m_finance_add_liquidity->setObjectName(
            "paymasterFinanceAddLiquidity");
        finance_actions_layout->addWidget(m_finance_withdraw_fees, 1, 0);
        finance_actions_layout->addWidget(m_finance_release_carrier, 1, 1);
        finance_actions_layout->addWidget(m_finance_review_dgb, 2, 0);
        finance_actions_layout->addWidget(m_finance_add_liquidity, 2, 1);
        finance_layout->addWidget(finance_actions_group);

        auto* finance_details = new QWidget(finance_column);
        finance_details->setObjectName("paymasterFinanceDetailsPanel");
        auto* finance_details_layout = new QVBoxLayout(finance_details);
        finance_details_layout->setContentsMargins(0, 0, 0, 0);
        m_finance_history_status = new QLabel(finance_details);
        m_finance_history_status->setObjectName(
            "paymasterFinanceHistoryStatus");
        m_finance_history_status->setWordWrap(true);
        auto* finance_daily_title = new QLabel(
            tr("Daily progression (UTC)"), finance_details);
        finance_daily_title->setProperty(
            "paymasterRole", QStringLiteral("cardHeading"));
        m_finance_daily_totals = new QTableWidget(0, 5, finance_details);
        m_finance_daily_totals->setObjectName(
            "paymasterFinanceDailyTotals");
        m_finance_daily_totals->setHorizontalHeaderLabels({
            tr("Day"), tr("DD income"), tr("DGB cost"),
            tr("Transfers"), tr("Maintenance")});
        m_finance_daily_totals->setEditTriggers(
            QAbstractItemView::NoEditTriggers);
        m_finance_daily_totals->setSelectionBehavior(
            QAbstractItemView::SelectRows);
        m_finance_daily_totals->setAlternatingRowColors(true);
        m_finance_daily_totals->verticalHeader()->hide();
        m_finance_daily_totals->horizontalHeader()->setSectionResizeMode(
            QHeaderView::Stretch);
        m_finance_daily_totals->setMinimumHeight(180);
        auto* finance_bookings_title = new QLabel(
            tr("Bookings"), finance_details);
        finance_bookings_title->setProperty(
            "paymasterRole", QStringLiteral("cardHeading"));
        m_finance_events = new QTableWidget(0, 6, finance_details);
        m_finance_events->setObjectName("paymasterFinanceEvents");
        m_finance_events->setHorizontalHeaderLabels({
            tr("Date (UTC)"), tr("Booking"), tr("Status"),
            tr("DD income"), tr("DGB cost"), tr("Payment model")});
        m_finance_events->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_finance_events->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_finance_events->setAlternatingRowColors(true);
        m_finance_events->verticalHeader()->hide();
        m_finance_events->horizontalHeader()->setSectionResizeMode(
            QHeaderView::ResizeToContents);
        m_finance_events->horizontalHeader()->setStretchLastSection(true);
        m_finance_events->setMinimumHeight(260);
        finance_details_layout->addWidget(m_finance_history_status);
        finance_details_layout->addWidget(finance_daily_title);
        finance_details_layout->addWidget(m_finance_daily_totals);
        finance_details_layout->addWidget(finance_bookings_title);
        finance_details_layout->addWidget(m_finance_events);
        m_finance_details_toggle = AddPaymasterDisclosure(
            finance_layout, finance_column, finance_details,
            tr("Show booking details"), tr("Hide booking details"),
            QStringLiteral("paymasterFinanceDetailsToggle"));
        finance_layout->addStretch();
        finance_scroll->setWidget(finance);
        ConfigurePaymasterScrollArea(finance_scroll, finance);
        m_finance_page = finance_scroll;
        m_tabs->addTab(finance_scroll, tr("Finances"));

        auto* activity_scroll = new QScrollArea(m_tabs);
        activity_scroll->setObjectName("paymasterActivityPage");
        activity_scroll->setWidgetResizable(true);
        auto* activity = new QWidget(activity_scroll);
        activity->setObjectName("paymasterActivityContents");
        QVBoxLayout* activity_layout{nullptr};
        auto* activity_column = CreatePaymasterPageColumn(activity, activity_layout);
        AddPaymasterPageHeading(
            activity_layout, activity_column, tr("Provider operation"),
            tr("Monitor the automatic provider service, queued work and any durable reservations that require attention."),
            QStringLiteral("paymasterActivity"));

        auto* activity_intro_group = new QGroupBox(
            tr("Current service status"), activity_column);
        activity_intro_group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* activity_intro_layout = new QVBoxLayout(activity_intro_group);
        auto* activity_intro = new QLabel(tr(
            "This page is used after setup. In the recommended automatic mode, Core answers "
            "capacity and quote requests and completes validated client submissions while the "
            "provider is running. The client must still confirm its exact provider and fee before signing."), activity_intro_group);
        activity_intro->setObjectName("paymasterActivityAutomaticExplanation");
        activity_intro->setWordWrap(true);
        activity_intro_layout->addWidget(activity_intro);
        auto* activity_responsibility = new QLabel(tr(
            "Keep the provider running and the wallet unlocked while serving clients. If the wallet "
            "is locked, automatic processing pauses without consuming queued messages and resumes "
            "after unlock. Wallet-local safety limits and full transaction validation remain enforced."), activity_intro_group);
        activity_responsibility->setObjectName("paymasterActivityResponsibility");
        activity_responsibility->setWordWrap(true);
        activity_intro_layout->addWidget(activity_responsibility);
        m_activity_runtime_status = new QLabel(
            tr("Provider status has not been loaded yet."), activity_intro_group);
        m_activity_runtime_status->setObjectName("paymasterActivityRuntimeStatus");
        m_activity_runtime_status->setWordWrap(true);
        QFont activity_status_font = m_activity_runtime_status->font();
        activity_status_font.setBold(true);
        m_activity_runtime_status->setFont(activity_status_font);
        activity_intro_layout->addWidget(m_activity_runtime_status);
        m_operation_primary = new QPushButton(
            tr("Check provider status"), activity_intro_group);
        m_operation_primary->setObjectName("paymasterOperationPrimaryAction");
        m_operation_primary->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        activity_intro_layout->addWidget(m_operation_primary, 0, Qt::AlignLeft);
        m_runtime_settings_toggle = new QPushButton(
            tr("Show advanced provider runtime settings"), activity_intro_group);
        m_runtime_settings_toggle->setObjectName("paymasterRuntimeSettingsToggle");
        m_runtime_settings_toggle->setCheckable(true);
        m_runtime_settings_toggle->setFlat(true);
        activity_intro_layout->addWidget(m_runtime_settings_toggle, 0, Qt::AlignLeft);
        activity_layout->addWidget(activity_intro_group);

        m_runtime_settings_panel = new QGroupBox(
            tr("Advanced provider runtime settings"), activity);
        m_runtime_settings_panel->setObjectName("paymasterRuntimeSettingsPanel");
        auto* runtime_settings_form = new QFormLayout(m_runtime_settings_panel);
        auto* runtime_settings_help = new QLabel(tr(
            "Automatic is recommended and processes bounded queue work after you consciously start "
            "the provider. Manual mode is intended only for diagnosis or expert-controlled operation. "
            "Changing modes requires the provider to be stopped."), m_runtime_settings_panel);
        runtime_settings_help->setObjectName("paymasterRuntimeSettingsExplanation");
        runtime_settings_help->setWordWrap(true);
        runtime_settings_form->addRow(runtime_settings_help);
        m_operation_mode_select = new NoWheelComboBox(m_runtime_settings_panel);
        m_operation_mode_select->setObjectName("paymasterOperationMode");
        m_operation_mode_select->addItem(
            tr("Automatic — recommended"), QStringLiteral("automatic"));
        m_operation_mode_select->addItem(
            tr("Manual — expert mode"), QStringLiteral("manual"));
        runtime_settings_form->addRow(tr("Processing mode:"), m_operation_mode_select);
        m_autostart = new QCheckBox(
            tr("Start this provider automatically after loading its wallet"),
            m_runtime_settings_panel);
        m_autostart->setObjectName("paymasterProviderAutostart");
        m_autostart->setToolTip(tr(
            "Autostart never stores a passphrase. An encrypted wallet waits for you to unlock it."));
        runtime_settings_form->addRow(m_autostart);
        m_save_runtime_settings = new QPushButton(
            tr("Save runtime settings"), m_runtime_settings_panel);
        m_save_runtime_settings->setObjectName("savePaymasterRuntimeSettings");
        m_save_runtime_settings->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        runtime_settings_form->addRow(m_save_runtime_settings);
        m_runtime_settings_result = new QLabel(
            tr("Automatic processing is the recommended default. Provider autostart is off by default."),
            m_runtime_settings_panel);
        m_runtime_settings_result->setObjectName("paymasterRuntimeSettingsResult");
        m_runtime_settings_result->setWordWrap(true);
        runtime_settings_form->addRow(m_runtime_settings_result);
        activity_layout->addWidget(m_runtime_settings_panel);
        // The Operations page itself is still hidden while it is built. Force
        // an explicit show/hide transition so Qt remembers that this expert
        // panel must remain collapsed when the page is shown later.
        m_runtime_settings_panel->show();
        m_runtime_settings_panel->hide();

        m_request_processing_group = new QGroupBox(
            tr("1. Answer one waiting request"), activity);
        m_request_processing_group->setObjectName("paymasterManualRequestGroup");
        auto* request_layout = new QVBoxLayout(m_request_processing_group);
        auto* request_explanation = new QLabel(tr(
            "Handles one capacity check, quote request, or recovery-provider request. "
            "Core verifies the message and may sign a response and reserve a pool slot and "
            "part of the safety budget. It does not yet authorize the client's final payment."),
            m_request_processing_group);
        request_explanation->setObjectName("paymasterRequestProcessingExplanation");
        request_explanation->setWordWrap(true);
        request_layout->addWidget(request_explanation);
        m_process_requests = new QPushButton(
            tr("Process one waiting request"), m_request_processing_group);
        m_process_requests->setObjectName("paymasterProcessOneRequest");
        request_layout->addWidget(m_process_requests, 0, Qt::AlignLeft);
        activity_layout->addWidget(m_request_processing_group);

        m_submit_processing_group = new QGroupBox(
            tr("2. Complete one submitted payment"), activity);
        m_submit_processing_group->setObjectName("paymasterManualSubmitGroup");
        auto* submit_layout = new QVBoxLayout(m_submit_processing_group);
        auto* submit_explanation = new QLabel(tr(
            "Handles one client-signed payment or recovery submission. Core revalidates the "
            "exact authorized transaction before signing provider inputs. A successful action "
            "can spend reserved provider DGB, commit the transaction durably, and broadcast it."),
            m_submit_processing_group);
        submit_explanation->setObjectName("paymasterSubmitProcessingExplanation");
        submit_explanation->setWordWrap(true);
        submit_layout->addWidget(submit_explanation);
        m_process_submits = new QPushButton(
            tr("Review and process one submitted payment…"), m_submit_processing_group);
        m_process_submits->setObjectName("paymasterProcessOneSubmit");
        submit_layout->addWidget(m_process_submits, 0, Qt::AlignLeft);
        activity_layout->addWidget(m_submit_processing_group);

        m_activity_result_group = new QGroupBox(
            tr("Last processing result"), activity);
        m_activity_result_group->setObjectName("paymasterManualResultGroup");
        auto* activity_result_layout = new QVBoxLayout(m_activity_result_group);
        m_activity_action_result = new QLabel(
            tr("No request or submitted payment has been processed from this screen yet."),
            m_activity_result_group);
        m_activity_action_result->setObjectName("paymasterActivityActionResult");
        m_activity_action_result->setWordWrap(true);
        m_activity_action_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        activity_result_layout->addWidget(m_activity_action_result);
        activity_layout->addWidget(m_activity_result_group);

        m_reservations_group = new QGroupBox(
            tr("Reserved and committed pool outputs"), activity);
        auto* reservations_group = m_reservations_group;
        reservations_group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* reservations_layout = new QVBoxLayout(reservations_group);
        auto* reservations_explanation = new QLabel(tr(
            "A reservation is a wallet pool output temporarily assigned to an in-progress "
            "quote, payment, or recovery. It is not an additional charge. This list also "
            "shows committed, spent, or replacement-pending outputs while Core preserves "
            "their durable state. They cannot be offered to another client."),
            reservations_group);
        reservations_explanation->setObjectName("paymasterReservationsExplanation");
        reservations_explanation->setWordWrap(true);
        reservations_layout->addWidget(reservations_explanation);
        auto* refresh_activity = new QPushButton(
            tr("Refresh provider activity"), reservations_group);
        refresh_activity->setObjectName("paymasterRefreshReservations");
        reservations_layout->addWidget(refresh_activity, 0, Qt::AlignLeft);
        m_activity_summary = new QLabel(
            tr("Reservations have not been checked yet."), reservations_group);
        m_activity_summary->setObjectName("paymasterActivitySummary");
        m_activity_summary->setWordWrap(true);
        m_activity_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        reservations_layout->addWidget(m_activity_summary);
        activity_layout->addWidget(reservations_group);

        m_recovery_group = new QGroupBox(
            tr("What recovery means here"), activity);
        auto* recovery_group = m_recovery_group;
        recovery_group->setProperty("paymasterRole", QStringLiteral("notice"));
        auto* recovery_layout = new QVBoxLayout(recovery_group);
        auto* recovery_explanation = new QLabel(tr(
            "Recovery is not a manual way to rewrite or release a payment. Core keeps signed "
            "and committed work durable across restarts. Recovery requests use the first queue; "
            "recovery submissions use the second. Exact committed transactions are also "
            "reconciled when the provider starts. If an item remains reserved, inspect the "
            "technical result before attempting any cancellation through the RPC console."),
            recovery_group);
        recovery_explanation->setObjectName("paymasterRecoveryExplanation");
        recovery_explanation->setWordWrap(true);
        recovery_layout->addWidget(recovery_explanation);
        activity_layout->addWidget(recovery_group);

        auto* activity_details_toggle = new QPushButton(
            tr("Show last technical result"), activity);
        activity_details_toggle->setObjectName("paymasterActivityDetailsToggle");
        activity_details_toggle->setCheckable(true);
        activity_details_toggle->setFlat(true);
        activity_layout->addWidget(activity_details_toggle, 0, Qt::AlignLeft);
        m_activity_output = new QPlainTextEdit(activity);
        m_activity_output->setObjectName("paymasterActivityTechnicalResult");
        m_activity_output->setReadOnly(true);
        m_activity_output->setMaximumHeight(240);
        m_activity_output->setPlaceholderText(tr("No technical result available"));
        m_activity_output->viewport()->setBackgroundRole(QPalette::Window);
        m_activity_output->viewport()->setAutoFillBackground(true);
        m_activity_output->setVisible(false);
        activity_layout->addWidget(m_activity_output);
        activity_layout->addStretch();
        activity_scroll->setWidget(activity);
        ConfigurePaymasterScrollArea(activity_scroll, activity);
        m_activity_page = activity_scroll;
        m_tabs->addTab(activity_scroll, tr("Operations"));

        connect(refresh, &QPushButton::clicked, this, [this] { refreshStatus(); });
        connect(m_guided_setup, &QPushButton::clicked, this, [this] {
            if (showSetupWizard()) {
                setSetupMode(PaymasterSetupMode::GUIDED);
                m_tabs->setCurrentIndex(0);
                refreshStatus();
            }
        });
        connect(m_reopen_wizard, &QPushButton::clicked, this, [this] {
            if (showSetupWizard()) {
                m_tabs->setCurrentIndex(0);
                refreshStatus();
            }
        });
        connect(m_expert_setup, &QPushButton::clicked, this, [this] {
            if (QMessageBox::warning(
                    this, tr("Manual Paymaster setup"),
                    tr("Manual setup exposes every provider control immediately. Incomplete settings keep the provider offline, while overly broad sponsored budgets can spend more DGB than intended.\n\nContinue with expert setup?"),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) == QMessageBox::Yes) {
                setSetupMode(PaymasterSetupMode::EXPERT);
                m_tabs->setCurrentWidget(m_configuration_page);
            }
        });
        connect(details_toggle, &QPushButton::toggled, this,
                [this, details_toggle, details](bool visible) {
                    details->setVisible(visible);
                    details_toggle->setText(visible ? tr("Hide technical details")
                                                    : tr("Show technical details"));
                });
        connect(activity_details_toggle, &QPushButton::toggled, this,
                [this, activity_details_toggle](bool visible) {
                    m_activity_output->setVisible(visible);
                    activity_details_toggle->setText(
                        visible ? tr("Hide last technical result")
                                 : tr("Show last technical result"));
                });
        connect(m_runtime_settings_toggle, &QPushButton::toggled, this,
                [this](bool visible) {
                    m_runtime_settings_panel->setVisible(visible);
                    m_runtime_settings_toggle->setText(
                        visible ? tr("Hide advanced provider runtime settings")
                                : tr("Show advanced provider runtime settings"));
                });
        connect(m_operation_mode_select,
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] {
                    if (!m_loading_runtime_settings) {
                        m_runtime_settings_dirty = true;
                        updateProviderButtons();
                    }
                });
        connect(m_autostart, &QCheckBox::toggled, this, [this] {
            if (!m_loading_runtime_settings) {
                m_runtime_settings_dirty = true;
                updateProviderButtons();
            }
        });
        connect(m_save_runtime_settings, &QPushButton::clicked, this,
                [this] { saveRuntimeSettings(); });
        connect(m_start, &QPushButton::clicked, this, [this] {
            if (QMessageBox::question(
                    this, tr("Start Paymaster provider"),
                    providerStartConfirmationText(),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes) {
                return;
            }
            call("startpaymaster", {}, false, nullptr,
                 [this](const UniValue& result) {
                     presentProviderStartResult(result);
                     refreshStatus();
                  });
        });
        connect(m_stop, &QPushButton::clicked, this, [this] { call("stoppaymaster", {}, false); });
        connect(m_operation_primary, &QPushButton::clicked, this, [this] {
            if (m_core_running) {
                m_stop->click();
            } else if (m_start->isEnabled()) {
                m_start->click();
            } else {
                refreshStatus();
            }
        });
        connect(m_overview_offer_action, &QPushButton::clicked, this,
                [this] { m_tabs->setCurrentWidget(m_configuration_page); });
        connect(m_overview_safety_action, &QPushButton::clicked, this,
                [this] { m_tabs->setCurrentWidget(m_safety_page); });
        connect(m_overview_liquidity_action, &QPushButton::clicked, this,
                [this] {
                    if (maintenanceFeeLimitExceeded()) {
                        reviewMaintenanceFeeLimit();
                        return;
                    }
                    if (m_maintenance_state ==
                        QLatin1String("waiting_for_maintenance_approval")) {
                        approveSuggestedLiquidityMaintenance();
                        return;
                    }
                    // This label is an action, not a navigation shortcut. If
                    // the bounded refill is already approved and the provider
                    // is stopped, start the provider through the same guarded
                    // confirmation path as the primary Overview button.
                    if (shouldStartAndRestoreLiquidity()) {
                        m_start->click();
                        return;
                    }
                    m_tabs->setCurrentWidget(m_liquidity_page);
                    if (!m_liquidity_targets_satisfy_provider_policy &&
                        m_liquidity_advanced_toggle) {
                        m_liquidity_advanced_toggle->setChecked(true);
                    }
                });
        connect(m_overview_operation_action, &QPushButton::clicked, this,
                [this] {
                    if (maintenanceFeeLimitExceeded()) {
                        reviewMaintenanceFeeLimit();
                    } else if (!m_liquidity_targets_satisfy_provider_policy) {
                        m_tabs->setCurrentWidget(m_liquidity_page);
                        if (m_liquidity_advanced_toggle) {
                            m_liquidity_advanced_toggle->setChecked(true);
                        }
                    } else if (!m_core_running && providerConfigurationComplete() &&
                        hasPassiveExternalWait()) {
                        refreshStatus();
                    } else if (m_maintenance_state ==
                               QLatin1String("waiting_for_maintenance_approval")) {
                        approveSuggestedLiquidityMaintenance();
                    } else if (shouldStartAndRestoreLiquidity()) {
                        m_start->click();
                    } else {
                        m_tabs->setCurrentWidget(m_activity_page);
                    }
                });
        connect(m_overview_finance_action, &QPushButton::clicked, this,
                [this] {
                    m_tabs->setCurrentWidget(m_finance_page);
                    if (!m_busy) refreshFinanceStatus();
                });
        connect(m_overview_backup_now, &QPushButton::clicked, this,
                [this] { requestProviderBackup(); });
        connect(m_overview_backup_external, &QPushButton::clicked, this,
                [this] { acknowledgeExternalBackup(); });
        connect(m_finance_backup_now, &QPushButton::clicked, this,
                [this] { requestProviderBackup(); });
        connect(m_finance_backup_external, &QPushButton::clicked, this,
                [this] { acknowledgeExternalBackup(); });
        connect(m_finance_refresh, &QPushButton::clicked, this,
                [this] { refreshFinanceStatus(); });
        connect(m_finance_period_select,
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] {
                    if (m_tabs->currentWidget() == m_finance_page && !m_busy) {
                        refreshFinanceStatus();
                    }
                });
        connect(m_finance_export, &QPushButton::clicked, this,
                [this] { exportFinanceCsv(); });
        connect(m_finance_withdraw_fees, &QPushButton::clicked, this,
                [this] { openLiquidityFinanceControl(m_preview_carrier_excess); });
        connect(m_finance_release_carrier, &QPushButton::clicked, this,
                [this] { openLiquidityFinanceControl(m_preview_carrier_release); });
        connect(m_finance_review_dgb, &QPushButton::clicked, this,
                [this] { openLiquidityFinanceControl(m_rebalance_preview); });
        connect(m_finance_add_liquidity, &QPushButton::clicked, this,
                [this] { openLiquidityFinanceControl(m_prepare_preview); });
        connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
            if (m_tabs->currentWidget() == m_finance_page &&
                hasRpcTransport() && !m_busy) {
                refreshFinanceStatus();
            }
        });
        connect(create_identity, &QPushButton::clicked, this, [this] {
            UniValue params{UniValue::VARR};
            params.push_back(m_display_name->text().trimmed().toStdString());
            call("createpaymasteridentity", std::move(params), true);
        });
        connect(save_policy, &QPushButton::clicked, this, [this] { savePolicy(); });
        connect(save_provider_safety, &QPushButton::clicked, this, [this] { saveProviderSafetyPolicy(); });
        connect(save_client_safety, &QPushButton::clicked, this, [this] { saveClientSafetyPolicy(); });
        connect(restore_quote_defaults, &QPushButton::clicked,
                this, [this] { restoreQuoteSafetyDefaults(); });
        connect(restore_client_defaults, &QPushButton::clicked,
                this, [this] { restoreClientSafetyDefaults(); });
        connect(m_enable, &QPushButton::clicked, this, [this] { enableProvider(); });
        connect(m_approve_liquidity_maintenance, &QPushButton::clicked,
                this, [this] {
                    if (maintenanceFeeLimitExceeded()) {
                        reviewMaintenanceFeeLimit();
                    } else {
                        approveSuggestedLiquidityMaintenance();
                    }
                });
        connect(restore_policy_defaults, &QPushButton::clicked,
                this, [this] { restorePolicyDefaults(); });
        connect(restore_liquidity_defaults, &QPushButton::clicked,
                this, [this] { restoreLiquidityDefaults(); });
        connect(m_save_liquidity_policy, &QPushButton::clicked,
                this, [this] { saveLiquidityPolicy(); });
        connect(m_save_liquidity_policy_primary, &QPushButton::clicked,
                this, [this] { saveLiquidityPolicy(); });
        connect(m_automatic_replenishment, &QCheckBox::toggled,
                this, [this] { markLiquidityPolicyDirty(); });
        connect(m_paid_maintenance_approved, &QCheckBox::toggled,
                this, [this] { markLiquidityPolicyDirty(); });
        for (QLineEdit* control : {m_maintenance_fee_per_transaction,
                                   m_maintenance_fee_per_hour,
                                   m_maintenance_fee_per_day}) {
            connect(control, &QLineEdit::textChanged, this,
                    [this] { markLiquidityPolicyDirty(); });
        }
        connect(m_preview_carrier_excess, &QPushButton::clicked,
                this, [this] { carrierWithdrawalAction("all_excess", false); });
        connect(m_execute_carrier_excess, &QPushButton::clicked,
                this, [this] { carrierWithdrawalAction("all_excess", true); });
        connect(m_preview_carrier_release, &QPushButton::clicked,
                this, [this] { carrierWithdrawalAction("release_slot", false); });
        connect(m_execute_carrier_release, &QPushButton::clicked,
                this, [this] { carrierWithdrawalAction("release_slot", true); });
        connect(m_release_carrier_select,
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { invalidateCarrierWithdrawalPreviews(); });
        connect(prepare_preview, &QPushButton::clicked, this, [this] { poolAction("preparepaymasterpool", false); });
        connect(prepare_execute, &QPushButton::clicked, this, [this] { poolAction("preparepaymasterpool", true); });
        connect(rebalance_preview, &QPushButton::clicked, this, [this] { poolAction("rebalancepaymasterpool", false); });
        connect(rebalance_execute, &QPushButton::clicked, this, [this] { poolAction("rebalancepaymasterpool", true); });
        connect(refresh_activity, &QPushButton::clicked, this, [this] {
            refreshActivity();
        });
        connect(m_process_requests, &QPushButton::clicked, this, [this] {
            processActivityRequest();
        });
        connect(m_process_submits, &QPushButton::clicked, this, [this] {
            processActivitySubmit();
        });
        watchFundingSafetyControls(m_user_paid_safety);
        watchFundingSafetyControls(m_public_sponsored_safety);
        watchFundingSafetyControls(m_restricted_sponsored_safety);
        for (QSpinBox* control : {m_max_active_quotes_total,
                                  m_max_active_quotes_per_netgroup,
                                  m_max_active_quotes_per_recipient,
                                  m_max_quote_requests_per_netgroup}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
                if (!m_loading_provider_safety) markProviderSafetyDirty();
                updateProviderButtons();
            });
        }
        connect(m_sponsored, &QCheckBox::toggled, this, [this] {
            if (!m_loading_policy) m_policy_dirty = true;
            updatePolicyDisplay();
            updateLiquidityDisplay();
            updateProviderButtons();
        });
        connect(m_user_paid, &QCheckBox::toggled, this, [this](bool checked) {
            if (!m_loading_policy) m_policy_dirty = true;
            if (checked && m_scope->currentData().toString() == QLatin1String("restricted")) {
                m_scope->setCurrentIndex(m_scope->findData(QStringLiteral("public")));
            }
            updatePolicyDisplay();
            updateLiquidityDisplay();
            updateProviderButtons();
        });
        connect(m_scope, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            if (!m_loading_policy) m_policy_dirty = true;
            if (m_scope->currentData().toString() == QLatin1String("restricted")) {
                m_sponsored->setChecked(true);
                m_user_paid->setChecked(false);
            }
            updatePolicyDisplay();
            updateProviderButtons();
        });
        for (QSpinBox* control : {m_fee_bps, m_min_amount, m_max_amount,
                                  m_quote_ttl, m_network_fee}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged),
                    this, [this] {
                        if (!m_loading_policy) m_policy_dirty = true;
                        updatePolicyDisplay();
                    });
        }
        for (QSpinBox* control : {m_admission_dgb, m_operational_dgb,
                                  m_admission_carriers, m_operational_carriers}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged),
                    this, [this] {
                        markLiquidityPolicyDirty();
                        updateLiquidityDisplay();
                    });
        }
        connect(m_client_fee_per_transaction, qOverload<int>(&QSpinBox::valueChanged),
                this, [this] {
                    if (!m_loading_client_safety) markClientSafetyDirty();
                    updateClientSafetyDisplay();
                });
        connect(m_client_fee_per_day, qOverload<int>(&QSpinBox::valueChanged),
                this, [this] {
                    if (!m_loading_client_safety) markClientSafetyDirty();
                    updateClientSafetyDisplay();
                });
        // Classify every retained operator control so the generic bright-blue
        // wallet button style cannot leak into these pages. Explicit primary,
        // danger and disclosure roles assigned above remain untouched.
        for (QGroupBox* group : findChildren<QGroupBox*>()) {
            if (!group->property("paymasterRole").isValid()) {
                group->setProperty("paymasterRole", QStringLiteral("card"));
            }
        }
        for (QPushButton* button : findChildren<QPushButton*>()) {
            if (!button->property("paymasterRole").isValid()) {
                button->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
            }
            button->setCursor(Qt::PointingHandCursor);
        }
        updateFundingSafetyDisplay(m_user_paid_safety);
        updateFundingSafetyDisplay(m_public_sponsored_safety);
        updateFundingSafetyDisplay(m_restricted_sponsored_safety);
        updateClientSafetyDisplay();
        updatePolicyDisplay();
        updateLiquidityDisplay();
        updateSetupAccess();
        updateProviderButtons();

        m_setup_status_timer = new QTimer(this);
        m_setup_status_timer->setInterval(10000);
        connect(m_setup_status_timer, &QTimer::timeout, this, [this] {
            const bool maintenance_wait =
                m_maintenance_state == QLatin1String("replenishing_liquidity") ||
                m_maintenance_state ==
                    QLatin1String("waiting_for_liquidity_confirmation");
            if ((m_setup_waiting_for_confirmations || maintenance_wait ||
                 hasPassiveExternalWait()) &&
                m_model && !m_busy) {
                refreshStatus();
            }
        });
    }

    /**
     * Install the application-level full-wallet backup action.
     *
     * The provider console deliberately reuses WalletView's established file
     * picker and backup reporting instead of duplicating backup mechanics in
     * this presentation-only widget.
     */
    void setProviderBackupRequestHandler(std::function<void()> handler)
    {
        m_provider_backup_request_handler = std::move(handler);
    }

    void setWalletModel(WalletModel* model)
    {
        m_model = model;
        m_busy = false;
        m_setup_mode = loadSetupMode();
        applyRecommendedPolicyDefaults(/*mark_dirty=*/false);
        restoreLiquidityDefaults(/*announce=*/false);
        restoreFundingSafetyDefaults(m_user_paid_safety,
                                     /*recommended_defaults=*/true,
                                     /*mark_dirty=*/false);
        restoreFundingSafetyDefaults(m_public_sponsored_safety,
                                     /*recommended_defaults=*/false,
                                     /*mark_dirty=*/false);
        restoreFundingSafetyDefaults(m_restricted_sponsored_safety,
                                     /*recommended_defaults=*/false,
                                     /*mark_dirty=*/false);
        restoreQuoteSafetyDefaults(/*mark_dirty=*/false);
        restoreClientSafetyDefaults(/*mark_dirty=*/false);
        m_loading_runtime_settings = true;
        m_operation_mode = QStringLiteral("automatic");
        m_autostart_enabled = false;
        m_service_state = QStringLiteral("stopped");
        m_last_service_error.clear();
        m_waiting_provider_requests = 0;
        m_waiting_provider_submits = 0;
        m_runtime_settings_dirty = false;
        m_operation_mode_select->setCurrentIndex(
            m_operation_mode_select->findData(m_operation_mode));
        m_autostart->setChecked(false);
        m_loading_runtime_settings = false;
        m_runtime_settings_result->setText(tr(
            "Automatic processing is the recommended default. Provider autostart is off by default."));
        m_provider_safety_configured = false;
        m_client_safety_configured = false;
        m_core_eligible = false;
        m_core_enabled = false;
        m_core_running = false;
        m_core_ready = false;
        m_core_locked = true;
        m_core_has_identity = false;
        m_provider_id.clear();
        m_backup_required = false;
        m_finance_last_result = UniValue{UniValue::VOBJ};
        updateBackupReminder(false, {});
        m_finance_export->setEnabled(false);
        m_finance_daily_totals->setRowCount(0);
        m_finance_events->setRowCount(0);
        m_finance_history_status->setText(
            tr("Finance history has not been loaded yet."));
        m_finance_result_estimate->setText(
            tr("Load finance data to calculate the current estimate."));
        m_finance_model_breakdown->clear();
        m_overview_finance_status->setText(
            tr("Waiting · finance history is available after provider identity creation"));
        setStatusLabel(m_overview_finance_status,
                       m_overview_finance_status->text(),
                       QStringLiteral("waiting"));
        m_oracle_state = OracleState::UNKNOWN;
        m_oracle_price_micro_usd = 0;
        m_pool_admission_dgb = 0;
        m_pool_admission_carriers = 0;
        m_pool_operational_dgb = 0;
        m_pool_operational_carriers = 0;
        m_pool_status_loaded = false;
        m_readiness_errors.clear();
        m_liquidity_policy_configured = false;
        m_liquidity_targets_satisfy_provider_policy = false;
        m_liquidity_policy_dirty = false;
        m_loading_liquidity_policy = false;
        m_maintenance_state = QStringLiteral("unavailable");
        m_carrier_base_cents = 0;
        m_carrier_excess_cents = 0;
        m_pending_successor_carrier_cents = 0;
        m_pending_successor_dgb_satoshis = 0;
        invalidateCarrierWithdrawalPreviews();
        m_setup_waiting_for_confirmations = false;
        if (m_setup_status_timer) m_setup_status_timer->stop();
        m_enable_status->setText(
            tr("Provider configuration is currently disabled for this wallet."));
        m_provider_safety_status->setText(
            tr("Provider safety policy: not configured (provider not ready)"));
        m_client_safety_status->setText(
            tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"));
        m_provider_safety_usage->setText(tr("Budget usage is not available yet."));
        m_liquidity_current_status->setText(
            tr("Current confirmed pool: status not loaded yet."));
        m_liquidity_policy_status->setText(
            tr("Liquidity policy has not been loaded yet."));
        m_liquidity_slot_status->setText(
            tr("Ready, pending and missing target counts have not been loaded yet."));
        m_liquidity_recycling_status->setText(
            tr("No recyclable successor information is available yet."));
        m_liquidity_budget_status->setText(
            tr("Maintenance budget use has not been loaded yet."));
        m_carrier_value_status->setText(
            tr("Carrier base and withdrawable excess have not been loaded yet."));
        m_liquidity_maintenance_state->setText(
            tr("Automatic liquidity status is not available yet."));
        m_liquidity_maintenance_next_step->setText(
            tr("Refresh after selecting a provider wallet."));
        m_liquidity_maintenance_cost->clear();
        updateExternalPrerequisites();
        m_approve_liquidity_maintenance->setVisible(false);
        m_release_carrier_select->clear();
        m_preview_carrier_excess->setEnabled(false);
        m_preview_carrier_release->setEnabled(false);
        m_wallet->setText(model ? tr("Selected wallet: %1").arg(model->getDisplayName())
                                : tr("No wallet selected"));
        setEnabled(model != nullptr);
        updateSetupAccess();
        updateProviderButtons();
        if (model) refreshStatus();
    }

    void refreshStatus()
    {
        // Retain the last authoritative snapshot while the asynchronous RPCs
        // are in flight. Temporarily forcing ready=false made every passive
        // ten-second refresh switch buttons and cards to an error-looking
        // state before immediately restoring the same result.
        call("getpaymasterinfo", {}, false, nullptr, [this](const UniValue& result) {
            const bool eligible = result.find_value("wallet_eligible").get_bool();
            const bool enabled = result.find_value("enabled").get_bool();
            const bool running = result.find_value("running").get_bool();
            const bool ready = result.find_value("ready").get_bool();
            const bool locked = result.find_value("wallet_locked").get_bool();
            m_core_eligible = eligible;
            m_core_enabled = enabled;
            m_core_running = running;
            m_core_ready = ready;
            m_core_locked = locked;
            const UniValue& operation_mode = result.find_value("operation_mode");
            const UniValue& autostart = result.find_value("autostart");
            const UniValue& service_state = result.find_value("service_state");
            const UniValue& last_service_error = result.find_value("last_service_error");
            const UniValue& service_queue = result.find_value("service_queue");
            m_operation_mode = operation_mode.isStr()
                ? QString::fromStdString(operation_mode.get_str())
                : QStringLiteral("automatic");
            m_autostart_enabled = autostart.isBool() && autostart.get_bool();
            m_service_state = service_state.isStr()
                ? QString::fromStdString(service_state.get_str())
                : QStringLiteral("stopped");
            m_last_service_error = last_service_error.isStr()
                ? QString::fromStdString(last_service_error.get_str())
                : QString{};
            const UniValue& waiting_requests =
                service_queue.find_value("waiting_requests");
            const UniValue& waiting_submits =
                service_queue.find_value("waiting_submits");
            m_waiting_provider_requests = waiting_requests.isNum()
                ? waiting_requests.getInt<int>()
                : 0;
            m_waiting_provider_submits = waiting_submits.isNum()
                ? waiting_submits.getInt<int>()
                : 0;
            if (!m_runtime_settings_dirty) {
                m_loading_runtime_settings = true;
                const int mode_index = m_operation_mode_select->findData(m_operation_mode);
                if (mode_index >= 0) m_operation_mode_select->setCurrentIndex(mode_index);
                m_autostart->setChecked(m_autostart_enabled);
                m_loading_runtime_settings = false;
            }
            m_enable_status->setText(enabled
                ? tr("Provider configuration enabled successfully. It is saved in this wallet. "
                     "The provider remains offline until you start it from Overview.")
                : tr("Provider configuration is currently disabled for this wallet."));
            if (m_service_state == QLatin1String("active")) {
                m_status->setText(tr("Automatic provider operation active — eligible requests and validated payments are processed by Core within the saved limits."));
            } else if (m_service_state == QLatin1String("manual")) {
                m_status->setText(tr("Provider running in manual expert mode — waiting messages require manual processing from Activity and recovery."));
            } else if (m_service_state == QLatin1String("waiting_for_unlock")) {
                m_status->setText(tr("Provider paused — unlock this wallet to resume automatic processing. No queued message is consumed while locked."));
            } else if (m_service_state == QLatin1String("waiting_for_readiness")) {
                m_status->setText(tr("Provider autostart is waiting for the remaining readiness requirements."));
            } else if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
                m_status->setText(tr("Provider started safely but is waiting for your one-time approval of finite paid liquidity-maintenance limits."));
            } else if (m_service_state == QLatin1String("replenishing_liquidity")) {
                m_status->setText(tr("Provider is automatically restoring missing liquidity within the approved maintenance limits."));
            } else if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
                m_status->setText(tr("Provider is waiting for replacement liquidity to confirm before accepting new requests."));
            } else if (m_service_state == QLatin1String("drain_only")) {
                m_status->setText(tr("Provider in recovery-only operation — no new quotes are accepted while existing durable work is completed safely."));
            } else if (m_service_state == QLatin1String("error")) {
                m_status->setText(tr("Automatic provider operation requires attention. Open Activity and recovery for the stable error code."));
            } else if (running) {
                m_status->setText(tr("Provider running — this wallet is currently available to eligible DigiDollar clients."));
            } else if (ready) {
                m_status->setText(tr("Setup complete — review the settings, then start the provider when you are ready."));
            } else {
                m_status->setText(tr("Setup incomplete — no provider service is currently running."));
            }
            const UniValue& provider = result.find_value("provider_id");
            m_core_has_identity = provider.isStr() && !provider.get_str().empty();
            m_provider_id = m_core_has_identity
                ? QString::fromStdString(provider.get_str()) : QString{};
            const UniValue& backup_status = result.find_value("backup_status");
            const bool backup_required = backup_status.isObject() &&
                backup_status.find_value("required").isBool() &&
                backup_status.find_value("required").get_bool();
            updateBackupReminder(backup_required, m_provider_id);
            const UniValue& finance_summary = result.find_value("finance_summary");
            m_overview_finance_action->setText(tr("Open finances"));
            if (finance_summary.isObject()) {
                const qint64 income = financeNumber(
                    finance_summary, "service_fee_income_cents");
                const qint64 cost = financeNumber(
                    finance_summary, "dgb_operating_cost_satoshis");
                const qint64 transfers = financeNumber(
                    finance_summary, "successful_transfers");
                setStatusLabel(
                    m_overview_finance_status,
                    tr("%1 DD service fees · %2 DGB costs · %3 successful transfer(s)")
                        .arg(ddAmount(income), dgbAmount(cost))
                        .arg(transfers),
                    QStringLiteral("ready"));
            } else if (m_core_has_identity) {
                setStatusLabel(
                    m_overview_finance_status,
                    tr("Waiting · finance ledger has not been initialized yet"),
                    QStringLiteral("waiting"));
            } else {
                setStatusLabel(
                    m_overview_finance_status,
                    tr("Available after the provider identity is created"),
                    QStringLiteral("waiting"));
            }
            const UniValue& endpoint = result.find_value("endpoint");
            const UniValue& display_name = result.find_value("display_name");
            if (display_name.isStr()) {
                m_display_name->setText(QString::fromStdString(display_name.get_str()));
            }
            const UniValue& policy = result.find_value("policy");
            if (policy.isObject() && !m_policy_dirty) loadPolicy(policy);
            m_identity->setText(tr("Identity: %1\nEndpoint: %2")
                .arg(provider.isStr() ? QString::fromStdString(provider.get_str()) : tr("not configured"),
                     endpoint.isStr() ? QString::fromStdString(endpoint.get_str())
                                      : tr("set -paymasterendpoint=<ip:port> and restart")));
            const UniValue& pool = result.find_value("pool");
            bool has_pool_entries{false};
            if (pool.isObject()) {
                m_pool_status_loaded = true;
                m_pool_admission_dgb = pool.find_value("admission_dgb").getInt<int>();
                m_pool_admission_carriers = pool.find_value("admission_carriers").getInt<int>();
                m_pool_operational_dgb = pool.find_value("operational_dgb").getInt<int>();
                m_pool_operational_carriers = pool.find_value("operational_carriers").getInt<int>();
                has_pool_entries =
                    m_pool_admission_dgb > 0 ||
                    m_pool_admission_carriers > 0 ||
                    m_pool_operational_dgb > 0 ||
                    m_pool_operational_carriers > 0 ||
                    pool.find_value("reserved").getInt<int>() > 0;
                m_pool->setText(tr("Pool: admission DGB %1, carriers %2; operational DGB %3, carriers %4; reserved %5")
                    .arg(m_pool_admission_dgb)
                    .arg(m_pool_admission_carriers)
                    .arg(m_pool_operational_dgb)
                    .arg(m_pool_operational_carriers)
                    .arg(pool.find_value("reserved").getInt<int>()));
                m_liquidity_current_status->setText(tr(
                    "Current confirmed and available pool — admission DGB: %1; admission carriers: %2; operational DGB: %3; operational carriers: %4; usable complete operational slots: %5; currently reserved: %6.")
                    .arg(pool.find_value("admission_dgb").getInt<int>())
                    .arg(pool.find_value("admission_carriers").getInt<int>())
                    .arg(pool.find_value("operational_dgb").getInt<int>())
                    .arg(pool.find_value("operational_carriers").getInt<int>())
                    .arg(pool.find_value("complete_operational_slots").getInt<int>())
                    .arg(pool.find_value("reserved").getInt<int>()));
            }
            const bool has_existing_configuration =
                (provider.isStr() && !provider.get_str().empty()) || policy.isObject() ||
                enabled || running || ready || has_pool_entries;
            if (m_setup_mode == PaymasterSetupMode::UNDECIDED &&
                has_existing_configuration) {
                setSetupMode(PaymasterSetupMode::EXISTING);
            }
            QStringList errors;
            for (const UniValue& error : result.find_value("readiness_errors").getValues()) {
                errors.push_back(QString::fromStdString(error.get_str()));
            }
            m_readiness_errors = errors;
            m_readiness->setPlainText(errors.isEmpty() ? tr("All readiness gates pass.") : errors.join('\n'));
            updateReadinessSummary(errors, running, ready);
            updateProviderHeaderStatus();
            if (m_setup_waiting_for_confirmations) {
                if (ready || running) {
                    m_setup_waiting_for_confirmations = false;
                    m_setup_status_timer->stop();
                } else if (errors.contains(QStringLiteral("PAYMASTER_POOLS_NOT_PREPARED"))) {
                    m_next_step->setText(tr("Waiting for liquidity confirmations"));
                    m_readiness_summary->setText(tr(
                        "All settings have been saved and no further Save buttons are required. "
                        "The newly prepared pool outputs must confirm before the provider can start. "
                        "This status refreshes automatically."));
                }
            }
            m_reopen_wizard->setText(has_existing_configuration
                ? tr("Review setup with assistant…")
                : tr("Continue guided setup…"));
            if (m_offer_identity_status) {
                setStatusLabel(
                    m_offer_identity_status,
                    m_core_has_identity
                        ? tr("Ready · persistent identity is stored in this wallet")
                        : tr("Action required · no provider identity has been created yet."),
                    m_core_has_identity ? QStringLiteral("ready")
                                        : QStringLiteral("action"));
                m_offer_identity_id->setText(
                    m_core_has_identity && provider.isStr()
                        ? QString::fromStdString(provider.get_str())
                        : tr("Not created yet"));
                m_create_identity->setVisible(!m_core_has_identity);
            }
            updateProviderButtons();
            updateAutomaticRefreshTimer();
            refreshOracleStatus();
            refreshLiquidityStatus();
        }, false);
    }

    void setReadinessStatusForTesting(const UniValue& status)
    {
        m_core_eligible = status.find_value("wallet_eligible").isBool()
            ? status.find_value("wallet_eligible").get_bool() : true;
        m_core_enabled = status.find_value("enabled").isBool()
            ? status.find_value("enabled").get_bool() : true;
        m_core_running = status.find_value("running").isBool() &&
                         status.find_value("running").get_bool();
        m_core_ready = status.find_value("ready").isBool() &&
                       status.find_value("ready").get_bool();
        m_core_locked = status.find_value("wallet_locked").isBool() &&
                        status.find_value("wallet_locked").get_bool();
        const UniValue& operation_mode = status.find_value("operation_mode");
        const UniValue& autostart = status.find_value("autostart");
        const UniValue& service_state = status.find_value("service_state");
        const UniValue& last_service_error =
            status.find_value("last_service_error");
        const UniValue& service_queue = status.find_value("service_queue");
        m_operation_mode = operation_mode.isStr()
            ? QString::fromStdString(operation_mode.get_str())
            : QStringLiteral("automatic");
        m_autostart_enabled = autostart.isBool() && autostart.get_bool();
        m_service_state = service_state.isStr()
            ? QString::fromStdString(service_state.get_str())
            : QStringLiteral("stopped");
        m_last_service_error = last_service_error.isStr()
            ? QString::fromStdString(last_service_error.get_str())
            : QString{};
        const UniValue& waiting_requests =
            service_queue.find_value("waiting_requests");
        const UniValue& waiting_submits =
            service_queue.find_value("waiting_submits");
        m_waiting_provider_requests = waiting_requests.isNum()
            ? waiting_requests.getInt<int>()
            : 0;
        m_waiting_provider_submits = waiting_submits.isNum()
            ? waiting_submits.getInt<int>()
            : 0;
        if (!m_runtime_settings_dirty) {
            m_loading_runtime_settings = true;
            const int mode_index =
                m_operation_mode_select->findData(m_operation_mode);
            if (mode_index >= 0) {
                m_operation_mode_select->setCurrentIndex(mode_index);
            }
            m_autostart->setChecked(m_autostart_enabled);
            m_loading_runtime_settings = false;
        }
        m_core_has_identity = status.find_value("has_identity").isBool()
            ? status.find_value("has_identity").get_bool() : true;
        const UniValue& provider_id = status.find_value("provider_id");
        if (m_offer_identity_id) {
            m_offer_identity_id->setText(
                m_core_has_identity && provider_id.isStr()
                    ? QString::fromStdString(provider_id.get_str())
                    : tr("Not created yet"));
        }
        m_policy_loaded = status.find_value("has_policy").isBool()
            ? status.find_value("has_policy").get_bool() : true;
        m_provider_safety_configured =
            status.find_value("has_safety_policy").isBool()
                ? status.find_value("has_safety_policy").get_bool() : true;
        // A readiness-only fixture has no pool object. Seed a neutral pool
        // snapshot only when no more specific liquidity fixture was applied
        // before it; otherwise preserve the authoritative missing/pending
        // counts just like the production asynchronous refresh does.
        if (!m_pool_status_loaded) {
            m_pool_status_loaded = true;
            m_pool_admission_dgb = m_admission_dgb->value();
            m_pool_operational_dgb = m_operational_dgb->value();
            m_pool_admission_carriers = m_admission_carriers->value();
            m_pool_operational_carriers = m_operational_carriers->value();
        }
        m_readiness_errors.clear();
        const UniValue& errors = status.find_value("readiness_errors");
        if (errors.isArray()) {
            for (const UniValue& error : errors.getValues()) {
                if (error.isStr()) {
                    m_readiness_errors.push_back(
                        QString::fromStdString(error.get_str()));
                }
            }
        }
        const UniValue& oracle = status.find_value("oracle_price_micro_usd");
        m_oracle_price_micro_usd = oracle.isNum() ? oracle.getInt<qint64>() : 0;
        m_oracle_state = m_oracle_price_micro_usd > 0
            ? OracleState::AVAILABLE : OracleState::UNAVAILABLE;
        updateReadinessSummary(m_readiness_errors, m_core_running, m_core_ready);
        updateProviderHeaderStatus();
        updateProviderButtons();
    }

    void setStartResultForTesting(const UniValue& result)
    {
        presentProviderStartResult(result);
    }

    void setLiquidityStatusForTesting(const UniValue& status)
    {
        applyLiquidityStatus(status);
    }

    void setLiquidityPoolForTesting(const UniValue& pool_info)
    {
        applyLiquidityPoolEntries(pool_info, /*refresh_safety=*/false);
    }

    void setRpcExecutorForTesting(
        DigiDollarTab::PaymasterRpcExecutorForTesting executor)
    {
        m_rpc_executor_for_testing = std::move(executor);
    }

private:
    enum class OracleState {
        UNKNOWN,
        CHECKING,
        AVAILABLE,
        UNAVAILABLE,
    };

    void presentProviderStartResult(const UniValue& result)
    {
        const UniValue& result_mode = result.find_value("operation_mode");
        const QString mode = result_mode.isStr()
            ? QString::fromStdString(result_mode.get_str())
            : m_operation_mode;
        const bool running = result.find_value("running").isBool() &&
                             result.find_value("running").get_bool();
        const bool ready = result.find_value("ready").isBool() &&
                           result.find_value("ready").get_bool();
        const UniValue& result_service_state =
            result.find_value("service_state");
        const QString service_state = result_service_state.isStr()
            ? QString::fromStdString(result_service_state.get_str())
            : QStringLiteral("stopped");

        if (!running) {
            QMessageBox::warning(
                this, tr("Paymaster provider not started"),
                !m_liquidity_targets_satisfy_provider_policy
                    ? tr("The saved liquidity targets cannot satisfy the active offer. A user-paid provider needs at least three admission DigiDollar carriers and one operational DigiDollar carrier. Review and save the liquidity targets before starting the provider.")
                    : tr("The provider is not running because one or more readiness requirements are still unmet. Refresh the Overview for the next safe action."));
            return;
        }

        if (ready && (service_state == QLatin1String("active") ||
                      service_state == QLatin1String("manual"))) {
            QMessageBox::information(
                this, tr("Paymaster provider started"),
                mode == QLatin1String("manual")
                    ? tr("The provider is now online in manual expert mode. Queue items must be processed from Operations.")
                    : tr("Automatic provider operation is active. Core now processes eligible requests and validated submissions within the saved safety budgets. No provider-side confirmation is required for each transfer."));
            return;
        }

        QString pending;
        if (service_state ==
            QLatin1String("waiting_for_maintenance_approval")) {
            pending = tr("waiting for approval of the finite refill limits");
        } else if (service_state ==
                   QLatin1String("waiting_for_liquidity_confirmation")) {
            pending = tr("replacement liquidity is waiting for confirmation");
        } else if (service_state ==
                   QLatin1String("replenishing_liquidity")) {
            pending = tr("restoring the saved liquidity targets");
        } else if (service_state == QLatin1String("drain_only")) {
            pending = tr("finishing previously authorized work in recovery-only mode");
        } else {
            pending = tr("checking the remaining readiness requirement");
        }

        // A pending start is normal background progress, not a result the
        // operator must acknowledge. A modal message box runs a nested event
        // loop, so the periodic refresh can make the Overview ready while the
        // old "startup pending" text remains in front of it. Keep progress in
        // the live status area instead; the caller immediately refreshes the
        // authoritative Core state after this short transitional message.
        m_status->setText(
            tr("Provider start accepted — %1.").arg(pending));
    }

    bool hasRpcTransport() const
    {
        return m_model || static_cast<bool>(m_rpc_executor_for_testing);
    }

    static bool isPassiveExternalReadinessError(const QString& error)
    {
        return error == QLatin1String("PAYMASTER_NODE_NOT_READY") ||
               error == QLatin1String("PAYMASTER_REQUIRES_READY_TXINDEX") ||
               error == QLatin1String("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE");
    }

    static bool isProviderSetupReadinessError(const QString& error)
    {
        return error == QLatin1String("PAYMASTER_PROVIDER_NOT_ENABLED") ||
               error == QLatin1String("PAYMASTER_IDENTITY_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_POLICY_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_SAFETY_POLICY_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_POLICY_BINDING_MISMATCH") ||
               error == QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
               error == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND") ||
               error == QLatin1String("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED") ||
               error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED") ||
               error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE");
    }

    static bool isLiquidityReadinessError(const QString& error)
    {
        // These errors describe the live inventory, not unsaved provider
        // configuration. Keeping the distinction explicit prevents a spent
        // or not-yet-confirmed slot from sending the operator back through
        // Offer or setup even though the saved targets are already correct.
        return error == QLatin1String("PAYMASTER_POOLS_NOT_PREPARED") ||
               error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
               error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
               error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING") ||
               error == QLatin1String("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING") ||
               error == QLatin1String("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED") ||
               error == QLatin1String("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED");
    }

    bool providerConfigurationComplete() const
    {
        return std::none_of(m_readiness_errors.cbegin(),
                            m_readiness_errors.cend(),
                            isProviderSetupReadinessError);
    }

    bool hasPassiveExternalWait() const
    {
        return !m_readiness_errors.isEmpty() &&
               std::any_of(m_readiness_errors.cbegin(),
                           m_readiness_errors.cend(),
                           isPassiveExternalReadinessError);
    }

    bool shouldStartAndRestoreLiquidity() const
    {
        return !m_core_running && m_start && m_start->isEnabled() &&
               !hasPassiveExternalWait() &&
               m_liquidity_targets_satisfy_provider_policy &&
               m_maintenance_state ==
                   QLatin1String("replenishing_liquidity");
    }

    bool maintenanceFeeLimitExceeded() const
    {
        return m_last_service_error ==
                   QLatin1String("PAYMASTER_MAINTENANCE_FEE_EXCEEDED") ||
               m_last_service_error ==
                   QLatin1String("PAYMASTER_MAINTENANCE_FEE_CHANGED");
    }

    void updateAutomaticRefreshTimer()
    {
        if (!m_setup_status_timer) return;
        const bool maintenance_wait =
            m_setup_waiting_for_confirmations ||
            m_maintenance_state == QLatin1String("replenishing_liquidity") ||
            m_maintenance_state ==
                QLatin1String("waiting_for_liquidity_confirmation");
        if (m_model && (maintenance_wait || hasPassiveExternalWait())) {
            m_setup_status_timer->start();
        } else {
            m_setup_status_timer->stop();
        }
    }

    void updateProviderHeaderStatus()
    {
        const bool liquidity_attention = std::any_of(
            m_readiness_errors.cbegin(), m_readiness_errors.cend(),
            [](const QString& error) { return isLiquidityReadinessError(error); });
        if (m_service_state == QLatin1String("active")) {
            m_status->setText(tr("Automatic provider operation active — eligible requests and validated payments are processed by Core within the saved limits."));
        } else if (m_service_state == QLatin1String("manual")) {
            m_status->setText(tr("Provider running in manual expert mode — waiting messages require manual processing from Operations."));
        } else if (m_service_state == QLatin1String("waiting_for_unlock")) {
            m_status->setText(tr("Provider paused — unlock this wallet to resume automatic processing. No queued message is consumed while locked."));
        } else if (m_readiness_errors.contains(QStringLiteral(
                       "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ||
                   (!m_liquidity_targets_satisfy_provider_policy &&
                    (m_last_service_error == QLatin1String(
                         "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
                     m_last_service_error == QLatin1String(
                         "PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
            m_status->setText(tr("Provider configuration needs one liquidity change — the active user-paid offer has no saved operational DigiDollar carrier target."));
        } else if (!m_core_ready && providerConfigurationComplete() &&
                   hasPassiveExternalWait()) {
            m_status->setText(tr("Provider fully configured — waiting for blockchain and node readiness. You do not need to change provider settings."));
        } else if (providerConfigurationComplete() && liquidity_attention) {
            m_status->setText(tr("Provider settings are saved — restore the missing wallet liquidity before the service can accept transfers."));
        } else if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
            m_status->setText(tr("Provider started safely but is waiting for your one-time approval of finite paid liquidity-maintenance limits."));
        } else if (m_service_state == QLatin1String("replenishing_liquidity") &&
                   maintenanceFeeLimitExceeded()) {
            m_status->setText(tr(
                "Provider running, but automatic liquidity refill is paused because the approved per-transaction maintenance-fee limit is too low. No refill transaction was created."));
        } else if (m_service_state == QLatin1String("replenishing_liquidity")) {
            m_status->setText(tr("Provider is automatically restoring missing liquidity within the approved maintenance limits."));
        } else if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
            m_status->setText(tr("Provider is waiting for replacement liquidity to confirm before accepting new requests."));
        } else if (m_service_state == QLatin1String("drain_only")) {
            m_status->setText(tr("Provider in recovery-only operation — no new quotes are accepted while existing durable work is completed safely."));
        } else if (m_service_state == QLatin1String("error")) {
            m_status->setText(tr("Automatic provider operation requires attention. Open Operations for details."));
        } else if (m_core_running) {
            m_status->setText(tr("Provider running — this wallet is currently available to eligible DigiDollar clients."));
        } else if (m_core_ready) {
            m_status->setText(tr("Provider fully configured — review the saved limits, then start it when you are ready."));
        } else {
            m_status->setText(tr("Setup incomplete — no provider service is currently running."));
        }
    }

    void updateExternalPrerequisites()
    {
        const bool node_wait = m_readiness_errors.contains(
            QStringLiteral("PAYMASTER_NODE_NOT_READY"));
        const bool txindex_missing = m_readiness_errors.contains(
            QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"));
        const bool txindex_wait = m_readiness_errors.contains(
            QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"));
        const bool activation_wait = m_readiness_errors.contains(
            QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"));

        // Readiness errors not represented by the explicit rows must remain
        // visible. Otherwise every listed prerequisite could appear green
        // while Core is still correctly refusing to start the provider.
        const QStringList represented_errors{
            QStringLiteral("PAYMASTER_NODE_NOT_READY"),
            QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"),
            QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"),
            QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"),
        };
        const bool has_other_requirement = std::any_of(
            m_readiness_errors.cbegin(), m_readiness_errors.cend(),
            [&represented_errors](const QString& error) {
                // Provider setup problems are presented by the task cards and
                // the next-action banner. Do not mislabel them as an external
                // node prerequisite merely because they have no row here.
                return !represented_errors.contains(error) &&
                       !isProviderSetupReadinessError(error) &&
                       !isLiquidityReadinessError(error);
            });
        const bool has_unclassified_failure = std::any_of(
            m_readiness_errors.cbegin(), m_readiness_errors.cend(),
            [&represented_errors](const QString& error) {
                return !represented_errors.contains(error) &&
                       !isProviderSetupReadinessError(error) &&
                       !isLiquidityReadinessError(error);
            });

        setStatusLabel(m_external_blockchain_status,
                       node_wait
                           ? Params().GetChainType() == ChainType::REGTEST
                                 ? tr("Waiting — mine a fresh Regtest block so the node leaves initial synchronization")
                                 : tr("Waiting — blockchain synchronization is not complete")
                                 : tr("Ready"),
                       node_wait ? QStringLiteral("waiting")
                                 : QStringLiteral("ready"));
        setStatusLabel(m_external_txindex_status,
                       txindex_missing
                           ? tr("Action required — transaction index is disabled")
                           : txindex_wait
                                 ? tr("Waiting — transaction index is synchronizing")
                                 : tr("Ready"),
                       txindex_missing ? QStringLiteral("action")
                                       : txindex_wait ? QStringLiteral("waiting")
                                                      : QStringLiteral("ready"));
        setStatusLabel(m_external_broadcast_status,
                       node_wait
                           ? Params().GetChainType() == ChainType::REGTEST
                                 ? tr("Waiting — Regtest has no sufficiently recent confirmed tip")
                                 : tr("Waiting — node is not ready to broadcast transactions")
                                 : tr("Ready"),
                       node_wait ? QStringLiteral("waiting")
                                 : QStringLiteral("ready"));
        setStatusLabel(m_external_activation_status,
                       activation_wait ? tr("Waiting — DigiDollar is not active yet")
                                       : tr("Ready"),
                       activation_wait ? QStringLiteral("waiting")
                                       : QStringLiteral("ready"));

        QString oracle_text;
        QString oracle_kind{QStringLiteral("waiting")};
        if (m_oracle_state == OracleState::AVAILABLE) {
            oracle_text = tr("Available — %1 $USD")
                              .arg(QString::number(
                                  m_oracle_price_micro_usd / 1000000.0,
                                  'f', 6));
            oracle_kind = QStringLiteral("ready");
        } else if (m_oracle_state == OracleState::CHECKING) {
            oracle_text = tr("Checking…");
        } else if (m_oracle_state == OracleState::UNAVAILABLE) {
            oracle_text = tr("Not available yet");
        } else {
            oracle_text = tr("Not checked yet");
        }
        setStatusLabel(m_external_oracle_status, oracle_text, oracle_kind);
        if (m_external_oracle_note) {
            m_external_oracle_note->setVisible(
                m_oracle_state != OracleState::AVAILABLE);
        }

        if (m_external_other_heading && m_external_other_status) {
            m_external_other_heading->setVisible(has_other_requirement);
            m_external_other_status->setVisible(has_other_requirement);
            if (has_other_requirement) {
                setStatusLabel(
                    m_external_other_status,
                    has_unclassified_failure
                        ? tr("Provider cannot start — review the next safe action and technical details")
                        : tr("Action required — complete the provider setup item shown above"),
                    has_unclassified_failure ? QStringLiteral("error")
                                             : QStringLiteral("action"));
            }
        }

        QString card_kind{QStringLiteral("ready")};
        if (has_unclassified_failure) {
            card_kind = QStringLiteral("error");
        } else if (has_other_requirement || txindex_missing) {
            card_kind = QStringLiteral("action");
        } else if (node_wait || txindex_wait || activation_wait) {
            card_kind = QStringLiteral("waiting");
        }
        setStatusKind(m_external_prerequisites_card, card_kind);
    }

    void refreshOracleStatus()
    {
        if (!m_model) return;
        // Keep an already displayed Oracle result stable while refreshing it.
        // Switching AVAILABLE -> CHECKING -> AVAILABLE every ten seconds made
        // the status card resize and visibly flash even when nothing changed.
        if (m_oracle_state == OracleState::UNKNOWN) {
            m_oracle_state = OracleState::CHECKING;
            updateExternalPrerequisites();
        }
        QPointer<DigiDollarPaymasterWidget> guard{this};
        WalletModel* request_model = m_model;
        m_model->executeRpcAsync(
            "getoracleprice", UniValue{UniValue::VARR},
            [guard, request_model](UniValue result, QString error) {
                if (!guard || guard->m_model != request_model) return;
                const UniValue& price = result.find_value("price_micro_usd");
                guard->m_oracle_price_micro_usd =
                    error.isEmpty() && price.isNum()
                        ? price.getInt<qint64>() : 0;
                guard->m_oracle_state = guard->m_oracle_price_micro_usd > 0
                    ? OracleState::AVAILABLE : OracleState::UNAVAILABLE;
                guard->updateExternalPrerequisites();
            });
    }

    static QString dgbAmount(qint64 satoshis)
    {
        return QString::number(satoshis / 100000000.0, 'f', 8);
    }

    static QString ddAmount(qint64 cents)
    {
        return QString::number(cents / 100.0, 'f', 2);
    }

    static qint64 financeNumber(const UniValue& object, const char* name)
    {
        const UniValue& value = object.find_value(name);
        return value.isNum() ? value.getInt<qint64>() : 0;
    }

    static QString financeEventKind(const QString& kind)
    {
        if (kind == QLatin1String("transfer")) return tr("Paymaster transfer");
        if (kind == QLatin1String("setup")) return tr("Pool setup");
        if (kind == QLatin1String("replenishment")) return tr("Liquidity replenishment");
        if (kind == QLatin1String("retirement")) return tr("Liquidity retirement");
        if (kind == QLatin1String("withdrawal")) return tr("Carrier withdrawal");
        return tr("Unknown booking");
    }

    static QString financeEventState(const QString& state)
    {
        if (state == QLatin1String("confirmed")) return tr("Confirmed");
        if (state == QLatin1String("pending")) return tr("Pending");
        if (state == QLatin1String("invalidated")) return tr("Reversed");
        return tr("Unknown");
    }

    static QString financeFundingModel(const UniValue& event)
    {
        const UniValue& model = event.find_value("funding_model");
        if (!model.isStr()) return tr("Maintenance");
        const QString funding = QString::fromStdString(model.get_str());
        if (funding == QLatin1String("user_paid")) return tr("User paid");
        const UniValue& scope = event.find_value("sponsorship_scope");
        return scope.isStr() && scope.get_str() == "restricted"
            ? tr("Restricted sponsored") : tr("Public sponsored");
    }

    void updateBackupReminder(bool required, const QString& provider_id)
    {
        m_backup_required = required;
        m_provider_id = provider_id;
        const bool visible = required && !provider_id.isEmpty();
        const QString explanation = tr(
            "Back up this complete provider wallet now. The provider identity key, "
            "configuration, pool state and finance ledger are stored in the wallet. "
            "A seed-only or descriptor-only export is not a complete Paymaster backup. "
            "Keep the wallet backup offline and confidential. This reminder does not "
            "prevent you from starting the provider.");
        const QString identity = tr("Provider ID: %1").arg(provider_id);
        if (m_overview_backup_notice) {
            m_overview_backup_notice->setVisible(visible);
            m_overview_backup_provider_id->setText(identity);
        }
        if (m_finance_backup_notice) {
            m_finance_backup_notice->setVisible(visible);
            m_finance_backup_text->setText(explanation);
            m_finance_backup_provider_id->setText(identity);
        }
        // The setup wizard is a child of this widget while it is open. Hide
        // its completion reminder as soon as the ordinary WalletView backup
        // workflow has successfully updated the wallet-scoped status.
        if (auto* setup_notice =
                findChild<QGroupBox*>(
                    QStringLiteral("paymasterSetupBackupReminder"))) {
            setup_notice->setVisible(visible);
        }
    }

    void requestProviderBackup()
    {
        if (!m_provider_backup_request_handler) {
            QMessageBox::warning(
                this, tr("Provider-wallet backup"),
                tr("The wallet backup action is not available in this window."));
            return;
        }
        m_provider_backup_request_handler();
        // WalletView records a successful full-wallet backup in the same
        // wallet database. Refresh only after its modal file workflow returns.
        QTimer::singleShot(0, this, [this] {
            if (hasRpcTransport() && !m_busy) refreshStatus();
        });
    }

    void acknowledgeExternalBackup()
    {
        if (!hasRpcTransport() || m_busy) return;
        const QString prompt = tr(
            "Only acknowledge this if another process has created a current, "
            "complete backup of this wallet file. Seed phrases, descriptor exports, "
            "CSV files and copied addresses do not preserve the complete Paymaster "
            "identity and accounting state.\n\nAcknowledge an external full-wallet backup?");
        if (QMessageBox::warning(
                this, tr("Acknowledge external wallet backup"), prompt,
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        UniValue options{UniValue::VOBJ};
        options.pushKV("external_backup", true);
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        call("acknowledgepaymasterproviderbackup", std::move(params), false,
             nullptr, [this](const UniValue&) {
                 updateBackupReminder(false, m_provider_id);
                 QMessageBox::information(
                     this, tr("External backup acknowledged"),
                     tr("The backup reminder has been cleared for the current provider configuration."));
                 refreshStatus();
             });
    }

    void refreshFinanceStatus()
    {
        if (!hasRpcTransport() || m_busy || !m_finance_period_select) return;
        UniValue options{UniValue::VOBJ};
        options.pushKV(
            "period",
            m_finance_period_select->currentData().toString().toStdString());
        options.pushKV("include_events", true);
        options.pushKV("limit", 10000);
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        m_finance_refresh->setText(tr("Loading…"));
        call("getpaymasterfinancestatus", std::move(params), false, nullptr,
             [this](const UniValue& result) {
                 m_finance_refresh->setText(tr("Refresh"));
                 applyFinanceStatus(result);
             }, true, [this](const QString& error) {
                 m_finance_refresh->setText(tr("Refresh"));
                 m_finance_history_status->setText(
                     tr("Finance data could not be loaded: %1").arg(error));
             });
    }

    void applyFinanceStatus(const UniValue& result)
    {
        if (!result.isObject()) return;
        m_finance_last_result = result;
        const UniValue& summaries = result.find_value("period_summaries");
        const std::array<const char*, 4> keys{{"today", "7d", "30d", "all"}};
        for (size_t index = 0; index < keys.size(); ++index) {
            const UniValue& summary = summaries.find_value(keys.at(index));
            const qint64 income = financeNumber(summary, "service_fee_income_cents");
            const qint64 cost = financeNumber(summary, "dgb_operating_cost_satoshis");
            const qint64 transfers = financeNumber(summary, "successful_transfers");
            m_finance_period_income.at(index)->setText(
                tr("Service fees: %1 DD").arg(ddAmount(income)));
            m_finance_period_cost.at(index)->setText(
                tr("Operating costs: %1 DGB").arg(dgbAmount(cost)));
            m_finance_period_transfers.at(index)->setText(
                tr("Successful transfers: %1").arg(transfers));
        }

        const qint64 income = financeNumber(result, "service_fee_income_cents");
        const qint64 cost = financeNumber(result, "dgb_operating_cost_satoshis");
        const qint64 transfers = financeNumber(result, "successful_transfers");
        const qint64 average = financeNumber(result, "average_service_fee_cents");
        const UniValue& estimate = result.find_value("estimated_result_usd");
        const UniValue& oracle = result.find_value("oracle_price_micro_usd");
        const UniValue& valuation_time = result.find_value("valuation_time");
        if (estimate.isNum() && oracle.isNum()) {
            const QString time = valuation_time.isNum()
                ? QDateTime::fromSecsSinceEpoch(
                      valuation_time.getInt<qint64>(), Qt::UTC)
                      .toString(Qt::ISODate)
                : tr("now");
            m_finance_result_estimate->setText(tr(
                "Selected period: %1 DD income minus %2 DGB cost ≈ %3 USD "
                "at %4 USD/DGB (valuation %5).")
                .arg(ddAmount(income), dgbAmount(cost))
                .arg(estimate.get_real(), 0, 'f', 2)
                .arg(oracle.getInt<qint64>() / 1000000.0, 0, 'f', 6)
                .arg(time));
        } else {
            m_finance_result_estimate->setText(tr(
                "Selected period: %1 DD income and %2 DGB cost. No current "
                "Oracle price is available, so no USD estimate is shown.")
                .arg(ddAmount(income), dgbAmount(cost)));
        }
        const UniValue& model_breakdown = result.find_value(
            "model_breakdown");
        const auto model_line = [&](const QString& title,
                                    const char* key) {
            const UniValue& model = model_breakdown.find_value(key);
            return tr("%1: %2 transfer(s) · %3 DD income · %4 DGB cost")
                .arg(title)
                .arg(financeNumber(model, "successful_transfers"))
                .arg(ddAmount(financeNumber(
                    model, "service_fee_income_cents")))
                .arg(dgbAmount(financeNumber(
                    model, "dgb_operating_cost_satoshis")));
        };
        m_finance_model_breakdown->setText(
            tr("%1 successful transfer(s) · average service-fee income %2 DD\n%3\n%4\n%5")
                .arg(transfers)
                .arg(ddAmount(average))
                .arg(model_line(tr("User paid"), "user_paid"))
                .arg(model_line(tr("Public sponsored"),
                                "public_sponsored"))
                .arg(model_line(tr("Restricted sponsored"),
                                "restricted_sponsored")));

        const UniValue& pool = result.find_value("pool_capital");
        m_finance_pool_dgb->setText(tr(
            "DGB capacity: %1 available · %2 reserved or committed · %3 pending confirmation")
            .arg(dgbAmount(financeNumber(pool, "dgb_available_satoshis")),
                 dgbAmount(financeNumber(pool, "dgb_reserved_satoshis")),
                 dgbAmount(financeNumber(pool, "dgb_pending_satoshis"))));
        m_finance_pool_carrier->setText(tr(
            "DD carriers: %1 DD base capital · %2 DD earned above the base · %3 DD currently withdrawable")
            .arg(ddAmount(financeNumber(pool, "carrier_base_cents")),
                 ddAmount(financeNumber(pool, "carrier_earned_cents")),
                 ddAmount(financeNumber(pool, "carrier_withdrawable_cents"))));
        const qint64 pending = financeNumber(
            pool, "pending_maintenance_transactions");
        m_finance_pending_maintenance->setText(
            pending == 0
                ? tr("No pool-maintenance or withdrawal transaction is pending.")
                : tr("%1 pool-maintenance or withdrawal transaction(s) are pending.")
                      .arg(pending));

        const bool partial = result.find_value(
            "history_partially_reconstructable").isBool() &&
            result.find_value("history_partially_reconstructable").get_bool();
        const qint64 complete_from = financeNumber(
            result, "history_complete_from");
        const QString complete_date = complete_from > 0
            ? QDateTime::fromSecsSinceEpoch(complete_from, Qt::UTC)
                  .toString(Qt::ISODate)
            : tr("unknown");
        m_finance_history_notice->setVisible(partial);
        if (partial) {
            m_finance_history_notice_text->setText(tr(
                "The totals below are complete only from %1 UTC. Earlier Paymaster transfers may be absent because their exact provider fee and transaction role cannot be proven from the remaining wallet records. New transfers are recorded automatically; no earlier income is estimated.")
                                                       .arg(complete_date));
        }
        m_finance_history_status->setText(
            partial
                ? tr("History is complete since %1 UTC. Earlier unprovable transfers are excluded from every total and export.")
                      .arg(complete_date)
                : tr("History is complete since %1 UTC.").arg(complete_date));

        const UniValue& daily_totals = result.find_value("daily_totals");
        m_finance_daily_totals->setRowCount(
            daily_totals.isArray()
                ? static_cast<int>(daily_totals.size()) : 0);
        if (daily_totals.isArray()) {
            int row{0};
            for (const UniValue& total : daily_totals.getValues()) {
                const qint64 day_start = financeNumber(total, "day_start");
                const std::array<QString, 5> cells{{
                    QDateTime::fromSecsSinceEpoch(day_start, Qt::UTC)
                        .date().toString(Qt::ISODate),
                    ddAmount(financeNumber(
                        total, "service_fee_income_cents")),
                    dgbAmount(financeNumber(
                        total, "dgb_operating_cost_satoshis")),
                    QString::number(financeNumber(
                        total, "successful_transfers")),
                    QString::number(financeNumber(
                        total, "maintenance_transactions")),
                }};
                for (int column = 0;
                     column < static_cast<int>(cells.size()); ++column) {
                    m_finance_daily_totals->setItem(
                        row, column,
                        new QTableWidgetItem(cells.at(column)));
                }
                ++row;
            }
        }

        const UniValue& events = result.find_value("events");
        m_finance_events->setRowCount(events.isArray()
            ? static_cast<int>(events.size()) : 0);
        if (events.isArray()) {
            int row{0};
            for (const UniValue& event : events.getValues()) {
                const qint64 created_at = financeNumber(event, "created_at");
                const QString kind = event.find_value("kind").isStr()
                    ? QString::fromStdString(event.find_value("kind").get_str())
                    : QString{};
                const QString state = event.find_value("state").isStr()
                    ? QString::fromStdString(event.find_value("state").get_str())
                    : QString{};
                const std::array<QString, 6> cells{{
                    QDateTime::fromSecsSinceEpoch(created_at, Qt::UTC)
                        .toString(Qt::ISODate),
                    financeEventKind(kind), financeEventState(state),
                    ddAmount(financeNumber(event, "dd_income_cents")),
                    dgbAmount(financeNumber(event, "dgb_cost_satoshis")),
                    financeFundingModel(event),
                }};
                for (int column = 0; column < static_cast<int>(cells.size()); ++column) {
                    m_finance_events->setItem(
                        row, column, new QTableWidgetItem(cells.at(column)));
                }
                ++row;
            }
        }
        m_finance_export->setEnabled(true);
        const UniValue& provider = result.find_value("provider_id");
        const QString provider_id = provider.isStr()
            ? QString::fromStdString(provider.get_str()) : m_provider_id;
        const bool backup_required = result.find_value("backup_required").isBool() &&
                                     result.find_value("backup_required").get_bool();
        updateBackupReminder(backup_required, provider_id);
    }

    static QString csvCell(QString value)
    {
        value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"") + value + QStringLiteral("\"");
    }

    void exportFinanceCsv()
    {
        if (!m_finance_last_result.isObject() ||
            !m_finance_last_result.find_value("events").isArray()) {
            QMessageBox::information(
                this, tr("Export provider accounting"),
                tr("Refresh the selected reporting period before exporting."));
            return;
        }
        if (QMessageBox::information(
                this, tr("Export provider accounting"),
                tr("This CSV contains accounting figures for the selected period. "
                   "It is not a wallet backup and cannot restore the provider identity, "
                   "keys, pool state or ledger."),
                QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Ok) != QMessageBox::Ok) {
            return;
        }
        const QString filename = GUIUtil::getSaveFileName(
            this, tr("Export Paymaster accounting"), QString(),
            tr("Comma-separated values") + QLatin1String(" (*.csv)"), nullptr);
        if (filename.isEmpty()) return;
        QSaveFile file(filename);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(
                this, tr("Export failed"),
                tr("Could not open %1 for writing.").arg(filename));
            return;
        }
        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        const UniValue& oracle = m_finance_last_result.find_value(
            "oracle_price_micro_usd");
        const bool include_valuation = oracle.isNum();
        stream << "utc_time,booking,status,dd_income,dgb_cost,payment_model";
        if (include_valuation) stream << ",current_value_usd";
        stream << "\n";
        for (const UniValue& event :
             m_finance_last_result.find_value("events").getValues()) {
            const qint64 created_at = financeNumber(event, "created_at");
            const qint64 dd_income = financeNumber(event, "dd_income_cents");
            const qint64 dgb_cost = financeNumber(event, "dgb_cost_satoshis");
            const QString kind = event.find_value("kind").isStr()
                ? QString::fromStdString(event.find_value("kind").get_str())
                : QString{};
            const QString state = event.find_value("state").isStr()
                ? QString::fromStdString(event.find_value("state").get_str())
                : QString{};
            stream << csvCell(QDateTime::fromSecsSinceEpoch(
                                  created_at, Qt::UTC).toString(Qt::ISODate))
                   << ',' << csvCell(financeEventKind(kind))
                   << ',' << csvCell(financeEventState(state))
                   << ',' << ddAmount(dd_income)
                   << ',' << dgbAmount(dgb_cost)
                   << ',' << csvCell(financeFundingModel(event));
            if (include_valuation) {
                const long double current_value =
                    static_cast<long double>(dd_income) / 100.0L -
                    static_cast<long double>(dgb_cost) *
                        oracle.getInt<qint64>() / 100000000.0L / 1000000.0L;
                stream << ',' << QString::number(
                    static_cast<double>(current_value), 'f', 8);
            }
            stream << '\n';
        }
        if (!file.commit()) {
            QMessageBox::warning(
                this, tr("Export failed"),
                tr("Could not finish writing %1.").arg(filename));
            return;
        }
        QMessageBox::information(
            this, tr("Export complete"),
            tr("The selected accounting period was exported successfully."));
    }

    void openLiquidityFinanceControl(QWidget* control)
    {
        m_tabs->setCurrentWidget(m_liquidity_page);
        if (m_liquidity_advanced_toggle &&
            !m_liquidity_advanced_toggle->isChecked()) {
            m_liquidity_advanced_toggle->setChecked(true);
        }
        if (control) {
            control->setFocus(Qt::OtherFocusReason);
            if (auto* scroll = qobject_cast<QScrollArea*>(m_liquidity_page)) {
                scroll->ensureWidgetVisible(control, 24, 24);
            }
        }
    }

    void reviewMaintenanceFeeLimit()
    {
        m_tabs->setCurrentWidget(m_liquidity_page);
        if (m_liquidity_advanced_toggle) {
            m_liquidity_advanced_toggle->setChecked(true);
        }
        if (m_maintenance_limits_toggle) {
            m_maintenance_limits_toggle->setChecked(true);
        }
        if (m_maintenance_fee_per_transaction) {
            m_maintenance_fee_per_transaction->setFocus(
                Qt::OtherFocusReason);
        }
        m_liquidity_policy_status->setText(tr(
            "Automatic refill is paused. Its estimated network fee is above the saved per-transaction maintenance limit, so no transaction has been created. Stop the provider before changing these protected runtime settings, then review all three finite limits and increase them only if the cost is acceptable."));
    }

    bool readLiquidityPolicy(LiquidityPolicyValues& values) const
    {
        bool ok_transaction{false};
        bool ok_hour{false};
        bool ok_day{false};
        values.automatic_replenishment =
            m_automatic_replenishment->isChecked();
        values.paid_maintenance_approved =
            m_paid_maintenance_approved->isChecked();
        values.admission_dgb = m_admission_dgb->value();
        values.operational_dgb = m_operational_dgb->value();
        values.admission_carriers = m_admission_carriers->value();
        values.operational_carriers = m_operational_carriers->value();
        values.fee_per_transaction =
            m_maintenance_fee_per_transaction->text().toLongLong(
                &ok_transaction);
        values.fee_per_hour =
            m_maintenance_fee_per_hour->text().toLongLong(&ok_hour);
        values.fee_per_day =
            m_maintenance_fee_per_day->text().toLongLong(&ok_day);
        return ok_transaction && ok_hour && ok_day;
    }

    static UniValue liquidityPolicyToJSON(
        const LiquidityPolicyValues& values)
    {
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("automatic_replenishment",
                      values.automatic_replenishment);
        policy.pushKV("paid_maintenance_approved",
                      values.paid_maintenance_approved);
        policy.pushKV("target_admission_dgb", values.admission_dgb);
        policy.pushKV("target_operational_dgb", values.operational_dgb);
        policy.pushKV("target_admission_carriers",
                      values.admission_carriers);
        policy.pushKV("target_operational_carriers",
                      values.operational_carriers);
        policy.pushKV(
            "maximum_maintenance_fee_per_transaction_satoshis",
            values.fee_per_transaction);
        policy.pushKV("maximum_maintenance_fee_per_hour_satoshis",
                      values.fee_per_hour);
        policy.pushKV("maximum_maintenance_fee_per_day_satoshis",
                      values.fee_per_day);
        return policy;
    }

    void loadLiquidityPolicy(const UniValue& policy, bool configured)
    {
        if (!policy.isObject()) return;
        m_loading_liquidity_policy = true;
        const UniValue& automatic =
            policy.find_value("automatic_replenishment");
        const UniValue& approved =
            policy.find_value("paid_maintenance_approved");
        if (automatic.isBool()) {
            m_automatic_replenishment->setChecked(automatic.get_bool());
        }
        if (approved.isBool()) {
            m_paid_maintenance_approved->setChecked(approved.get_bool());
        }
        const auto load_spin = [&policy](QSpinBox* control,
                                         const char* name) {
            const UniValue& value = policy.find_value(name);
            if (value.isNum()) control->setValue(value.getInt<int>());
        };
        load_spin(m_admission_dgb, "target_admission_dgb");
        load_spin(m_operational_dgb, "target_operational_dgb");
        load_spin(m_admission_carriers, "target_admission_carriers");
        load_spin(m_operational_carriers, "target_operational_carriers");
        const auto load_amount = [&policy](QLineEdit* control,
                                           const char* name) {
            const UniValue& value = policy.find_value(name);
            if (value.isNum()) {
                control->setText(QString::number(value.getInt<qint64>()));
            }
        };
        load_amount(m_maintenance_fee_per_transaction,
                    "maximum_maintenance_fee_per_transaction_satoshis");
        load_amount(m_maintenance_fee_per_hour,
                    "maximum_maintenance_fee_per_hour_satoshis");
        load_amount(m_maintenance_fee_per_day,
                    "maximum_maintenance_fee_per_day_satoshis");
        m_loading_liquidity_policy = false;
        m_liquidity_policy_configured = configured;
        m_liquidity_policy_dirty = false;
        updateLiquidityDisplay();
        m_liquidity_policy_status->setText(configured
            ? approved.isBool() && approved.get_bool()
                ? tr("Automatic liquidity policy saved. Paid replenishment is permitted only within the displayed finite limits.")
                : tr("Liquidity targets are saved, but paid replenishment remains disabled. Free successor recycling still works.")
            : tr("Suggested migration values are shown. Review them and explicitly approve finite paid maintenance before Core can create a replenishment transaction."));
    }

    void markLiquidityPolicyDirty()
    {
        if (m_loading_liquidity_policy) return;
        m_liquidity_policy_dirty = true;
        invalidateCarrierWithdrawalPreviews();
        if (m_liquidity_policy_status) {
            m_liquidity_policy_status->setText(tr(
                "Unsaved liquidity-policy changes. The previously saved targets and limits remain active until these values are saved."));
        }
        updateLiquidityDisplay();
        updateProviderButtons();
    }

    QString liquidityApprovalText(
        const LiquidityPolicyValues& values) const
    {
        const qint64 admission_value =
            DigiDollar::Paymaster::MIN_ADMISSION_DGB_SATOSHIS;
        const qint64 operational_value = std::max<qint64>(
            admission_value, m_network_fee->value());
        const qint64 bound_dgb =
            admission_value * values.admission_dgb +
            operational_value * values.operational_dgb;
        const qint64 bound_carriers =
            100LL * (values.admission_carriers +
                     values.operational_carriers);
        return tr(
            "Approve automatic paid liquidity maintenance for this wallet?\n\n"
            "Targets: %1 admission DGB, %2 operational DGB, %3 admission carriers and %4 operational carriers.\n"
            "Approximate target capital at the current provider ceiling: %5 DGB and %6 DD carrier base.\n\n"
            "Maintenance network-fee limits:\n"
            "• %7 DGB per transaction\n"
            "• %8 DGB per rolling hour\n"
            "• %9 DGB per rolling day\n\n"
            "Core can spend maintenance fees only while automatic provider operation or autostart is active. Zero never means unlimited.")
            .arg(values.admission_dgb)
            .arg(values.operational_dgb)
            .arg(values.admission_carriers)
            .arg(values.operational_carriers)
            .arg(dgbAmount(bound_dgb))
            .arg(QString::number(bound_carriers / 100.0, 'f', 2))
            .arg(dgbAmount(values.fee_per_transaction))
            .arg(dgbAmount(values.fee_per_hour))
            .arg(dgbAmount(values.fee_per_day));
    }

    bool validateLiquidityPolicy(const LiquidityPolicyValues& values,
                                 QString& error) const
    {
        if (values.admission_dgb < 3 || values.operational_dgb < 1 ||
            values.admission_dgb > 16 || values.operational_dgb > 16 ||
            values.admission_dgb < values.operational_dgb) {
            error = tr("Choose at least three admission DGB slots and one operational DGB slot. Admission slots cannot be lower than operational slots.");
            return false;
        }
        if ((values.admission_carriers != 0 &&
             values.admission_carriers < 3) ||
            values.admission_carriers < values.operational_carriers ||
            values.admission_carriers > 16 ||
            values.operational_carriers > 16) {
            error = tr("Carrier targets must either both be zero, or use at least three admission carriers and no more operational carriers than admission carriers.");
            return false;
        }
        if (values.fee_per_transaction < 0 ||
            values.fee_per_hour < 0 || values.fee_per_day < 0) {
            error = tr("Maintenance-fee limits cannot be negative.");
            return false;
        }
        if (values.paid_maintenance_approved &&
            (values.fee_per_transaction <= 0 ||
             values.fee_per_hour < values.fee_per_transaction ||
             values.fee_per_day < values.fee_per_hour)) {
            error = tr("Paid maintenance requires finite positive limits ordered as: transaction limit no greater than hourly limit, and hourly limit no greater than daily limit.");
            return false;
        }
        return true;
    }

    void saveLiquidityPolicy()
    {
        if (!hasRpcTransport() || m_busy) return;
        LiquidityPolicyValues values;
        QString validation_error;
        if (!readLiquidityPolicy(values)) {
            QMessageBox::warning(
                this, tr("Automatic liquidity policy"),
                tr("One or more maintenance-fee amounts are outside the supported whole-satoshi range."));
            return;
        }
        if (!validateLiquidityPolicy(values, validation_error)) {
            QMessageBox::warning(this, tr("Automatic liquidity policy"),
                                 validation_error);
            return;
        }
        if (values.paid_maintenance_approved &&
            QMessageBox::question(
                this, tr("Approve finite paid maintenance"),
                liquidityApprovalText(values),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        UniValue params{UniValue::VARR};
        params.push_back(liquidityPolicyToJSON(values));
        call("setpaymasterliquiditypolicy", std::move(params), false,
             nullptr, [this](const UniValue& result) {
                 loadLiquidityPolicy(result, /*configured=*/true);
                 m_liquidity_policy_status->setText(tr(
                     "Automatic liquidity policy saved successfully. The provider still cannot spend beyond its separate provider safety limits."));
                 invalidateCarrierWithdrawalPreviews();
                 refreshStatus();
             });
    }

    void approveSuggestedLiquidityMaintenance()
    {
        if (!hasRpcTransport() || m_busy) return;
        m_tabs->setCurrentWidget(m_liquidity_page);
        if (!m_liquidity_targets_satisfy_provider_policy) {
            // Restore only the visible minimums here. The operator must still
            // review and save the policy explicitly; this button never creates
            // a pool transaction or silently reverses an intentional slot
            // release.
            if (m_liquidity_advanced_toggle) {
                m_liquidity_advanced_toggle->setChecked(true);
            }
            m_admission_carriers->setValue(
                std::max(3, m_admission_carriers->value()));
            m_operational_carriers->setValue(
                std::max(1, m_operational_carriers->value()));
            // One explicit save can now persist both the corrected targets and
            // the operator's finite paid-maintenance approval. Keep the limits
            // expanded so the financial authority being granted is visible
            // before the confirmation dialog appears.
            m_paid_maintenance_approved->setChecked(true);
            if (m_maintenance_limits_toggle) {
                m_maintenance_limits_toggle->setChecked(true);
            }
            m_liquidity_policy_status->setText(tr(
                "The required carrier targets and finite refill limits are now shown. Review them and choose Save targets and approve refill. No funds have moved."));
            return;
        }
        m_paid_maintenance_approved->setChecked(true);
        saveLiquidityPolicy();
    }

    static QString slotStatusText(const QString& name,
                                  const UniValue& status)
    {
        if (!status.isObject()) return {};
        return QStringLiteral("%1: %2 ready, %3 pending, %4 missing (target %5)")
            .arg(name)
            .arg(poolNumber(status, "ready"))
            .arg(poolNumber(status, "pending"))
            .arg(poolNumber(status, "missing"))
            .arg(poolNumber(status, "target"));
    }

    void applyLiquidityStatus(const UniValue& result)
    {
        const UniValue& policy = result.find_value("policy");
        const bool configured =
            result.find_value("policy_configured").isBool() &&
            result.find_value("policy_configured").get_bool();
        if (policy.isObject() && !m_liquidity_policy_dirty) {
            loadLiquidityPolicy(policy, configured);
        } else {
            m_liquidity_policy_configured = configured;
        }
        const UniValue& state = result.find_value("maintenance_state");
        m_maintenance_state = state.isStr()
            ? QString::fromStdString(state.get_str())
            : QStringLiteral("unavailable");
        const UniValue& targets_satisfy =
            result.find_value("targets_satisfy_provider_policy");
        // Treat the field as true when talking to an older test fixture, but
        // use the explicit Core result whenever it is present. Core owns this
        // invariant because a locally valid zero carrier target can still be
        // insufficient for an active USER_PAID offer.
        m_liquidity_targets_satisfy_provider_policy =
            !targets_satisfy.isBool() || targets_satisfy.get_bool();

        const QStringList slot_lines{
            slotStatusText(tr("Admission DGB"),
                           result.find_value("admission_dgb")),
            slotStatusText(tr("Operational DGB"),
                           result.find_value("operational_dgb")),
            slotStatusText(tr("Admission carriers"),
                           result.find_value("admission_carriers")),
            slotStatusText(tr("Operational carriers"),
                           result.find_value("operational_carriers")),
        };
        m_liquidity_slot_status->setText(slot_lines.join('\n'));
        // The liquidity RPC is the authoritative source for the current
        // ready/pending/missing breakdown. Keep the Overview counters in the
        // same snapshot so an out-of-order getpaymasterinfo reply cannot make
        // one card report a missing slot while another treats the target as
        // satisfied. Pending outputs intentionally do not count as ready.
        const UniValue& admission_dgb = result.find_value("admission_dgb");
        const UniValue& operational_dgb = result.find_value("operational_dgb");
        const UniValue& admission_carriers =
            result.find_value("admission_carriers");
        const UniValue& operational_carriers =
            result.find_value("operational_carriers");
        if (admission_dgb.isObject() && operational_dgb.isObject() &&
            admission_carriers.isObject() && operational_carriers.isObject()) {
            m_pool_status_loaded = true;
            m_pool_admission_dgb = poolNumber(admission_dgb, "ready");
            m_pool_operational_dgb = poolNumber(operational_dgb, "ready");
            m_pool_admission_carriers =
                poolNumber(admission_carriers, "ready");
            m_pool_operational_carriers =
                poolNumber(operational_carriers, "ready");
        }
        const auto total_field = [&result](const char* object_name,
                                           const char* field_name) {
            return poolNumber(result.find_value(object_name), field_name);
        };
        const qint64 missing =
            total_field("admission_dgb", "missing") +
            total_field("operational_dgb", "missing") +
            total_field("admission_carriers", "missing") +
            total_field("operational_carriers", "missing");
        const qint64 pending =
            total_field("admission_dgb", "pending") +
            total_field("operational_dgb", "pending") +
            total_field("admission_carriers", "pending") +
            total_field("operational_carriers", "pending");
        const bool approved = policy.isObject() &&
            policy.find_value("paid_maintenance_approved").isBool() &&
            policy.find_value("paid_maintenance_approved").get_bool();
        // The wallet backend distinguishes an unconfirmed maintenance-fee
        // exposure from fees that are already confirmed and charged to the
        // rolling budgets. Keep both visible in the ordinary operator view;
        // otherwise a self-transfer that rebuilds a 1 DD carrier misleadingly
        // looks free because its two DD transaction rows net to zero.
        const qint64 reserved = poolNumber(
            result, "maintenance_fee_reserved_satoshis");
        const qint64 spent_hour = poolNumber(
            result, "maintenance_fee_spent_last_hour_satoshis");
        const qint64 spent_day = poolNumber(
            result, "maintenance_fee_spent_last_day_satoshis");

        QString state_text;
        QString next_step;
        QString status_kind;
        if (m_maintenance_state ==
            QLatin1String("waiting_for_target_configuration")) {
            status_kind = QStringLiteral("action");
            state_text = tr("Saved liquidity targets cannot make this offer ready");
            next_step = tr(
                "User-paid service needs at least three admission DigiDollar carriers and one operational DigiDollar carrier. Review the displayed targets, save them, and then prepare only the missing output.");
        } else if (m_maintenance_state ==
            QLatin1String("waiting_for_maintenance_approval")) {
            status_kind = QStringLiteral("action");
            state_text = tr("Liquidity targets are saved — refill spending is not yet approved");
            next_step = tr(
                "No target update is needed. To create the missing pool output, review the finite transaction, hourly and daily maintenance limits and explicitly approve that bounded refill cost.");
        } else if (m_maintenance_state ==
                   QLatin1String("replenishing_liquidity")) {
            if (maintenanceFeeLimitExceeded()) {
                status_kind = QStringLiteral("action");
                state_text = tr(
                    "Automatic refill paused — approved fee limit is too low");
                const qint64 fee_limit = policy.isObject()
                    ? poolNumber(policy,
                                 "maximum_maintenance_fee_per_transaction_satoshis")
                    : 0;
                next_step = tr(
                    "No transaction was created. The estimated network fee is above the saved per-transaction maintenance limit of %1 DGB. Review the limit and increase it only if that bounded cost is acceptable.")
                                .arg(dgbAmount(fee_limit));
            } else if (pending > 0) {
                status_kind = QStringLiteral("waiting");
                state_text = tr("Replacement liquidity is waiting for confirmation");
                next_step = tr(
                    "%1 pending slot(s) already count toward the target. Mine or wait for a confirming block; Core will not create a duplicate refill.")
                    .arg(pending);
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_NODE_NOT_READY"))) {
                status_kind = QStringLiteral("waiting");
                state_text = Params().GetChainType() == ChainType::REGTEST
                    ? tr("Refill approved — waiting for a fresh Regtest block")
                    : tr("Refill approved — waiting for node readiness");
                next_step = Params().GetChainType() == ChainType::REGTEST
                    ? tr("No refill transaction has been created yet. Mine a fresh Regtest block so Core can leave initial synchronization and safely create the missing pool output.")
                    : tr("No refill transaction has been created yet. Core will create it automatically after synchronization and transaction-broadcast readiness are restored.");
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_WALLET_LOCKED"))) {
                status_kind = QStringLiteral("waiting");
                state_text = tr("Refill approved — waiting for wallet unlock");
                next_step = tr(
                    "No refill transaction has been created yet. Unlock the provider wallet; automatic maintenance will then resume without another approval.");
            } else if (!m_core_running) {
                status_kind = QStringLiteral("waiting");
                state_text = tr("Refill approved — waiting for provider start");
                next_step = tr(
                    "No refill transaction has been created yet. Start the provider with “Start and restore liquidity”; Core will then create the bounded refill automatically.");
            } else {
                status_kind = QStringLiteral("waiting");
                state_text = tr("Refill approved — preparing missing liquidity");
                next_step = tr(
                    "Core may now create at most one bounded maintenance transaction. Keep the provider service running and the wallet unlocked; the display changes to pending confirmation as soon as that transaction exists.");
            }
        } else if (m_maintenance_state ==
                   QLatin1String("waiting_for_liquidity_confirmation")) {
            status_kind = QStringLiteral("waiting");
            state_text = tr("Replacement liquidity is waiting for confirmation");
            next_step = tr(
                "%1 pending slot(s) count toward the target, so Core will not create duplicates. Provider readiness resumes after confirmation.")
                .arg(pending);
        } else if (m_maintenance_state == QLatin1String("ready")) {
            status_kind = QStringLiteral("ready");
            state_text = tr("Liquidity targets are satisfied");
            next_step = tr(
                "No maintenance transaction is required. Confirmed successor outputs are reused automatically.");
        } else {
            status_kind = QStringLiteral("error");
            state_text = tr("Liquidity maintenance status unavailable");
            next_step = tr("Refresh the provider status before changing liquidity.");
        }
        setStatusLabel(m_liquidity_maintenance_state, state_text,
                       status_kind);
        // Re-polishing an unchanged card on every poll caused the complete
        // Overview layout to flash even though no user-visible state changed.
        setStatusKind(m_liquidity_maintenance_card, status_kind);
        m_liquidity_maintenance_next_step->setText(next_step);
        QString maintenance_cost;
        if (!m_liquidity_targets_satisfy_provider_policy) {
            maintenance_cost = tr(
                "Automatic restoration is paused because the saved operational carrier target is zero. No maintenance transaction has been created.");
        } else {
            maintenance_cost = tr(
                "%1 missing and %2 pending slot(s). Refill spending authorization: %3.")
                    .arg(missing)
                    .arg(pending)
                    .arg(approved ? tr("approved within finite limits")
                                  : tr("not approved"));
            if (reserved > 0) {
                maintenance_cost += tr(
                    " Pending maintenance transactions currently expose %1 DGB in network fees; confirmed costs appear in Finances.")
                                        .arg(dgbAmount(reserved));
            }
            if (spent_hour > 0 || spent_day > 0) {
                maintenance_cost += tr(
                    " Confirmed maintenance cost: %1 DGB in the rolling hour and %2 DGB in the rolling day.")
                                        .arg(dgbAmount(spent_hour),
                                             dgbAmount(spent_day));
            }
        }
        m_liquidity_maintenance_cost->setText(maintenance_cost);
        m_approve_liquidity_maintenance->setText(
            maintenanceFeeLimitExceeded()
                ? tr("Review refill cost limit…")
            : m_liquidity_targets_satisfy_provider_policy
                ? tr("Approve bounded refill costs…")
                : tr("Review required liquidity targets…"));
        m_approve_liquidity_maintenance->setVisible(
            !m_liquidity_targets_satisfy_provider_policy || !configured ||
            (missing > 0 && !approved) || maintenanceFeeLimitExceeded());
        // Keep polling for either liquidity confirmation or an external node
        // gate. A successful liquidity refresh must not accidentally stop the
        // synchronization poll used by the Overview page.
        updateAutomaticRefreshTimer();

        m_liquidity_budget_status->setText(tr(
            "Maintenance fees — reserved: %1 DGB; spent in the rolling hour: %2 DGB; spent in the rolling day: %3 DGB.")
            .arg(dgbAmount(reserved), dgbAmount(spent_hour),
                 dgbAmount(spent_day)));
        m_carrier_base_cents = poolNumber(result, "carrier_base_cents");
        m_carrier_excess_cents = poolNumber(
            result, "carrier_withdrawable_excess_cents");
        m_carrier_value_status->setText(tr(
            "Confirmed carrier base: %1 DD · currently withdrawable excess: %2 DD. The base remains wallet-owned and reserved only while its slot is active.")
            .arg(QString::number(m_carrier_base_cents / 100.0, 'f', 2),
                 QString::number(m_carrier_excess_cents / 100.0, 'f', 2)));
        m_preview_carrier_excess->setEnabled(
            configured && m_carrier_excess_cents > 0 && !m_busy);
        updateProviderButtons();
    }

    void refreshLiquidityStatus()
    {
        call("getpaymasterliquiditystatus", {}, false, nullptr,
             [this](const UniValue& result) {
                 applyLiquidityStatus(result);
                 refreshLiquidityPoolEntries();
             }, false,
             [this](const QString& error) {
                 m_maintenance_state = QStringLiteral("unavailable");
                 m_liquidity_maintenance_state->setText(tr(
                     "Liquidity maintenance status unavailable"));
                 m_liquidity_maintenance_next_step->setText(error);
                 m_liquidity_policy_status->setText(tr(
                     "Core did not return a liquidity policy. No paid maintenance action is available."));
                 refreshProviderSafetyStatus();
             });
    }

    void applyLiquidityPoolEntries(const UniValue& result,
                                   bool refresh_safety)
    {
        const QString previous = m_release_carrier_select->currentData().toString();
        m_release_carrier_select->blockSignals(true);
        m_release_carrier_select->clear();
        m_pending_successor_carrier_cents = 0;
        m_pending_successor_dgb_satoshis = 0;
        const UniValue& pool = result.find_value("pool");
        if (pool.isArray()) {
            for (const UniValue& entry : pool.getValues()) {
                const QString state = activityText(entry, "state");
                const QString purpose = activityText(entry, "purpose");
                const QString asset = activityText(entry, "asset");
                if (state == QLatin1String("pending_successor")) {
                    if (asset == QLatin1String("dd_carrier")) {
                        m_pending_successor_carrier_cents +=
                            poolNumber(entry, "dd_cents");
                    } else if (asset == QLatin1String("dgb")) {
                        m_pending_successor_dgb_satoshis +=
                            poolNumber(entry, "dgb_satoshis");
                    }
                }
                if (state != QLatin1String("available") ||
                    purpose != QLatin1String("operational") ||
                    asset != QLatin1String("dd_carrier") ||
                    poolNumber(entry, "confirmation_height") <= 0) {
                    continue;
                }
                const QString txid = activityText(entry, "txid");
                const qint64 vout = poolNumber(entry, "vout");
                const qint64 cents = poolNumber(entry, "dd_cents");
                const QString key = QStringLiteral("%1:%2")
                    .arg(txid).arg(vout);
                m_release_carrier_select->addItem(
                    tr("%1 DD — %2…:%3")
                        .arg(QString::number(cents / 100.0, 'f', 2),
                             txid.left(12))
                        .arg(vout),
                    key);
            }
        }
        const int previous_index =
            m_release_carrier_select->findData(previous);
        if (previous_index >= 0) {
            m_release_carrier_select->setCurrentIndex(previous_index);
        }
        m_release_carrier_select->blockSignals(false);
        QStringList recycling;
        if (m_pending_successor_carrier_cents > 0) {
            recycling.push_back(tr(
                "Carrier is being reused: %1 DD, waiting for confirmation.")
                .arg(QString::number(
                    m_pending_successor_carrier_cents / 100.0,
                    'f', 2)));
        }
        if (m_pending_successor_dgb_satoshis > 0) {
            recycling.push_back(tr(
                "DGB successor slot is being reused: %1 DGB, waiting for confirmation.")
                .arg(dgbAmount(m_pending_successor_dgb_satoshis)));
        }
        if (recycling.isEmpty() &&
            m_maintenance_state == QLatin1String("replenishing_liquidity")) {
            recycling.push_back(tr(
                "A missing DGB slot will be replenished within the approved limits."));
        }
        m_liquidity_recycling_status->setText(
            recycling.isEmpty()
                ? tr("No replacement output is currently waiting for confirmation.")
                : recycling.join('\n'));
        const bool release_available =
            m_liquidity_policy_configured && !m_core_running &&
            m_release_carrier_select->count() > 0;
        m_preview_carrier_release->setEnabled(release_available);
        if (previous_index < 0 && !previous.isEmpty()) {
            m_release_plan_id.clear();
            m_release_plan_expires_at = 0;
        }
        updateCarrierWithdrawalButtons();
        if (refresh_safety) refreshProviderSafetyStatus();
    }

    void refreshLiquidityPoolEntries()
    {
        call("getpaymasterpoolinfo", {}, false, nullptr,
             [this](const UniValue& result) {
                 applyLiquidityPoolEntries(result, /*refresh_safety=*/true);
             }, false,
             [this](const QString& error) {
                 m_release_carrier_select->clear();
                 m_preview_carrier_release->setEnabled(false);
                 m_liquidity_recycling_status->setText(tr(
                     "Pool entry details are unavailable: %1").arg(error));
                 refreshProviderSafetyStatus();
             });
    }

    void invalidateCarrierWithdrawalPreviews()
    {
        m_excess_plan_id.clear();
        m_release_plan_id.clear();
        m_excess_plan_expires_at = 0;
        m_release_plan_expires_at = 0;
        m_excess_preview_cents = 0;
        m_excess_preview_fee_satoshis = 0;
        if (m_carrier_withdrawal_status) {
            m_carrier_withdrawal_status->setText(tr(
                "No carrier withdrawal has been previewed."));
        }
        updateCarrierWithdrawalButtons();
    }

    bool carrierPlanCurrent(const QString& plan_id, qint64 expires_at) const
    {
        return !plan_id.isEmpty() && expires_at > 0 &&
               QDateTime::currentSecsSinceEpoch() <= expires_at;
    }

    void updateCarrierWithdrawalButtons()
    {
        if (!m_execute_carrier_excess || !m_execute_carrier_release) return;
        m_execute_carrier_excess->setEnabled(
            carrierPlanCurrent(m_excess_plan_id,
                               m_excess_plan_expires_at));
        m_execute_carrier_release->setEnabled(
            !m_core_running &&
            carrierPlanCurrent(m_release_plan_id,
                               m_release_plan_expires_at));
    }

    UniValue carrierWithdrawalOptions(const QString& mode, bool execute,
                                      const QString& plan_id = {}) const
    {
        UniValue options{UniValue::VOBJ};
        options.pushKV("mode", mode.toStdString());
        options.pushKV("execute", execute);
        if (execute) {
            options.pushKV("plan_id", plan_id.toStdString());
        } else if (mode == QLatin1String("release_slot")) {
            const QString selected =
                m_release_carrier_select->currentData().toString();
            const int separator = selected.lastIndexOf(':');
            if (separator > 0) {
                options.pushKV("txid",
                               selected.left(separator).toStdString());
                options.pushKV("vout",
                               selected.mid(separator + 1).toInt());
            }
        }
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        return params;
    }

    void carrierWithdrawalAction(const QString& mode, bool execute)
    {
        // Accept either the production wallet transport or the synchronous
        // test transport. Keeping this guard aligned with call() lets widget
        // tests exercise the same preview/plan-binding path without a live
        // wallet, while production still fails closed when no RPC transport
        // is available.
        if (!hasRpcTransport() || m_busy) return;
        const bool excess = mode == QLatin1String("all_excess");
        QString plan_id = excess ? m_excess_plan_id : m_release_plan_id;
        const qint64 expires_at = excess ? m_excess_plan_expires_at
                                         : m_release_plan_expires_at;
        if (!execute && !excess &&
            m_release_carrier_select->currentData().toString().isEmpty()) {
            QMessageBox::warning(this, tr("Release carrier slot"),
                                 tr("Select a confirmed available operational carrier first."));
            return;
        }
        if (execute && !carrierPlanCurrent(plan_id, expires_at)) {
            QMessageBox::warning(
                this, tr("Carrier plan expired"),
                tr("The reviewed plan is no longer current. Preview the action again before executing."));
            invalidateCarrierWithdrawalPreviews();
            return;
        }
        if (execute) {
            const QString confirmation = excess
                ? tr("Execute the reviewed withdrawal of %1 DD carrier excess?\n\nEvery selected carrier keeps exactly 1.00 DD. The reviewed DGB network fee is %2 DGB.")
                      .arg(QString::number(
                          m_excess_preview_cents / 100.0, 'f', 2),
                           dgbAmount(m_excess_preview_fee_satoshis))
                : tr("Release the reviewed operational carrier slot completely?\n\nThis costs no network fee, makes the carrier value ordinary wallet balance, and reduces the saved operational-carrier target by one.");
            if (QMessageBox::question(
                    this, tr("Confirm carrier withdrawal"), confirmation,
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes) {
                return;
            }
        }
        call("withdrawpaymastercarrier",
             carrierWithdrawalOptions(mode, execute, plan_id),
             /*needs_unlock=*/execute && excess, nullptr,
             [this, mode, execute](const UniValue& result) {
                 const bool excess = mode == QLatin1String("all_excess");
                 const QString returned_plan = activityText(result, "plan_id");
                 if (!execute) {
                     const qint64 expiry = poolNumber(result, "expires_at");
                     if (excess) {
                         m_excess_plan_id = returned_plan;
                         m_excess_plan_expires_at = expiry;
                         m_excess_preview_cents = poolNumber(
                             result, "withdrawable_excess_cents");
                         m_excess_preview_fee_satoshis = poolNumber(
                             result, "estimated_network_fee_satoshis");
                         m_release_plan_id.clear();
                         m_release_plan_expires_at = 0;
                         m_carrier_withdrawal_status->setText(tr(
                             "Preview only — withdraw %1 DD excess, retain %2 DD carrier base and pay an estimated %3 DGB network fee. Plan valid until %4.")
                             .arg(QString::number(
                                      m_excess_preview_cents / 100.0,
                                      'f', 2),
                                  QString::number(
                                      poolNumber(result,
                                                 "retained_carrier_cents") /
                                          100.0,
                                      'f', 2),
                                  dgbAmount(
                                      m_excess_preview_fee_satoshis),
                                  QDateTime::fromSecsSinceEpoch(expiry)
                                      .toString(Qt::ISODate)));
                     } else {
                         m_release_plan_id = returned_plan;
                         m_release_plan_expires_at = expiry;
                         m_excess_plan_id.clear();
                         m_excess_plan_expires_at = 0;
                         m_carrier_withdrawal_status->setText(tr(
                             "Preview only — release the selected carrier without a network fee and reduce the operational-carrier target to %1. Plan valid until %2.")
                             .arg(poolNumber(
                                      result,
                                      "operational_carrier_target"))
                             .arg(QDateTime::fromSecsSinceEpoch(expiry)
                                      .toString(Qt::ISODate)));
                     }
                     updateCarrierWithdrawalButtons();
                     return;
                 }
                 const QString txid = activityText(result, "txid");
                 m_carrier_withdrawal_status->setText(excess
                     ? tr("Carrier excess withdrawal submitted successfully.%1")
                           .arg(txid.isEmpty()
                                    ? QString{}
                                    : tr(" Transaction: %1").arg(txid))
                     : tr("Carrier slot released successfully. New operational-carrier target: %1.")
                           .arg(poolNumber(
                               result,
                               "operational_carrier_target")));
                 m_excess_plan_id.clear();
                 m_release_plan_id.clear();
                 m_excess_plan_expires_at = 0;
                 m_release_plan_expires_at = 0;
                 refreshStatus();
             });
    }

    PaymasterSetupMode loadSetupMode() const
    {
        const QString key = PaymasterSetupModeSettingsKey(m_model);
        if (key.isEmpty()) return PaymasterSetupMode::UNDECIDED;
        QSettings settings;
        const QString value = settings.value(key).toString();
        if (value == QLatin1String("guided")) return PaymasterSetupMode::GUIDED;
        if (value == QLatin1String("expert")) return PaymasterSetupMode::EXPERT;
        if (value == QLatin1String("existing")) return PaymasterSetupMode::EXISTING;
        return PaymasterSetupMode::UNDECIDED;
    }

    void setSetupMode(PaymasterSetupMode mode)
    {
        m_setup_mode = mode;
        const QString key = PaymasterSetupModeSettingsKey(m_model);
        if (!key.isEmpty()) {
            QString value;
            if (mode == PaymasterSetupMode::GUIDED) value = QStringLiteral("guided");
            if (mode == PaymasterSetupMode::EXPERT) value = QStringLiteral("expert");
            if (mode == PaymasterSetupMode::EXISTING) value = QStringLiteral("existing");
            QSettings settings;
            if (value.isEmpty()) {
                settings.remove(key);
            } else {
                settings.setValue(key, value);
            }
        }
        updateSetupAccess();
        const bool expert = mode == PaymasterSetupMode::EXPERT;
        if (m_liquidity_advanced_toggle) {
            m_liquidity_advanced_toggle->setChecked(expert);
        }
        if (m_runtime_settings_toggle) {
            m_runtime_settings_toggle->setChecked(expert);
        }
    }

    void updateSetupAccess()
    {
        const bool unlocked = m_setup_mode != PaymasterSetupMode::UNDECIDED;
        if (m_setup_choice) m_setup_choice->setVisible(!unlocked);
        if (m_setup_content) m_setup_content->setVisible(unlocked);
        if (!m_tabs) return;
        for (QWidget* page : {m_configuration_page, m_safety_page,
                              m_liquidity_page, m_finance_page}) {
            const int index = m_tabs->indexOf(page);
            if (index >= 0) {
                m_tabs->setTabEnabled(index, unlocked);
                m_tabs->setTabToolTip(index, unlocked ? QString() : tr(
                    "Complete guided setup or choose manual expert setup to unlock this page."));
            }
        }
        // Recovery and durable reservation inspection must never be hidden by
        // a local onboarding preference.
        const int activity_index = m_tabs->indexOf(m_activity_page);
        if (activity_index >= 0) {
            m_tabs->setTabEnabled(activity_index, true);
            m_tabs->setTabToolTip(activity_index, !unlocked ? tr(
                "Recovery remains available so existing reservations can always be inspected.")
                : QString());
        }
        if (!unlocked) m_tabs->setCurrentIndex(0);
    }

    void restorePolicyDefaults()
    {
        applyRecommendedPolicyDefaults(/*mark_dirty=*/true);
    }

    void applyRecommendedPolicyDefaults(bool mark_dirty)
    {
        m_loading_policy = true;
        m_scope->setCurrentIndex(m_scope->findData(QStringLiteral("public")));
        m_user_paid->setChecked(true);
        m_sponsored->setChecked(false);
        m_fee_bps->setValue(50);
        m_min_amount->setValue(100);
        m_max_amount->setValue(100000);
        m_quote_ttl->setValue(60);
        m_network_fee->setValue(20000000);
        m_loading_policy = false;
        m_policy_loaded = false;
        m_policy_dirty = mark_dirty;
        updatePolicyDisplay();
    }

    void loadPolicy(const UniValue& policy)
    {
        const UniValue& funding_models = policy.find_value("funding_models");
        if (!funding_models.isArray()) return;

        bool sponsored{false};
        bool user_paid{false};
        for (const UniValue& model : funding_models.getValues()) {
            if (!model.isStr()) continue;
            sponsored |= model.get_str() == "sponsored";
            user_paid |= model.get_str() == "user_paid";
        }

        m_loading_policy = true;
        m_sponsored->setChecked(sponsored);
        m_user_paid->setChecked(user_paid);
        const UniValue& scope = policy.find_value("sponsorship_scope");
        if (scope.isStr()) {
            const int scope_index = m_scope->findData(QString::fromStdString(scope.get_str()));
            if (scope_index >= 0) m_scope->setCurrentIndex(scope_index);
        }
        m_fee_bps->setValue(policy.find_value("fee_rate_bps").getInt<int>());
        m_min_amount->setValue(policy.find_value("min_amount_cents").getInt<int>());
        m_max_amount->setValue(policy.find_value("max_amount_cents").getInt<int>());
        m_quote_ttl->setValue(policy.find_value("quote_ttl").getInt<int>());
        m_network_fee->setValue(
            policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int>());
        m_loading_policy = false;
        m_policy_loaded = true;
        m_policy_dirty = false;
        updatePolicyDisplay();
    }

    void updatePolicyDisplay()
    {
        const bool user_paid = m_user_paid->isChecked();
        const bool sponsored = m_sponsored->isChecked();
        const bool restricted = sponsored &&
                                m_scope->currentData().toString() == QLatin1String("restricted");
        m_scope->setEnabled(sponsored);
        m_fee_bps->setEnabled(user_paid);
        if (!user_paid && m_fee_bps->value() != 0) m_fee_bps->setValue(0);

        QString model_text;
        if (user_paid && sponsored) {
            model_text = tr("User paid and public sponsored");
            m_funding_model_status->setText(tr(
                "Both models may be active together. User-paid transfers charge the configured DigiDollar service fee; public sponsored transfers charge no DigiDollar service fee. Each model has separate safety budgets."));
        } else if (user_paid) {
            model_text = tr("User paid only");
            m_funding_model_status->setText(tr(
                "Clients pay the configured DigiDollar service fee while this provider supplies the DGB network fee."));
        } else if (sponsored) {
            model_text = restricted ? tr("Restricted sponsored only")
                                    : tr("Public sponsored only");
            m_funding_model_status->setText(restricted
                ? tr("Only clients with a valid invitation may use this sponsored service. No DigiDollar service fee is charged.")
                : tr("Discovered clients may use this sponsored service. No DigiDollar service fee is charged; finite safety budgets are essential."));
        } else {
            m_funding_model_status->setText(tr("Select at least one payment model."));
            m_policy_summary->setText(tr(
                "No payment model selected. Select at least one model before saving."));
            return;
        }

        const QString minimum = QString::number(m_min_amount->value() / 100.0, 'f', 2);
        const QString maximum = QString::number(m_max_amount->value() / 100.0, 'f', 2);
        const QString service_fee = QString::number(m_fee_bps->value() / 100.0, 'f', 2);
        const QString network_fee = QString::number(m_network_fee->value() / 100000000.0, 'f', 8);
        const QString persistence = m_policy_dirty
            ? tr("Unsaved changes — save the policy to activate them.")
            : m_policy_loaded
                ? tr("These are the currently saved settings.")
                : tr("Recommended starting values — save the policy to activate them.");
        m_policy_summary->setText(tr(
            "%1; payments from %2 to %3 DD; user-paid service fee %4%; quotes valid for %5 seconds; DGB network-fee ceiling %6 DGB per transfer. %7")
            .arg(model_text, minimum, maximum, service_fee)
            .arg(m_quote_ttl->value())
            .arg(network_fee, persistence));
    }

    QString readinessExplanation(const QString& error) const
    {
        if (error == QLatin1String("PAYMASTER_DISABLED"))
            return tr("Paymaster support is disabled for this node. Enable it and restart DigiByte Core.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_NOT_ENABLED"))
            return tr("The provider configuration has not been enabled yet.");
        if (error == QLatin1String("PAYMASTER_IDENTITY_NOT_FOUND"))
            return tr("Create the provider identity in the Configuration tab.");
        if (error == QLatin1String("PAYMASTER_POLICY_NOT_FOUND"))
            return tr("Choose and save an operating policy in the Configuration tab.");
        if (error == QLatin1String("PAYMASTER_SAFETY_POLICY_NOT_FOUND") ||
            error == QLatin1String("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND"))
            return tr("Review and save finite spending and rate limits in the Safety limits tab.");
        if (error == QLatin1String("PAYMASTER_POOLS_NOT_PREPARED"))
            return tr("Preview and prepare the required wallet liquidity in the Liquidity tab.");
        if (error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
            error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
            error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING"))
            return tr("The saved targets are correct, but the wallet does not yet contain every confirmed pool output. Restore the missing liquidity; no target update is required.");
        if (error == QLatin1String("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED") ||
            error == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND"))
            return tr("Review and explicitly approve finite automatic liquidity-maintenance limits in the Liquidity tab.");
        if (error == QLatin1String("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING"))
            return tr("Wait for the automatically prepared replacement liquidity to confirm.");
        if (error == QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE"))
            return tr("The saved liquidity targets cannot support the active user-paid offer. Review Liquidity and save at least three carrier outputs for capacity checks and one carrier output for payments.");
        if (error == QLatin1String("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"))
            return tr("The finite automatic-maintenance budget is exhausted. Wait for the rolling limit to recover or review the saved limits in the Liquidity tab.");
        if (error == QLatin1String("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"))
            return tr("Automatic replenishment could not create or resume a safe maintenance transaction. Review the detailed liquidity status before retrying.");
        if (error == QLatin1String("PAYMASTER_WALLET_LOCKED"))
            return tr("Unlock the wallet before starting the provider or signing provider transactions.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_REQUIRES_DESCRIPTOR_WALLET"))
            return tr("Select a descriptor wallet. Legacy wallets cannot hold the Paymaster provider identity.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_REQUIRES_PRIVATE_KEYS"))
            return tr("Select a wallet with local private keys. Watch-only wallets cannot operate a Paymaster provider.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_REJECTS_EXTERNAL_SIGNER"))
            return tr("Select a wallet without an external signer. Provider identity and transaction signing must be available locally.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED"))
            return tr("Configure a reachable provider endpoint and restart DigiByte Core.");
        if (error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE"))
            return tr("The configured provider endpoint is not reachable by clients on this network.");
        if (error == QLatin1String("PAYMASTER_REQUIRES_PRUNE_0"))
            return tr("Provider operation requires an unpruned node.");
        if (error == QLatin1String("PAYMASTER_REQUIRES_TXINDEX"))
            return tr("Enable the transaction index and restart DigiByte Core.");
        if (error == QLatin1String("PAYMASTER_REQUIRES_READY_TXINDEX"))
            return tr("Wait until the transaction index has finished synchronizing.");
        if (error == QLatin1String("PAYMASTER_REQUIRES_V2_TRANSPORT"))
            return tr("Enable BIP324 version 2 transport and restart DigiByte Core.");
        if (error == QLatin1String("PAYMASTER_NODE_NOT_READY"))
            return tr("Wait until the node has synchronized and is ready to broadcast transactions.");
        if (error == QLatin1String("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"))
            return tr("DigiDollar is not active on the current chain yet.");
        if (error == QLatin1String("PAYMASTER_SAFETY_LIMIT_EXHAUSTED")) {
            qint64 applicable_per_transaction{std::numeric_limits<qint64>::max()};
            const auto include_model = [&applicable_per_transaction](
                    const FundingSafetyControls& controls, bool selected) {
                if (!selected) return;
                FundingSafetyValues values;
                if (readFundingSafety(controls, values) && values.per_transaction > 0) {
                    applicable_per_transaction = std::min(
                        applicable_per_transaction, values.per_transaction);
                }
            };
            include_model(m_user_paid_safety, m_user_paid->isChecked());
            include_model(
                m_public_sponsored_safety,
                m_sponsored->isChecked() &&
                    m_scope->currentData().toString() == QLatin1String("public"));
            include_model(
                m_restricted_sponsored_safety,
                m_sponsored->isChecked() &&
                    m_scope->currentData().toString() == QLatin1String("restricted"));
            if (applicable_per_transaction != std::numeric_limits<qint64>::max() &&
                m_network_fee->value() > applicable_per_transaction) {
                return tr(
                    "The advertised network-fee ceiling (%1 DGB) exceeds the applicable safety limit per transfer (%2 DGB). Lower the ceiling in Configuration or increase the limit in Safety limits, then save the changed setting.")
                    .arg(QString::number(m_network_fee->value() / 100000000.0, 'f', 8))
                    .arg(QString::number(applicable_per_transaction / 100000000.0, 'f', 8));
            }
            return tr("The configured provider budget or rate limit has been reached.");
        }
        if (error == QLatin1String("PAYMASTER_MESSAGE_CAPTURE_ENABLED"))
            return tr("Disable network message capture before operating a mainnet provider.");
        if (error.contains(QLatin1String("WALLET")))
            return tr("The selected wallet does not meet the provider requirements.");
        return tr("An additional provider requirement is not satisfied. See technical details below.");
    }

    void updateReadinessSummary(const QStringList& errors, bool running, bool ready)
    {
        updateExternalPrerequisites();
        if (running) {
            setStatusLabel(m_next_step, tr("Provider is running"),
                           QStringLiteral("ready"));
            m_readiness_summary->setText(tr(
                "Keep this node online and monitor reservations, liquidity and safety-budget usage."));
            return;
        }
        if (ready) {
            setStatusLabel(m_next_step,
                           tr("All external prerequisites are ready"),
                           QStringLiteral("ready"));
            m_readiness_summary->setText(tr(
                "All requirements are satisfied and all setup settings are saved. No additional Save action is required. Start the provider only when you are ready for it to accept client requests within the displayed safety limits."));
            return;
        }

        if (providerConfigurationComplete() && hasPassiveExternalWait()) {
            setStatusLabel(m_next_step,
                           tr("Waiting for blockchain synchronization"),
                           QStringLiteral("waiting"));
            m_readiness_summary->setText(tr(
                "Provider configuration is complete. You do not need to change any provider settings. "
                "Core will enable provider start automatically when the node is synchronized, the transaction index is ready and transactions can be broadcast."));
            return;
        }

        const bool liquidity_attention = std::any_of(
            errors.cbegin(), errors.cend(),
            [](const QString& error) { return isLiquidityReadinessError(error); });
        if (providerConfigurationComplete() && liquidity_attention) {
            setStatusLabel(
                m_next_step,
                tr("All external prerequisites are ready"),
                QStringLiteral("ready"));
            m_readiness_summary->setText(tr(
                "Blockchain synchronization, transaction indexing, transaction broadcast and DigiDollar activation are ready. The remaining wallet-liquidity action is shown separately below."));
            return;
        }

        QString next;
        const QStringList priority{
            QStringLiteral("PAYMASTER_DISABLED"),
            QStringLiteral("PAYMASTER_IDENTITY_NOT_FOUND"),
            QStringLiteral("PAYMASTER_POLICY_NOT_FOUND"),
            QStringLiteral("PAYMASTER_SAFETY_POLICY_NOT_FOUND"),
            QStringLiteral("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND"),
            QStringLiteral("PAYMASTER_PROVIDER_NOT_ENABLED"),
            QStringLiteral("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED"),
            QStringLiteral("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND"),
            QStringLiteral("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE"),
            QStringLiteral("PAYMASTER_POOLS_NOT_PREPARED"),
            QStringLiteral("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING"),
            QStringLiteral("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"),
            QStringLiteral("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"),
            QStringLiteral("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED"),
            QStringLiteral("PAYMASTER_REQUIRES_PRUNE_0"),
            QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"),
            QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"),
            QStringLiteral("PAYMASTER_REQUIRES_V2_TRANSPORT"),
            QStringLiteral("PAYMASTER_NODE_NOT_READY"),
            QStringLiteral("PAYMASTER_WALLET_LOCKED"),
        };
        for (const QString& candidate : priority) {
            if (errors.contains(candidate)) {
                next = readinessExplanation(candidate);
                break;
            }
        }
        if (next.isEmpty() && !errors.isEmpty()) next = readinessExplanation(errors.first());
        const bool unknown_error = std::any_of(
            errors.cbegin(), errors.cend(), [](const QString& error) {
                return !isProviderSetupReadinessError(error) &&
                       !isLiquidityReadinessError(error) &&
                       !isPassiveExternalReadinessError(error) &&
                       error != QLatin1String("PAYMASTER_REQUIRES_TXINDEX");
            });
        setStatusLabel(
            m_next_step,
            next.isEmpty() ? tr("Setup status unavailable")
                           : tr("Next step: %1").arg(next),
            unknown_error ? QStringLiteral("error")
                          : QStringLiteral("action"));

        QStringList explanations;
        for (const QString& error : errors) {
            const QString explanation = readinessExplanation(error);
            if (!explanations.contains(explanation)) explanations.push_back(explanation);
        }
        m_readiness_summary->setText(explanations.isEmpty()
            ? tr("Refresh the status to check the provider requirements.")
            : QStringLiteral("• ") + explanations.join(QStringLiteral("\n• ")));
    }

    static QLineEdit* integerField(QWidget* parent, qint64 value)
    {
        auto* result = new QLineEdit(QString::number(value), parent);
        result->setMaxLength(19);
        result->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9]{1,19}")), result));
        return result;
    }

    FundingSafetyControls createFundingSafetyControls(
        QTabWidget* tabs, const QString& title, const QString& object_prefix,
        bool recommended_defaults)
    {
        auto* page = new QWidget(tabs);
        page->setObjectName(object_prefix + QStringLiteral("Page"));
        auto* form = new QFormLayout(page);
        form->setContentsMargins(14, 14, 14, 14);
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(9);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        QString explanation;
        if (object_prefix == QLatin1String("paymasterSafetyUserPaid")) {
            explanation = tr(
                "Use these limits when clients compensate this provider with a DigiDollar service fee. The provider still spends DGB for network fees, so finite DGB budgets remain necessary.");
        } else if (object_prefix == QLatin1String("paymasterSafetyPublicSponsored")) {
            explanation = tr(
                "Public sponsorship is available to discovered clients and therefore has the greatest budget-exhaustion risk. Its safe default is disabled; enable it only with finite values you can afford to lose.");
        } else {
            explanation = tr(
                "Restricted sponsorship requires a provider-issued invitation but still needs finite limits. Its safe default is disabled until you deliberately configure a budget.");
        }
        auto* help = new QLabel(explanation, page);
        help->setObjectName(object_prefix + QStringLiteral("Explanation"));
        help->setProperty("paymasterRole", QStringLiteral("mutedText"));
        help->setWordWrap(true);
        form->addRow(help);
        FundingSafetyControls controls;
        controls.per_transaction = dgbAmountField(page, recommended_defaults ? 10000000 : 0);
        controls.reserved = dgbAmountField(page, recommended_defaults ? 50000000 : 0);
        controls.per_hour = dgbAmountField(page, recommended_defaults ? 100000000 : 0);
        controls.per_day = dgbAmountField(page, recommended_defaults ? 500000000 : 0);
        controls.completed_per_hour = spin(page, 0, 1000000, recommended_defaults ? 10 : 0);
        controls.completed_per_day = spin(page, 0, 1000000, recommended_defaults ? 100 : 0);
        controls.per_transaction->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerTransaction"));
        controls.reserved->setObjectName(object_prefix + QStringLiteral("MaxReservedNetworkFee"));
        controls.per_hour->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerHour"));
        controls.per_day->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerDay"));
        controls.completed_per_hour->setObjectName(object_prefix + QStringLiteral("MaxCompletedPerHour"));
        controls.completed_per_day->setObjectName(object_prefix + QStringLiteral("MaxCompletedPerDay"));
        controls.per_transaction->setToolTip(tr("Maximum DGB network fee committed by one transfer."));
        controls.reserved->setToolTip(tr("Maximum DGB network fees reserved across simultaneous open quotes."));
        controls.per_hour->setToolTip(tr("Maximum cumulative DGB network fees in any rolling hour."));
        controls.per_day->setToolTip(tr("Maximum cumulative DGB network fees in any rolling 24 hours."));
        controls.completed_per_hour->setToolTip(tr("Maximum completed transfers in any rolling hour."));
        controls.completed_per_day->setToolTip(tr("Maximum completed transfers in any rolling 24 hours."));
        form->addRow(tr("Maximum per transfer (DGB):"), controls.per_transaction);
        form->addRow(tr("Maximum reserved at once (DGB):"), controls.reserved);
        form->addRow(tr("Maximum per rolling hour (DGB):"), controls.per_hour);
        form->addRow(tr("Maximum per rolling day (DGB):"), controls.per_day);
        form->addRow(tr("Completed transfers per rolling hour:"), controls.completed_per_hour);
        form->addRow(tr("Completed transfers per rolling day:"), controls.completed_per_day);
        controls.mode = new QLabel(page);
        controls.mode->setObjectName(object_prefix + QStringLiteral("Mode"));
        controls.mode->setWordWrap(true);
        form->addRow(controls.mode);
        auto* restore_defaults = new QPushButton(
            recommended_defaults ? tr("Restore recommended limits")
                                 : tr("Restore safe disabled defaults"), page);
        restore_defaults->setObjectName(object_prefix + QStringLiteral("RestoreDefaults"));
        restore_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        restore_defaults->setToolTip(recommended_defaults
            ? tr("Restore the recommended finite starting limits for this model. Save the provider safety policy to activate them.")
            : tr("Set all six limits to zero so this unused model is safely disabled. Save the provider safety policy to activate the change."));
        form->addRow(restore_defaults);
        connect(restore_defaults, &QPushButton::clicked, this,
                [this, controls, recommended_defaults] {
                    restoreFundingSafetyDefaults(controls, recommended_defaults);
                });
        tabs->addTab(page, title);
        return controls;
    }

    void restoreFundingSafetyDefaults(const FundingSafetyControls& controls,
                                      bool recommended_defaults,
                                      bool mark_dirty = true)
    {
        m_loading_provider_safety = true;
        static_cast<DgbAmountLineEdit*>(controls.per_transaction)->setSatoshis(
            recommended_defaults ? 10000000 : 0);
        static_cast<DgbAmountLineEdit*>(controls.reserved)->setSatoshis(
            recommended_defaults ? 50000000 : 0);
        static_cast<DgbAmountLineEdit*>(controls.per_hour)->setSatoshis(
            recommended_defaults ? 100000000 : 0);
        static_cast<DgbAmountLineEdit*>(controls.per_day)->setSatoshis(
            recommended_defaults ? 500000000 : 0);
        controls.completed_per_hour->setValue(recommended_defaults ? 10 : 0);
        controls.completed_per_day->setValue(recommended_defaults ? 100 : 0);
        m_loading_provider_safety = false;
        if (mark_dirty) {
            markProviderSafetyDirty();
        } else {
            m_provider_safety_dirty = false;
        }
        updateFundingSafetyDisplay(controls);
    }

    void restoreQuoteSafetyDefaults(bool mark_dirty = true)
    {
        m_loading_provider_safety = true;
        m_max_active_quotes_total->setValue(16);
        m_max_active_quotes_per_netgroup->setValue(4);
        m_max_active_quotes_per_recipient->setValue(2);
        m_max_quote_requests_per_netgroup->setValue(10);
        m_loading_provider_safety = false;
        if (mark_dirty) {
            markProviderSafetyDirty();
        } else {
            m_provider_safety_dirty = false;
        }
    }

    void restoreClientSafetyDefaults(bool mark_dirty = true)
    {
        m_loading_client_safety = true;
        m_client_fee_per_transaction->setValue(100);
        m_client_fee_per_day->setValue(1000);
        m_loading_client_safety = false;
        if (mark_dirty) {
            markClientSafetyDirty();
        } else {
            m_client_safety_dirty = false;
        }
        updateClientSafetyDisplay();
    }

    void markProviderSafetyDirty()
    {
        m_provider_safety_dirty = true;
        m_provider_safety_status->setText(tr(
            "Unsaved provider safety changes — the previously saved limits remain active until these values are saved."));
        updateProviderButtons();
    }

    void markClientSafetyDirty()
    {
        m_client_safety_dirty = true;
        m_client_safety_status->setText(tr(
            "Unsaved client safety changes — the previously saved limits remain active until these values are saved."));
    }

    void updateFundingSafetyDisplay(const FundingSafetyControls& controls,
                                    bool persisted = false)
    {
        if (!controls.mode) return;
        FundingSafetyValues values;
        if (!readFundingSafety(controls, values)) {
            controls.mode->setText(tr(
                "Not saved — one or more amounts are outside the supported whole-satoshi range."));
        } else if (values.allZero()) {
            controls.mode->setText(tr(
                "Disabled — all limits are zero; zero never means unlimited."));
        } else if (values.hasZero()) {
            controls.mode->setText(tr(
                "Incomplete — an enabled model requires all six limits to be greater than zero. Use all zero values only to disable the model."));
        } else {
            const QString per_transaction = QString::number(
                values.per_transaction / 100000000.0, 'f', 8);
            const QString per_hour = QString::number(
                values.per_hour / 100000000.0, 'f', 8);
            const QString per_day = QString::number(
                values.per_day / 100000000.0, 'f', 8);
            controls.mode->setText(persisted
                ? tr("Active finite limits — at most %1 DGB per transfer, %2 DGB per rolling hour and %3 DGB per rolling day.")
                      .arg(per_transaction, per_hour, per_day)
                : tr("Unsaved limits — at most %1 DGB per transfer, %2 DGB per rolling hour and %3 DGB per rolling day. Save the provider safety policy to activate them.")
                      .arg(per_transaction, per_hour, per_day));
        }
    }

    void updateClientSafetyDisplay(bool persisted = false)
    {
        if (!m_client_safety_mode) return;
        if (m_client_fee_per_transaction->value() == 0 &&
            m_client_fee_per_day->value() == 0) {
            m_client_safety_mode->setText(tr(
                "Zero service-fee budget — only zero-fee authorizations fit; zero never means unlimited."));
        } else if (persisted) {
            m_client_safety_mode->setText(tr(
                "Active limits — at most %1 DD per transfer and %2 DD in a rolling day.")
                .arg(QString::number(m_client_fee_per_transaction->value() / 100.0, 'f', 2),
                     QString::number(m_client_fee_per_day->value() / 100.0, 'f', 2)));
        } else {
            m_client_safety_mode->setText(tr(
                "Unsaved limits — at most %1 DD per transfer and %2 DD in a rolling day. Save the client safety policy to activate them.")
                .arg(QString::number(m_client_fee_per_transaction->value() / 100.0, 'f', 2),
                     QString::number(m_client_fee_per_day->value() / 100.0, 'f', 2)));
        }
    }

    void watchFundingSafetyControls(const FundingSafetyControls& controls)
    {
        for (QLineEdit* control : {controls.per_transaction, controls.reserved,
                                   controls.per_hour, controls.per_day}) {
            connect(control, &QLineEdit::textChanged, this, [this, controls] {
                if (!m_loading_provider_safety) markProviderSafetyDirty();
                updateFundingSafetyDisplay(controls);
                updateProviderButtons();
            });
        }
        for (QSpinBox* control : {controls.completed_per_hour,
                                  controls.completed_per_day}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), this, [this, controls] {
                if (!m_loading_provider_safety) markProviderSafetyDirty();
                updateFundingSafetyDisplay(controls);
                updateProviderButtons();
            });
        }
    }

    static bool readFundingSafety(const FundingSafetyControls& controls,
                                  FundingSafetyValues& values)
    {
        bool ok_per_transaction{false};
        bool ok_reserved{false};
        bool ok_per_hour{false};
        bool ok_per_day{false};
        ok_per_transaction = static_cast<DgbAmountLineEdit*>(
            controls.per_transaction)->satoshis(values.per_transaction);
        ok_reserved = static_cast<DgbAmountLineEdit*>(
            controls.reserved)->satoshis(values.reserved);
        ok_per_hour = static_cast<DgbAmountLineEdit*>(
            controls.per_hour)->satoshis(values.per_hour);
        ok_per_day = static_cast<DgbAmountLineEdit*>(
            controls.per_day)->satoshis(values.per_day);
        values.completed_per_hour = controls.completed_per_hour->value();
        values.completed_per_day = controls.completed_per_day->value();
        return ok_per_transaction && ok_reserved && ok_per_hour && ok_per_day;
    }

    static UniValue fundingSafetyToJSON(const FundingSafetyValues& values)
    {
        UniValue result{UniValue::VOBJ};
        result.pushKV("maximum_network_fee_per_transaction_satoshis", values.per_transaction);
        result.pushKV("maximum_reserved_network_fee_satoshis", values.reserved);
        result.pushKV("maximum_network_fee_per_hour_satoshis", values.per_hour);
        result.pushKV("maximum_network_fee_per_day_satoshis", values.per_day);
        result.pushKV("maximum_completed_per_hour", values.completed_per_hour);
        result.pushKV("maximum_completed_per_day", values.completed_per_day);
        return result;
    }

    static void loadFundingSafety(const UniValue& value,
                                  const FundingSafetyControls& controls)
    {
        if (!value.isObject()) return;
        static_cast<DgbAmountLineEdit*>(controls.per_transaction)->setSatoshis(
            value.find_value("maximum_network_fee_per_transaction_satoshis").getInt<qint64>());
        static_cast<DgbAmountLineEdit*>(controls.reserved)->setSatoshis(
            value.find_value("maximum_reserved_network_fee_satoshis").getInt<qint64>());
        static_cast<DgbAmountLineEdit*>(controls.per_hour)->setSatoshis(
            value.find_value("maximum_network_fee_per_hour_satoshis").getInt<qint64>());
        static_cast<DgbAmountLineEdit*>(controls.per_day)->setSatoshis(
            value.find_value("maximum_network_fee_per_day_satoshis").getInt<qint64>());
        controls.completed_per_hour->setValue(
            value.find_value("maximum_completed_per_hour").getInt<int>());
        controls.completed_per_day->setValue(
            value.find_value("maximum_completed_per_day").getInt<int>());
    }

    bool providerSafetyAllowsSelectedModels() const
    {
        // Core is authoritative for funding-model applicability and all limit
        // relationships. Qt only prevents activation with missing or unsaved
        // wallet-local policy data.
        return m_provider_safety_configured && !m_provider_safety_dirty;
    }

    QString providerStartConfirmationText() const
    {
        QStringList budgets;
        const auto append_budget = [this, &budgets](const QString& name,
                                                     const FundingSafetyControls& controls) {
            FundingSafetyValues values;
            if (!readFundingSafety(controls, values) || values.allZero()) return;
            budgets.push_back(tr(
                "%1: at most %2 DGB per transfer, %3 DGB per rolling hour and %4 DGB per rolling day; %5 transfers/hour and %6 transfers/day.")
                .arg(name)
                .arg(QString::number(values.per_transaction / 100000000.0, 'f', 8))
                .arg(QString::number(values.per_hour / 100000000.0, 'f', 8))
                .arg(QString::number(values.per_day / 100000000.0, 'f', 8))
                .arg(values.completed_per_hour)
                .arg(values.completed_per_day));
        };
        if (m_user_paid->isChecked()) {
            append_budget(tr("User paid"), m_user_paid_safety);
        }
        if (m_sponsored->isChecked() &&
            m_scope->currentData().toString() == QLatin1String("restricted")) {
            append_budget(tr("Restricted sponsored"),
                          m_restricted_sponsored_safety);
        } else if (m_sponsored->isChecked()) {
            append_budget(tr("Public sponsored"), m_public_sponsored_safety);
        }

        const bool automatic = m_operation_mode != QLatin1String("manual");
        QString liquidity_budget;
        LiquidityPolicyValues liquidity_values;
        if (automatic && m_liquidity_policy_configured &&
            !m_liquidity_policy_dirty &&
            readLiquidityPolicy(liquidity_values) &&
            liquidity_values.paid_maintenance_approved) {
            liquidity_budget = tr(
                "\nAutomatic liquidity maintenance: at most %1 DGB per maintenance transaction, %2 DGB per rolling hour and %3 DGB per rolling day.")
                .arg(QString::number(
                    liquidity_values.fee_per_transaction / 100000000.0, 'f', 8))
                .arg(QString::number(
                    liquidity_values.fee_per_hour / 100000000.0, 'f', 8))
                .arg(QString::number(
                    liquidity_values.fee_per_day / 100000000.0, 'f', 8));
        }
        return tr(
            "Start this provider now in %1 mode?\n\n%2%3\n\n"
            "Once online, the provider may accept client requests and spend DGB for network fees only within these persisted wallet-local limits. "
            "%4")
            .arg(automatic ? tr("automatic") : tr("manual expert"),
                  budgets.isEmpty() ? tr("No active funding-model budget is available.")
                                   : budgets.join('\n'),
                  liquidity_budget,
                  automatic
                     ? tr("Core will process eligible provider-side work without a separate dialog for every transfer. Clients must still confirm the exact provider and fee before signing.")
                     : tr("You must process waiting requests and submitted payments manually from Activity and recovery."));
    }

    QString providerServiceStatusText() const
    {
        if (m_service_state == QLatin1String("waiting_for_readiness") &&
            (m_last_service_error ==
                 QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
             (!m_liquidity_targets_satisfy_provider_policy &&
              m_last_service_error ==
                  QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
            return tr("Automatic start is waiting because the saved user-paid liquidity targets do not include an operational DigiDollar carrier. Review Liquidity, save a target of at least one payment carrier and prepare the missing output.");
        }
        if (m_service_state == QLatin1String("replenishing_liquidity") &&
            maintenanceFeeLimitExceeded()) {
            return tr(
                "Automatic refill is paused because its estimated network fee exceeds the saved per-transaction maintenance limit. No transaction was created and no DGB was spent. Review the refill cost limit in Liquidity before retrying.");
        }
        const QString error_suffix = m_last_service_error.isEmpty()
            ? QString{}
            : tr(" Stable error: %1").arg(m_last_service_error);
        const QString queue_suffix = tr(
            " Queue now: %1 request(s), %2 submitted payment(s).")
            .arg(m_waiting_provider_requests)
            .arg(m_waiting_provider_submits);
        if (!m_model) {
            return tr("No wallet is selected. Select a provider wallet first.");
        }
        if (m_service_state == QLatin1String("active")) {
            return tr("Automatic operation active — Core processes bounded request and submission work in the background. No manual queue action is required.") + queue_suffix;
        }
        if (m_service_state == QLatin1String("manual")) {
            return tr("Manual expert mode active — use the two processing actions below in order as messages arrive.") + queue_suffix;
        }
        if (m_service_state == QLatin1String("waiting_for_unlock")) {
            return tr("Automatic operation paused while the wallet is locked. Queued messages remain untouched and processing resumes after unlock.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("waiting_for_readiness")) {
            return tr("Autostart is enabled, but Core is waiting for provider readiness. Review Overview for the next required action.") + error_suffix;
        }
        if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
            return tr("Automatic operation is paused until finite paid liquidity-maintenance limits are explicitly approved. No maintenance transaction is created before approval.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("replenishing_liquidity")) {
            return tr("Automatic operation is restoring missing DGB or carrier slots within the saved maintenance budget. New requests wait until confirmed capacity is ready.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
            return tr("Replacement liquidity has been prepared and is waiting for blockchain confirmation. Existing authorized work remains recoverable.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("drain_only")) {
            return tr("Recovery-only operation active — Core rejects new quotes and automatically completes only previously authorized durable work.") + queue_suffix + error_suffix;
        }
        if (m_service_state == QLatin1String("error")) {
            return tr("Automatic operation stopped on a safe, privacy-neutral error. Review the stable code and provider readiness before retrying.") + error_suffix;
        }
        if (!m_core_running) {
            return m_autostart_enabled
                ? tr("Provider stopped. Autostart is enabled and will start it when the wallet and readiness requirements permit.")
                : tr("Provider stopped — start it on Overview after completing setup. Automatic processing begins only after that conscious start.");
        }
        if (m_core_locked) {
            return tr("Provider paused while the wallet is locked. No queued message is consumed.");
        }
        return tr("Provider runtime state is being refreshed. Core exposes no manual action until a known safe state is available.");
    }

    void saveRuntimeSettings()
    {
        if (!hasRpcTransport() || m_busy || !m_runtime_settings_dirty) return;
        const QString selected_mode =
            m_operation_mode_select->currentData().toString();
        if (m_core_running && selected_mode != m_operation_mode) {
            QMessageBox::warning(
                this, tr("Stop provider before changing mode"),
                tr("Stop the provider from Overview before switching between automatic and manual operation. Autostart may still be changed while it is running."));
            return;
        }
        UniValue settings{UniValue::VOBJ};
        settings.pushKV("operation_mode", selected_mode.toStdString());
        settings.pushKV("autostart", m_autostart->isChecked());
        UniValue params{UniValue::VARR};
        params.push_back(std::move(settings));
        call("setpaymasterruntimesettings", std::move(params), false, nullptr,
             [this](const UniValue& result) {
                 m_operation_mode = QString::fromStdString(
                     result.find_value("operation_mode").get_str());
                 m_autostart_enabled =
                     result.find_value("autostart").get_bool();
                 m_runtime_settings_dirty = false;
                 m_runtime_settings_result->setText(
                     m_operation_mode == QLatin1String("manual")
                         ? tr("Saved: manual expert processing. The provider must be stopped before returning to automatic mode.")
                         : m_autostart_enabled
                               ? tr("Saved: automatic processing with provider autostart. No passphrase is stored; a locked wallet waits for unlock.")
                               : tr("Saved: automatic processing after an explicit provider start. Provider autostart remains off."));
                 refreshStatus();
             }, /*show_error=*/false,
             [this](const QString& error) {
                 // Preserve the unsaved edit after a backend rejection and
                 // report the failure beside the affected controls. This
                 // prevents a failed RPC from looking like a successful save.
                 m_runtime_settings_result->setText(
                     tr("Runtime settings were not saved: %1").arg(error));
                 updateProviderButtons();
             });
    }

    void updateProviderButtons()
    {
        const bool safety_ready = providerSafetyAllowsSelectedModels();
        const bool manual_mode = m_operation_mode == QLatin1String("manual");
        // Starting into maintenance is safe only when the saved policy can
        // actually restore every slot required by the active offer. In
        // particular, an intentionally released zero carrier target must not
        // be presented as a repairable shortfall.
        const bool automatic_liquidity_can_restore =
            m_liquidity_policy_configured &&
            m_liquidity_targets_satisfy_provider_policy &&
            m_automatic_replenishment->isChecked() &&
            m_paid_maintenance_approved->isChecked();
        const bool repairable_liquidity_gap =
            !m_readiness_errors.isEmpty() && !manual_mode &&
            automatic_liquidity_can_restore &&
            std::all_of(m_readiness_errors.cbegin(), m_readiness_errors.cend(),
                        [](const QString& error) {
                            return error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
                                   error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
                                   error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING");
                        });
        const bool can_start = m_core_ready || repairable_liquidity_gap;
        if (m_enable) {
            m_enable->setText(m_core_enabled
                ? tr("Provider configuration enabled")
                : tr("Enable provider configuration"));
            m_enable->setEnabled(safety_ready && !m_core_enabled);
            m_enable->setToolTip(m_core_enabled
                ? tr("Provider configuration is already enabled and saved in this wallet")
                : safety_ready
                ? tr("Enable the provider using the persisted finite safety limits")
                : tr("Save a complete finite safety policy for every selected funding model first"));
        }
        if (m_start) {
            m_start->setText(repairable_liquidity_gap && !m_core_running
                ? tr("Start and restore liquidity")
                : m_core_ready && !m_core_running
                    ? tr("Start provider now")
                    : tr("Start provider"));
            m_start->setEnabled(m_core_eligible && m_core_enabled && can_start &&
                                !m_core_running && !m_core_locked && safety_ready &&
                                !m_policy_dirty && !m_provider_safety_dirty &&
                                !m_runtime_settings_dirty &&
                                !m_liquidity_policy_dirty);
            if (m_policy_dirty || m_provider_safety_dirty ||
                m_runtime_settings_dirty) {
                m_start->setToolTip(tr(
                    "Save or discard the pending provider setting changes before starting. The start confirmation must show the persisted effective limits."));
            } else if (m_readiness_errors.contains(QStringLiteral(
                           "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ||
                       (!m_liquidity_targets_satisfy_provider_policy &&
                        m_readiness_errors.contains(QStringLiteral(
                            "PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
                m_start->setToolTip(tr(
                    "The saved user-paid liquidity targets cannot make this provider ready. Review Liquidity and save at least three carrier targets for capacity checks and one carrier target for payments."));
            } else if (!repairable_liquidity_gap &&
                       std::any_of(
                           m_readiness_errors.cbegin(),
                           m_readiness_errors.cend(),
                           [](const QString& error) {
                               return error == QLatin1String(
                                          "PAYMASTER_ADMISSION_DGB_MISSING") ||
                                      error == QLatin1String(
                                          "PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
                                      error == QLatin1String(
                                          "PAYMASTER_OPERATIONAL_SLOT_MISSING");
                           })) {
                m_start->setToolTip(tr(
                    "Liquidity is missing, but automatic restoration is not fully configured and approved. Review the Liquidity page before starting."));
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_NODE_NOT_READY"))) {
                m_start->setToolTip(tr(
                    "Provider start is waiting for blockchain synchronization and transaction broadcast readiness. No provider setting needs changing."));
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"))) {
                m_start->setToolTip(tr(
                    "Provider start is waiting for the transaction index to finish synchronizing."));
            } else if (m_readiness_errors.contains(
                           QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"))) {
                m_start->setToolTip(tr(
                    "Provider start is waiting for DigiDollar activation on this chain."));
            } else {
                m_start->setToolTip(tr(
                    "Review the persisted effective budgets, then consciously start this provider runtime."));
            }
        }
        if (m_stop) m_stop->setEnabled(m_core_running);
        if (m_request_processing_group) {
            m_request_processing_group->setVisible(manual_mode);
        }
        if (m_submit_processing_group) {
            m_submit_processing_group->setVisible(manual_mode);
        }
        if (m_activity_result_group) {
            m_activity_result_group->setVisible(manual_mode);
        }
        if (m_process_requests) {
            m_process_requests->setEnabled(manual_mode && m_core_running &&
                                           !m_core_locked);
        }
        if (m_process_submits) {
            m_process_submits->setEnabled(manual_mode && m_core_running &&
                                          !m_core_locked);
        }
        if (m_operation_mode_select) {
            m_operation_mode_select->setEnabled(!m_core_running);
        }
        if (m_autostart) m_autostart->setEnabled(hasRpcTransport());
        if (m_save_runtime_settings) {
            m_save_runtime_settings->setEnabled(
                hasRpcTransport() && m_runtime_settings_dirty && !m_busy &&
                (!m_core_running ||
                 m_operation_mode_select->currentData().toString() ==
                     m_operation_mode));
        }
        if (m_activity_runtime_status) {
            m_activity_runtime_status->setText(providerServiceStatusText());
        }
        if (m_operation_primary) {
            if (m_core_running) {
                m_operation_primary->setText(tr("Stop provider"));
                m_operation_primary->setEnabled(!m_busy);
            } else if (m_start && m_start->isEnabled()) {
                m_operation_primary->setText(m_start->text());
                m_operation_primary->setEnabled(!m_busy);
            } else {
                m_operation_primary->setText(tr("Refresh provider status"));
                m_operation_primary->setEnabled(m_model && !m_busy);
            }
        }
        updateOperatorDashboard();
    }

    void setStatusLabel(QLabel* label, const QString& text,
                        const QString& kind) const
    {
        if (!label) return;
        QString symbol{QStringLiteral("•")};
        if (kind == QLatin1String("ready")) symbol = QStringLiteral("✓");
        if (kind == QLatin1String("waiting")) symbol = QStringLiteral("…");
        if (kind == QLatin1String("action")) symbol = QStringLiteral("!");
        if (kind == QLatin1String("error")) symbol = QStringLiteral("!");
        const QString rendered = QStringLiteral("%1  %2").arg(symbol, text);
        const bool kind_changed =
            label->property("statusKind").toString() != kind;
        if (label->text() != rendered) label->setText(rendered);
        if (kind_changed) {
            label->setProperty("statusKind", kind);
            if (label->style()) {
                label->style()->unpolish(label);
                label->style()->polish(label);
            }
        }
    }

    void setStatusKind(QWidget* widget, const QString& kind) const
    {
        if (!widget) return;
        if (widget->property("statusKind").toString() == kind) return;
        widget->setProperty("statusKind", kind);
        if (widget->style()) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
    }

    void updateOperatorDashboard()
    {
        const bool offer_ready = m_core_has_identity && m_policy_loaded &&
                                 !m_policy_dirty && m_core_enabled;
        setStatusLabel(
            m_overview_offer_status,
            offer_ready ? tr("Ready · identity and offer are saved")
                        : m_policy_dirty
                              ? tr("Action required · save the offer changes")
                              : !m_core_has_identity
                                    ? tr("Action required · create a provider identity")
                                    : tr("Action required · complete and enable the offer"),
            offer_ready ? QStringLiteral("ready") : QStringLiteral("action"));
        if (m_overview_offer_action) {
            m_overview_offer_action->setText(offer_ready ? tr("Review offer")
                                                         : tr("Complete offer"));
        }

        const bool safety_ready = providerSafetyAllowsSelectedModels() &&
                                  !m_provider_safety_dirty;
        setStatusLabel(
            m_overview_safety_status,
            safety_ready ? tr("Ready · finite spending limits are active")
                         : m_provider_safety_dirty
                               ? tr("Action required · save the changed limits")
                               : tr("Action required · set finite spending limits"),
            safety_ready ? QStringLiteral("ready") : QStringLiteral("action"));
        if (m_overview_safety_action) {
            m_overview_safety_action->setText(safety_ready ? tr("Review limits")
                                                           : tr("Set limits"));
        }

        const int ready_dgb = m_pool_admission_dgb + m_pool_operational_dgb;
        const int target_dgb = m_admission_dgb->value() + m_operational_dgb->value();
        const int ready_carriers = m_pool_admission_carriers + m_pool_operational_carriers;
        const int target_carriers = m_user_paid->isChecked()
            ? m_admission_carriers->value() + m_operational_carriers->value()
            : 0;
        const bool liquidity_ready = m_pool_status_loaded &&
                                     m_liquidity_targets_satisfy_provider_policy &&
                                     ready_dgb >= target_dgb &&
                                     ready_carriers >= target_carriers;
        const bool liquidity_waiting = m_maintenance_state == QLatin1String("replenishing") ||
                                       m_maintenance_state == QLatin1String("waiting_for_confirmation") ||
                                       m_service_state == QLatin1String("replenishing_liquidity") ||
                                       m_service_state == QLatin1String("waiting_for_liquidity_confirmation");
        const bool refill_approval_needed =
            m_maintenance_state ==
            QLatin1String("waiting_for_maintenance_approval");
        const bool refill_waiting_for_start =
            shouldStartAndRestoreLiquidity() && !liquidity_ready;
        const bool refill_fee_limit_exceeded =
            maintenanceFeeLimitExceeded() && !liquidity_ready;
        setStatusLabel(
            m_overview_liquidity_status,
            liquidity_ready ? tr("Ready · target capacity is available")
                            : !m_liquidity_targets_satisfy_provider_policy
                                  ? tr("Action required · saved carrier targets cannot support the active offer")
                            : refill_approval_needed
                                  ? tr("Action required · targets are saved; approve the bounded refill cost")
                            : refill_fee_limit_exceeded
                                  ? tr("Action required · refill cost exceeds the approved limit")
                            : refill_waiting_for_start
                                  ? tr("Action required · refill approved; start the provider to restore liquidity")
                            : liquidity_waiting
                                  ? tr("Waiting · replacement liquidity is being prepared")
                                  : tr("Action required · liquidity is below its target"),
            liquidity_ready ? QStringLiteral("ready")
                            : !m_liquidity_targets_satisfy_provider_policy
                                  ? QStringLiteral("action")
                                  : refill_fee_limit_exceeded
                                        ? QStringLiteral("action")
                                  : liquidity_waiting
                                        ? QStringLiteral("waiting")
                                        : QStringLiteral("action"));
        if (m_overview_liquidity_action) {
            m_overview_liquidity_action->setText(
                liquidity_ready
                    ? tr("Review liquidity")
                    : !m_liquidity_targets_satisfy_provider_policy
                          ? tr("Review liquidity targets")
                    : refill_approval_needed
                          ? tr("Approve refill costs")
                    : refill_fee_limit_exceeded
                          ? tr("Review refill cost limit")
                    : refill_waiting_for_start
                          ? tr("Start and restore liquidity")
                          : tr("Restore liquidity"));
        }

        QString operation_kind{QStringLiteral("action")};
        QString operation_text;
        if (m_service_state == QLatin1String("active")) {
            operation_kind = QStringLiteral("ready");
            operation_text = tr("Ready · automatic provider operation is active");
        } else if (m_readiness_errors.contains(QStringLiteral(
                       "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ||
                   (!m_liquidity_targets_satisfy_provider_policy &&
                    (m_last_service_error == QLatin1String(
                         "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
                     m_last_service_error == QLatin1String(
                         "PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
            operation_text = tr(
                "Action required · save a payment-carrier target before starting");
        } else if (refill_fee_limit_exceeded) {
            operation_kind = QStringLiteral("action");
            operation_text = tr(
                "Paused · automatic refill exceeds the approved cost limit");
        } else if (m_service_state.startsWith(QLatin1String("waiting_")) ||
                   m_service_state == QLatin1String("replenishing_liquidity")) {
            operation_kind = QStringLiteral("waiting");
            operation_text = providerServiceStatusText();
        } else if (m_service_state == QLatin1String("error")) {
            operation_kind = QStringLiteral("error");
            operation_text = tr("Error · provider operation requires attention");
        } else if (providerConfigurationComplete() &&
                   m_readiness_errors.contains(
                       QStringLiteral("PAYMASTER_NODE_NOT_READY"))) {
            operation_kind = QStringLiteral("waiting");
            operation_text = Params().GetChainType() == ChainType::REGTEST
                ? tr("Waiting · mine a fresh Regtest block; provider settings are already saved")
                : tr("Waiting · node is not ready to broadcast transactions");
        } else if (refill_approval_needed) {
            operation_kind = QStringLiteral("action");
            operation_text = tr(
                "Action required · approve bounded refill costs; targets are already saved");
        } else if (refill_waiting_for_start) {
            operation_kind = QStringLiteral("action");
            operation_text = tr(
                "Action required · start the provider to restore missing liquidity");
        } else if (m_core_ready) {
            operation_text = tr("Action required · provider is ready to start");
        } else {
            operation_text = tr("Action required · complete the remaining setup steps");
        }
        setStatusLabel(m_overview_operation_status, operation_text, operation_kind);
        if (m_overview_operation_action) {
            if (!m_liquidity_targets_satisfy_provider_policy) {
                m_overview_operation_action->setText(
                    tr("Review liquidity targets"));
            } else if (m_service_state == QLatin1String("active")) {
                m_overview_operation_action->setText(tr("Open operations"));
            } else if (providerConfigurationComplete() &&
                       hasPassiveExternalWait()) {
                m_overview_operation_action->setText(tr("Refresh status"));
            } else if (refill_approval_needed) {
                m_overview_operation_action->setText(
                    tr("Approve refill costs"));
            } else if (refill_fee_limit_exceeded) {
                m_overview_operation_action->setText(
                    tr("Review refill cost limit"));
            } else if (refill_waiting_for_start) {
                m_overview_operation_action->setText(
                    tr("Start and restore liquidity"));
            } else {
                m_overview_operation_action->setText(tr("Review operation"));
            }
        }

        setStatusLabel(
            m_liquidity_dgb_capacity_status,
            (ready_dgb >= target_dgb
                 ? tr("Ready — enough DGB is prepared.\nClient capacity checks: %1 ready (target %2)\nSimultaneous payments: %3 ready (target %4)")
                 : tr("Action required — more DGB capacity is needed.\nClient capacity checks: %1 ready (target %2)\nSimultaneous payments: %3 ready (target %4)"))
                .arg(m_pool_admission_dgb)
                .arg(m_admission_dgb->value())
                .arg(m_pool_operational_dgb)
                .arg(m_operational_dgb->value()),
            ready_dgb >= target_dgb ? QStringLiteral("ready")
                                    : QStringLiteral("action"));
        const bool carrier_capacity_ready =
            !m_user_paid->isChecked() ||
            (m_liquidity_targets_satisfy_provider_policy &&
             ready_carriers >= target_carriers);
        QString carrier_capacity_text;
        if (!m_user_paid->isChecked()) {
            carrier_capacity_text = tr(
                "Not required. This provider currently offers sponsored transfers without a DigiDollar service fee.");
        } else if (!m_liquidity_targets_satisfy_provider_policy) {
            carrier_capacity_text = tr(
                "Action required — the saved user-paid targets omit payment-carrier capacity. Save at least three carrier targets for capacity checks and one carrier target for payments before starting.");
        } else {
            carrier_capacity_text =
                (ready_carriers >= target_carriers
                     ? tr("Ready — DigiDollar service fees can be received.\nClient capacity checks: %1 ready (target %2)\nSimultaneous payments: %3 ready (target %4)")
                     : tr("Action required — more DigiDollar fee capacity is needed.\nClient capacity checks: %1 ready (target %2)\nSimultaneous payments: %3 ready (target %4)"))
                    .arg(m_pool_admission_carriers)
                    .arg(m_admission_carriers->value())
                    .arg(m_pool_operational_carriers)
                    .arg(m_operational_carriers->value());
        }
        setStatusLabel(m_liquidity_carrier_capacity_status,
                       carrier_capacity_text,
                       carrier_capacity_ready ? QStringLiteral("ready")
                                              : QStringLiteral("action"));
    }

    void enableProvider()
    {
        if (!providerSafetyAllowsSelectedModels()) {
            m_enable_status->setText(tr(
                "Provider configuration was not enabled: save a complete finite safety policy first."));
            QMessageBox::warning(
                this, tr("Paymaster safety policy required"),
                tr("The provider cannot be enabled until a complete wallet-local safety policy "
                   "has been saved. Public sponsorship requires explicit finite non-zero "
                   "hourly and daily budgets."));
            return;
        }
        if (m_busy) {
            m_enable_status->setText(tr(
                "Provider configuration was not enabled because another wallet operation is still running. Please try again."));
            QMessageBox::information(
                this, tr("Paymaster operation in progress"),
                tr("Wait for the current wallet operation to finish, then enable the provider configuration again."));
            return;
        }
        m_enable_status->setText(tr("Enabling provider configuration…"));
        m_enable->setEnabled(false);
        UniValue params{UniValue::VARR};
        params.push_back(true);
        call("setpaymasterenabled", std::move(params), false, nullptr,
             [this](const UniValue& result) {
                 const UniValue& enabled = result.find_value("enabled");
                 if (!enabled.isBool() || !enabled.get_bool()) {
                     m_enable_status->setText(tr(
                         "Provider configuration was not enabled: the wallet returned an unexpected result."));
                     QMessageBox::warning(
                         this, tr("Provider configuration not enabled"),
                         tr("The wallet did not confirm that provider configuration was enabled."));
                     updateProviderButtons();
                     return;
                 }
                 m_core_enabled = true;
                 m_enable_status->setText(tr(
                     "Provider configuration enabled successfully. It is saved in this wallet. "
                     "The provider remains offline until you start it from Overview."));
                 updateProviderButtons();
                 QMessageBox::information(
                     this, tr("Provider configuration enabled"),
                     tr("Provider configuration was enabled successfully and saved in this wallet.\n\n"
                        "This does not start the provider yet. Start it from Overview after all readiness checks pass."));
                 refreshStatus();
             }, false,
             [this](const QString& error) {
                 m_enable_status->setText(tr(
                     "Provider configuration could not be enabled: %1").arg(error));
                 updateProviderButtons();
                 QMessageBox::warning(
                     this, tr("Provider configuration not enabled"),
                     tr("The provider configuration could not be enabled.\n\n%1").arg(error));
             });
    }

    void saveProviderSafetyPolicy()
    {
        FundingSafetyValues user_paid;
        FundingSafetyValues public_sponsored;
        FundingSafetyValues restricted_sponsored;
        if (!readFundingSafety(m_user_paid_safety, user_paid) ||
            !readFundingSafety(m_public_sponsored_safety, public_sponsored) ||
            !readFundingSafety(m_restricted_sponsored_safety,
                               restricted_sponsored)) {
            QMessageBox::warning(
                this, tr("Paymaster provider safety policy"),
                tr("One or more amounts are outside the supported whole-satoshi range."));
            return;
        }
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("user_paid", fundingSafetyToJSON(user_paid));
        policy.pushKV("public_sponsored", fundingSafetyToJSON(public_sponsored));
        policy.pushKV("restricted_sponsored", fundingSafetyToJSON(restricted_sponsored));
        policy.pushKV("maximum_active_quotes_total", m_max_active_quotes_total->value());
        policy.pushKV("maximum_active_quotes_per_netgroup", m_max_active_quotes_per_netgroup->value());
        policy.pushKV("maximum_active_quotes_per_recipient", m_max_active_quotes_per_recipient->value());
        policy.pushKV("maximum_quote_requests_per_netgroup_per_minute",
                      m_max_quote_requests_per_netgroup->value());
        UniValue params{UniValue::VARR};
        params.push_back(std::move(policy));
        call("setpaymastersafetypolicy", std::move(params), false, nullptr,
             [this](const UniValue&) {
                 m_provider_safety_dirty = false;
                 refreshProviderSafetyStatus();
             });
    }

    void saveClientSafetyPolicy()
    {
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("maximum_service_fee_per_transaction_cents",
                      m_client_fee_per_transaction->value());
        policy.pushKV("maximum_service_fee_per_day_cents",
                      m_client_fee_per_day->value());
        UniValue params{UniValue::VARR};
        params.push_back(std::move(policy));
        call("setpaymasterclientsafetypolicy", std::move(params), false, nullptr,
             [this](const UniValue&) {
                 m_client_safety_dirty = false;
                 refreshClientSafetyStatus();
             });
    }

    QString safetyClassStatus(const QString& name, const UniValue& value) const
    {
        if (!value.isObject()) return tr("%1: unavailable").arg(name);
        QString status = tr("%1: %2 active, %3 sat reserved, %4 sat spent/hour, %5 sat spent/day, "
                            "%6 completed/hour, %7 completed/day, accepting: %8")
            .arg(name)
            .arg(value.find_value("active_quotes").getInt<qint64>())
            .arg(value.find_value("reserved_network_fee_satoshis").getInt<qint64>())
            .arg(value.find_value("spent_network_fee_last_hour_satoshis").getInt<qint64>())
            .arg(value.find_value("spent_network_fee_last_day_satoshis").getInt<qint64>())
            .arg(value.find_value("completed_last_hour").getInt<qint64>())
            .arg(value.find_value("completed_last_day").getInt<qint64>())
            .arg(yesNo(value.find_value("can_accept_minimum_quote").get_bool()));
        QStringList errors;
        const UniValue& error_values = value.find_value("errors");
        if (error_values.isArray()) {
            for (const UniValue& error : error_values.getValues()) {
                if (error.isStr()) errors.push_back(QString::fromStdString(error.get_str()));
            }
        }
        if (!errors.isEmpty()) status += tr(" · limits: %1").arg(errors.join(tr(", ")));
        return status;
    }

    void refreshProviderSafetyStatus()
    {
        if (!m_provider_safety_dirty) {
            m_provider_safety_configured = false;
            m_provider_safety_status->setText(
                tr("Provider safety policy: unavailable (provider not ready)"));
        }
        if (!m_client_safety_dirty) {
            m_client_safety_status->setText(
                tr("Client safety policy: unavailable (automatic Paymaster transfers unavailable)"));
        }
        updateProviderButtons();
        call("getpaymastersafetystatus", {}, false, nullptr,
             [this](const UniValue& result) {
                 m_provider_safety_configured =
                     result.find_value("configured").isBool() &&
                     result.find_value("configured").get_bool();
                 const UniValue& policy = result.find_value("policy");
                 if (policy.isObject() && !m_provider_safety_dirty) {
                     m_loading_provider_safety = true;
                     loadFundingSafety(policy.find_value("user_paid"), m_user_paid_safety);
                     loadFundingSafety(policy.find_value("public_sponsored"), m_public_sponsored_safety);
                     loadFundingSafety(policy.find_value("restricted_sponsored"), m_restricted_sponsored_safety);
                     m_max_active_quotes_total->setValue(
                         policy.find_value("maximum_active_quotes_total").getInt<int>());
                     m_max_active_quotes_per_netgroup->setValue(
                         policy.find_value("maximum_active_quotes_per_netgroup").getInt<int>());
                     m_max_active_quotes_per_recipient->setValue(
                         policy.find_value("maximum_active_quotes_per_recipient").getInt<int>());
                     m_max_quote_requests_per_netgroup->setValue(
                         policy.find_value("maximum_quote_requests_per_netgroup_per_minute").getInt<int>());
                     m_loading_provider_safety = false;
                     m_provider_safety_dirty = false;
                     updateFundingSafetyDisplay(m_user_paid_safety,
                                                m_provider_safety_configured);
                     updateFundingSafetyDisplay(m_public_sponsored_safety,
                                                m_provider_safety_configured);
                     updateFundingSafetyDisplay(m_restricted_sponsored_safety,
                                                m_provider_safety_configured);
                 }
                 if (!m_provider_safety_dirty) {
                     m_provider_safety_status->setText(m_provider_safety_configured
                         ? tr("Provider safety policy: configured and persisted")
                         : tr("Provider safety policy: not configured (provider not ready)"));
                 }
                 QStringList usage;
                 usage.push_back(safetyClassStatus(tr("User paid"), result.find_value("user_paid")));
                 usage.push_back(safetyClassStatus(tr("Public sponsored"), result.find_value("public_sponsored")));
                 usage.push_back(safetyClassStatus(tr("Restricted sponsored"), result.find_value("restricted_sponsored")));
                 m_provider_safety_usage->setText(usage.join('\n'));
                 updateProviderButtons();
                 refreshClientSafetyStatus();
             }, false);
    }

    void refreshClientSafetyStatus()
    {
        if (!m_client_safety_dirty) {
            m_client_safety_status->setText(
                tr("Client safety policy: unavailable (automatic Paymaster transfers unavailable)"));
        }
        call("getpaymasterclientsafetystatus", {}, false, nullptr,
             [this](const UniValue& result) {
                 const bool configured = result.find_value("configured").isBool() &&
                                         result.find_value("configured").get_bool();
                 m_client_safety_configured = configured;
                 const UniValue& policy = result.find_value("policy");
                 if (policy.isObject() && !m_client_safety_dirty) {
                     m_loading_client_safety = true;
                     m_client_fee_per_transaction->setValue(
                         policy.find_value("maximum_service_fee_per_transaction_cents").getInt<int>());
                     m_client_fee_per_day->setValue(
                         policy.find_value("maximum_service_fee_per_day_cents").getInt<int>());
                     m_loading_client_safety = false;
                     m_client_safety_dirty = false;
                     updateClientSafetyDisplay(configured);
                 }
                 if (m_client_safety_dirty) return;
                 if (!m_client_safety_configured) {
                     m_client_safety_status->setText(
                         tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"));
                     return;
                 }
                  m_client_safety_status->setText(
                      tr("Client safety policy: configured · active reservations %1 · "
                         "reserved %2 cents · spent today %3 cents · available today %4 cents")
                          .arg(result.find_value("active_reservations").getInt<qint64>())
                          .arg(result.find_value("reserved_service_fee_cents").getInt<qint64>())
                          .arg(result.find_value("spent_service_fee_last_day_cents").getInt<qint64>())
                          .arg(result.find_value("available_service_fee_today_cents").getInt<qint64>()));
             }, false);
    }

    static QSpinBox* spin(QWidget* parent, int minimum, int maximum, int value)
    {
        auto* result = new NoWheelSpinBox(parent);
        result->setRange(minimum, maximum);
        result->setValue(value);
        return result;
    }

    static QLineEdit* dgbAmountField(QWidget* parent, qint64 satoshis)
    {
        return new DgbAmountLineEdit(satoshis, parent);
    }

    static QSpinBox* scaledSpin(QWidget* parent, int minimum, int maximum,
                                int value, double scale, int decimals)
    {
        auto* result = new NoWheelScaledSpinBox(scale, decimals, parent);
        result->setRange(minimum, maximum);
        result->setValue(value);
        return result;
    }

    QString yesNo(bool value) const { return value ? tr("yes") : tr("no"); }

    QString liquidityTargetKey() const
    {
        return QStringLiteral("%1:%2:%3:%4")
            .arg(m_admission_dgb->value())
            .arg(m_operational_dgb->value())
            .arg(m_admission_carriers->value())
            .arg(m_operational_carriers->value());
    }

    void restoreLiquidityDefaults(bool announce = true)
    {
        const bool needs_carriers = m_user_paid->isChecked();
        m_loading_liquidity_policy = true;
        m_automatic_replenishment->setChecked(true);
        m_paid_maintenance_approved->setChecked(false);
        m_admission_dgb->setValue(3);
        m_operational_dgb->setValue(1);
        m_admission_carriers->setValue(needs_carriers ? 3 : 0);
        m_operational_carriers->setValue(needs_carriers ? 1 : 0);
        m_maintenance_fee_per_transaction->setText(
            QString::number(10000000));
        m_maintenance_fee_per_hour->setText(QString::number(50000000));
        m_maintenance_fee_per_day->setText(QString::number(200000000));
        m_loading_liquidity_policy = false;
        m_liquidity_policy_dirty = announce;
        m_prepare_preview_target.clear();
        m_rebalance_preview_target.clear();
        invalidateCarrierWithdrawalPreviews();
        updateLiquidityDisplay();
        if (announce) {
            m_liquidity_policy_status->setText(tr(
                "Recommended targets and conservative finite maintenance limits restored in the form. Paid maintenance is disabled until you review, approve and save these values."));
            m_liquidity_output->setPlainText(tr(
                "Recommended target fields restored. No wallet output was created or retired. Save the automatic liquidity policy, then preview pool preparation only if you are using manual expert operation."));
        } else {
            m_liquidity_output->clear();
        }
        updateProviderButtons();
    }

    void updateLiquidityDisplay()
    {
        if (!m_liquidity_summary || !m_liquidity_preview_status) return;
        const bool needs_carriers = m_user_paid->isChecked();
        const bool sponsored = m_sponsored->isChecked();
        QString summary = tr(
            "Displayed targets: %1 admission DGB, %2 operational DGB, %3 admission carrier and %4 operational carrier slots.")
            .arg(m_admission_dgb->value())
            .arg(m_operational_dgb->value())
            .arg(m_admission_carriers->value())
            .arg(m_operational_carriers->value());
        if (!needs_carriers && !sponsored) {
            summary += tr(
                " No payment model is selected; choose and save an operating policy before preparing liquidity.");
        } else if (needs_carriers &&
            (m_admission_carriers->value() < 3 ||
             m_operational_carriers->value() < 1)) {
            summary += tr(
                " User paid is selected, so at least three admission and one operational carrier slots are required.");
        } else if (!needs_carriers &&
                   (m_admission_carriers->value() > 0 ||
                    m_operational_carriers->value() > 0)) {
            summary += tr(
                " User paid is not selected; these carrier targets are optional and can be reset to zero.");
        } else if (needs_carriers) {
            summary += tr(" These targets satisfy the minimum user-paid pool shape.");
        } else {
            summary += tr(" These targets satisfy the minimum sponsored-only pool shape.");
        }
        m_liquidity_summary->setText(summary);

        if (m_liquidity_target_save_status) {
            if (m_liquidity_policy_dirty) {
                setStatusLabel(
                    m_liquidity_target_save_status,
                    tr("Not saved — Core is still using the previous wallet targets. Save these values before attempting to refill or start the provider."),
                    QStringLiteral("action"));
            } else if (m_liquidity_policy_configured) {
                setStatusLabel(
                    m_liquidity_target_save_status,
                    tr("Saved in this provider wallet."),
                    QStringLiteral("ready"));
            } else {
                setStatusLabel(
                    m_liquidity_target_save_status,
                    tr("Not saved yet — review these suggested targets before continuing."),
                    QStringLiteral("action"));
            }
        }
        if (m_save_liquidity_policy && m_save_liquidity_policy_primary) {
            const bool needs_approval =
                m_paid_maintenance_approved->isChecked();
            const bool can_save = hasRpcTransport() && !m_busy &&
                (m_liquidity_policy_dirty ||
                 !m_liquidity_policy_configured);
            const QString primary_text = needs_approval
                ? tr("Save settings and approve bounded refill…")
                : tr("Save liquidity settings");
            const QString target_text = needs_approval
                ? tr("Save targets and approve refill…")
                : tr("Save liquidity targets");
            m_save_liquidity_policy_primary->setText(primary_text);
            m_save_liquidity_policy_primary->setEnabled(can_save);
            m_save_liquidity_policy->setText(target_text);
            m_save_liquidity_policy->setEnabled(can_save);
        }

        const QString target = liquidityTargetKey();
        const bool preparation_current = m_prepare_preview_target == target;
        const bool retirement_current = m_rebalance_preview_target == target;
        if (m_prepare_execute) m_prepare_execute->setEnabled(preparation_current);
        if (m_rebalance_execute) m_rebalance_execute->setEnabled(retirement_current);
        if (preparation_current || retirement_current) {
            m_liquidity_preview_status->setText(preparation_current
                ? tr("A preparation preview exists for the current targets.")
                : tr("A retirement preview exists for the current targets."));
        } else {
            m_liquidity_preview_status->setText(tr(
                "No reviewed preview matches the current targets. Preview before executing."));
        }
    }

    static qint64 poolNumber(const UniValue& result, const char* name)
    {
        const UniValue& value = result.find_value(name);
        return value.isNum() ? value.getInt<qint64>() : 0;
    }

    QString formatPoolResult(const char* command, const UniValue& result) const
    {
        const bool executed = result.find_value("executed").isBool() &&
                              result.find_value("executed").get_bool();
        QStringList lines;
        if (std::string{command} == "preparepaymasterpool") {
            lines.push_back(executed
                ? tr("Preparation executed: the required pool transaction(s) were created. Wait for confirmation before expecting provider readiness.")
                : tr("Preparation preview only: no wallet funds were moved."));
            lines.push_back(tr(
                "Missing outputs to create — admission DGB: %1; operational DGB: %2; admission carriers: %3; operational carriers: %4.")
                .arg(poolNumber(result, "missing_admission_dgb_slots"))
                .arg(poolNumber(result, "missing_operational_dgb_slots"))
                .arg(poolNumber(result, "missing_admission_carrier_slots"))
                .arg(poolNumber(result, "missing_operational_carrier_slots")));
            lines.push_back(tr(
                "Value per output — admission DGB: %1 DGB; operational DGB: %2 DGB; DD carrier: %3 DD.")
                .arg(QString::number(poolNumber(result, "admission_dgb_satoshis_each") / 100000000.0, 'f', 8),
                     QString::number(poolNumber(result, "operational_dgb_satoshis_each") / 100000000.0, 'f', 8),
                     QString::number(poolNumber(result, "carrier_cents_each") / 100.0, 'f', 2)));
            lines.push_back(tr("New pool total — %1 DGB and %2 DD.")
                .arg(QString::number(poolNumber(result, "total_output_satoshis") / 100000000.0, 'f', 8),
                     QString::number(poolNumber(result, "total_carrier_cents") / 100.0, 'f', 2)));
        } else {
            lines.push_back(executed
                ? tr("Retirement executed: selected excess available liquidity is being returned to the wallet.")
                : tr("Retirement preview only: no wallet funds were moved."));
            lines.push_back(tr(
                "Outputs selected for retirement — admission DGB: %1; operational DGB: %2; admission carriers: %3; operational carriers: %4.")
                .arg(poolNumber(result, "retired_admission_dgb_slots"))
                .arg(poolNumber(result, "retired_operational_dgb_slots"))
                .arg(poolNumber(result, "retired_admission_carrier_slots"))
                .arg(poolNumber(result, "retired_operational_carrier_slots")));
            lines.push_back(tr("Returned before network fee — %1 DGB and %2 DD.")
                .arg(QString::number(poolNumber(result, "retired_dgb_satoshis") / 100000000.0, 'f', 8),
                     QString::number(poolNumber(result, "retired_carrier_cents") / 100.0, 'f', 2)));
        }
        if (result.find_value("network_fee_satoshis").isNum()) {
            lines.push_back(tr("DGB network fee paid: %1 DGB.")
                .arg(QString::number(poolNumber(result, "network_fee_satoshis") / 100000000.0, 'f', 8)));
        }
        const UniValue& dgb_txid = result.find_value("dgb_txid");
        const UniValue& dd_txid = result.find_value("dd_txid");
        if (dgb_txid.isStr()) lines.push_back(tr("DGB transaction: %1").arg(QString::fromStdString(dgb_txid.get_str())));
        if (dd_txid.isStr()) lines.push_back(tr("DD transaction: %1").arg(QString::fromStdString(dd_txid.get_str())));
        if (!executed) {
            lines.push_back(tr(
                "Review these values. If they are acceptable, use the matching Execute reviewed action without changing the targets."));
        }
        return lines.join('\n');
    }

    UniValue poolOptions(bool execute) const
    {
        UniValue options{UniValue::VOBJ};
        options.pushKV("admission_dgb_slots", m_admission_dgb->value());
        options.pushKV("operational_dgb_slots", m_operational_dgb->value());
        options.pushKV("admission_carrier_slots", m_admission_carriers->value());
        options.pushKV("operational_carrier_slots", m_operational_carriers->value());
        options.pushKV("execute", execute);
        UniValue params{UniValue::VARR};
        params.push_back(std::move(options));
        return params;
    }

    void poolAction(const char* command, bool execute)
    {
        const bool preparation = std::string{command} == "preparepaymasterpool";
        const QString target = liquidityTargetKey();
        const QString reviewed_target = preparation ? m_prepare_preview_target
                                                    : m_rebalance_preview_target;
        if (execute && reviewed_target != target) {
            QMessageBox::warning(
                this, tr("Current pool targets have not been reviewed"),
                tr("Preview this action with the currently displayed target values before executing it."));
            return;
        }
        if (execute && QMessageBox::question(
                this, tr("Confirm Paymaster pool change"),
                tr("Execute the exact pool change shown by the most recent preview?\n\n"
                   "This creates or retires wallet outputs and may pay a network fee.")) != QMessageBox::Yes) {
            return;
        }
        call(command, poolOptions(execute), execute, nullptr,
             [this, command = std::string{command}, execute, preparation, target](
                 const UniValue& result) {
                 m_liquidity_output->setPlainText(
                     formatPoolResult(command.c_str(), result));
                 if (preparation) {
                     m_prepare_preview_target = execute ? QString{} : target;
                 } else {
                     m_rebalance_preview_target = execute ? QString{} : target;
                 }
                 updateLiquidityDisplay();
                 if (execute) refreshStatus();
             });
    }

    static QString activityText(const UniValue& object, const char* name)
    {
        const UniValue& value = object.find_value(name);
        return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
    }

    QString reservationPurpose(const UniValue& entry) const
    {
        const QString purpose = activityText(entry, "purpose");
        if (purpose == QLatin1String("admission")) return tr("admission reserve");
        if (purpose == QLatin1String("operational")) return tr("operational payment slot");
        return purpose.isEmpty() ? tr("unknown purpose") : purpose;
    }

    QString reservationAsset(const UniValue& entry) const
    {
        const QString asset = activityText(entry, "asset");
        if (asset == QLatin1String("dgb")) {
            return tr("%1 DGB").arg(QString::number(
                poolNumber(entry, "dgb_satoshis") / 100000000.0, 'f', 8));
        }
        if (asset == QLatin1String("dd_carrier")) {
            return tr("%1 DD carrier").arg(QString::number(
                poolNumber(entry, "dd_cents") / 100.0, 'f', 2));
        }
        return asset.isEmpty() ? tr("unknown asset") : asset;
    }

    QString reservationState(const UniValue& entry) const
    {
        const QString state = activityText(entry, "state");
        if (state == QLatin1String("reserved")) return tr("reserved for in-progress work");
        if (state == QLatin1String("pending_successor")) return tr("waiting for a replacement output");
        if (state == QLatin1String("committed")) return tr("committed to an exact payment");
        if (state == QLatin1String("spent")) return tr("spent and retained as durable history");
        return state.isEmpty() ? tr("unknown") : state;
    }

    void showReservations(const UniValue& result)
    {
        m_activity_output->setPlainText(QString::fromStdString(result.write(2)));
        if (!result.isArray()) {
            if (m_reservations_group) m_reservations_group->setVisible(true);
            if (m_recovery_group) m_recovery_group->setVisible(true);
            m_activity_summary->setText(tr(
                "The wallet returned an unexpected reservation result. See the technical result."));
            return;
        }
        const auto& entries = result.getValues();
        if (entries.empty()) {
            if (m_reservations_group) m_reservations_group->setVisible(false);
            if (m_recovery_group) m_recovery_group->setVisible(false);
            m_activity_summary->setText(tr(
                "Healthy · no reservations or recovery actions require attention."));
            return;
        }
        if (m_reservations_group) m_reservations_group->setVisible(true);
        if (m_recovery_group) m_recovery_group->setVisible(true);
        QStringList lines;
        lines.push_back(tr(
            "%1 unavailable or durable pool record(s). Core controls when each output can safely return to the available pool.")
                            .arg(static_cast<qulonglong>(entries.size())));
        const size_t visible_entries = std::min<size_t>(entries.size(), 8);
        for (size_t i = 0; i < visible_entries; ++i) {
            const UniValue& entry = entries[i];
            lines.push_back(tr("• %1 — %2 — state: %3")
                                .arg(reservationPurpose(entry),
                                     reservationAsset(entry),
                                     reservationState(entry)));
        }
        if (entries.size() > visible_entries) {
            lines.push_back(tr(
                "%1 more reservation(s) are listed in the technical result.")
                                .arg(static_cast<qulonglong>(entries.size() - visible_entries)));
        }
        m_activity_summary->setText(lines.join('\n'));
    }

    QString formatActivityResult(const UniValue& result, bool submitted_payment) const
    {
        if (!result.isObject()) {
            return tr("Core returned an unexpected processing result. See the technical result.");
        }
        const UniValue& processed_value = result.find_value("processed");
        const bool processed = processed_value.isBool() && processed_value.get_bool();
        if (!processed) {
            return submitted_payment
                ? tr("No submitted payment is waiting in this provider wallet.")
                : tr("No capacity, quote, or recovery request is waiting in this provider wallet.");
        }

        const QString type = activityText(result, "message_type");
        QString type_description;
        if (type == QLatin1String("capacity")) {
            type_description = tr("capacity check");
        } else if (type == QLatin1String("quote")) {
            type_description = tr("quote request");
        } else if (type == QLatin1String("recovery_request")) {
            type_description = tr("recovery-provider request");
        } else if (type == QLatin1String("recovery_submit")) {
            type_description = tr("recovery submission");
        } else if (type == QLatin1String("submit")) {
            type_description = tr("submitted payment");
        } else {
            type_description = type.isEmpty() ? tr("provider message") : type;
        }

        const UniValue& queued_value = result.find_value("queued");
        const bool queued = queued_value.isBool() && queued_value.get_bool();
        QStringList lines;
        lines.push_back(tr("Processed one %1.").arg(type_description));
        lines.push_back(queued
            ? tr("The signed provider response was queued for delivery to the client.")
            : tr("The provider response was not queued. Check the technical result before retrying."));
        const QString result_status = activityText(result, "result_status");
        const QString attempt_state = activityText(result, "attempt_state");
        const QString rejection_code = activityText(result, "rejection_code");
        const QString txid = activityText(result, "txid");
        if (!result_status.isEmpty()) {
            lines.push_back(tr("Result status: %1").arg(result_status));
        }
        if (!attempt_state.isEmpty()) {
            lines.push_back(tr("Payment state: %1").arg(attempt_state));
        }
        if (!rejection_code.isEmpty()) {
            lines.push_back(tr("Rejected safely: %1").arg(rejection_code));
        }
        if (!txid.isEmpty()) {
            lines.push_back(tr("Transaction: %1").arg(txid));
        }
        if (!submitted_payment) {
            lines.push_back(tr(
                "If more clients are waiting, process another request. A quote may now hold a reservation."));
        }
        return lines.join('\n');
    }

    void refreshActivity()
    {
        call("listpaymasterreservations", {}, false, m_activity_output,
             [this](const UniValue& result) {
                 showReservations(result);
                 refreshStatus();
             });
    }

    void processActivityRequest()
    {
        if (!m_core_running || m_core_locked) return;
        call("processpaymasterrequests", {}, true, m_activity_output,
             [this](const UniValue& result) {
                 m_activity_action_result->setText(
                     formatActivityResult(result, /*submitted_payment=*/false));
                 refreshStatus();
             });
    }

    void processActivitySubmit()
    {
        if (!m_core_running || m_core_locked) return;
        if (QMessageBox::question(
                this, tr("Process one submitted Paymaster payment?"),
                tr("Core will validate one waiting client-signed submission. If it exactly "
                   "matches the durable authorization, this action may sign provider inputs, "
                   "spend reserved DGB within your safety policy, and broadcast the transaction.\n\n"
                   "Continue?")) != QMessageBox::Yes) {
            return;
        }
        call("processpaymastersubmits", {}, true, m_activity_output,
             [this](const UniValue& result) {
                 m_activity_action_result->setText(
                     formatActivityResult(result, /*submitted_payment=*/true));
                 refreshStatus();
             });
    }

    void savePolicy()
    {
        if (m_fee_bps->value() % 10 != 0 || m_min_amount->value() > m_max_amount->value()) {
            QMessageBox::warning(this, tr("Paymaster policy"),
                tr("The fee must be a multiple of 10 basis points and the minimum cannot exceed the maximum."));
            return;
        }
        UniValue funding{UniValue::VARR};
        if (m_sponsored->isChecked()) funding.push_back("sponsored");
        if (m_user_paid->isChecked()) funding.push_back("user_paid");
        if (funding.empty()) {
            QMessageBox::warning(this, tr("Paymaster policy"), tr("Select at least one funding model."));
            return;
        }
        UniValue policy{UniValue::VOBJ};
        policy.pushKV("funding_models", std::move(funding));
        policy.pushKV("sponsorship_scope", m_scope->currentData().toString().toStdString());
        policy.pushKV("fee_rate_bps", m_fee_bps->value());
        policy.pushKV("min_amount_cents", m_min_amount->value());
        policy.pushKV("max_amount_cents", m_max_amount->value());
        policy.pushKV("quote_ttl", m_quote_ttl->value());
        policy.pushKV("maximum_network_fee_dgb_satoshis", m_network_fee->value());
        UniValue params{UniValue::VARR};
        params.push_back(std::move(policy));
        call("setpaymasterpolicy", std::move(params), false, nullptr,
             [this](const UniValue&) {
                 m_policy_loaded = true;
                 m_policy_dirty = false;
                 updatePolicyDisplay();
                 refreshStatus();
             });
    }

    bool showSetupWizard()
    {
        const QPointer<WalletModel> setup_model{m_model};
        const QString setup_wallet_name = setup_model
            ? setup_model->getDisplayName()
            : tr("No wallet selected");
        const QString setup_wallet_id = setup_model
            ? setup_model->getWalletName()
            : QString{};

        PaymasterSetupWizard wizard(this);
        wizard.setObjectName("PaymasterSetupWizard");
        wizard.setWindowTitle(tr("Guided Paymaster provider setup"));
        wizard.setWizardStyle(QWizard::ClassicStyle);
        wizard.setOption(QWizard::NoBackButtonOnStartPage);
        wizard.setOption(QWizard::HaveHelpButton, false);
        wizard.setSizeGripEnabled(true);
        wizard.setMinimumSize(560, 440);
        wizard.resize(760, 600);
        wizard.setStyleSheet(PaymasterWizardStyleSheet(
            UseDarkPaymasterWizardTheme(m_model, this)));

        auto* intro = new CompletableWizardPage(&wizard);
        intro->setObjectName("paymasterSetupRequirementsPage");
        intro->setTitle(tr("Choose the provider wallet"));
        intro->setSubTitle(tr("Confirm which wallet will own the provider identity, liquidity and safety settings."));
        auto* intro_page_layout = new QVBoxLayout(intro);
        auto* intro_scroll = new QScrollArea(intro);
        intro_scroll->setObjectName("paymasterSetupRequirementsScroll");
        intro_scroll->setWidgetResizable(true);
        intro_scroll->setFrameShape(QFrame::NoFrame);
        intro_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* intro_content = new QWidget(intro_scroll);
        intro_content->setObjectName("paymasterSetupRequirementsContent");
        auto* intro_layout = new QVBoxLayout(intro_content);
        // Word-wrapped labels otherwise report an overly small vertical size
        // hint while the hidden wizard page is being laid out. Preserve the
        // complete content height and let the page scroll on compact displays.
        intro_layout->setSizeConstraint(QLayout::SetMinimumSize);
        intro_scroll->setWidget(intro_content);
        intro_page_layout->addWidget(intro_scroll);
        auto* intro_text = new QLabel(tr(
            "A Paymaster supplies DGB network fees for DigiDollar transfers. This assistant "
            "prepares a complete, conservative starting configuration and explains every step. "
            "Before anything is saved, the assistant shows the complete plan and previews the exact missing liquidity. "
            "After your confirmation it saves the identity, policy and safety limits and creates only missing pool outputs. "
            "It never starts the provider automatically."), intro);
        intro_text->setObjectName("paymasterSetupIntroduction");
        intro_text->setWordWrap(true);
        intro_layout->addWidget(intro_text);

        auto* wallet_frame = new QFrame(intro);
        wallet_frame->setObjectName("paymasterSetupWalletCard");
        wallet_frame->setFrameShape(QFrame::StyledPanel);
        auto* wallet_layout = new QVBoxLayout(wallet_frame);
        auto* wallet_name = new QLabel(
            tr("Provider wallet: %1").arg(setup_wallet_name), wallet_frame);
        wallet_name->setObjectName("paymasterSetupWalletName");
        wallet_name->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                             Qt::TextSelectableByMouse);
        wallet_name->setAccessibleName(tr("Selected Paymaster provider wallet"));
        wallet_name->setAccessibleDescription(tr(
            "The wallet whose identity, prepared liquidity, budgets and provider history will be used by this setup."));
        QFont wallet_name_font = wallet_name->font();
        wallet_name_font.setBold(true);
        wallet_name->setFont(wallet_name_font);
        wallet_layout->addWidget(wallet_name);

        auto* wallet_status = new QLabel(wallet_frame);
        wallet_status->setObjectName("paymasterSetupWalletStatus");
        wallet_status->setWordWrap(true);
        if (!setup_model) {
            wallet_status->setText(tr(
                "No wallet is selected. Close this assistant, create or open a suitable wallet and select it before continuing."));
        } else if (m_core_has_identity) {
            wallet_status->setText(tr(
                "This wallet already has a Paymaster provider identity. The assistant will retain it unchanged and update only this wallet's configuration."));
        } else if (m_core_eligible) {
            wallet_status->setText(tr(
                "Core currently reports this wallet as eligible. Eligibility is checked again immediately before any setting is written."));
        } else {
            wallet_status->setText(tr(
                "Eligibility has not yet been confirmed. Before changing anything, Core will require a descriptor wallet with local private keys and no external signer."));
        }
        wallet_layout->addWidget(wallet_status);

        auto* wallet_recommendation = new QLabel(tr(
            "Recommended: use a dedicated provider wallet. This keeps provider identity, prepared DGB/DD outputs, safety budgets and transaction history separate from everyday personal funds. A dedicated wallet is recommended for operations and accounting, but is not a protocol requirement."),
            wallet_frame);
        wallet_recommendation->setObjectName("paymasterSetupWalletRecommendation");
        wallet_recommendation->setWordWrap(true);
        wallet_layout->addWidget(wallet_recommendation);

        auto* wallet_consequence = new QLabel(tr(
            "Continuing does not create another wallet. The assistant will create or reuse the provider identity and prepare the selected liquidity inside the wallet named above. Paymaster carrier DD remains wallet-owned but is shown as reserved until it is safely retired."),
            wallet_frame);
        wallet_consequence->setObjectName("paymasterSetupWalletConsequence");
        wallet_consequence->setWordWrap(true);
        wallet_layout->addWidget(wallet_consequence);

        auto* wallet_confirmation = new QCheckBox(
            tr("Use \"%1\" as the provider wallet").arg(setup_wallet_name),
            wallet_frame);
        wallet_confirmation->setObjectName("paymasterSetupWalletConfirmation");
        wallet_confirmation->setAccessibleName(tr("Confirm Paymaster provider wallet"));
        wallet_confirmation->setAccessibleDescription(tr(
            "Required confirmation that the assistant may configure only the named wallet."));
        wallet_confirmation->setEnabled(setup_model != nullptr);
        wallet_layout->addWidget(wallet_confirmation);

        auto* choose_another_wallet = new QPushButton(
            tr("Close and choose or create another wallet…"), wallet_frame);
        choose_another_wallet->setObjectName("paymasterSetupChooseAnotherWallet");
        choose_another_wallet->setToolTip(tr(
            "Use DigiByte Core's normal wallet menu and wallet selector, then reopen Paymaster Network in that wallet."));
        wallet_layout->addWidget(choose_another_wallet, 0, Qt::AlignLeft);
        intro_layout->addWidget(wallet_frame);

        auto* wallet_scope_note = new QLabel(tr(
            "This assistant cannot switch wallets and will not create a wallet or move balances. To use another wallet, close the assistant, use the normal wallet menu to create or open it, select that wallet, and reopen Paymaster Network."), intro);
        wallet_scope_note->setObjectName("paymasterSetupWalletScopeNote");
        wallet_scope_note->setWordWrap(true);
        intro_layout->addWidget(wallet_scope_note);

        connect(wallet_confirmation, &QCheckBox::toggled, intro,
                [intro, setup_model](bool checked) {
                    intro->setComplete(checked && setup_model);
                });
        connect(choose_another_wallet, &QPushButton::clicked, &wizard,
                [&wizard] { wizard.reject(); });
        intro->setComplete(false);

        auto* current_status = new QLabel(
            m_readiness_summary->text().isEmpty()
                ? tr("Current check: refresh the provider status after opening the detailed pages.")
                : tr("Current check:\n%1").arg(m_readiness_summary->text()), intro);
        current_status->setObjectName("paymasterSetupCurrentReadiness");
        current_status->setWordWrap(true);
        current_status->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                                Qt::TextSelectableByMouse);
        intro_layout->addWidget(current_status);
        auto* requirements = new QLabel(tr(
            "Required before the provider can run:\n\n"
            "• a descriptor wallet with local private keys\n"
            "• DigiDollar active, a synchronized node, txindex=1 and pruning disabled\n"
            "• BIP324 version 2 transport enabled\n"
            "• a reachable -paymasterendpoint configured before node startup\n"
            "• network message capture disabled for mainnet operation\n"
            "• confirmed DGB liquidity and, for user-paid service, confirmed DD carriers\n"
            "• a saved finite provider safety policy"), intro);
        requirements->setObjectName("paymasterSetupRequirements");
        requirements->setWordWrap(true);
        intro_layout->addWidget(requirements);
        intro_layout->addStretch();
        wizard.addPage(intro);

        auto* identity_page = new QWizardPage(&wizard);
        identity_page->setObjectName("paymasterSetupIdentityPage");
        identity_page->setTitle(tr("Identity"));
        identity_page->setSubTitle(tr("Choose the public name clients will see for this provider."));
        auto* identity_layout = new QFormLayout(identity_page);
        auto* display = new QLineEdit(m_display_name->text(), identity_page);
        display->setObjectName("paymasterSetupDisplayName");
        display->setMaxLength(32);
        display->setPlaceholderText(tr("Optional provider name"));
        display->setAccessibleName(tr("Public provider display name"));
        display->setAccessibleDescription(tr(
            "An optional informational name shown to clients. The signed provider identity remains authoritative."));
        identity_layout->addRow(tr("Public display name:"), display);
        auto* identity_note = new QLabel(tr(
            "The name is only a human-readable label. The assistant creates the persistent wallet-managed provider identity when the plan is applied. "
            "If this wallet already has an identity, it is retained unchanged. Clients authenticate its signed BIP86 key, not this display name."),
            identity_page);
        identity_note->setObjectName("paymasterSetupIdentityExplanation");
        identity_note->setWordWrap(true);
        identity_layout->addRow(identity_note);
        wizard.addPage(identity_page);

        auto* model_page = new QWizardPage(&wizard);
        model_page->setObjectName("paymasterSetupFundingPage");
        model_page->setTitle(tr("Choose the service model"));
        model_page->setSubTitle(tr("Decide who pays for the service. Both public models may be offered together."));
        auto* model_layout = new QVBoxLayout(model_page);
        auto* wizard_user_paid = new QCheckBox(tr("User paid — recommended for general service"), model_page);
        wizard_user_paid->setObjectName("paymasterSetupUserPaid");
        wizard_user_paid->setChecked(m_user_paid->isChecked());
        auto* user_paid_note = new QLabel(tr(
            "The client pays your configured service fee in DigiDollar. Your wallet still supplies the DGB network fee."), model_page);
        user_paid_note->setWordWrap(true);
        auto* wizard_sponsored = new QCheckBox(tr("Sponsored — no DigiDollar service fee"), model_page);
        wizard_sponsored->setObjectName("paymasterSetupSponsored");
        wizard_sponsored->setChecked(m_sponsored->isChecked());
        auto* sponsored_note = new QLabel(tr(
            "Your wallet pays the DGB fee without compensation. Public sponsorship can be consumed by unknown clients, so finite budgets are essential."), model_page);
        sponsored_note->setWordWrap(true);
        auto* wizard_scope = new NoWheelComboBox(model_page);
        wizard_scope->setObjectName("paymasterSetupSponsorshipScope");
        wizard_scope->addItem(tr("Public — discovered clients may request sponsorship"), QStringLiteral("public"));
        wizard_scope->addItem(tr("Restricted — invitation required"), QStringLiteral("restricted"));
        const int selected_scope = wizard_scope->findData(m_scope->currentData());
        if (selected_scope >= 0) wizard_scope->setCurrentIndex(selected_scope);
        wizard_scope->setEnabled(wizard_sponsored->isChecked());
        auto* scope_note = new QLabel(tr(
            "Restricted sponsorship cannot be combined with user-paid public service. The assistant will switch to sponsored-only when Restricted is selected."), model_page);
        scope_note->setWordWrap(true);
        model_layout->addWidget(wizard_user_paid);
        model_layout->addWidget(user_paid_note);
        model_layout->addSpacing(8);
        model_layout->addWidget(wizard_sponsored);
        model_layout->addWidget(sponsored_note);
        model_layout->addWidget(wizard_scope);
        model_layout->addWidget(scope_note);
        model_layout->addStretch();
        wizard.addPage(model_page);

        connect(wizard_sponsored, &QCheckBox::toggled, wizard_scope, &QWidget::setEnabled);
        connect(wizard_scope, qOverload<int>(&QComboBox::currentIndexChanged),
                model_page, [wizard_scope, wizard_sponsored, wizard_user_paid] {
                    if (wizard_scope->currentData().toString() == QLatin1String("restricted")) {
                        wizard_sponsored->setChecked(true);
                        wizard_user_paid->setChecked(false);
                    }
                });
        connect(wizard_user_paid, &QCheckBox::toggled, model_page,
                [wizard_user_paid, wizard_scope](bool checked) {
                    if (checked && wizard_scope->currentData().toString() == QLatin1String("restricted")) {
                        wizard_scope->setCurrentIndex(
                            wizard_scope->findData(QStringLiteral("public")));
                    }
                });

        auto* policy_page = new QWizardPage(&wizard);
        policy_page->setObjectName("paymasterSetupPolicyPage");
        policy_page->setTitle(tr("Offer limits"));
        policy_page->setSubTitle(tr("Set the payment range, quote lifetime and absolute fee ceiling."));
        auto* policy_layout = new QFormLayout(policy_page);
        auto* fee = new NoWheelDoubleSpinBox(policy_page);
        fee->setObjectName("paymasterSetupServiceFeePercent");
        fee->setRange(0.0, 100.0);
        fee->setDecimals(2);
        fee->setSingleStep(0.10);
        fee->setValue(m_fee_bps->value() / 100.0);
        fee->setSuffix(tr(" %"));
        fee->setAccessibleName(tr("User-paid service fee in percent"));
        fee->setToolTip(tr(
            "Percentage charged on a user-paid DigiDollar transfer. Changes in 0.10% steps; Core stores the exact equivalent in basis points."));
        auto* minimum = spin(policy_page, 1, 10000000, m_min_amount->value());
        minimum->setSuffix(tr(" cents"));
        minimum->setAccessibleName(tr("Smallest supported payment"));
        auto* maximum = spin(policy_page, 1, 10000000, m_max_amount->value());
        maximum->setSuffix(tr(" cents"));
        maximum->setAccessibleName(tr("Largest supported payment"));
        auto* lifetime = spin(policy_page, 1, 60, m_quote_ttl->value());
        lifetime->setSuffix(tr(" seconds"));
        lifetime->setAccessibleName(tr("Quote validity in seconds"));
        constexpr int CONSERVATIVE_MAXIMUM_NETWORK_FEE{10000000};
        constexpr int RECOMMENDED_MAXIMUM_NETWORK_FEE{20000000};
        auto* network_fee = spin(
            policy_page, 1, 2000000000,
            std::min(m_network_fee->value(), CONSERVATIVE_MAXIMUM_NETWORK_FEE));
        network_fee->setObjectName("paymasterSetupNetworkFee");
        network_fee->setSuffix(tr(" sat"));
        network_fee->setAccessibleName(tr("Maximum DGB network fee per transfer"));
        policy_layout->addRow(tr("User-paid service fee:"), fee);
        policy_layout->addRow(tr("Smallest payment:"), minimum);
        policy_layout->addRow(tr("Largest payment:"), maximum);
        policy_layout->addRow(tr("Quote validity:"), lifetime);
        policy_layout->addRow(tr("Maximum DGB network fee per transfer:"), network_fee);
        auto* field_help = new QFrame(policy_page);
        field_help->setObjectName("paymasterSetupFieldHelp");
        field_help->setFrameShape(QFrame::StyledPanel);
        auto* field_help_layout = new QVBoxLayout(field_help);
        field_help_layout->setContentsMargins(12, 9, 12, 9);
        field_help_layout->setSpacing(3);
        auto* field_help_title = new QLabel(field_help);
        field_help_title->setObjectName("paymasterSetupFieldHelpTitle");
        auto* field_help_text = new QLabel(field_help);
        field_help_text->setObjectName("paymasterSetupFieldHelpText");
        field_help_text->setWordWrap(true);
        field_help_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        field_help_layout->addWidget(field_help_title);
        field_help_layout->addWidget(field_help_text);
        policy_layout->addRow(field_help);

        const auto update_policy_help = [this, fee, minimum, maximum, lifetime, network_fee,
                                         field_help_title, field_help_text](QWidget* field) {
            if (field == fee) {
                field_help_title->setText(tr("User-paid service fee"));
                field_help_text->setText(tr(
                    "For user-paid offers, the provider charges %1% of the transferred $DD amount. "
                    "Core stores this exactly as %2 basis points. Sponsored transfers always charge 0.00% service fee.")
                    .arg(QString::number(fee->value(), 'f', 2))
                    .arg(qRound(fee->value() * 100.0)));
            } else if (field == minimum) {
                field_help_title->setText(tr("Smallest supported payment"));
                field_help_text->setText(tr(
                    "Your provider will not offer service for payments below %1 $DD. "
                    "A sensible minimum avoids spending provider resources on very small requests.")
                    .arg(QString::number(minimum->value() / 100.0, 'f', 2)));
            } else if (field == maximum) {
                field_help_title->setText(tr("Largest supported payment"));
                field_help_text->setText(tr(
                    "Your provider will reject payments above %1 $DD. Keep this limit aligned with the amount of liquidity and financial exposure you intend to provide.")
                    .arg(QString::number(maximum->value() / 100.0, 'f', 2)));
            } else if (field == lifetime) {
                field_help_title->setText(tr("Quote validity"));
                field_help_text->setText(tr(
                    "A client has %1 seconds to accept this provider quote. Shorter validity releases unused reservations sooner; longer validity gives slower clients more time.")
                    .arg(lifetime->value()));
            } else if (field == network_fee) {
                field_help_title->setText(tr("Maximum DGB network fee per transfer"));
                field_help_text->setText(tr(
                    "The provider will never fund more than %1 DGB of network fee for one transfer. "
                    "This is an additional ceiling: a lower wallet safety limit still wins.")
                    .arg(QString::number(network_fee->value() / 100000000.0, 'f', 8)));
            }
        };
        connect(qApp, &QApplication::focusChanged, policy_page,
                [policy_page, fee, minimum, maximum, lifetime, network_fee,
                 update_policy_help](QWidget*, QWidget* focused) {
                    QWidget* candidate = focused;
                    while (candidate && candidate != policy_page) {
                        if (candidate == fee || candidate == minimum || candidate == maximum ||
                            candidate == lifetime || candidate == network_fee) {
                            update_policy_help(candidate);
                            return;
                        }
                        candidate = candidate->parentWidget();
                    }
                });
        connect(fee, qOverload<double>(&QDoubleSpinBox::valueChanged), policy_page,
                [fee, update_policy_help] { update_policy_help(fee); });
        for (QSpinBox* control : {minimum, maximum, lifetime, network_fee}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), policy_page,
                    [control, update_policy_help] { update_policy_help(control); });
        }
        update_policy_help(fee);
        wizard.addPage(policy_page);

        auto* safety_page = new QWizardPage(&wizard);
        safety_page->setObjectName("paymasterSetupSafetyPage");
        safety_page->setTitle(tr("Safety profile"));
        safety_page->setSubTitle(tr("Choose finite wallet-local spending and request limits."));
        auto* safety_layout = new QVBoxLayout(safety_page);
        auto* safety_text = new QLabel(tr(
            "Safety limits are the final brake against repeated or expensive remote requests. "
            "They are never advertised and never mean unlimited. Selected service models receive "
            "finite limits; unused models remain disabled with all values set to zero."), safety_page);
        safety_text->setObjectName("paymasterSetupSafetyExplanation");
        safety_text->setWordWrap(true);
        auto* safety_profile = new NoWheelComboBox(safety_page);
        safety_profile->setObjectName("paymasterSetupSafetyProfile");
        safety_profile->addItem(tr("Conservative — lower initial hourly and daily exposure"), QStringLiteral("conservative"));
        safety_profile->addItem(tr("Recommended — standard finite starting limits"), QStringLiteral("recommended"));
        auto* safety_summary = new QLabel(safety_page);
        safety_summary->setObjectName("paymasterSetupSafetySummary");
        safety_summary->setWordWrap(true);
        const auto update_safety_summary = [this, safety_profile, safety_summary] {
            if (safety_profile->currentData().toString() == QLatin1String("conservative")) {
                safety_summary->setText(tr(
                    "Per selected model: up to 0.10000000 DGB per transfer, 0.20000000 DGB reserved, "
                    "0.50000000 DGB per rolling hour and 2.00000000 DGB per rolling day; "
                    "5 completed transfers per hour and 25 per day."));
            } else {
                safety_summary->setText(tr(
                    "Per selected model: up to 0.20000000 DGB per transfer, 1.00000000 DGB reserved, "
                    "2.00000000 DGB per rolling hour and 10.00000000 DGB per rolling day; "
                    "10 completed transfers per hour and 100 per day."));
            }
        };
        connect(safety_profile, qOverload<int>(&QComboBox::currentIndexChanged),
                safety_page,
                [safety_profile, network_fee, update_safety_summary] {
                    update_safety_summary();
                    if (safety_profile->currentData().toString() ==
                            QLatin1String("conservative") &&
                        network_fee->value() > CONSERVATIVE_MAXIMUM_NETWORK_FEE) {
                        network_fee->setValue(CONSERVATIVE_MAXIMUM_NETWORK_FEE);
                    }
                });
        update_safety_summary();
        safety_layout->addWidget(safety_text);
        safety_layout->addWidget(safety_profile);
        safety_layout->addWidget(safety_summary);
        safety_layout->addStretch();
        wizard.addPage(safety_page);

        auto* pool_page = new QWizardPage(&wizard);
        pool_page->setObjectName("paymasterSetupLiquidityPage");
        pool_page->setTitle(tr("Liquidity targets"));
        pool_page->setSubTitle(tr("Choose how many reserve and immediately usable payment slots to prepare."));
        auto* pool_layout = new QFormLayout(pool_page);
        auto* admission_dgb = spin(pool_page, 3, 16, m_admission_dgb->value());
        admission_dgb->setAccessibleName(tr("Reserve DGB slots for network admission"));
        auto* operational_dgb = spin(pool_page, 1, 16, m_operational_dgb->value());
        operational_dgb->setAccessibleName(tr("Immediately usable DGB slots"));
        auto* admission_carriers = spin(pool_page, 0, 16, m_admission_carriers->value());
        admission_carriers->setAccessibleName(tr("Reserve DigiDollar carrier slots"));
        auto* operational_carriers = spin(pool_page, 0, 16, m_operational_carriers->value());
        operational_carriers->setAccessibleName(tr("Immediately usable DigiDollar carrier slots"));
        connect(wizard_user_paid, &QCheckBox::toggled, pool_page,
                [admission_carriers, operational_carriers](bool checked) {
                    if (checked) {
                        if (admission_carriers->value() == 0) admission_carriers->setValue(3);
                        if (operational_carriers->value() == 0) operational_carriers->setValue(1);
                    } else {
                        admission_carriers->setValue(0);
                        operational_carriers->setValue(0);
                    }
                });
        if (!wizard_user_paid->isChecked()) {
            admission_carriers->setValue(0);
            operational_carriers->setValue(0);
        }
        pool_layout->addRow(tr("Reserve DGB slots for network admission:"), admission_dgb);
        pool_layout->addRow(tr("Immediately usable DGB slots:"), operational_dgb);
        pool_layout->addRow(tr("Reserve DigiDollar carrier slots:"), admission_carriers);
        pool_layout->addRow(tr("Immediately usable carrier slots:"), operational_carriers);
        auto* liquidity_help = new QFrame(pool_page);
        liquidity_help->setObjectName("paymasterSetupLiquidityFieldHelp");
        liquidity_help->setFrameShape(QFrame::StyledPanel);
        auto* liquidity_help_layout = new QVBoxLayout(liquidity_help);
        liquidity_help_layout->setContentsMargins(12, 9, 12, 9);
        liquidity_help_layout->setSpacing(3);
        auto* liquidity_help_title = new QLabel(liquidity_help);
        liquidity_help_title->setObjectName("paymasterSetupLiquidityFieldHelpTitle");
        auto* liquidity_help_text = new QLabel(liquidity_help);
        liquidity_help_text->setObjectName("paymasterSetupLiquidityFieldHelpText");
        liquidity_help_text->setWordWrap(true);
        liquidity_help_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        liquidity_help_layout->addWidget(liquidity_help_title);
        liquidity_help_layout->addWidget(liquidity_help_text);
        pool_layout->addRow(liquidity_help);

        const auto update_liquidity_help = [this, wizard_user_paid, admission_dgb,
                                            operational_dgb, admission_carriers,
                                            operational_carriers, liquidity_help_title,
                                            liquidity_help_text](QWidget* field) {
            if (field == admission_dgb) {
                liquidity_help_title->setText(tr("Reserve DGB slots for network admission"));
                liquidity_help_text->setText(tr(
                    "%1 separate confirmed DGB outputs will be targeted as the provider's admission reserve. "
                    "They prove that the provider has minimum capacity before a client reveals payment details. "
                    "At least three are required, and they are separate from immediately spendable payment slots.")
                    .arg(admission_dgb->value()));
            } else if (field == operational_dgb) {
                liquidity_help_title->setText(tr("Immediately usable DGB slots"));
                liquidity_help_text->setText(tr(
                    "%1 DGB slot(s) will be targeted for active payments. Each slot can be reserved for one transfer at a time and supplies its DigiByte network fee. "
                    "A higher value permits more parallel work but keeps more wallet funds prepared for Paymaster use.")
                    .arg(operational_dgb->value()));
            } else if (field == admission_carriers) {
                liquidity_help_title->setText(tr("Reserve DigiDollar carrier slots"));
                liquidity_help_text->setText(wizard_user_paid->isChecked()
                    ? tr("%1 confirmed DigiDollar carrier output(s) will be targeted for user-paid admission. At least three are required when the provider accepts service fees in $DD.")
                          .arg(admission_carriers->value())
                    : tr("Carrier outputs are not needed for sponsored-only service, so this target remains zero."));
            } else if (field == operational_carriers) {
                liquidity_help_title->setText(tr("Immediately usable DigiDollar carrier slots"));
                liquidity_help_text->setText(wizard_user_paid->isChecked()
                    ? tr("%1 carrier slot(s) will be targeted for active user-paid transfers. A complete user-paid operational slot needs both one DGB slot and one carrier slot; the lower count therefore limits parallel user-paid transfers.")
                          .arg(operational_carriers->value())
                    : tr("Sponsored transfers charge no $DD service fee and therefore need no operational carrier slots."));
            }
        };
        connect(qApp, &QApplication::focusChanged, pool_page,
                [pool_page, admission_dgb, operational_dgb, admission_carriers,
                 operational_carriers, update_liquidity_help](QWidget*, QWidget* focused) {
                    QWidget* candidate = focused;
                    while (candidate && candidate != pool_page) {
                        if (candidate == admission_dgb || candidate == operational_dgb ||
                            candidate == admission_carriers || candidate == operational_carriers) {
                            update_liquidity_help(candidate);
                            return;
                        }
                        candidate = candidate->parentWidget();
                    }
                });
        for (QSpinBox* control : {admission_dgb, operational_dgb,
                                  admission_carriers, operational_carriers}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged), pool_page,
                    [control, update_liquidity_help] {
                        if (control->hasFocus()) update_liquidity_help(control);
                    });
        }
        update_liquidity_help(admission_dgb);
        wizard.addPage(pool_page);

        auto* review_page = new ValidatedWizardPage(&wizard);
        review_page->setObjectName("paymasterSetupReviewPage");
        review_page->setTitle(tr("Review the setup plan"));
        review_page->setSubTitle(tr("Review the complete configuration and exact missing liquidity before applying it."));
        auto* review_layout = new QVBoxLayout(review_page);
        auto* review = new QLabel(review_page);
        review->setObjectName("paymasterSetupReview");
        review->setWordWrap(true);
        review->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                        Qt::TextSelectableByMouse);
        auto* next_actions = new QLabel(tr(
            "When you apply this plan, the assistant creates or reuses the identity, saves the operating, safety and automatic-liquidity policies, selects recommended automatic processing with autostart off, enables the provider configuration and creates only missing pool outputs. The provider will not be started."), review_page);
        next_actions->setObjectName("paymasterSetupNextActions");
        next_actions->setWordWrap(true);
        auto* preview_status = new QLabel(tr("Liquidity requirement calculated from the current wallet state."), review_page);
        preview_status->setObjectName("paymasterSetupLiquidityPreviewStatus");
        preview_status->setWordWrap(true);
        auto* preview_details = new QLabel(review_page);
        preview_details->setObjectName("paymasterSetupLiquidityPreviewDetails");
        preview_details->setWordWrap(true);
        preview_details->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                                 Qt::TextSelectableByMouse);
        auto* preview_retry = new QPushButton(tr("Recalculate liquidity requirement"), review_page);
        preview_retry->setObjectName("paymasterSetupLiquidityPreviewRetry");
        preview_retry->setVisible(false);
        auto* maintenance_confirmation = new QCheckBox(tr(
            "I approve automatic paid liquidity maintenance only within the finite limits shown in this review."),
            review_page);
        maintenance_confirmation->setObjectName(
            "paymasterSetupMaintenanceApproval");
        const auto invalidate_maintenance_confirmation =
            [maintenance_confirmation] {
                maintenance_confirmation->setChecked(false);
            };
        for (QSpinBox* control : {network_fee, admission_dgb,
                                  operational_dgb, admission_carriers,
                                  operational_carriers}) {
            connect(control, qOverload<int>(&QSpinBox::valueChanged),
                    review_page,
                    [invalidate_maintenance_confirmation](int) {
                        invalidate_maintenance_confirmation();
                    });
        }
        connect(safety_profile, qOverload<int>(&QComboBox::currentIndexChanged),
                review_page,
                [invalidate_maintenance_confirmation](int) {
                    invalidate_maintenance_confirmation();
                });
        review_layout->addWidget(review);
        review_layout->addWidget(next_actions);
        review_layout->addWidget(preview_status);
        review_layout->addWidget(preview_details);
        review_layout->addWidget(maintenance_confirmation);
        review_layout->addWidget(preview_retry, 0, Qt::AlignLeft);
        review_layout->addStretch();
        const int review_id = wizard.addPage(review_page);

        auto* progress_page = new CompletableWizardPage(&wizard);
        progress_page->setObjectName("paymasterSetupProgressPage");
        progress_page->setTitle(tr("Apply Paymaster setup"));
        progress_page->setSubTitle(tr("The assistant saves each setting in order. The provider remains offline."));
        auto* progress_layout = new QVBoxLayout(progress_page);
        auto* progress_intro = new QLabel(tr(
            "Do not close DigiByte Core while a step is running. Completed steps remain safely persisted if a later step needs to be retried."), progress_page);
        progress_intro->setWordWrap(true);
        progress_layout->addWidget(progress_intro);
        const QStringList progress_names{
            tr("Verify selected provider wallet"),
            tr("Unlock wallet when required"),
            tr("Create or reuse provider identity"),
            tr("Save operating policy"),
            tr("Save provider safety policy"),
            tr("Save automatic liquidity policy"),
            tr("Select automatic provider operation"),
            tr("Enable provider configuration"),
            tr("Create missing pool liquidity"),
        };
        QList<QLabel*> progress_steps;
        for (int index = 0; index < progress_names.size(); ++index) {
            const QString& name = progress_names.at(index);
            auto* label = new QLabel(tr("○ Pending — %1").arg(name), progress_page);
            label->setObjectName(QStringLiteral("paymasterSetupProgressStep%1").arg(index));
            label->setWordWrap(true);
            progress_layout->addWidget(label);
            progress_steps.push_back(label);
        }
        auto* progress_result = new QLabel(
            tr("The setup has not started yet."), progress_page);
        progress_result->setObjectName("paymasterSetupProgressResult");
        progress_result->setWordWrap(true);
        progress_result->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                                 Qt::TextSelectableByMouse);
        auto* setup_retry = new QPushButton(tr("Retry failed step"), progress_page);
        setup_retry->setObjectName("paymasterSetupRetry");
        setup_retry->setVisible(false);
        auto* setup_backup = new QGroupBox(
            tr("Protect the new provider identity"), progress_page);
        setup_backup->setObjectName("paymasterSetupBackupReminder");
        setup_backup->setProperty("paymasterRole", QStringLiteral("notice"));
        setup_backup->setProperty("statusKind", QStringLiteral("waiting"));
        auto* setup_backup_layout = new QVBoxLayout(setup_backup);
        auto* setup_backup_text = new QLabel(tr(
            "The provider identity key, Paymaster settings, pool state and finance ledger are stored in this wallet. "
            "Create a complete wallet backup and keep it offline and confidential. A seed or descriptor export alone is not a complete Paymaster backup. "
            "The reminder is important but does not block provider start."), setup_backup);
        setup_backup_text->setWordWrap(true);
        auto* setup_backup_id = new QLabel(setup_backup);
        setup_backup_id->setObjectName("paymasterSetupBackupProviderId");
        setup_backup_id->setWordWrap(true);
        setup_backup_id->setTextInteractionFlags(
            Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        auto* setup_backup_actions = new QHBoxLayout();
        auto* setup_backup_now = new QPushButton(
            tr("Back up provider wallet now…"), setup_backup);
        setup_backup_now->setObjectName("paymasterSetupBackupNow");
        setup_backup_now->setProperty(
            "paymasterRole", QStringLiteral("primaryAction"));
        auto* setup_backup_external = new QPushButton(
            tr("I use another full-wallet backup method…"), setup_backup);
        setup_backup_external->setObjectName("paymasterSetupExternalBackup");
        setup_backup_actions->addWidget(setup_backup_now);
        setup_backup_actions->addWidget(setup_backup_external);
        setup_backup_actions->addStretch();
        setup_backup_layout->addWidget(setup_backup_text);
        setup_backup_layout->addWidget(setup_backup_id);
        setup_backup_layout->addLayout(setup_backup_actions);
        setup_backup->hide();
        connect(setup_backup_now, &QPushButton::clicked, progress_page,
                [this] { requestProviderBackup(); });
        connect(setup_backup_external, &QPushButton::clicked, progress_page,
                [this] { acknowledgeExternalBackup(); });
        progress_layout->addSpacing(10);
        progress_layout->addWidget(progress_result);
        progress_layout->addWidget(setup_backup);
        progress_layout->addWidget(setup_retry, 0, Qt::AlignLeft);
        progress_layout->addStretch();
        const int progress_id = wizard.addPage(progress_page);

        auto reviewed_pool = std::make_shared<UniValue>();
        const auto wizard_pool_options = [admission_dgb, operational_dgb,
                                          admission_carriers,
                                          operational_carriers](bool execute) {
            UniValue options{UniValue::VOBJ};
            options.pushKV("admission_dgb_slots", admission_dgb->value());
            options.pushKV("operational_dgb_slots", operational_dgb->value());
            options.pushKV("admission_carrier_slots", admission_carriers->value());
            options.pushKV("operational_carrier_slots", operational_carriers->value());
            options.pushKV("execute", execute);
            UniValue params{UniValue::VARR};
            params.push_back(std::move(options));
            return params;
        };
        const auto wizard_pool_preview_text = [this](const UniValue& result) {
            QString text = formatPoolResult("preparepaymasterpool", result);
            text.replace(
                tr("Review these values. If they are acceptable, use the matching Execute reviewed action without changing the targets."),
                tr("These exact targets will be used when you confirm and apply the complete setup."));
            return text;
        };
        auto refresh_preview = std::make_shared<std::function<void()>>();
        *refresh_preview = [this, review_page, preview_status, preview_details,
                            preview_retry, reviewed_pool, admission_dgb,
                            operational_dgb, admission_carriers,
                            operational_carriers, network_fee,
                            maintenance_confirmation,
                            wizard_pool_preview_text] {
            const int missing_admission_dgb =
                std::max(0, admission_dgb->value() - m_pool_admission_dgb);
            const int missing_operational_dgb =
                std::max(0, operational_dgb->value() - m_pool_operational_dgb);
            const int missing_admission_carriers =
                std::max(0, admission_carriers->value() - m_pool_admission_carriers);
            const int missing_operational_carriers =
                std::max(0, operational_carriers->value() - m_pool_operational_carriers);
            const qint64 admission_value =
                DigiDollar::Paymaster::MIN_ADMISSION_DGB_SATOSHIS;
            const qint64 operational_value = std::max<qint64>(
                admission_value, network_fee->value());
            UniValue estimate{UniValue::VOBJ};
            estimate.pushKV("executed", false);
            estimate.pushKV("missing_admission_dgb_slots", missing_admission_dgb);
            estimate.pushKV("missing_operational_dgb_slots", missing_operational_dgb);
            estimate.pushKV("missing_admission_carrier_slots", missing_admission_carriers);
            estimate.pushKV("missing_operational_carrier_slots", missing_operational_carriers);
            estimate.pushKV("admission_dgb_satoshis_each", admission_value);
            estimate.pushKV("operational_dgb_satoshis_each", operational_value);
            estimate.pushKV("carrier_cents_each", 100);
            estimate.pushKV("total_output_satoshis",
                admission_value * missing_admission_dgb +
                operational_value * missing_operational_dgb);
            estimate.pushKV("total_carrier_cents",
                100 * (missing_admission_carriers + missing_operational_carriers));
            *reviewed_pool = std::move(estimate);
            preview_status->setText(m_pool_status_loaded
                ? tr("Funding estimate complete — no funds have moved. Core will recheck all existing and pending pool outputs after saving the policies and before any funding transaction.")
                : tr("Conservative funding estimate — provider pool status has not loaded yet, so this estimate assumes no existing confirmed outputs. Core will recheck the wallet before any funding transaction."));
            preview_details->setText(wizard_pool_preview_text(*reviewed_pool) + tr(
                "\nThe pool transaction network fee is calculated by Core immediately before funding and requires a separate confirmation. Pending wallet-owned pool outputs can only reduce the amount shown here."));
            preview_retry->setVisible(false);
            review_page->setComplete(
                maintenance_confirmation->isChecked());
        };
        connect(preview_retry, &QPushButton::clicked, review_page,
                [refresh_preview] { (*refresh_preview)(); });
        connect(maintenance_confirmation, &QCheckBox::toggled, review_page,
                [review_page](bool checked) {
                    review_page->setComplete(checked);
                });

        connect(&wizard, &QWizard::currentIdChanged, &wizard,
                [=, &wizard](int current_id) {
                    wizard.setButtonText(QWizard::NextButton,
                        current_id == review_id ? tr("Apply setup…") : tr("Next >"));
                    if (current_id != review_id) return;
                    QString model;
                    if (wizard_user_paid->isChecked() && wizard_sponsored->isChecked()) {
                        model = tr("User paid and public sponsored");
                    } else if (wizard_user_paid->isChecked()) {
                        model = tr("User paid");
                    } else if (wizard_scope->currentData().toString() == QLatin1String("restricted")) {
                        model = tr("Restricted sponsored");
                    } else {
                        model = tr("Public sponsored");
                    }
                    QString review_text = tr(
                        "Provider wallet: %1\n"
                        "Provider name: %2\n"
                        "Service model: %3\n"
                        "Payment range: %4 to %5 DD\n"
                        "User-paid service fee: %6%\n"
                        "Quote validity: %7 seconds\n"
                        "Network-fee ceiling: %8 DGB per transfer\n"
                        "Safety profile: %9\n"
                        "Provider operation: automatic after an explicit start; autostart off")
                        .arg(setup_wallet_name)
                        .arg(display->text().trimmed().isEmpty() ? tr("No public name") : display->text().trimmed())
                        .arg(model)
                        .arg(QString::number(minimum->value() / 100.0, 'f', 2))
                        .arg(QString::number(maximum->value() / 100.0, 'f', 2))
                        .arg(QString::number(fee->value(), 'f', 2))
                        .arg(lifetime->value())
                        .arg(QString::number(network_fee->value() / 100000000.0, 'f', 8))
                        .arg(safety_profile->currentText());
                    review_text += tr(
                        "\nLiquidity targets: admission DGB %1, operational DGB %2, admission carriers %3, operational carriers %4")
                        .arg(admission_dgb->value())
                        .arg(operational_dgb->value())
                        .arg(admission_carriers->value())
                        .arg(operational_carriers->value());
                    const bool conservative =
                        safety_profile->currentData().toString() ==
                        QLatin1String("conservative");
                    const qint64 maintenance_per_transaction =
                        conservative ? 10000000 : 20000000;
                    const qint64 maintenance_per_hour =
                        conservative ? 50000000 : 200000000;
                    const qint64 maintenance_per_day =
                        conservative ? 200000000 : 1000000000;
                    review_text += tr(
                        "\nAutomatic replenishment: approved only up to %1 DGB per maintenance transaction, %2 DGB per rolling hour and %3 DGB per rolling day")
                        .arg(dgbAmount(maintenance_per_transaction),
                             dgbAmount(maintenance_per_hour),
                             dgbAmount(maintenance_per_day));
                    maintenance_confirmation->setText(tr(
                        "I approve automatic paid liquidity maintenance for wallet \"%1\" up to %2 DGB per transaction, %3 DGB per rolling hour and %4 DGB per rolling day. The provider will still remain offline until I start it separately.")
                        .arg(setup_wallet_name,
                             dgbAmount(maintenance_per_transaction),
                             dgbAmount(maintenance_per_hour),
                             dgbAmount(maintenance_per_day)));
                    if (m_core_has_identity) {
                        review_text += tr(
                            "\nExisting provider identity: retained unchanged; its saved display name remains authoritative.");
                    } else {
                        review_text += tr("\nProvider identity: a new persistent BIP86 identity will be created.");
                    }
                    review->setText(review_text);
                    (*refresh_preview)();
                });

        review_page->setValidator([&, reviewed_pool,
                                   wizard_pool_preview_text] {
            QString validation_error;
            if (!wizard_user_paid->isChecked() && !wizard_sponsored->isChecked()) {
                validation_error = tr("Select at least one service model.");
            } else if (qRound(fee->value() * 100.0) % 10 != 0 ||
                       qAbs(fee->value() * 100.0 - qRound(fee->value() * 100.0)) > 0.001) {
                validation_error = tr("The user-paid service fee must use increments of 0.10%.");
            } else if (minimum->value() > maximum->value()) {
                validation_error = tr("The smallest payment cannot exceed the largest payment.");
            } else if (network_fee->value() >
                       (safety_profile->currentData().toString() == QLatin1String("conservative")
                            ? CONSERVATIVE_MAXIMUM_NETWORK_FEE
                            : RECOMMENDED_MAXIMUM_NETWORK_FEE)) {
                validation_error = tr(
                    "The network-fee ceiling exceeds the selected safety profile's per-transfer limit. Lower the ceiling or choose a safety profile that permits it.");
            } else if (admission_dgb->value() < operational_dgb->value()) {
                validation_error = tr("Admission DGB slots cannot be lower than operational DGB slots.");
            } else if (wizard_user_paid->isChecked() &&
                       (admission_carriers->value() < 3 ||
                        operational_carriers->value() == 0)) {
                validation_error = tr("User-paid service requires at least three admission carrier slots and one operational carrier slot.");
            } else if (admission_carriers->value() > 0 &&
                       admission_carriers->value() < 3) {
                validation_error = tr("A non-zero admission carrier target must contain at least three slots.");
            } else if (admission_carriers->value() < operational_carriers->value()) {
                validation_error = tr("Admission carrier slots cannot be lower than operational carrier slots.");
            } else if (!maintenance_confirmation->isChecked()) {
                validation_error = tr(
                    "Review and explicitly approve the finite automatic liquidity-maintenance limits before applying setup.");
            }
            if (!validation_error.isEmpty()) {
                QMessageBox::warning(&wizard, tr("Review Paymaster setup"),
                                     validation_error);
                return false;
            }
            const QString confirmation = tr(
                "Apply and save this complete Paymaster setup in wallet \"%1\"?\n\n%2\n\n"
                "Only missing pool outputs will be created. Before any funds move, Core will recheck the exact missing outputs and ask you to confirm the resulting DGB/DD funding separately. Future paid replenishment is limited by the maintenance ceilings you explicitly approved above. The provider will remain offline until you start it separately from Overview.")
                .arg(setup_wallet_name, wizard_pool_preview_text(*reviewed_pool));
            if (QMessageBox::question(
                    &wizard, tr("Confirm complete Paymaster setup"), confirmation,
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes) {
                return false;
            }
            setSetupMode(PaymasterSetupMode::GUIDED);
            return true;
        });

        auto setup_step = std::make_shared<int>(0);
        auto setup_running = std::make_shared<bool>(false);
        auto setup_started = std::make_shared<bool>(false);
        auto setup_completed = std::make_shared<bool>(false);
        auto setup_needs_identity = std::make_shared<bool>(false);
        auto setup_needs_liquidity = std::make_shared<bool>(false);
        auto setup_provider_id = std::make_shared<QString>();
        auto setup_unlock = std::make_shared<std::shared_ptr<WalletModel::UnlockContext>>();
        auto identity_params = std::make_shared<UniValue>();
        auto policy_params = std::make_shared<UniValue>();
        auto safety_params = std::make_shared<UniValue>();
        auto liquidity_policy_params = std::make_shared<UniValue>();
        auto liquidity_preview_params = std::make_shared<UniValue>();
        auto liquidity_params = std::make_shared<UniValue>();

        const auto set_progress = [progress_steps, progress_names](int step,
                                                                  const QString& marker,
                                                                  const QString& state) {
            progress_steps.at(step)->setText(
                QStringLiteral("%1 %2 — %3").arg(marker, state, progress_names.at(step)));
        };
        const auto set_wizard_running = [&wizard](bool running) {
            wizard.setCloseBlocked(running);
            if (wizard.button(QWizard::BackButton)) {
                wizard.button(QWizard::BackButton)->setEnabled(false);
            }
            if (wizard.button(QWizard::CancelButton)) {
                wizard.button(QWizard::CancelButton)->setEnabled(!running);
            }
        };
        const auto fail_setup = [&, setup_running, setup_retry, progress_result,
                                 setup_unlock,
                                 setup_step, set_progress, set_wizard_running](
                                        const QString& error) {
            *setup_running = false;
            setup_unlock->reset();
            set_progress(*setup_step, QStringLiteral("✕"), tr("Failed"));
            progress_result->setText(tr(
                "Setup stopped at this step: %1\n\nCompleted earlier steps remain saved. Correct the problem and retry; no completed step needs to be repeated manually.")
                .arg(error));
            setup_retry->setVisible(true);
            set_wizard_running(false);
        };

        auto advance_setup = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak_advance_setup{advance_setup};
        *advance_setup = [this, &wizard, progress_page, progress_result,
                          setup_backup, setup_backup_id,
                          progress_steps, progress_names, setup_retry,
                          setup_step, setup_running, setup_completed,
                          setup_needs_identity, setup_needs_liquidity,
                          setup_provider_id,
                          setup_unlock, identity_params, policy_params,
                          safety_params, liquidity_policy_params,
                          liquidity_preview_params, liquidity_params, set_progress,
                          set_wizard_running, fail_setup, weak_advance_setup,
                          setup_model, setup_wallet_id, setup_wallet_name] {
            const auto advance = weak_advance_setup.lock();
            if (!advance) return;
            if (*setup_step >= progress_steps.size()) {
                setup_unlock->reset();
                *setup_running = false;
                *setup_completed = true;
                m_policy_loaded = true;
                m_policy_dirty = false;
                m_provider_safety_configured = true;
                m_provider_safety_dirty = false;
                m_liquidity_policy_configured = true;
                m_liquidity_policy_dirty = false;
                m_core_enabled = true;
                progress_result->setText(m_setup_waiting_for_confirmations
                    ? tr("All settings were saved successfully in wallet \"%1\". You do not need to click any additional Save buttons. New pool outputs are waiting for blockchain confirmations; continue to Overview and leave the provider offline until readiness is complete.").arg(setup_wallet_name)
                    : tr("All settings were saved successfully in wallet \"%1\". You do not need to click any additional Save buttons. All setup requirements can now be checked on Overview; the provider has not been started.").arg(setup_wallet_name));
                if (*setup_needs_identity && !setup_provider_id->isEmpty()) {
                    setup_backup_id->setText(
                        tr("Provider ID: %1").arg(*setup_provider_id));
                    setup_backup->show();
                    updateBackupReminder(true, *setup_provider_id);
                }
                setup_retry->setVisible(false);
                progress_page->setComplete(true);
                wizard.setButtonText(QWizard::FinishButton, tr("Go to Overview"));
                set_wizard_running(false);
                if (m_setup_waiting_for_confirmations) m_setup_status_timer->start();
                refreshStatus();
                return;
            }

            *setup_running = true;
            setup_retry->setVisible(false);
            set_wizard_running(true);
            set_progress(*setup_step, QStringLiteral("…"), tr("Running"));
            const auto succeed = [this, setup_step, set_progress,
                                  weak_advance_setup] {
                set_progress(*setup_step, QStringLiteral("✓"), tr("Done"));
                ++*setup_step;
                if (const auto advance = weak_advance_setup.lock()) (*advance)();
            };

            switch (*setup_step) {
            case 0: {
                if (!setup_model || m_model != setup_model ||
                    m_model->getWalletName() != setup_wallet_id) {
                    fail_setup(tr(
                        "The selected wallet changed or was unloaded. No setup setting was written. Close the assistant, select the intended wallet and start again."));
                    return;
                }
                call("getpaymasterinfo", {}, false, nullptr,
                     [this, setup_needs_identity, setup_provider_id, succeed,
                      fail_setup](const UniValue& result) {
                         const UniValue& eligible = result.find_value("wallet_eligible");
                         if (!eligible.isBool() || !eligible.get_bool()) {
                             fail_setup(tr(
                                 "Core rejected this provider wallet. Use a descriptor wallet with local private keys and no external signer."));
                             return;
                         }
                         m_core_eligible = true;
                         const UniValue& provider = result.find_value("provider_id");
                         m_core_has_identity = provider.isStr() &&
                                               !provider.get_str().empty();
                         *setup_provider_id = m_core_has_identity
                             ? QString::fromStdString(provider.get_str())
                             : QString{};
                         *setup_needs_identity = !m_core_has_identity;
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 1: {
                if (!*setup_needs_identity && !*setup_needs_liquidity) {
                    set_progress(1, QStringLiteral("✓"), tr("Not required"));
                    ++*setup_step;
                    (*advance)();
                    return;
                }
                *setup_unlock = m_model ? m_model->requestUnlockForAsync() : nullptr;
                if (!*setup_unlock || !(*setup_unlock)->isValid()) {
                    fail_setup(tr("The wallet was not unlocked. No setup operation was started."));
                    return;
                }
                succeed();
                return;
            }
            case 2:
                if (!*setup_needs_identity) {
                    set_progress(2, QStringLiteral("✓"), tr("Existing identity retained"));
                    ++*setup_step;
                    (*advance)();
                    return;
                }
                if (!*setup_unlock || !(*setup_unlock)->isValid()) {
                    *setup_unlock = m_model
                        ? m_model->requestUnlockForAsync()
                        : nullptr;
                    if (!*setup_unlock || !(*setup_unlock)->isValid()) {
                        fail_setup(tr(
                            "The wallet must be unlocked before a provider identity can be created."));
                        return;
                    }
                }
                call("createpaymasteridentity", *identity_params, false, nullptr,
                     [this, setup_provider_id, succeed](const UniValue& result) {
                         m_core_has_identity = true;
                         const UniValue& provider = result.find_value("provider_id");
                         if (provider.isStr()) {
                             *setup_provider_id =
                                 QString::fromStdString(provider.get_str());
                             m_provider_id = *setup_provider_id;
                         }
                         succeed();
                     }, false, fail_setup);
                return;
            case 3:
                call("setpaymasterpolicy", *policy_params, false, nullptr,
                     [this, succeed](const UniValue&) {
                         m_policy_loaded = true;
                         m_policy_dirty = false;
                         succeed();
                     }, false, fail_setup);
                return;
            case 4:
                call("setpaymastersafetypolicy", *safety_params, false, nullptr,
                     [this, succeed](const UniValue&) {
                         m_provider_safety_configured = true;
                         m_provider_safety_dirty = false;
                         succeed();
                     }, false, fail_setup);
                return;
            case 5: {
                call("setpaymasterliquiditypolicy", *liquidity_policy_params,
                     false, nullptr,
                     [this, succeed](const UniValue&) {
                         m_liquidity_policy_configured = true;
                         m_liquidity_policy_dirty = false;
                         m_liquidity_policy_status->setText(tr(
                             "Automatic liquidity targets and maintenance ceilings are saved in this wallet."));
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 6: {
                UniValue runtime{UniValue::VOBJ};
                runtime.pushKV("operation_mode", "automatic");
                runtime.pushKV("autostart", false);
                UniValue runtime_params{UniValue::VARR};
                runtime_params.push_back(std::move(runtime));
                call("setpaymasterruntimesettings", std::move(runtime_params),
                     false, nullptr,
                     [this, succeed](const UniValue&) {
                         m_operation_mode = QStringLiteral("automatic");
                         m_autostart_enabled = false;
                         m_runtime_settings_dirty = false;
                         m_loading_runtime_settings = true;
                         m_operation_mode_select->setCurrentIndex(
                             m_operation_mode_select->findData(
                                 QStringLiteral("automatic")));
                         m_autostart->setChecked(false);
                         m_loading_runtime_settings = false;
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 7: {
                UniValue enable_params{UniValue::VARR};
                enable_params.push_back(true);
                call("setpaymasterenabled", std::move(enable_params), false, nullptr,
                     [this, succeed, fail_setup](const UniValue& result) {
                         const UniValue& enabled = result.find_value("enabled");
                         if (!enabled.isBool() || !enabled.get_bool()) {
                             fail_setup(tr("The wallet did not confirm provider enablement."));
                             return;
                         }
                         m_core_enabled = true;
                         succeed();
                     }, false, fail_setup);
                return;
            }
            case 8:
                call("preparepaymasterpool", *liquidity_preview_params, false, nullptr,
                     [this, &wizard, setup_unlock, liquidity_params, succeed,
                      fail_setup](const UniValue& preview) {
                         m_liquidity_output->setPlainText(
                             formatPoolResult("preparepaymasterpool", preview));
                         const qint64 missing_outputs =
                             poolNumber(preview, "missing_admission_dgb_slots") +
                             poolNumber(preview, "missing_operational_dgb_slots") +
                             poolNumber(preview, "missing_admission_carrier_slots") +
                             poolNumber(preview, "missing_operational_carrier_slots");
                         if (missing_outputs == 0) {
                             succeed();
                             return;
                         }
                         if (!*setup_unlock || !(*setup_unlock)->isValid()) {
                             *setup_unlock = m_model
                                 ? m_model->requestUnlockForAsync()
                                 : nullptr;
                             if (!*setup_unlock || !(*setup_unlock)->isValid()) {
                                 fail_setup(tr(
                                     "The wallet must be unlocked before missing pool liquidity can be created."));
                                 return;
                             }
                         }
                         const QString exact_preview =
                             formatPoolResult("preparepaymasterpool", preview);
                         if (QMessageBox::question(
                                 &wizard, tr("Confirm exact pool funding"),
                                 tr("Core has rechecked the current wallet and policies. Create exactly the following missing liquidity?\n\n%1\n\nThe wallet will create only these still-missing outputs. The provider will not be started.")
                                     .arg(exact_preview),
                                 QMessageBox::Yes | QMessageBox::Cancel,
                                 QMessageBox::Cancel) != QMessageBox::Yes) {
                             fail_setup(tr(
                                 "Pool funding was not approved. All configuration settings are already saved, but no new liquidity was created. You can retry this step when ready."));
                             return;
                         }
                         call("preparepaymasterpool", *liquidity_params, false, nullptr,
                              [this, succeed](const UniValue& result) {
                                  m_liquidity_output->setPlainText(
                                      formatPoolResult("preparepaymasterpool", result));
                                  m_setup_waiting_for_confirmations =
                                      result.find_value("executed").isBool() &&
                                      result.find_value("executed").get_bool();
                                  succeed();
                              }, false, fail_setup);
                     }, false, fail_setup);
                return;
            }
        };

        const auto prepare_setup_execution = [&, setup_step, setup_started,
                                              setup_needs_identity,
                                              setup_needs_liquidity,
                                              identity_params, policy_params,
                                              safety_params,
                                              liquidity_policy_params,
                                              liquidity_preview_params,
                                              liquidity_params,
                                              reviewed_pool, advance_setup,
                                              setup_model, setup_wallet_id,
                                              wallet_confirmation] {
            if (*setup_started) return;

            if (!wallet_confirmation->isChecked() || !setup_model ||
                m_model != setup_model ||
                m_model->getWalletName() != setup_wallet_id) {
                *setup_started = true;
                fail_setup(tr(
                    "The confirmed provider wallet is no longer selected. No setup setting was written. Close the assistant and reopen it in the intended wallet."));
                setup_retry->setVisible(false);
                return;
            }
            *setup_started = true;

            if (!m_core_has_identity) {
                m_display_name->setText(display->text());
            }
            m_loading_policy = true;
            m_user_paid->setChecked(wizard_user_paid->isChecked());
            m_sponsored->setChecked(wizard_sponsored->isChecked());
            m_scope->setCurrentIndex(m_scope->findData(wizard_scope->currentData()));
            m_fee_bps->setValue(qRound(fee->value() * 100.0));
            m_min_amount->setValue(minimum->value());
            m_max_amount->setValue(maximum->value());
            m_quote_ttl->setValue(lifetime->value());
            m_network_fee->setValue(network_fee->value());
            m_loading_policy = false;
            m_policy_dirty = true;
            updatePolicyDisplay();

            const bool conservative =
                safety_profile->currentData().toString() == QLatin1String("conservative");
            const auto apply_safety_profile = [this, conservative](
                    const FundingSafetyControls& controls, bool selected) {
                m_loading_provider_safety = true;
                controls.per_transaction->setText(QString::number(
                    selected ? (conservative ? 10000000 : 20000000) : 0));
                controls.reserved->setText(QString::number(
                    selected ? (conservative ? 20000000 : 100000000) : 0));
                controls.per_hour->setText(QString::number(
                    selected ? (conservative ? 50000000 : 200000000) : 0));
                controls.per_day->setText(QString::number(
                    selected ? (conservative ? 200000000 : 1000000000) : 0));
                controls.completed_per_hour->setValue(
                    selected ? (conservative ? 5 : 10) : 0);
                controls.completed_per_day->setValue(
                    selected ? (conservative ? 25 : 100) : 0);
                m_loading_provider_safety = false;
                updateFundingSafetyDisplay(controls);
            };
            apply_safety_profile(m_user_paid_safety, wizard_user_paid->isChecked());
            apply_safety_profile(
                m_public_sponsored_safety,
                wizard_sponsored->isChecked() &&
                    wizard_scope->currentData().toString() == QLatin1String("public"));
            apply_safety_profile(
                m_restricted_sponsored_safety,
                wizard_sponsored->isChecked() &&
                    wizard_scope->currentData().toString() == QLatin1String("restricted"));
            restoreQuoteSafetyDefaults(/*mark_dirty=*/false);
            m_provider_safety_dirty = true;

            m_loading_liquidity_policy = true;
            m_automatic_replenishment->setChecked(true);
            m_paid_maintenance_approved->setChecked(true);
            m_admission_dgb->setValue(admission_dgb->value());
            m_operational_dgb->setValue(operational_dgb->value());
            m_admission_carriers->setValue(admission_carriers->value());
            m_operational_carriers->setValue(operational_carriers->value());
            m_maintenance_fee_per_transaction->setText(QString::number(
                conservative ? 10000000 : 20000000));
            m_maintenance_fee_per_hour->setText(QString::number(
                conservative ? 50000000 : 200000000));
            m_maintenance_fee_per_day->setText(QString::number(
                conservative ? 200000000 : 1000000000));
            m_loading_liquidity_policy = false;
            m_liquidity_policy_dirty = true;
            updateLiquidityDisplay();

            UniValue identity{UniValue::VARR};
            identity.push_back(display->text().trimmed().toStdString());
            *identity_params = std::move(identity);

            UniValue funding{UniValue::VARR};
            if (wizard_sponsored->isChecked()) funding.push_back("sponsored");
            if (wizard_user_paid->isChecked()) funding.push_back("user_paid");
            UniValue policy{UniValue::VOBJ};
            policy.pushKV("funding_models", std::move(funding));
            policy.pushKV("sponsorship_scope", wizard_scope->currentData().toString().toStdString());
            policy.pushKV("fee_rate_bps", qRound(fee->value() * 100.0));
            policy.pushKV("min_amount_cents", minimum->value());
            policy.pushKV("max_amount_cents", maximum->value());
            policy.pushKV("quote_ttl", lifetime->value());
            policy.pushKV("maximum_network_fee_dgb_satoshis", network_fee->value());
            UniValue policy_array{UniValue::VARR};
            policy_array.push_back(std::move(policy));
            *policy_params = std::move(policy_array);

            FundingSafetyValues user_paid;
            FundingSafetyValues public_sponsored;
            FundingSafetyValues restricted_sponsored;
            if (!readFundingSafety(m_user_paid_safety, user_paid) ||
                !readFundingSafety(m_public_sponsored_safety, public_sponsored) ||
                !readFundingSafety(m_restricted_sponsored_safety, restricted_sponsored)) {
                fail_setup(tr("The selected safety values are outside the supported range."));
                return;
            }
            UniValue safety{UniValue::VOBJ};
            safety.pushKV("user_paid", fundingSafetyToJSON(user_paid));
            safety.pushKV("public_sponsored", fundingSafetyToJSON(public_sponsored));
            safety.pushKV("restricted_sponsored", fundingSafetyToJSON(restricted_sponsored));
            safety.pushKV("maximum_active_quotes_total", m_max_active_quotes_total->value());
            safety.pushKV("maximum_active_quotes_per_netgroup", m_max_active_quotes_per_netgroup->value());
            safety.pushKV("maximum_active_quotes_per_recipient", m_max_active_quotes_per_recipient->value());
            safety.pushKV("maximum_quote_requests_per_netgroup_per_minute", m_max_quote_requests_per_netgroup->value());
            UniValue safety_array{UniValue::VARR};
            safety_array.push_back(std::move(safety));
            *safety_params = std::move(safety_array);

            LiquidityPolicyValues liquidity_policy;
            if (!readLiquidityPolicy(liquidity_policy)) {
                fail_setup(tr(
                    "The selected automatic-liquidity targets or maintenance ceilings are outside the supported range."));
                return;
            }
            UniValue liquidity_policy_array{UniValue::VARR};
            liquidity_policy_array.push_back(
                liquidityPolicyToJSON(liquidity_policy));
            *liquidity_policy_params = std::move(liquidity_policy_array);
            *liquidity_preview_params = wizard_pool_options(false);
            *liquidity_params = wizard_pool_options(true);

            const qint64 missing_outputs =
                poolNumber(*reviewed_pool, "missing_admission_dgb_slots") +
                poolNumber(*reviewed_pool, "missing_operational_dgb_slots") +
                poolNumber(*reviewed_pool, "missing_admission_carrier_slots") +
                poolNumber(*reviewed_pool, "missing_operational_carrier_slots");
            *setup_needs_identity = !m_core_has_identity;
            *setup_needs_liquidity = missing_outputs > 0;
            *setup_step = 0;
            (*advance_setup)();
        };

        connect(setup_retry, &QPushButton::clicked, progress_page,
                [setup_running, advance_setup] {
                    if (*setup_running) return;
                    (*advance_setup)();
                });
        connect(&wizard, &QWizard::currentIdChanged, progress_page,
                [&, progress_id, prepare_setup_execution](int current_id) {
                    if (current_id != progress_id) return;
                    wizard.setButtonText(QWizard::FinishButton, tr("Applying setup…"));
                    if (wizard.button(QWizard::FinishButton)) {
                        wizard.button(QWizard::FinishButton)->setEnabled(false);
                    }
                    prepare_setup_execution();
                });

        wizard.exec();
        setup_unlock->reset();
        return *setup_completed;
    }

    using ResultHandler = std::function<void(const UniValue&)>;
    using ErrorHandler = std::function<void(const QString&)>;
    void call(std::string command, UniValue params, bool needs_unlock,
              QPlainTextEdit* output = nullptr, ResultHandler handler = {},
              bool show_error = true, ErrorHandler error_handler = {})
    {
        if ((!m_model && !m_rpc_executor_for_testing) || m_busy) return;
        if (params.isNull()) params = UniValue{UniValue::VARR};

        // Widget tests use a synchronous executor so each user action and its
        // resulting state can be asserted deterministically. This branch is
        // unreachable in production because no executor is installed there.
        if (m_rpc_executor_for_testing) {
            m_busy = true;
            UniValue result;
            QString error;
            try {
                result = m_rpc_executor_for_testing(command, params);
            } catch (const UniValue& rpc_error) {
                const UniValue& message = rpc_error.find_value("message");
                error = message.isStr()
                    ? QString::fromStdString(message.get_str())
                    : QString::fromStdString(rpc_error.write());
            } catch (const std::exception& exception) {
                error = QString::fromUtf8(exception.what());
            } catch (...) {
                error = tr("Unknown RPC error");
            }
            m_busy = false;
            if (!error.isEmpty()) {
                if (output) output->setPlainText(error);
                if (error_handler) {
                    error_handler(error);
                } else if (show_error) {
                    QMessageBox::warning(this, tr("Paymaster operation failed"), error);
                } else {
                    m_status->setText(
                        tr("Provider status unavailable: %1").arg(error));
                }
                return;
            }
            if (output) {
                output->setPlainText(QString::fromStdString(result.write(2)));
            }
            if (handler) {
                handler(result);
            } else {
                refreshStatus();
            }
            return;
        }

        std::shared_ptr<WalletModel::UnlockContext> unlock;
        if (needs_unlock) {
            unlock = m_model->requestUnlockForAsync();
            if (!unlock->isValid()) return;
        }
        m_busy = true;
        QPointer<DigiDollarPaymasterWidget> guard{this};
        WalletModel* request_model = m_model;
        m_model->executeRpcAsync(std::move(command), std::move(params),
            [guard, request_model, output, handler = std::move(handler),
             unlock = std::move(unlock), show_error,
             error_handler = std::move(error_handler)](
                UniValue result, QString error) mutable {
                unlock.reset();
                if (!guard || guard->m_model != request_model) return;
                guard->m_busy = false;
                if (!error.isEmpty()) {
                    if (output) output->setPlainText(error);
                    if (error_handler) {
                        error_handler(error);
                    } else if (show_error) {
                        QMessageBox::warning(guard, guard->tr("Paymaster operation failed"), error);
                    } else {
                        guard->m_status->setText(
                            guard->tr("Provider status unavailable: %1").arg(error));
                    }
                    return;
                }
                if (output) output->setPlainText(QString::fromStdString(result.write(2)));
                if (handler) handler(result);
                if (!handler) guard->refreshStatus();
            });
    }

    WalletModel* m_model{nullptr};
    DigiDollarTab::PaymasterRpcExecutorForTesting m_rpc_executor_for_testing;
    bool m_busy{false};
    PaymasterSetupMode m_setup_mode{PaymasterSetupMode::UNDECIDED};
    QTabWidget* m_tabs{nullptr};
    QGroupBox* m_setup_choice{nullptr};
    QWidget* m_setup_content{nullptr};
    QWidget* m_configuration_page{nullptr};
    QWidget* m_safety_page{nullptr};
    QWidget* m_liquidity_page{nullptr};
    QWidget* m_finance_page{nullptr};
    QWidget* m_activity_page{nullptr};
    QPushButton* m_guided_setup{nullptr};
    QPushButton* m_expert_setup{nullptr};
    QPushButton* m_reopen_wizard{nullptr};
    QLabel* m_overview_offer_status{nullptr};
    QLabel* m_overview_safety_status{nullptr};
    QLabel* m_overview_liquidity_status{nullptr};
    QLabel* m_overview_operation_status{nullptr};
    QLabel* m_overview_finance_status{nullptr};
    QPushButton* m_overview_offer_action{nullptr};
    QPushButton* m_overview_safety_action{nullptr};
    QPushButton* m_overview_liquidity_action{nullptr};
    QPushButton* m_overview_operation_action{nullptr};
    QPushButton* m_overview_finance_action{nullptr};
    QGroupBox* m_overview_backup_notice{nullptr};
    QLabel* m_overview_backup_provider_id{nullptr};
    QPushButton* m_overview_backup_now{nullptr};
    QPushButton* m_overview_backup_external{nullptr};
    QLabel* m_wallet;
    QLabel* m_status;
    QLabel* m_identity;
    QLabel* m_pool;
    QLabel* m_next_step;
    QLabel* m_readiness_summary;
    QGroupBox* m_external_prerequisites_card{nullptr};
    QLabel* m_external_blockchain_status{nullptr};
    QLabel* m_external_txindex_status{nullptr};
    QLabel* m_external_broadcast_status{nullptr};
    QLabel* m_external_activation_status{nullptr};
    QLabel* m_external_oracle_status{nullptr};
    QLabel* m_external_oracle_note{nullptr};
    QLabel* m_external_other_heading{nullptr};
    QLabel* m_external_other_status{nullptr};
    QPlainTextEdit* m_readiness;
    QGroupBox* m_liquidity_maintenance_card{nullptr};
    QLabel* m_liquidity_maintenance_state{nullptr};
    QLabel* m_liquidity_maintenance_next_step{nullptr};
    QLabel* m_liquidity_maintenance_cost{nullptr};
    QPushButton* m_approve_liquidity_maintenance{nullptr};
    QPushButton* m_start;
    QPushButton* m_stop;
    QPushButton* m_enable;
    QLabel* m_enable_status;
    QPushButton* m_create_identity{nullptr};
    QLabel* m_offer_identity_status{nullptr};
    QLabel* m_offer_identity_id{nullptr};
    QLineEdit* m_display_name;
    QCheckBox* m_sponsored;
    QCheckBox* m_user_paid;
    QComboBox* m_scope;
    QSpinBox* m_fee_bps;
    QSpinBox* m_min_amount;
    QSpinBox* m_max_amount;
    QSpinBox* m_quote_ttl;
    QSpinBox* m_network_fee;
    QLabel* m_funding_model_status;
    QLabel* m_policy_summary;
    QSpinBox* m_admission_dgb;
    QSpinBox* m_operational_dgb;
    QSpinBox* m_admission_carriers;
    QSpinBox* m_operational_carriers;
    QLabel* m_liquidity_summary;
    QLabel* m_liquidity_current_status;
    QLabel* m_liquidity_preview_status;
    QLabel* m_liquidity_dgb_capacity_status{nullptr};
    QLabel* m_liquidity_carrier_capacity_status{nullptr};
    QPushButton* m_liquidity_dgb_capacity_action{nullptr};
    QPushButton* m_liquidity_carrier_capacity_action{nullptr};
    QPushButton* m_liquidity_advanced_toggle{nullptr};
    QCheckBox* m_automatic_replenishment{nullptr};
    QCheckBox* m_paid_maintenance_approved{nullptr};
    QPushButton* m_maintenance_limits_toggle{nullptr};
    QLineEdit* m_maintenance_fee_per_transaction{nullptr};
    QLineEdit* m_maintenance_fee_per_hour{nullptr};
    QLineEdit* m_maintenance_fee_per_day{nullptr};
    QPushButton* m_save_liquidity_policy_primary{nullptr};
    QPushButton* m_save_liquidity_policy{nullptr};
    QLabel* m_liquidity_policy_status{nullptr};
    QLabel* m_liquidity_target_save_status{nullptr};
    QLabel* m_liquidity_slot_status{nullptr};
    QLabel* m_liquidity_recycling_status{nullptr};
    QLabel* m_liquidity_budget_status{nullptr};
    QLabel* m_carrier_value_status{nullptr};
    QLabel* m_carrier_withdrawal_status{nullptr};
    QPushButton* m_preview_carrier_excess{nullptr};
    QPushButton* m_execute_carrier_excess{nullptr};
    QComboBox* m_release_carrier_select{nullptr};
    QPushButton* m_preview_carrier_release{nullptr};
    QPushButton* m_execute_carrier_release{nullptr};
    QPushButton* m_prepare_execute;
    QPushButton* m_rebalance_execute;
    QPushButton* m_prepare_preview{nullptr};
    QPushButton* m_rebalance_preview{nullptr};
    QPlainTextEdit* m_liquidity_output;
    QString m_prepare_preview_target;
    QString m_rebalance_preview_target;
    QComboBox* m_finance_period_select{nullptr};
    QPushButton* m_finance_refresh{nullptr};
    QPushButton* m_finance_export{nullptr};
    std::array<QLabel*, 4> m_finance_period_income{};
    std::array<QLabel*, 4> m_finance_period_cost{};
    std::array<QLabel*, 4> m_finance_period_transfers{};
    QLabel* m_finance_result_estimate{nullptr};
    QLabel* m_finance_model_breakdown{nullptr};
    QLabel* m_finance_pool_dgb{nullptr};
    QLabel* m_finance_pool_carrier{nullptr};
    QLabel* m_finance_pending_maintenance{nullptr};
    QGroupBox* m_finance_backup_notice{nullptr};
    QLabel* m_finance_backup_text{nullptr};
    QLabel* m_finance_backup_provider_id{nullptr};
    QPushButton* m_finance_backup_now{nullptr};
    QPushButton* m_finance_backup_external{nullptr};
    QPushButton* m_finance_withdraw_fees{nullptr};
    QPushButton* m_finance_release_carrier{nullptr};
    QPushButton* m_finance_review_dgb{nullptr};
    QPushButton* m_finance_add_liquidity{nullptr};
    QPushButton* m_finance_details_toggle{nullptr};
    QGroupBox* m_finance_history_notice{nullptr};
    QLabel* m_finance_history_notice_text{nullptr};
    QLabel* m_finance_history_status{nullptr};
    QTableWidget* m_finance_daily_totals{nullptr};
    QTableWidget* m_finance_events{nullptr};
    UniValue m_finance_last_result{UniValue::VOBJ};
    QString m_provider_id;
    bool m_backup_required{false};
    std::function<void()> m_provider_backup_request_handler;
    QLabel* m_activity_runtime_status;
    QLabel* m_activity_action_result;
    QLabel* m_activity_summary;
    QPushButton* m_operation_primary{nullptr};
    QGroupBox* m_reservations_group{nullptr};
    QGroupBox* m_recovery_group{nullptr};
    QPushButton* m_runtime_settings_toggle{nullptr};
    QGroupBox* m_runtime_settings_panel{nullptr};
    QComboBox* m_operation_mode_select{nullptr};
    QCheckBox* m_autostart{nullptr};
    QPushButton* m_save_runtime_settings{nullptr};
    QLabel* m_runtime_settings_result{nullptr};
    QGroupBox* m_request_processing_group{nullptr};
    QGroupBox* m_submit_processing_group{nullptr};
    QGroupBox* m_activity_result_group{nullptr};
    QPushButton* m_process_requests;
    QPushButton* m_process_submits;
    QPlainTextEdit* m_activity_output;
    FundingSafetyControls m_user_paid_safety;
    FundingSafetyControls m_public_sponsored_safety;
    FundingSafetyControls m_restricted_sponsored_safety;
    QSpinBox* m_max_active_quotes_total;
    QSpinBox* m_max_active_quotes_per_netgroup;
    QSpinBox* m_max_active_quotes_per_recipient;
    QSpinBox* m_max_quote_requests_per_netgroup;
    QLabel* m_provider_safety_status;
    QLabel* m_provider_safety_usage;
    QSpinBox* m_client_fee_per_transaction;
    QSpinBox* m_client_fee_per_day;
    QLabel* m_client_safety_status;
    QLabel* m_client_safety_mode;
    bool m_loading_policy{false};
    bool m_policy_loaded{false};
    bool m_policy_dirty{false};
    bool m_provider_safety_configured{false};
    bool m_provider_safety_dirty{false};
    bool m_loading_provider_safety{false};
    bool m_loading_liquidity_policy{false};
    bool m_liquidity_policy_configured{false};
    bool m_liquidity_targets_satisfy_provider_policy{false};
    bool m_liquidity_policy_dirty{false};
    bool m_client_safety_configured{false};
    bool m_client_safety_dirty{false};
    bool m_loading_client_safety{false};
    bool m_core_eligible{false};
    bool m_core_enabled{false};
    bool m_core_running{false};
    bool m_core_ready{false};
    bool m_core_locked{true};
    bool m_core_has_identity{false};
    OracleState m_oracle_state{OracleState::UNKNOWN};
    qint64 m_oracle_price_micro_usd{0};
    bool m_loading_runtime_settings{false};
    bool m_runtime_settings_dirty{false};
    bool m_autostart_enabled{false};
    QString m_operation_mode{QStringLiteral("automatic")};
    QString m_service_state{QStringLiteral("stopped")};
    QString m_last_service_error;
    QString m_maintenance_state{QStringLiteral("unavailable")};
    QStringList m_readiness_errors;
    qint64 m_carrier_base_cents{0};
    qint64 m_carrier_excess_cents{0};
    qint64 m_pending_successor_carrier_cents{0};
    qint64 m_pending_successor_dgb_satoshis{0};
    QString m_excess_plan_id;
    QString m_release_plan_id;
    qint64 m_excess_plan_expires_at{0};
    qint64 m_release_plan_expires_at{0};
    qint64 m_excess_preview_cents{0};
    qint64 m_excess_preview_fee_satoshis{0};
    int m_waiting_provider_requests{0};
    int m_waiting_provider_submits{0};
    int m_pool_admission_dgb{0};
    int m_pool_admission_carriers{0};
    int m_pool_operational_dgb{0};
    int m_pool_operational_carriers{0};
    bool m_pool_status_loaded{false};
    bool m_setup_waiting_for_confirmations{false};
    QTimer* m_setup_status_timer{nullptr};
};

DigiDollarTab::DigiDollarTab(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    m_tabWidget(nullptr),
    m_mainLayout(nullptr),
    m_overviewWidget(nullptr),
    m_receiveWidget(nullptr),
    m_sendWidget(nullptr),
    m_mintWidget(nullptr),
    m_redeemWidget(nullptr),
    m_positionsWidget(nullptr),
    m_transactionsWidget(nullptr),
    m_paymasterWidget(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr),
    m_platformStyle(platformStyle),
    m_stackedWidget(nullptr),
    m_activationLabel(nullptr),
    m_activationTimer(nullptr),
    m_activated(false)
{
    setupUI();
    connectSignals();
}

DigiDollarTab::~DigiDollarTab()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarTab::setupUI()
{
    setObjectName("digiDollarTab");

    // Create main layout
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // Create tab widget
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setObjectName("digiDollarSubTabs");
    m_tabWidget->tabBar()->setElideMode(Qt::ElideNone);      // Don't truncate tab text
    m_tabWidget->tabBar()->setExpanding(true);               // Expand tabs to fill width
    m_tabWidget->tabBar()->setUsesScrollButtons(true);       // Use scroll if needed

    // Create sub-widgets
    m_overviewWidget = new DigiDollarOverviewWidget(this);
    m_overviewWidget->setObjectName("overviewWidget");

    m_receiveWidget = new DigiDollarReceiveWidget();
    m_receiveWidget->setObjectName("receiveWidget");

    auto* send_scroll = new QScrollArea(m_tabWidget);
    send_scroll->setObjectName("digiDollarSendPage");
    send_scroll->setWidgetResizable(true);
    m_sendWidget = new DigiDollarSendWidget(m_platformStyle, send_scroll);
    m_sendWidget->setObjectName("sendWidget");
    send_scroll->setWidget(m_sendWidget);
    ConfigurePaymasterScrollArea(send_scroll, m_sendWidget);

    m_mintWidget = new DigiDollarMintWidget(this);
    m_mintWidget->setObjectName("mintWidget");

    m_redeemWidget = new DigiDollarRedeemWidget(this);
    m_redeemWidget->setObjectName("redeemWidget");

    m_positionsWidget = new DigiDollarPositionsWidget(this);
    m_positionsWidget->setObjectName("positionsWidget");

    m_transactionsWidget = new DigiDollarTransactionsWidget(this);
    m_transactionsWidget->setObjectName("transactionsWidget");

    m_paymasterWidget = new DigiDollarPaymasterWidget(this);
    m_paymasterWidget->setObjectName("paymasterWidget");
    m_paymasterWidget->setProviderBackupRequestHandler([this] {
        Q_EMIT providerWalletBackupRequested();
    });

    // Add tabs in order: $DD Overview, Send $DD, Receive $DD, Mint $DD, Redeem $DD, $DD Vault, $DD Transactions
    m_tabWidget->addTab(m_overviewWidget, tr("$DD Overview"));
    m_tabWidget->addTab(send_scroll, tr("Send $DD"));
    m_tabWidget->addTab(m_receiveWidget, tr("Receive $DD"));
    m_tabWidget->addTab(m_mintWidget, tr("Mint $DD"));
    m_tabWidget->addTab(m_redeemWidget, tr("Redeem $DD"));
    m_tabWidget->addTab(m_positionsWidget, tr("$DD Vault"));
    m_tabWidget->addTab(m_transactionsWidget, tr("$DD Transactions"));

    // Create activation status overlay
    m_activationLabel = new QLabel(this);
    m_activationLabel->setAlignment(Qt::AlignCenter);
    m_activationLabel->setWordWrap(true);
    m_activationLabel->setTextFormat(Qt::RichText);
    QFont labelFont = m_activationLabel->font();
    labelFont.setPointSize(14);
    m_activationLabel->setFont(labelFont);
    m_activationLabel->setStyleSheet("QLabel { color: #CCCCCC; padding: 40px; }");

    // Use stacked widget to switch between activation message and DD tabs
    m_stackedWidget = new QStackedWidget(this);
    m_stackedWidget->setObjectName("digiDollarStack");
    m_stackedWidget->addWidget(m_activationLabel);  // index 0: activation message
    m_stackedWidget->addWidget(m_tabWidget);         // index 1: DD functionality

    // Add stacked widget to main layout
    m_mainLayout->addWidget(m_stackedWidget);

    setLayout(m_mainLayout);

    // Start activation check timer (every 5 seconds)
    m_activationTimer = new QTimer(this);
    connect(m_activationTimer, &QTimer::timeout, this, &DigiDollarTab::checkActivationStatus);
    m_activationTimer->start(5000);

    // Check immediately
    checkActivationStatus();
}

void DigiDollarTab::connectSignals()
{
    // Connect tab change signal
    connect(m_tabWidget, &QTabWidget::currentChanged,
            this, &DigiDollarTab::onTabChanged);

    // Connect sub-widget signals
    if (m_overviewWidget) {
        connect(m_overviewWidget, &DigiDollarOverviewWidget::message,
                this, &DigiDollarTab::message);
        connect(m_overviewWidget, &DigiDollarOverviewWidget::recentTransactionActivated,
                this, &DigiDollarTab::showTransaction);
    }

    if (m_receiveWidget) {
        connect(m_receiveWidget, &DigiDollarReceiveWidget::message,
                this, &DigiDollarTab::message);
    }

    if (m_sendWidget) {
        connect(m_sendWidget, &DigiDollarSendWidget::message,
                this, &DigiDollarTab::message);
    }

    if (m_mintWidget) {
        connect(m_mintWidget, &DigiDollarMintWidget::message,
                this, &DigiDollarTab::message);
    }

    if (m_redeemWidget) {
        connect(m_redeemWidget, &DigiDollarRedeemWidget::message,
                this, &DigiDollarTab::message);
    }

    if (m_positionsWidget) {
        connect(m_positionsWidget, &DigiDollarPositionsWidget::message,
                this, &DigiDollarTab::message);
        connect(m_positionsWidget, &DigiDollarPositionsWidget::redeemRequested,
                this, &DigiDollarTab::onRedeemRequested);
    }

    if (m_transactionsWidget) {
        connect(m_transactionsWidget, &DigiDollarTransactionsWidget::message,
                this, &DigiDollarTab::message);
    }
}

void DigiDollarTab::setWalletModel(WalletModel* model)
{
    m_walletModel = model;

    // Pass wallet model to sub-widgets
    if (m_overviewWidget)
        m_overviewWidget->setWalletModel(model);
    if (m_receiveWidget)
        m_receiveWidget->setWalletModel(model);
    if (m_sendWidget)
        m_sendWidget->setWalletModel(model);
    if (m_mintWidget)
        m_mintWidget->setWalletModel(model);
    if (m_redeemWidget)
        m_redeemWidget->setWalletModel(model);
    if (m_positionsWidget)
        m_positionsWidget->setWalletModel(model);
    if (m_transactionsWidget)
        m_transactionsWidget->setWalletModel(model);
    if (m_paymasterWidget)
        m_paymasterWidget->setWalletModel(model);

    // Keep the tab's cached DGB/DD balance displays live while the user stays
    // on DigiDollar. The signal's WalletBalances payload is intentionally
    // discarded, and the slot re-reads each child widget's current balance.
    if (m_walletModel) {
        connect(m_walletModel, &WalletModel::balanceChanged,
                this, &DigiDollarTab::updateBalance);
    }

    // Update view when wallet model changes
    updateView();
}

void DigiDollarTab::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    // Pass client model to sub-widgets
    if (m_overviewWidget)
        m_overviewWidget->setClientModel(model);
    if (m_receiveWidget)
        m_receiveWidget->setClientModel(model);
    if (m_sendWidget)
        m_sendWidget->setClientModel(model);
    if (m_mintWidget)
        m_mintWidget->setClientModel(model);
    if (m_redeemWidget)
        m_redeemWidget->setClientModel(model);
    if (m_positionsWidget)
        m_positionsWidget->setClientModel(model);
    if (m_transactionsWidget)
        m_transactionsWidget->setClientModel(model);

    // The constructor cannot determine activation before ClientModel exists.
    // Waiting for the five-second activation timer left a newly opened wallet
    // showing placeholder or empty DD pages even when activation was already
    // complete. Evaluate it immediately once the node interface is available.
    checkActivationStatus();
}

void DigiDollarTab::updateView()
{
    // Some child refreshes need wallet or RPC locks. Refreshing every hidden
    // page made navigation wait behind unrelated history work. Queue the
    // selected page instead: this also guarantees that its own isVisible()
    // guard observes the final QStackedWidget/QTabWidget state.
    scheduleCurrentPageRefresh();
}

void DigiDollarTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    scheduleCurrentPageRefresh();
}

void DigiDollarTab::scheduleCurrentPageRefresh()
{
    if (m_currentPageRefreshScheduled || !isVisible()) return;

    m_currentPageRefreshScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_currentPageRefreshScheduled = false;
        if (!isVisible() || !m_stackedWidget ||
            m_stackedWidget->currentWidget() != m_tabWidget) {
            return;
        }
        refreshCurrentPage();
    });
}

void DigiDollarTab::refreshCurrentPage()
{
    if (!m_tabWidget) return;

    switch (m_tabWidget->currentIndex()) {
    case 0: // Overview
        if (m_overviewWidget) m_overviewWidget->updateView();
        break;
    case 1: // Send
        if (m_sendWidget) m_sendWidget->updateView();
        break;
    case 2: // Receive
        if (m_receiveWidget) m_receiveWidget->updateView();
        break;
    case 3: // Mint
        if (m_mintWidget) m_mintWidget->updateView();
        break;
    case 4: // Redeem
        if (m_redeemWidget) m_redeemWidget->updateView();
        break;
    case 5: // Vault
        if (m_positionsWidget) m_positionsWidget->updateView();
        break;
    case 6: // Transactions
        if (m_transactionsWidget) m_transactionsWidget->updateView();
        break;
    case 7: // Paymaster Network
        if (m_paymasterWidget) m_paymasterWidget->refreshStatus();
        break;
    default:
        break;
    }
}

void DigiDollarTab::setPaymasterLiquidityStatusForTesting(
    const UniValue& status)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setLiquidityStatusForTesting(status);
    }
}

void DigiDollarTab::setPaymasterLiquidityPoolForTesting(
    const UniValue& pool_info)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setLiquidityPoolForTesting(pool_info);
    }
}

void DigiDollarTab::setPaymasterReadinessStatusForTesting(
    const UniValue& status)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setReadinessStatusForTesting(status);
    }
}

void DigiDollarTab::setPaymasterStartResultForTesting(
    const UniValue& result)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setStartResultForTesting(result);
    }
}

void DigiDollarTab::setPaymasterRpcExecutorForTesting(
    PaymasterRpcExecutorForTesting executor)
{
    if (m_paymasterWidget) {
        m_paymasterWidget->setRpcExecutorForTesting(std::move(executor));
    }
}

void DigiDollarTab::incomingDDTransaction(const QString& date, const QString& amount,
                                         const QString& type, const QString& address)
{
    // Notify overview widget of incoming transaction
    if (m_overviewWidget) {
        m_overviewWidget->incomingDDTransaction(date, amount, type, address);
    }

    // Update balance displays
    updateBalance();
}

void DigiDollarTab::updateBalance()
{
    if (!isVisible() || !m_tabWidget) return;

    // The Mint balance comes from WalletModel's already cached DGB balance and
    // is safe to keep current while hidden. DD balance queries may acquire
    // cs_wallet, so perform those only for the page the user can see.
    if (m_mintWidget) m_mintWidget->updateBalance();
    switch (m_tabWidget->currentIndex()) {
    case 0:
        if (m_overviewWidget) m_overviewWidget->updateBalance();
        break;
    case 1:
        if (m_sendWidget) m_sendWidget->updateBalance();
        break;
    case 4:
        if (m_redeemWidget) m_redeemWidget->updateBalance();
        break;
    default:
        break;
    }
}

void DigiDollarTab::updateOraclePrice()
{
    if (!isVisible() || !m_tabWidget) return;
    switch (m_tabWidget->currentIndex()) {
    case 0:
        if (m_overviewWidget) m_overviewWidget->updateOraclePrice();
        break;
    case 1:
        if (m_sendWidget) m_sendWidget->updateOraclePrice();
        break;
    case 3:
        if (m_mintWidget) m_mintWidget->updateOraclePrice();
        break;
    default:
        break;
    }
}

void DigiDollarTab::updateSystemHealth()
{
    if (isVisible() && m_tabWidget && m_tabWidget->currentIndex() == 0 && m_overviewWidget) {
        m_overviewWidget->updateSystemHealth();
    }
}

void DigiDollarTab::updatePositions()
{
    if (!isVisible() || !m_tabWidget) return;
    if (m_tabWidget->currentIndex() == 5 && m_positionsWidget) {
        m_positionsWidget->updatePositions();
    } else if (m_tabWidget->currentIndex() == 4 && m_redeemWidget) {
        m_redeemWidget->updatePositions();
    }
}

void DigiDollarTab::onTabChanged(int index)
{
    Q_UNUSED(index);
    // currentChanged is emitted before the newly selected child necessarily
    // reports isVisible(). A queued refresh avoids losing the request and
    // prevents the five-second child timers from becoming the accidental
    // first-load mechanism.
    scheduleCurrentPageRefresh();
}

void DigiDollarTab::setPrivacy(bool privacy)
{
    m_privacy = privacy;

    // Relay privacy setting to all sub-widgets
    if (m_overviewWidget)
        m_overviewWidget->setPrivacy(privacy);
    if (m_sendWidget)
        m_sendWidget->setPrivacy(privacy);
    if (m_mintWidget)
        m_mintWidget->setPrivacy(privacy);
    if (m_redeemWidget)
        m_redeemWidget->setPrivacy(privacy);
    if (m_positionsWidget)
        m_positionsWidget->setPrivacy(privacy);
    if (m_transactionsWidget)
        m_transactionsWidget->setPrivacy(privacy);
}

void DigiDollarTab::setPaymasterOperatorVisible(bool visible)
{
    const int index = m_tabWidget->indexOf(m_paymasterWidget);
    if (visible && index < 0) {
        m_tabWidget->addTab(m_paymasterWidget, tr("Paymaster Network"));
    } else if (!visible && index >= 0) {
        m_tabWidget->removeTab(index);
    }
}

void DigiDollarTab::onRedeemRequested(const QString &positionId)
{
    // Switch to Redeem tab (index 4) and populate the position ID
    if (m_redeemWidget) {
        m_redeemWidget->setPosition(positionId);
        m_tabWidget->setCurrentIndex(4); // Redeem tab
    }
}

void DigiDollarTab::showTransaction(const QString& txid)
{
    if (!m_tabWidget || !m_transactionsWidget || txid.isEmpty()) {
        return;
    }

    if (m_stackedWidget) {
        m_stackedWidget->setCurrentWidget(m_tabWidget);
    }
    m_tabWidget->setCurrentWidget(m_transactionsWidget);
    m_transactionsWidget->updateView();
    m_transactionsWidget->focusTransaction(txid);
}

void DigiDollarTab::checkActivationStatus()
{
    if (m_activated) {
        // Already activated, stop checking
        if (m_activationTimer) m_activationTimer->stop();
        return;
    }

    QString status = getDeploymentStatus();
    bool active = isDigiDollarActive();

    if (active) {
        m_activated = true;
        m_stackedWidget->setCurrentIndex(1); // Show DD tabs
        if (m_activationTimer) m_activationTimer->stop();
        // The tab page becomes visible only after the stack transition. Queue
        // its complete initial load instead of issuing child updates too early.
        scheduleCurrentPageRefresh();
        return;
    }

    // Build activation status message
    QString msg = QString(
        "<div style='text-align: center;'>"
        "<h2 style='color: #0066CC;'>💎 DigiDollar</h2>"
        "<p style='font-size: 16px; margin: 20px 0;'>"
        "DigiDollar is not yet active on this blockchain.</p>"
        "<p style='font-size: 13px; color: #999999;'>"
        "Deployment Status: <b>%1</b></p>"
        "<p style='font-size: 12px; color: #777777; margin-top: 20px;'>"
        "DigiDollar activates at a fixed block height on this network.<br>"
        "Use <code>getdigidollardeploymentinfo</code> for details.</p>"
        "</div>"
    ).arg(status.toUpper());

    m_activationLabel->setText(msg);
    m_stackedWidget->setCurrentIndex(0); // Show activation message
}

QString DigiDollarTab::getDeploymentStatus() const
{
    if (!m_clientModel) return "unknown";

    try {
        interfaces::Node& node = m_clientModel->node();
        node::NodeContext* ctx = node.context();
        if (!ctx || !ctx->chainman) return "unknown";

        ChainstateManager& chainman = *ctx->chainman;
        // DigiDollar is a buried deployment (BIP90): status is a pure height
        // comparison against the hardcoded activation height.
        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
        return DigiDollar::IsDigiDollarEnabled(tip, chainman) ? "active" : "defined";
    } catch (...) {
        return "unknown";
    }
}

bool DigiDollarTab::isDigiDollarActive() const
{
    if (!m_clientModel) return false;

    try {
        interfaces::Node& node = m_clientModel->node();
        // Use the node's context to check actual BIP9 status
        node::NodeContext* ctx = node.context();
        if (!ctx || !ctx->chainman) return false;

        ChainstateManager& chainman = *ctx->chainman;
        const CBlockIndex* tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
        if (!tip) return false;

        return DigiDollar::IsDigiDollarEnabled(tip, chainman);
    } catch (...) {
        return false;
    }
}

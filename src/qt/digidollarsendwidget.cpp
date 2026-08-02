// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/digidollarsendwidget.h>
#include <qt/ddaddressbookpage.h>
#include <qt/digidollarstatus.h>

#include <qt/walletmodel.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/digidollarcoincontroldialog.h>
#include <qt/platformstyle.h>
#include <wallet/ddcoincontrol.h>
#include <wallet/digidollarwallet.h>
#include <consensus/amount.h>
#include <base58.h>
#include <logging.h>
#include <kernel/chainparams.h>
#include <oracle/mock_oracle.h>
#include <interfaces/node.h>
#include <univalue.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QValidator>
#include <QFont>
#include <QRegularExpression>
#include <QMessageBox>
#include <QToolButton>
#include <QApplication>
#include <QClipboard>
#include <QScrollArea>
#include <QSpacerItem>
#include <QSizePolicy>
#include <QPalette>
#include <QProgressDialog>
#include <QPointer>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QRadioButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QUuid>
#include <QWheelEvent>

using namespace std::chrono_literals;

namespace {
class NoWheelSpinBox final : public QSpinBox
{
public:
    explicit NoWheelSpinBox(QWidget* parent) : QSpinBox(parent) {}

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

class FeeFundingCard final : public QFrame
{
public:
    explicit FeeFundingCard(QWidget* parent) : QFrame(parent) {}

    void setChoiceButton(QRadioButton* button) { m_button = button; }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_button && isEnabled()) {
            m_button->setChecked(true);
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    QRadioButton* m_button{nullptr};
};
} // namespace

DigiDollarSendWidget::DigiDollarSendWidget(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    m_mainLayout(nullptr),
    m_addressFrame(nullptr),
    m_addressLayout(nullptr),
    m_addressLabel(nullptr),
    m_addressEdit(nullptr),
    m_pasteAddressButton(nullptr),
    m_addressBookButton(nullptr),
    m_addressValidationLabel(nullptr),
    m_amountFrame(nullptr),
    m_amountLayout(nullptr),
    m_amountLabel(nullptr),
    m_amountEdit(nullptr),
    m_amountSuffix(nullptr),
    m_useAvailableBalanceButton(nullptr),
    m_usdEquivalentLabel(nullptr),
    m_usdEquivalentValue(nullptr),
    m_availableBalanceLabel(nullptr),
    m_availableBalanceValue(nullptr),
    m_noteFrame(nullptr),
    m_noteLayout(nullptr),
    m_noteLabel(nullptr),
    m_noteEdit(nullptr),
    m_feeFrame(nullptr),
    m_feeLayout(nullptr),
    m_feeHeading(nullptr),
    m_feeChoicesFrame(nullptr),
    m_feeChoicesLayout(nullptr),
    m_dgbFeeCard(nullptr),
    m_autoFeeCard(nullptr),
    m_paymasterFeeCard(nullptr),
    m_feeLabel(nullptr),
    m_feeValue(nullptr),
    m_totalLabel(nullptr),
    m_totalValue(nullptr),
    m_feeIntroduction(nullptr),
    m_dgbFeeRadio(nullptr),
    m_autoFeeRadio(nullptr),
    m_paymasterFeeRadio(nullptr),
    m_feeModeExplanation(nullptr),
    m_feeSummary(nullptr),
    m_subtractPaymasterFeeCheck(nullptr),
    m_paymasterExplanationButton(nullptr),
    m_advancedPaymasterButton(nullptr),
    m_feeModeCombo(nullptr),
    m_advancedPaymasterFrame(nullptr),
    m_privacyCombo(nullptr),
    m_selectionCombo(nullptr),
    m_feeCapSpin(nullptr),
    m_maxAttemptsSpin(nullptr),
    m_refreshOffersButton(nullptr),
    m_offersStatus(nullptr),
    m_offersTable(nullptr),
    m_clientSafetyFrame(nullptr),
    m_clientSafetyStatus(nullptr),
    m_clientSafetyDetails(nullptr),
    m_configureClientSafetyButton(nullptr),
    m_paymasterSessionFrame(nullptr),
    m_paymasterStateValue(nullptr),
    m_paymasterTransferValue(nullptr),
    m_paymasterIdentityValue(nullptr),
    m_paymasterCostValue(nullptr),
    m_paymasterExpiryValue(nullptr),
    m_retrySessionButton(nullptr),
    m_fallbackSessionButton(nullptr),
    m_recoverSessionButton(nullptr),
    m_abandonSessionButton(nullptr),
    m_cancelQuoteButton(nullptr),
    m_paymasterNextStepValue(nullptr),
    m_paymasterPrimaryButton(nullptr),
    m_paymasterMoreButton(nullptr),
    m_paymasterTechnicalButton(nullptr),
    m_paymasterSecondaryActions(nullptr),
    m_paymasterTechnicalDetails(nullptr),
    m_buttonFrame(nullptr),
    m_buttonLayout(nullptr),
    m_sendButton(nullptr),
    m_clearButton(nullptr),
    m_coinControlFrame(nullptr),
    m_coinControlLayout(nullptr),
    m_coinControlButton(nullptr),
    m_coinControlQuantityLabel(nullptr),
    m_coinControlAmountLabel(nullptr),
    m_addressValidator(nullptr),
    m_amountValidator(nullptr),
    m_walletModel(nullptr),
    m_clientModel(nullptr),
    m_platformStyle(platformStyle),
    m_availableBalance(0.0),
    m_oraclePrice(1.0),
    m_estimatedFee(0.001),  // TODO: Implement dynamic fee estimation based on transaction size and network conditions
    m_paymasterPollTimer(new QTimer(this))
{
    setupUI();
    connectSignals();
    // REMOVED: applyTheme() - Let CSS handle all theming
}

DigiDollarSendWidget::~DigiDollarSendWidget()
{
    // Qt will handle cleanup of child widgets
}

void DigiDollarSendWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateFeeChoiceLayout();
}

void DigiDollarSendWidget::updateFeeChoiceLayout()
{
    if (!m_feeChoicesLayout || !m_dgbFeeCard || !m_autoFeeCard || !m_paymasterFeeCard) return;

    const bool stack = width() < 980;
    const std::array<QFrame*, 3> cards{m_dgbFeeCard, m_autoFeeCard, m_paymasterFeeCard};
    for (QFrame* card : cards) m_feeChoicesLayout->removeWidget(card);
    for (int column = 0; column < 3; ++column) m_feeChoicesLayout->setColumnStretch(column, 0);
    for (int index = 0; index < static_cast<int>(cards.size()); ++index) {
        m_feeChoicesLayout->addWidget(cards[index], stack ? index : 0, stack ? 0 : index);
        if (!stack) m_feeChoicesLayout->setColumnStretch(index, 1);
    }
}

void DigiDollarSendWidget::setupUI()
{
    // Create main layout - compact like DGB tabs
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setSpacing(0);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // Create validators
    m_addressValidator = new DigiDollarAddressValidator(this);
    // DD amounts are in dollars with max 2 decimal places (cents precision)
    // Send limits: $1 minimum (dust threshold), $100,000 maximum
    m_amountValidator = new AmountValidator(1.00, 100000.00, 2, this);

    // Setup sections
    setupCoinControlSection();
    setupAddressSection();
    setupAmountSection();
    setupNoteSection();
    setupFeeSection();
    setupButtonSection();

    // Add stretch to push content to top
    m_mainLayout->addStretch();

    setLayout(m_mainLayout);
}

void DigiDollarSendWidget::setupCoinControlSection()
{
    // Create coin control frame
    m_coinControlFrame = new QFrame(this);
    m_coinControlFrame->setObjectName("coinControlFrame");
    m_coinControlFrame->setFrameStyle(QFrame::StyledPanel);
    m_coinControlFrame->setFrameShadow(QFrame::Sunken);

    m_coinControlLayout = new QHBoxLayout(m_coinControlFrame);
    m_coinControlLayout->setSpacing(10);
    m_coinControlLayout->setContentsMargins(10, 8, 10, 8);

    // "Inputs..." button to open coin control dialog
    m_coinControlButton = new QPushButton(tr("Inputs..."), this);
    m_coinControlButton->setObjectName("coinControlButton");
    m_coinControlButton->setToolTip(tr("Manually select $DD inputs to spend"));
    m_coinControlButton->setMinimumWidth(80);
    m_coinControlLayout->addWidget(m_coinControlButton);

    // Quantity label (number of selected inputs)
    m_coinControlQuantityLabel = new QLabel(this);
    m_coinControlQuantityLabel->setObjectName("coinControlQuantityLabel");
    m_coinControlQuantityLabel->setText(tr("automatically selected"));
    m_coinControlLayout->addWidget(m_coinControlQuantityLabel);

    // Amount label (total selected DD amount)
    m_coinControlAmountLabel = new QLabel(this);
    m_coinControlAmountLabel->setObjectName("coinControlAmountLabel");
    m_coinControlAmountLabel->setText(QString());
    m_coinControlLayout->addWidget(m_coinControlAmountLabel);

    m_coinControlLayout->addStretch();

    m_mainLayout->addWidget(m_coinControlFrame);

    // Initially visible - will be hidden if coin control is disabled in settings
    m_coinControlFrame->setVisible(true);
}

void DigiDollarSendWidget::setupAddressSection()
{
    // Create address frame
    m_addressFrame = new QFrame(this);
    m_addressFrame->setFrameStyle(QFrame::StyledPanel);
    m_addressFrame->setFrameShadow(QFrame::Sunken);
    m_addressFrame->setObjectName("addressFrame");

    m_addressLayout = new QGridLayout(m_addressFrame);
    m_addressLayout->setSpacing(8);
    m_addressLayout->setContentsMargins(10, 10, 10, 10);
    m_addressLayout->setHorizontalSpacing(12);
    m_addressLayout->setVerticalSpacing(8);

    // Address label and input with paste button
    m_addressLabel = new QLabel(tr("Pay To:"), this);
    m_addressLabel->setToolTip(tr("Enter the DigiDollar address of the recipient"));
    m_addressLabel->setBuddy(m_addressEdit);

    // Create horizontal layout for address input and paste button
    QHBoxLayout* addressInputLayout = new QHBoxLayout();
    addressInputLayout->setSpacing(0);

    m_addressEdit = new QLineEdit(this);
    m_addressEdit->setObjectName("addressEdit");
    m_addressEdit->setValidator(m_addressValidator);
    m_addressEdit->setPlaceholderText("DD1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4");
    m_addressEdit->setToolTip(tr("The DigiDollar address to send the payment to.\n\nValid formats:\n• DD... (Mainnet)\n• TD... (Testnet)\n• RD... (Regtest)"));
    m_addressEdit->setFocusPolicy(Qt::StrongFocus);
    m_addressEdit->setAttribute(Qt::WA_InputMethodEnabled, true);
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_addressEdit->setFont(monospaceFont);

    m_pasteAddressButton = new QToolButton(this);
    m_pasteAddressButton->setToolTip(tr("Paste address from clipboard (Alt+P)"));
    m_pasteAddressButton->setIconSize(QSize(22, 22));
    m_pasteAddressButton->setShortcut(QKeySequence("Alt+P"));
    m_pasteAddressButton->setIcon(m_platformStyle->SingleColorIcon(":/icons/editpaste"));

    m_addressBookButton = new QToolButton(this);
    m_addressBookButton->setToolTip(tr("Choose from address book (Alt+A)"));
    m_addressBookButton->setIconSize(QSize(22, 22));
    m_addressBookButton->setShortcut(QKeySequence("Alt+A"));
    m_addressBookButton->setIcon(m_platformStyle->SingleColorIcon(":/icons/address-book"));

    addressInputLayout->addWidget(m_addressEdit);
    addressInputLayout->addWidget(m_addressBookButton);
    addressInputLayout->addWidget(m_pasteAddressButton);

    m_addressLayout->addWidget(m_addressLabel, 0, 0);
    m_addressLayout->addLayout(addressInputLayout, 0, 1);

    // Address validation label
    m_addressValidationLabel = new QLabel(this);
    m_addressValidationLabel->setObjectName("addressValidationLabel");
    m_addressValidationLabel->setText(tr("Enter a valid DigiDollar address (DD, TD, or RD prefix)"));
    m_addressValidationLabel->setWordWrap(true);
    DigiDollarStatus::SetText(m_addressValidationLabel, DigiDollarStatus::Kind::INFO);
    m_addressLayout->addWidget(m_addressValidationLabel, 1, 1);

    m_mainLayout->addWidget(m_addressFrame);
}

void DigiDollarSendWidget::setupAmountSection()
{
    // Create amount frame
    m_amountFrame = new QFrame(this);
    m_amountFrame->setFrameStyle(QFrame::StyledPanel);
    m_amountFrame->setFrameShadow(QFrame::Sunken);
    m_amountFrame->setObjectName("amountFrame");

    m_amountLayout = new QGridLayout(m_amountFrame);
    m_amountLayout->setSpacing(8);
    m_amountLayout->setContentsMargins(10, 10, 10, 10);
    m_amountLayout->setHorizontalSpacing(12);
    m_amountLayout->setVerticalSpacing(8);

    // Amount input with use available balance button
    m_amountLabel = new QLabel(tr("Amount:"), this);
    m_amountLabel->setToolTip(tr("Enter the amount of DigiDollar to send"));
    m_amountLabel->setBuddy(m_amountEdit);

    // Create horizontal layout for amount input and buttons
    QHBoxLayout* amountInputLayout = new QHBoxLayout();
    amountInputLayout->setSpacing(8);

    m_amountEdit = new QLineEdit(this);
    m_amountEdit->setObjectName("amountEdit");
    m_amountEdit->setValidator(m_amountValidator);
    m_amountEdit->setPlaceholderText("0.00");
    m_amountEdit->setToolTip(tr("The amount of DigiDollar to send.\n\n• Minimum: 1.00 $DD\n• Maximum: 100,000.00 $DD\n• Up to 2 decimal places (cents)"));
    m_amountEdit->setFocusPolicy(Qt::StrongFocus);
    m_amountEdit->setAttribute(Qt::WA_InputMethodEnabled, true);
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_amountEdit->setFont(monospaceFont);

    // Add stretch to push the button to the right
    m_useAvailableBalanceButton = new QPushButton(tr("Use available balance"), this);
    m_useAvailableBalanceButton->setObjectName("useAvailableBalanceButton");
    m_useAvailableBalanceButton->setToolTip(tr("Use the full available DigiDollar balance (fees are paid in DGB)"));

    amountInputLayout->addWidget(m_amountEdit, 0);
    amountInputLayout->addWidget(m_useAvailableBalanceButton, 1);

    m_amountLayout->addWidget(m_amountLabel, 0, 0);
    m_amountLayout->addLayout(amountInputLayout, 0, 1);

    // USD equivalent display
    m_usdEquivalentLabel = new QLabel(tr("$USD Equivalent:"), this);
    m_usdEquivalentLabel->setObjectName("usdEquivalentLabel");
    m_usdEquivalentLabel->setToolTip(tr("Equivalent value in US Dollars (DigiDollar is pegged to $1 USD)"));
    m_usdEquivalentValue = new QLabel("0.00 $USD", this);
    m_usdEquivalentValue->setObjectName("usdEquivalentValue");
    m_usdEquivalentValue->setFont(monospaceFont);
    m_usdEquivalentValue->setToolTip(tr("USD value updates in real-time as you type"));

    m_amountLayout->addWidget(m_usdEquivalentLabel, 1, 0);
    m_amountLayout->addWidget(m_usdEquivalentValue, 1, 1);

    // Available balance display
    m_availableBalanceLabel = new QLabel(tr("Available:"), this);
    m_availableBalanceLabel->setObjectName("availableBalanceLabel");
    m_availableBalanceLabel->setToolTip(tr("Your current available DigiDollar balance"));
    m_availableBalanceValue = new QLabel("0.00 $DD", this);
    m_availableBalanceValue->setObjectName("availableBalanceValue");
    m_availableBalanceValue->setFont(monospaceFont);
    m_availableBalanceValue->setToolTip(tr("Your current spendable DigiDollar balance"));

    m_amountLayout->addWidget(m_availableBalanceLabel, 2, 0);
    m_amountLayout->addWidget(m_availableBalanceValue, 2, 1);

    // Set column widths to prevent layout distortion on initial display
    m_amountLayout->setColumnMinimumWidth(0, 110);  // Label column
    m_amountLayout->setColumnStretch(0, 0);
    m_amountLayout->setColumnStretch(1, 1);

    m_mainLayout->addWidget(m_amountFrame);
}

void DigiDollarSendWidget::setupNoteSection()
{
    m_noteFrame = new QFrame(this);
    m_noteFrame->setFrameStyle(QFrame::StyledPanel);
    m_noteFrame->setFrameShadow(QFrame::Sunken);
    m_noteFrame->setObjectName("noteFrame");

    m_noteLayout = new QGridLayout(m_noteFrame);
    m_noteLayout->setSpacing(8);
    m_noteLayout->setContentsMargins(10, 10, 10, 10);
    m_noteLayout->setHorizontalSpacing(12);
    m_noteLayout->setVerticalSpacing(8);

    m_noteLabel = new QLabel(tr("Note:"), this);
    m_noteLabel->setToolTip(tr("Enter a local note for this DigiDollar transaction"));

    m_noteEdit = new QLineEdit(this);
    m_noteEdit->setObjectName("noteEdit");
    m_noteEdit->setPlaceholderText(tr("Enter a local note for this transaction"));
    m_noteEdit->setMaxLength(256);
    m_noteEdit->setToolTip(tr("Saved locally with this DigiDollar transaction"));

    m_noteLayout->addWidget(m_noteLabel, 0, 0);
    m_noteLayout->addWidget(m_noteEdit, 0, 1);

    m_noteLayout->setColumnMinimumWidth(0, 110);
    m_noteLayout->setColumnStretch(0, 0);
    m_noteLayout->setColumnStretch(1, 1);

    m_mainLayout->addWidget(m_noteFrame);
}

void DigiDollarSendWidget::setupFeeSection()
{
    // This is intentionally a user decision about who funds the network fee,
    // not a raw protocol-mode selector. Own DGB remains the safe default;
    // Paymaster-specific limits and offer details stay hidden until relevant.
    m_feeFrame = new QFrame(this);
    m_feeFrame->setFrameStyle(QFrame::StyledPanel);
    m_feeFrame->setFrameShadow(QFrame::Sunken);
    m_feeFrame->setObjectName("feeFrame");

    m_feeLayout = new QGridLayout(m_feeFrame);
    m_feeLayout->setSpacing(8);
    m_feeLayout->setContentsMargins(10, 10, 10, 10);
    m_feeLayout->setHorizontalSpacing(12);
    m_feeLayout->setVerticalSpacing(8);

    m_feeHeading = new QLabel(tr("How should the network fee be paid?"), m_feeFrame);
    m_feeHeading->setObjectName("feeFundingHeading");
    QFont heading_font = m_feeHeading->font();
    heading_font.setBold(true);
    heading_font.setPointSize(heading_font.pointSize() + 1);
    m_feeHeading->setFont(heading_font);
    m_feeLayout->addWidget(m_feeHeading, 0, 0, 1, 2);

    m_feeIntroduction = new QLabel(tr(
        "Every $DD transfer needs a DigiByte network fee. This fee is paid "
        "separately and is never deducted from the amount received."), m_feeFrame);
    m_feeIntroduction->setObjectName("feeFundingIntroduction");
    m_feeIntroduction->setWordWrap(true);
    m_feeLayout->addWidget(m_feeIntroduction, 1, 0, 1, 2);

    m_feeChoicesFrame = new QFrame(m_feeFrame);
    m_feeChoicesFrame->setObjectName("feeFundingChoices");
    m_feeChoicesLayout = new QGridLayout(m_feeChoicesFrame);
    m_feeChoicesLayout->setContentsMargins(0, 0, 0, 0);
    m_feeChoicesLayout->setHorizontalSpacing(10);
    m_feeChoicesLayout->setVerticalSpacing(8);
    m_feeLayout->addWidget(m_feeChoicesFrame, 2, 0, 1, 2);

    auto* choices = new QButtonGroup(this);
    auto add_choice = [this, choices](QFrame*& card_member, QRadioButton*& radio,
                                      const QString& object_name,
                                      const QString& title, const QString& description,
                                      bool include_paymaster_help = false) {
        auto* card = new FeeFundingCard(m_feeFrame);
        card_member = card;
        card->setObjectName(object_name + QStringLiteral("Card"));
        card->setProperty("feeChoice", true);
        card->setProperty("feeSelected", false);
        card->setFrameShape(QFrame::StyledPanel);
        card->setCursor(Qt::PointingHandCursor);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        card->setMinimumHeight(112);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(14, 10, 14, 10);
        layout->setSpacing(5);
        auto* title_row = new QHBoxLayout();
        radio = new QRadioButton(title, card);
        radio->setObjectName(object_name);
        radio->setAccessibleName(title);
        radio->setAccessibleDescription(description);
        QFont title_font = radio->font();
        title_font.setBold(true);
        radio->setFont(title_font);
        auto* selected_badge = new QLabel(tr("Selected"), card);
        selected_badge->setProperty("feeChoiceBadge", true);
        selected_badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        selected_badge->hide();
        title_row->addWidget(radio, 1);
        title_row->addWidget(selected_badge, 0, Qt::AlignRight | Qt::AlignVCenter);
        auto* help = new QLabel(description, card);
        help->setObjectName(object_name + QStringLiteral("Description"));
        help->setProperty("feeChoiceDescription", true);
        help->setWordWrap(true);
        help->setTextInteractionFlags(Qt::TextSelectableByMouse);
        help->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addLayout(title_row);
        layout->addWidget(help);
        if (include_paymaster_help) {
            m_paymasterExplanationButton = new QPushButton(tr("Learn how Paymasters work"), card);
            m_paymasterExplanationButton->setObjectName("paymasterExplanationButton");
            m_paymasterExplanationButton->setAccessibleDescription(
                tr("Explain Paymaster fees, privacy and transaction checks"));
            layout->addWidget(m_paymasterExplanationButton, 0, Qt::AlignLeft);
        }
        choices->addButton(radio);
        card->setChoiceButton(radio);
        connect(radio, &QRadioButton::toggled, card, [card, selected_badge](bool checked) {
            card->setProperty("feeSelected", checked);
            selected_badge->setVisible(checked);
            card->style()->unpolish(card);
            card->style()->polish(card);
            card->update();
        });
    };
    add_choice(m_dgbFeeCard, m_dgbFeeRadio, QStringLiteral("feeFundingDgb"),
               tr("Own DGB"),
               tr("Recommended · estimated ~0.1 DGB · no additional $DD fee."));
    add_choice(m_autoFeeCard, m_autoFeeRadio, QStringLiteral("feeFundingAuto"),
               tr("Automatic"),
               tr("Use own DGB first; find a Paymaster only when suitable DGB is insufficient."));
    add_choice(m_paymasterFeeCard, m_paymasterFeeRadio, QStringLiteral("feeFundingPaymaster"),
               tr("Paymaster"),
               tr("A provider supplies DGB; the additional $DD service fee may be zero."), true);
    updateFeeChoiceLayout();
    m_dgbFeeRadio->setChecked(true);

    // Keep the canonical RPC values in one internal control. It is deliberately
    // hidden; the explanatory radio choices above are the user-facing surface.
    m_feeModeCombo = new NoWheelComboBox(this);
    m_feeModeCombo->setObjectName("paymasterFeeMode");
    m_feeModeCombo->addItem(tr("Own DGB"), QStringLiteral("dgb"));
    m_feeModeCombo->addItem(tr("Automatic (DGB first, Paymaster if needed)"), QStringLiteral("auto"));
    m_feeModeCombo->addItem(tr("Paymaster Network"), QStringLiteral("paymaster"));
    m_feeModeCombo->hide();

    m_feeModeExplanation = new QLabel(m_feeFrame);
    m_feeModeExplanation->setObjectName("feeFundingModeExplanation");
    m_feeModeExplanation->setWordWrap(true);
    m_feeModeExplanation->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Retained as a hidden compatibility/accessibility source for existing
    // translations and tests. The visible summary below now carries the one
    // contextual explanation, avoiding duplicate paragraphs.
    m_feeModeExplanation->hide();

    m_feeSummary = new QLabel(m_feeFrame);
    m_feeSummary->setObjectName("feeFundingSummary");
    m_feeSummary->setWordWrap(true);
    m_feeSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_feeSummary->setFrameShape(QFrame::StyledPanel);
    m_feeSummary->setMargin(8);
    m_feeLayout->addWidget(m_feeSummary, 3, 0, 1, 2);

    m_subtractPaymasterFeeCheck = new QCheckBox(
        tr("Deduct the Paymaster fee from the entered amount — the recipient receives less."),
        m_feeFrame);
    m_subtractPaymasterFeeCheck->setObjectName("subtractPaymasterFeeFromAmount");
    m_subtractPaymasterFeeCheck->setToolTip(tr(
        "The amount entered above becomes the exact maximum $DD outflow. Core accepts only "
        "an offer whose rounded service fee and recipient amount add up exactly to it."));
    m_subtractPaymasterFeeCheck->setAccessibleDescription(tr(
        "Use the entered amount as an exact total and subtract the authenticated "
        "Paymaster service fee before paying the recipient"));
    m_feeLayout->addWidget(m_subtractPaymasterFeeCheck, 4, 0);

    m_advancedPaymasterButton = new QPushButton(tr("Advanced Paymaster settings"), m_feeFrame);
    m_advancedPaymasterButton->setObjectName("advancedPaymasterSettingsToggle");
    m_advancedPaymasterButton->setCheckable(true);
    m_advancedPaymasterButton->setAccessibleDescription(
        tr("Show or hide optional provider, privacy and offer controls"));
    m_feeLayout->addWidget(m_advancedPaymasterButton, 4, 1, Qt::AlignRight);

    m_clientSafetyFrame = new QFrame(m_feeFrame);
    m_clientSafetyFrame->setObjectName("paymasterClientSafetyFrame");
    m_clientSafetyFrame->setFrameShape(QFrame::StyledPanel);
    DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::WAITING);
    auto* safety_layout = new QHBoxLayout(m_clientSafetyFrame);
    m_clientSafetyStatus = new QLabel(
        tr("Checking this wallet's Paymaster service-fee limits…"), m_clientSafetyFrame);
    m_clientSafetyStatus->setObjectName("sendPaymasterClientSafetyStatus");
    m_clientSafetyStatus->setWordWrap(true);
    m_configureClientSafetyButton = new QPushButton(
        tr("Set service-fee limits…"), m_clientSafetyFrame);
    m_configureClientSafetyButton->setObjectName("configurePaymasterClientSafety");
    m_configureClientSafetyButton->setAccessibleDescription(
        tr("Configure wallet-local maximum Paymaster service fees"));
    safety_layout->addWidget(m_clientSafetyStatus, 1);
    safety_layout->addWidget(m_configureClientSafetyButton);
    m_feeLayout->addWidget(m_clientSafetyFrame, 5, 0, 1, 2);

    m_advancedPaymasterFrame = new QFrame(m_feeFrame);
    m_advancedPaymasterFrame->setObjectName("advancedPaymasterSettings");
    m_advancedPaymasterFrame->setFrameShape(QFrame::StyledPanel);
    auto* advanced_layout = new QGridLayout(m_advancedPaymasterFrame);

    auto* advanced_help = new QLabel(tr(
        "These controls are optional. Core always enforces the lower of this transfer's "
        "limit, the wallet-local safety limit and the provider's signed offer."),
        m_advancedPaymasterFrame);
    advanced_help->setObjectName("advancedPaymasterSettingsExplanation");
    advanced_help->setWordWrap(true);
    advanced_layout->addWidget(advanced_help, 0, 0, 1, 2);

    m_clientSafetyDetails = new QLabel(
        tr("Wallet protection details are being loaded…"), m_advancedPaymasterFrame);
    m_clientSafetyDetails->setObjectName("sendPaymasterClientSafetyDetails");
    m_clientSafetyDetails->setWordWrap(true);
    advanced_layout->addWidget(m_clientSafetyDetails, 1, 0, 1, 2);

    m_feeCapSpin = new NoWheelSpinBox(m_advancedPaymasterFrame);
    m_feeCapSpin->setObjectName("paymasterFeeCap");
    m_feeCapSpin->setRange(0, 10000000);
    m_feeCapSpin->setValue(100);
    m_feeCapSpin->setSuffix(tr(" cents"));
    m_feeCapSpin->setToolTip(tr("Hard maximum Paymaster service fee; it can never be exceeded"));
    advanced_layout->addWidget(new QLabel(tr("Maximum additional Paymaster fee:"), m_advancedPaymasterFrame), 2, 0);
    advanced_layout->addWidget(m_feeCapSpin, 2, 1);

    m_maxAttemptsSpin = new NoWheelSpinBox(m_advancedPaymasterFrame);
    m_maxAttemptsSpin->setObjectName("paymasterMaximumAttempts");
    m_maxAttemptsSpin->setRange(1, 16);
    m_maxAttemptsSpin->setValue(3);
    m_maxAttemptsSpin->setToolTip(tr("Maximum number of strictly sequential provider attempts"));
    advanced_layout->addWidget(new QLabel(tr("Maximum provider attempts:"), m_advancedPaymasterFrame), 3, 0);
    advanced_layout->addWidget(m_maxAttemptsSpin, 3, 1);

    m_privacyCombo = new NoWheelComboBox(m_advancedPaymasterFrame);
    m_privacyCombo->setObjectName("paymasterPrivacy");
    m_privacyCombo->addItem(tr("Standard privacy (BIP324)"), QStringLiteral("standard"));
    m_privacyCombo->addItem(tr("High privacy (Tor only)"), QStringLiteral("high"));
    m_privacyCombo->setToolTip(tr(
        "Paymaster privacy improves pseudonymity but does not guarantee anonymity. "
        "The selected provider necessarily receives the payment details needed to sign."));
    m_selectionCombo = new NoWheelComboBox(m_advancedPaymasterFrame);
    m_selectionCombo->setObjectName("paymasterSelection");
    m_selectionCombo->addItem(tr("Lowest total cost"), QStringLiteral("lowest_total_cost"));
    m_selectionCombo->addItem(tr("Privacy weighted"), QStringLiteral("privacy_weighted"));
    advanced_layout->addWidget(new QLabel(tr("Privacy:"), m_advancedPaymasterFrame), 4, 0);
    advanced_layout->addWidget(m_privacyCombo, 4, 1);
    advanced_layout->addWidget(new QLabel(tr("Provider selection:"), m_advancedPaymasterFrame), 5, 0);
    advanced_layout->addWidget(m_selectionCombo, 5, 1);

    m_refreshOffersButton = new QPushButton(tr("Show currently eligible Paymaster offers"), m_advancedPaymasterFrame);
    m_refreshOffersButton->setObjectName("refreshPaymasterOffers");
    advanced_layout->addWidget(m_refreshOffersButton, 6, 0, 1, 2);
    m_offersStatus = new QLabel(
        tr("Enter a transfer amount, then request offers if you want to inspect them manually."),
        m_advancedPaymasterFrame);
    m_offersStatus->setObjectName("paymasterOffersStatus");
    m_offersStatus->setWordWrap(true);
    DigiDollarStatus::SetBanner(m_offersStatus, DigiDollarStatus::Kind::INFO);
    advanced_layout->addWidget(m_offersStatus, 7, 0, 1, 2);
    m_offersTable = new QTableWidget(0, 6, m_advancedPaymasterFrame);
    m_offersTable->setObjectName("paymasterOffers");
    m_offersTable->setHorizontalHeaderLabels({tr("Provider"), tr("Payment model"), tr("Service fee"),
                                               tr("Total $DD"), tr("Reliability"), tr("Valid until")});
    m_offersTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_offersTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_offersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_offersTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_offersTable->setFocusPolicy(Qt::NoFocus);
    m_offersTable->setToolTip(tr(
        "Offer preview only. Core authenticates and selects the exact offer when the transfer is prepared."));
    m_offersTable->setMinimumHeight(130);
    advanced_layout->addWidget(m_offersTable, 8, 0, 1, 2);
    m_feeLayout->addWidget(m_advancedPaymasterFrame, 6, 0, 1, 2);

    auto* totals = new QFrame(m_feeFrame);
    totals->setObjectName("feeTotalsFrame");
    auto* totals_layout = new QGridLayout(totals);
    m_feeLabel = new QLabel(tr("Transaction fee:"), totals);
    m_feeLabel->setObjectName("feeLabel");
    m_feeLabel->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_feeLabel->setToolTip(tr("Network fee paid in DGB (not deducted from $DD amount)"));
    m_feeValue = new QLabel("~0.1 DGB", totals);
    m_feeValue->setObjectName("feeValue");
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_feeValue->setFont(monospaceFont);
    m_feeValue->setToolTip(tr("Estimated network fee paid in DGB from your DGB balance"));
    totals_layout->addWidget(m_feeLabel, 0, 0);
    totals_layout->addWidget(m_feeValue, 0, 1);

    m_totalLabel = new QLabel(tr("Recipient receives:"), totals);
    m_totalLabel->setObjectName("totalLabel");
    m_totalLabel->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_totalLabel->setToolTip(tr("Exact DigiDollar amount delivered to the recipient"));
    QFont boldFont = m_totalLabel->font();
    boldFont.setBold(true);
    m_totalLabel->setFont(boldFont);
    m_totalValue = new QLabel("0.00 $DD", totals);
    m_totalValue->setObjectName("totalValue");
    m_totalValue->setFont(monospaceFont);
    m_totalValue->setToolTip(tr("The network or service fee is shown separately"));
    totals_layout->addWidget(m_totalLabel, 1, 0);
    totals_layout->addWidget(m_totalValue, 1, 1);
    totals_layout->setColumnStretch(1, 1);
    m_feeLayout->addWidget(totals, 7, 0, 1, 2);
    // The concise summary above already shows these values. Keep the legacy
    // labels available to existing update paths and accessibility tests without
    // presenting the same totals twice in the normal send flow.
    totals->hide();

    m_paymasterSessionFrame = new QFrame(m_feeFrame);
    m_paymasterSessionFrame->setObjectName("paymasterSessionFrame");
    m_paymasterSessionFrame->setFrameShape(QFrame::StyledPanel);
    auto* session_layout = new QGridLayout(m_paymasterSessionFrame);
    auto* session_heading = new QLabel(tr("Current Paymaster transfer"), m_paymasterSessionFrame);
    session_heading->setObjectName("paymasterSessionHeading");
    QFont session_heading_font = session_heading->font();
    session_heading_font.setBold(true);
    session_heading_font.setPointSize(session_heading_font.pointSize() + 1);
    session_heading->setFont(session_heading_font);
    session_layout->addWidget(session_heading, 0, 0, 1, 2);

    m_paymasterNextStepValue = new QLabel(tr("Checking the protected transfer…"), m_paymasterSessionFrame);
    m_paymasterNextStepValue->setObjectName("paymasterSessionNextStep");
    m_paymasterNextStepValue->setWordWrap(true);
    m_paymasterNextStepValue->setAccessibleName(tr("Paymaster transfer status and next step"));
    session_layout->addWidget(m_paymasterNextStepValue, 1, 0, 1, 2);

    m_paymasterTransferValue = new QLabel(tr("—"), m_paymasterSessionFrame);
    m_paymasterTransferValue->setObjectName("paymasterSessionTransfer");
    m_paymasterTransferValue->setWordWrap(true);
    m_paymasterTransferValue->setAccessibleName(tr("Locked Paymaster transfer recipient and amount"));
    session_layout->addWidget(new QLabel(tr("Transfer:"), m_paymasterSessionFrame), 2, 0);
    session_layout->addWidget(m_paymasterTransferValue, 2, 1);

    m_paymasterIdentityValue = new QLabel(tr("Provider not selected yet"), m_paymasterSessionFrame);
    m_paymasterIdentityValue->setObjectName("paymasterSessionProvider");
    m_paymasterIdentityValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    session_layout->addWidget(new QLabel(tr("Provider:"), m_paymasterSessionFrame), 3, 0);
    session_layout->addWidget(m_paymasterIdentityValue, 3, 1);
    m_paymasterCostValue = new QLabel(tr("No service fee authorized yet"), m_paymasterSessionFrame);
    m_paymasterCostValue->setObjectName("paymasterSessionCost");
    m_paymasterCostValue->setWordWrap(true);
    session_layout->addWidget(new QLabel(tr("Cost:"), m_paymasterSessionFrame), 4, 0);
    session_layout->addWidget(m_paymasterCostValue, 4, 1);

    m_paymasterPrimaryButton = new QPushButton(tr("Check status"), m_paymasterSessionFrame);
    m_paymasterPrimaryButton->setObjectName("paymasterSessionPrimaryAction");
    m_paymasterPrimaryButton->setAccessibleDescription(
        tr("Perform the one recommended safe action for the protected Paymaster transfer"));
    session_layout->addWidget(m_paymasterPrimaryButton, 5, 0, 1, 2);

    auto* disclosure_row = new QHBoxLayout();
    m_paymasterMoreButton = new QPushButton(tr("More options"), m_paymasterSessionFrame);
    m_paymasterMoreButton->setObjectName("paymasterSessionMoreOptions");
    m_paymasterMoreButton->setCheckable(true);
    m_paymasterMoreButton->setAccessibleDescription(
        tr("Show additional actions that Core allows for this exact session"));
    m_paymasterTechnicalButton = new QPushButton(tr("Technical details"), m_paymasterSessionFrame);
    m_paymasterTechnicalButton->setObjectName("paymasterSessionTechnicalToggle");
    m_paymasterTechnicalButton->setCheckable(true);
    m_paymasterTechnicalButton->setAccessibleDescription(
        tr("Show raw wallet state and expiry information for troubleshooting"));
    disclosure_row->addWidget(m_paymasterMoreButton);
    disclosure_row->addWidget(m_paymasterTechnicalButton);
    disclosure_row->addStretch();
    session_layout->addLayout(disclosure_row, 6, 0, 1, 2);

    m_paymasterSecondaryActions = new QFrame(m_paymasterSessionFrame);
    m_paymasterSecondaryActions->setObjectName("paymasterSessionSecondaryActions");
    auto* secondary_layout = new QGridLayout(m_paymasterSecondaryActions);
    secondary_layout->setContentsMargins(0, 4, 0, 0);
    m_retrySessionButton = new QPushButton(tr("Retry the exact provider step"), m_paymasterSecondaryActions);
    m_retrySessionButton->setObjectName("retryPaymasterSession");
    m_retrySessionButton->setToolTip(tr(
        "Idempotently retry only the already persisted provider step; this never selects a different transaction"));
    m_fallbackSessionButton = new QPushButton(tr("Try another Paymaster"), m_paymasterSecondaryActions);
    m_fallbackSessionButton->setObjectName("fallbackPaymasterSession");
    m_fallbackSessionButton->setToolTip(
        tr("Available only before a user payment signature exists; keeps the exact reserved inputs"));
    m_recoverSessionButton = new QPushButton(tr("Recover safely to this wallet"), m_paymasterSecondaryActions);
    m_recoverSessionButton->setObjectName("recoverPaymasterSession");
    m_recoverSessionButton->setToolTip(
        tr("Creates one durable same-input conflict transaction; the original payment may still confirm first"));
    m_abandonSessionButton = new QPushButton(
        tr("Cancel unsigned transfer and release $DD"), m_paymasterSecondaryActions);
    m_abandonSessionButton->setObjectName("abandonUnsignedPaymasterSession");
    m_abandonSessionButton->setToolTip(tr(
        "Available only while Core can prove that no transaction signature or final transaction exists"));
    m_cancelQuoteButton = new QPushButton(tr("Stop automatic checks"), m_paymasterSecondaryActions);
    m_cancelQuoteButton->setObjectName("stopPaymasterAutomaticChecks");
    m_cancelQuoteButton->setToolTip(tr("Stop polling without releasing any durable reservation"));
    secondary_layout->addWidget(m_retrySessionButton, 0, 0);
    secondary_layout->addWidget(m_fallbackSessionButton, 0, 1);
    secondary_layout->addWidget(m_recoverSessionButton, 1, 0, 1, 2);
    secondary_layout->addWidget(m_abandonSessionButton, 2, 0, 1, 2);
    secondary_layout->addWidget(m_cancelQuoteButton, 3, 0, 1, 2);
    session_layout->addWidget(m_paymasterSecondaryActions, 7, 0, 1, 2);

    m_paymasterTechnicalDetails = new QFrame(m_paymasterSessionFrame);
    m_paymasterTechnicalDetails->setObjectName("paymasterSessionTechnicalDetails");
    auto* technical_layout = new QGridLayout(m_paymasterTechnicalDetails);
    technical_layout->setContentsMargins(0, 4, 0, 0);
    m_paymasterStateValue = new QLabel(tr("No active session"), m_paymasterTechnicalDetails);
    m_paymasterStateValue->setObjectName("paymasterSessionState");
    m_paymasterStateValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_paymasterExpiryValue = new QLabel(tr("—"), m_paymasterTechnicalDetails);
    technical_layout->addWidget(new QLabel(tr("Core state:"), m_paymasterTechnicalDetails), 0, 0);
    technical_layout->addWidget(m_paymasterStateValue, 0, 1);
    technical_layout->addWidget(new QLabel(tr("Valid until:"), m_paymasterTechnicalDetails), 1, 0);
    technical_layout->addWidget(m_paymasterExpiryValue, 1, 1);
    session_layout->addWidget(m_paymasterTechnicalDetails, 8, 0, 1, 2);

    m_feeLayout->addWidget(m_paymasterSessionFrame, 8, 0, 1, 2);

    m_mainLayout->addWidget(m_feeFrame);

    m_advancedPaymasterFrame->hide();
    m_paymasterSessionFrame->hide();
    m_paymasterSecondaryActions->hide();
    m_paymasterTechnicalDetails->hide();
    onFeeModeChanged();
}

void DigiDollarSendWidget::setupButtonSection()
{
    // Create button frame
    m_buttonFrame = new QFrame(this);
    m_buttonFrame->setObjectName("buttonFrame");
    m_buttonFrame->setFrameStyle(QFrame::NoFrame);

    m_buttonLayout = new QHBoxLayout(m_buttonFrame);
    m_buttonLayout->setSpacing(10);
    m_buttonLayout->setContentsMargins(10, 10, 10, 10);

    // Clear button
    m_clearButton = new QPushButton(tr("&Clear"), this);
    m_clearButton->setObjectName("clearButton");
    m_clearButton->setToolTip(tr("Clear all fields"));
    m_clearButton->setAutoDefault(false);
    // Removed minimum height - CSS handles button sizing
    m_buttonLayout->addWidget(m_clearButton);

    // Add stretch to push send button to the right
    m_buttonLayout->addStretch();

    // Send button - styled to match main wallet
    m_sendButton = new QPushButton(tr("S&end DigiDollar"), this);
    m_sendButton->setObjectName("sendButton");
    m_sendButton->setEnabled(false);
    m_sendButton->setDefault(true);
    m_sendButton->setAutoDefault(true);
    // Removed minimum height - CSS handles button sizing
    m_sendButton->setToolTip(tr("Confirm and send this DigiDollar transaction"));
    m_buttonLayout->addWidget(m_sendButton);

    m_mainLayout->addWidget(m_buttonFrame);
}

void DigiDollarSendWidget::connectSignals()
{
    // Connect address validation
    connect(m_addressEdit, &QLineEdit::textChanged,
            this, &DigiDollarSendWidget::onAddressChanged);

    // Connect amount validation
    connect(m_amountEdit, &QLineEdit::textChanged,
            this, &DigiDollarSendWidget::onAmountChanged);

    // Connect buttons
    connect(m_sendButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onSendClicked);
    connect(m_clearButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onClearClicked);
    connect(m_useAvailableBalanceButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onUseAvailableBalanceClicked);
    connect(m_pasteAddressButton, &QToolButton::clicked,
            this, &DigiDollarSendWidget::onPasteAddressClicked);
    connect(m_addressBookButton, &QToolButton::clicked,
            this, &DigiDollarSendWidget::onAddressBookClicked);

    // Connect coin control button
    connect(m_coinControlButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onCoinControlButtonClicked);
    connect(m_dgbFeeRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (!checked) return;
        m_feeModeCombo->setCurrentIndex(m_feeModeCombo->findData(QStringLiteral("dgb")));
    });
    connect(m_autoFeeRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (!checked) return;
        m_feeModeCombo->setCurrentIndex(m_feeModeCombo->findData(QStringLiteral("auto")));
    });
    connect(m_paymasterFeeRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (!checked) return;
        m_feeModeCombo->setCurrentIndex(m_feeModeCombo->findData(QStringLiteral("paymaster")));
    });
    connect(m_feeModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &DigiDollarSendWidget::onFeeModeChanged);
    connect(m_subtractPaymasterFeeCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) m_sendAllSpendableDD = false;
        m_paymasterPreviewRecipientCents = -1;
        m_paymasterPreviewServiceFeeCents = -1;
        m_paymasterPreviewTotalCents = -1;
        updateFeeDisplay();
    });
    connect(m_paymasterExplanationButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::showPaymasterExplanation);
    connect(m_advancedPaymasterButton, &QPushButton::toggled, this, [this](bool checked) {
        m_advancedPaymasterButton->setText(
            checked ? tr("Hide advanced Paymaster settings")
                    : tr("Advanced Paymaster settings"));
        onFeeModeChanged();
    });
    connect(m_configureClientSafetyButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::configureClientSafetyPolicy);
    connect(m_feeCapSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        m_paymasterPreviewRecipientCents = -1;
        m_paymasterPreviewServiceFeeCents = -1;
        m_paymasterPreviewTotalCents = -1;
        updateFeeDisplay();
    });
    connect(m_privacyCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const bool high = m_privacyCombo->currentData().toString() == QStringLiteral("high");
        if (high) m_maxAttemptsSpin->setValue(1);
        m_maxAttemptsSpin->setEnabled(!high);
    });
    connect(m_refreshOffersButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::refreshPaymasterOffers);
    connect(m_retrySessionButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::retryPaymasterSession);
    connect(m_fallbackSessionButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::fallbackPaymasterSession);
    connect(m_recoverSessionButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::recoverPaymasterSessionToSelf);
    connect(m_abandonSessionButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::abandonUnsignedPaymasterSession);
    connect(m_cancelQuoteButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::cancelPaymasterQuote);
    connect(m_paymasterPrimaryButton, &QPushButton::clicked,
            this, &DigiDollarSendWidget::onPaymasterPrimaryAction);
    connect(m_paymasterMoreButton, &QPushButton::toggled,
            m_paymasterSecondaryActions, &QFrame::setVisible);
    connect(m_paymasterTechnicalButton, &QPushButton::toggled,
            m_paymasterTechnicalDetails, &QFrame::setVisible);
    connect(m_paymasterPollTimer, &QTimer::timeout,
            this, &DigiDollarSendWidget::pollPaymasterSession);
    m_paymasterPollTimer->setInterval(1500);
}

void DigiDollarSendWidget::setWalletModel(WalletModel* model)
{
    if (m_walletModel != model) {
        m_paymasterPollTimer->stop();
        m_paymasterBusy = false;
        m_paymasterRequestId.clear();
        m_paymasterSessionId.clear();
        m_paymasterSessionState.clear();
        m_paymasterAttemptState.clear();
        m_paymasterArtifact.clear();
        m_paymasterPendingPhase.clear();
        m_paymasterBroadcastState.clear();
        m_paymasterConfirmationState.clear();
        m_paymasterSessionPersisted = false;
        m_paymasterAddress.clear();
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterSessionPrivacy.clear();
        m_paymasterRecoveryAuthorizationCommitment.clear();
        m_paymasterAmount = 0.0;
        m_paymasterInitialAvailableBalance = 0.0;
        m_paymasterPreviewRecipientCents = -1;
        m_paymasterPreviewServiceFeeCents = -1;
        m_paymasterPreviewTotalCents = -1;
        m_sendAllSpendableDD = false;
        if (m_subtractPaymasterFeeCheck) m_subtractPaymasterFeeCheck->setChecked(false);
        m_paymasterRecoveryMaximumServiceFeeCents = 0;
        m_paymasterRecoveryActive = false;
        m_paymasterConfirmationGuard.Reset();
        m_paymasterRecoveryConfirmationGuard.Reset();
        m_clientSafetyStatusKnown = false;
        m_clientSafetyConfigured = false;
        m_clientSafetyActiveReservations = 0;
        m_clientSafetyReservedCents = 0;
        m_clientSafetySpentTodayCents = 0;
        m_clientSafetyAvailableTodayCents = 0;
        m_clientSafetyError.clear();
        m_paymasterStateValue->setText(tr("No active session"));
        m_paymasterIdentityValue->setText(tr("—"));
        m_paymasterCostValue->setText(tr("—"));
        m_paymasterExpiryValue->setText(tr("—"));
        m_paymasterSessionFrame->hide();
    }
    m_walletModel = model;

    if (m_walletModel) {
        // Connect wallet model signals
        updateBalance();
        refreshClientSafetyStatus();
        // REMOVED: applyTheme() - Let CSS handle all theming
    } else {
        updateClientSafetyDisplay();
    }
    updatePaymasterFocusMode();
}

void DigiDollarSendWidget::setClientModel(ClientModel* model)
{
    m_clientModel = model;

    if (m_clientModel) {
        // Connect client model signals
        updateOraclePrice();
        // REMOVED: applyTheme() - Let CSS handle all theming
    }
}

void DigiDollarSendWidget::updateView()
{
    updateBalance();
    updateOraclePrice();
    updateFeeDisplay();
}

void DigiDollarSendWidget::updateBalance()
{
    if (m_walletModel) {
        // Only confirmed DD is spendable. Pending DD, including wallet-created
        // transfer change, must confirm before another DD spend can use it.
        CAmount balanceCents = m_walletModel->getDigiDollarBalance();
        m_availableBalance = balanceCents / 100.0; // Convert cents to DD
    } else {
        m_availableBalance = 0.0;
    }

    if (m_privacy) {
        m_availableBalanceValue->setText(maskValue(formatDDAmount(0)));
    } else {
        m_availableBalanceValue->setText(formatDDAmount(m_availableBalance));
    }

    // The amount field is the recipient amount. Direct funding uses DGB;
    // an exact Paymaster service fee is validated separately before signing.
    m_useAvailableBalanceButton->setEnabled(m_availableBalance > 0);

    // Re-validate amount against updated balance so the border color
    // refreshes when pending DD confirms (fixes stale yellow warning).
    updateAmountValidation();
    updateSendButton();
}

void DigiDollarSendWidget::updateOraclePrice()
{
    if (!isVisible()) return;
    // Get oracle price from RPC for testnet/mainnet, MockOracleManager for regtest
    if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        // BUG #6 FIX: GetCurrentPrice() returns micro-USD, not cents
        CAmount priceMicroUsd = MockOracleManager::GetInstance().GetCurrentPrice();
        m_oraclePrice = priceMicroUsd / 1000000.0;
    } else if (m_walletModel) {
        UniValue params{UniValue::VARR};
        QPointer<DigiDollarSendWidget> guard{this};
        m_walletModel->executeRpcAsync("getoracleprice", std::move(params),
            [guard](UniValue result, QString error) {
                if (!guard) return;
                if (!error.isEmpty()) {
                    guard->m_oraclePrice = 0.0;
                } else {
                    guard->m_oraclePrice = result.find_value("price_micro_usd").getInt<int64_t>() / 1000000.0;
                }
                guard->updateUSDEquivalent();
            });
        return;
    } else {
        m_oraclePrice = 0.0;
    }

    updateUSDEquivalent();
}

void DigiDollarSendWidget::onAddressChanged()
{
    QString address = m_addressEdit->text();

    updateAddressValidation();

    updateSendButton();
}

void DigiDollarSendWidget::onAmountChanged()
{
    if (!m_settingSweepAmount) m_sendAllSpendableDD = false;
    m_paymasterPreviewRecipientCents = -1;
    m_paymasterPreviewServiceFeeCents = -1;
    m_paymasterPreviewTotalCents = -1;

    // Validate amount format
    updateAmountValidation();

    updateUSDEquivalent();
    updateFeeDisplay();
    updateSendButton();
}

void DigiDollarSendWidget::onSendClicked()
{
    // PHASE 7.3: Comprehensive input validation with user-friendly messages
    QString address = m_addressEdit->text().trimmed();
    QString amountText = m_amountEdit->text().trimmed();

    // Error: Empty fields
    if (address.isEmpty()) {
        showError(tr("Missing Address"),
                  tr("Please enter a DigiDollar address.\n\nThe recipient's DD address is required to send DigiDollar."));
        m_addressEdit->setFocus();
        return;
    }

    if (amountText.isEmpty()) {
        showError(tr("Missing Amount"),
                  tr("Please enter an amount to send.\n\nSpecify how much DigiDollar you want to send (e.g., 100.00)."));
        m_amountEdit->setFocus();
        return;
    }

    // Error: Invalid address format
    if (!validateAddress()) {
        showError(tr("Invalid DigiDollar Address"),
                  tr("The address format is invalid.\n\n"
                     "Enter a valid DigiDollar address for the active network.\n\n"
                     "Please check the address and try again."));
        m_addressEdit->setFocus();
        return;
    }

    // Error: Invalid amount format
    if (!validateAmount()) {
        showError(tr("Invalid Amount"),
                  tr("The amount is invalid.\n\n"
                     "Valid amount format:\n"
                     "• Positive number\n"
                     "• Maximum 2 decimal places\n"
                     "• Between 1.00 $DD and 100,000.00 $DD\n\n"
                     "Please enter a valid amount."));
        m_amountEdit->setFocus();
        return;
    }

    double amount = amountText.toDouble();

    // Error: Insufficient balance
    if (!validateBalance()) {
        const bool subtract_fee = paymasterModeSelected() &&
            m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked();
        const QString fee_note = paymasterModeSelected()
            ? (subtract_fee
                   ? tr("The entered amount is the exact maximum $DD outflow; the provider fee is deducted within it.")
                   : tr("A user-paid Paymaster offer may additionally require a $DD service fee. "
                        "The exact fee is checked before signing."))
            : tr("The network fee is paid separately from your DGB balance.");
        showError(tr("Insufficient DigiDollar Balance"),
                  tr("You don't have enough DigiDollar for this transfer.\n\n"
                     "Available balance: %1\n"
                     "Amount to send: %2\n\n"
                     "%3\n\n"
                     "Please enter a smaller amount or add more $DD to your wallet.")
                  .arg(formatDDAmount(m_availableBalance))
                  .arg(formatDDAmount(amount))
                  .arg(fee_note));
        m_amountEdit->setFocus();
        return;
    }

    if (m_coinControl && m_coinControl->HasSelected()) {
        const CAmount amount_cents = static_cast<CAmount>(std::llround(amount * 100));
        const CAmount selected_amount = selectedDigiDollarAmount();
        if (selected_amount < amount_cents) {
            showError(tr("Insufficient Selected DigiDollar Inputs"),
                      tr("The selected DigiDollar inputs total %1, but this send requires %2.\n\n"
                         "Select more inputs or clear manual input selection.")
                          .arg(formatDDAmount(selected_amount / 100.0))
                          .arg(formatDDAmount(amount)));
            return;
        }
    }

    // PHASE 7.3: Wallet state validation
    if (!checkWalletState()) {
        return; // Error already displayed by checkWalletState()
    }

    if (paymasterModeSelected() &&
        (!m_clientSafetyStatusKnown || !m_clientSafetyConfigured)) {
        showWarning(tr("Paymaster service-fee limits required"),
                    tr("Configure positive wallet-local Paymaster service-fee limits before "
                       "using Automatic or Paymaster fee funding."));
        return;
    }

    if (paymasterModeSelected()) {
        const CAmount amount_cents = static_cast<CAmount>(std::llround(amount * 100));
        const CAmount active_amount_cents =
            static_cast<CAmount>(std::llround(m_paymasterAmount * 100));
        const bool new_request = m_paymasterRequestId.isEmpty();

        if (!new_request &&
            (address != m_paymasterAddress || amount_cents != active_amount_cents)) {
            showWarning(
                tr("Paymaster transfer already in progress"),
                tr("The active Paymaster request is bound to %1 for %2.\n\n"
                   "Changing its recipient or amount cannot reuse the same authorization. "
                   "Finish or safely cancel the active request before starting a different transfer.")
                    .arg(m_paymasterAddress, formatDDAmount(m_paymasterAmount)));
            return;
        }

        // The first dialog authorizes only preparation and must be shown once
        // per request. A repeated Send click resumes the exact request instead
        // of returning to the generic preparation question. The separate
        // provider/fee confirmation remains mandatory before any signature.
        if (new_request && !showConfirmationDialog(address, amount)) {
            return;
        }

        if (new_request) {
            m_paymasterRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
            m_paymasterSessionPersisted = false;
            m_paymasterSessionId.clear();
            m_paymasterSessionState.clear();
            m_paymasterAttemptState.clear();
            m_paymasterArtifact.clear();
            m_paymasterPendingPhase.clear();
            m_paymasterBroadcastState.clear();
            m_paymasterConfirmationState.clear();
            m_paymasterAuthorizationCommitment.clear();
            m_paymasterSessionPrivacy = m_privacyCombo->currentData().toString();
            m_paymasterRecoveryAuthorizationCommitment.clear();
            m_paymasterRecoveryMaximumServiceFeeCents = 0;
            m_paymasterRecoveryActive = false;
            m_paymasterInitialAvailableBalance = m_availableBalance;
            m_paymasterConfirmationGuard.Reset();
            m_paymasterRecoveryConfirmationGuard.Reset();
            m_paymasterStateValue->setText(tr("Preparing a new Paymaster request…"));
            m_paymasterIdentityValue->setText(tr("No provider selected yet"));
            m_paymasterCostValue->setText(tr("No service fee authorized yet"));
            m_paymasterExpiryValue->setText(tr("—"));
        }
        m_paymasterAddress = address;
        m_paymasterAmount = amount;
        executePaymasterTransfer(address, amount, /*allow_unlock=*/false);
    } else {
        // PHASE 7.2: Enhanced confirmation dialog with fee display. The direct
        // path has only one signing/broadcast confirmation.
        if (!showConfirmationDialog(address, amount)) {
            return;
        }
        // The legacy direct path stays synchronous and keeps its unlock only
        // for local transaction creation/signing.
        WalletModel::UnlockContext ctx(m_walletModel->requestUnlock());
        if (!ctx.isValid()) return;
        executeTransfer(address, amount);
    }
}

void DigiDollarSendWidget::onClearClicked()
{
    m_addressEdit->clear();
    m_amountEdit->clear();
    if (m_noteEdit) m_noteEdit->clear();
    m_sendAllSpendableDD = false;
    m_paymasterInitialAvailableBalance = 0.0;
    m_paymasterPreviewRecipientCents = -1;
    m_paymasterPreviewServiceFeeCents = -1;
    m_paymasterPreviewTotalCents = -1;
    if (m_subtractPaymasterFeeCheck) m_subtractPaymasterFeeCheck->setChecked(false);
    const bool preserve_durable_session =
        m_paymasterSessionPersisted && !m_paymasterRequestId.isEmpty();
    if (!m_paymasterPollTimer->isActive() && !preserve_durable_session) {
        m_paymasterRequestId.clear();
        m_paymasterSessionId.clear();
        m_paymasterSessionState.clear();
        m_paymasterAttemptState.clear();
        m_paymasterArtifact.clear();
        m_paymasterPendingPhase.clear();
        m_paymasterBroadcastState.clear();
        m_paymasterConfirmationState.clear();
        m_paymasterSessionPersisted = false;
        m_paymasterAddress.clear();
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterSessionPrivacy.clear();
        m_paymasterRecoveryAuthorizationCommitment.clear();
        m_paymasterAmount = 0.0;
        m_paymasterRecoveryMaximumServiceFeeCents = 0;
        m_paymasterRecoveryActive = false;
        m_paymasterConfirmationGuard.Reset();
        m_paymasterRecoveryConfirmationGuard.Reset();
        m_paymasterStateValue->setText(tr("No active session"));
        m_paymasterIdentityValue->setText(tr("—"));
        m_paymasterCostValue->setText(tr("—"));
        m_paymasterExpiryValue->setText(tr("—"));
        m_paymasterSessionFrame->hide();
    } else if (preserve_durable_session) {
        m_paymasterStateValue->setText(tr(
            "Entry fields cleared. The durable Paymaster transfer remains protected. "
            "Use ‘Cancel unsigned transfer and release $DD’ if no signature exists, "
            "or use the recovery actions shown here."));
    }
    onAddressChanged();
    onAmountChanged();
    updatePaymasterFocusMode();
}

bool DigiDollarSendWidget::paymasterModeSelected() const
{
    return feeMode() != QStringLiteral("dgb");
}

QString DigiDollarSendWidget::feeMode() const
{
    return m_feeModeCombo ? m_feeModeCombo->currentData().toString() : QStringLiteral("dgb");
}

void DigiDollarSendWidget::onFeeModeChanged()
{
    const QString mode = feeMode();
    if (m_dgbFeeRadio && mode == QStringLiteral("dgb")) m_dgbFeeRadio->setChecked(true);
    if (m_autoFeeRadio && mode == QStringLiteral("auto")) m_autoFeeRadio->setChecked(true);
    if (m_paymasterFeeRadio && mode == QStringLiteral("paymaster")) m_paymasterFeeRadio->setChecked(true);
    const bool paymaster_enabled = paymasterModeSelected();
    if (!paymaster_enabled && m_subtractPaymasterFeeCheck) {
        m_subtractPaymasterFeeCheck->setChecked(false);
        m_sendAllSpendableDD = false;
    }
    if (m_subtractPaymasterFeeCheck) {
        m_subtractPaymasterFeeCheck->setVisible(paymaster_enabled);
    }
    if (m_useAvailableBalanceButton) {
        m_useAvailableBalanceButton->setText(
            paymaster_enabled ? tr("Empty wallet with Paymaster")
                              : tr("Use available balance"));
        m_useAvailableBalanceButton->setToolTip(paymaster_enabled
            ? tr("Use every confirmed, ordinary spendable $DD input. If a Paymaster is needed, "
                 "its exact fee is deducted so no spendable $DD remains.")
            : tr("Use the full available DigiDollar balance; the network fee is paid separately in DGB."));
    }
    if (m_advancedPaymasterButton) m_advancedPaymasterButton->setVisible(paymaster_enabled);
    if (m_clientSafetyFrame) m_clientSafetyFrame->setVisible(paymaster_enabled);
    if (m_advancedPaymasterFrame) {
        m_advancedPaymasterFrame->setVisible(
            paymaster_enabled && m_advancedPaymasterButton->isChecked());
    }
    updateClientSafetyDisplay();
    updateFeeDisplay();
    updatePaymasterFocusMode();
    updateSendButton();
}

QString DigiDollarSendWidget::friendlyPaymasterSessionStatus() const
{
    if (m_paymasterBusy) {
        return tr("Core is securely checking the current Paymaster transfer. Please wait.");
    }
    if (m_paymasterSessionState == QStringLiteral("CONFIRMED")) {
        return tr("Transfer confirmed. The recipient has received the DigiDollar payment.");
    }
    if (m_paymasterSessionState == QStringLiteral("CANCELED_SAFE")) {
        return tr("Transfer canceled safely. Reserved DigiDollar is available again.");
    }
    if (m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE")) {
        return tr("An exact provider offer is ready for your review. Nothing has been signed yet.");
    }
    if (m_paymasterSessionState == QStringLiteral("PENDING_PROVIDER")) {
        return m_paymasterArtifact == QStringLiteral("user_psbt")
            ? tr("Your authorized transaction is waiting for the provider. Keep this session protected until it completes or is safely recovered.")
            : tr("Waiting for the selected provider. No additional action is normally required.");
    }
    if (m_paymasterSessionState == QStringLiteral("MEMPOOL") ||
        m_paymasterSessionState == QStringLiteral("STEMPOOL")) {
        return tr("The transaction was submitted and is waiting for blockchain confirmation.");
    }
    if (m_paymasterSessionState == QStringLiteral("FAILED")) {
        if (m_paymasterArtifact == QStringLiteral("none")) {
            return tr("This attempt failed before a transaction was signed. Check the protected wallet state; Core will offer only cancellation or provider fallback actions that are still provably safe.");
        }
        if (!m_paymasterArtifact.isEmpty()) {
            return tr("The transfer needs recovery attention. A transaction authorization may already exist, so the reserved DigiDollar remains protected.");
        }
        return tr("The transfer needs attention. Check the exact session before choosing a recovery action.");
    }
    if (m_paymasterSessionState == QStringLiteral("CONFLICTED")) {
        return tr("A transaction conflict was detected. Check the protected session before taking further action.");
    }
    if (m_paymasterSessionState == QStringLiteral("CREATED") ||
        m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_WALLET_UNLOCK") ||
        m_paymasterSessionState.isEmpty()) {
        return tr("Preparing and authenticating a Paymaster offer. No service fee has been authorized yet.");
    }
    return tr("The Paymaster transfer is protected. Check its current wallet state before continuing.");
}

void DigiDollarSendWidget::updatePaymasterFocusMode()
{
    // A durable session owns the recipient, amount, inputs, and authorization
    // choices shown here. Hide the editable compose controls until Core reports
    // a terminal/cancelled state so the UI cannot suggest that editing fields
    // mutates an already persisted authorization.
    if (!m_feeChoicesFrame || !m_paymasterSessionFrame) return;
    const bool focus = !m_paymasterRequestId.isEmpty();
    const bool terminal = m_paymasterSessionState == QStringLiteral("CONFIRMED") ||
                          m_paymasterSessionState == QStringLiteral("CANCELED_SAFE");

    m_feeHeading->setVisible(!focus);
    m_feeIntroduction->setVisible(!focus);
    m_feeChoicesFrame->setVisible(!focus);
    m_feeSummary->setVisible(!focus);
    m_subtractPaymasterFeeCheck->setVisible(!focus && paymasterModeSelected());
    m_advancedPaymasterButton->setVisible(!focus && paymasterModeSelected());
    m_clientSafetyFrame->setVisible(!focus && paymasterModeSelected());
    m_advancedPaymasterFrame->setVisible(
        !focus && paymasterModeSelected() && m_advancedPaymasterButton->isChecked());
    m_paymasterSessionFrame->setVisible(focus);

    m_addressEdit->setReadOnly(focus);
    m_amountEdit->setReadOnly(focus);
    if (m_noteEdit) m_noteEdit->setReadOnly(focus);
    for (QLineEdit* field : {m_addressEdit, m_amountEdit, m_noteEdit}) {
        if (!field) continue;
        field->setProperty("paymasterSessionLocked", focus);
        field->style()->unpolish(field);
        field->style()->polish(field);
    }
    m_pasteAddressButton->setEnabled(!focus);
    m_addressBookButton->setEnabled(!focus);
    m_useAvailableBalanceButton->setEnabled(!focus);
    m_coinControlButton->setEnabled(!focus);
    if (m_clearButton) m_clearButton->setVisible(!focus);
    if (m_sendButton) m_sendButton->setVisible(!focus);
    if (m_buttonFrame) m_buttonFrame->setVisible(!focus);

    if (!focus) return;

    m_paymasterTransferValue->setText(
        tr("%1 to %2").arg(formatDDAmount(m_paymasterAmount), m_paymasterAddress));
    m_paymasterNextStepValue->setText(friendlyPaymasterSessionStatus());

    const bool artifact_unknown = m_paymasterArtifact.isEmpty();
    const bool artifact_none = m_paymasterArtifact == QStringLiteral("none");
    const bool signed_artifact = m_paymasterArtifact == QStringLiteral("user_psbt") ||
                                 m_paymasterArtifact == QStringLiteral("final_transaction") ||
                                 m_paymasterArtifact == QStringLiteral("alternative_recovery");
    const bool exact_offer_reviewable =
        m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE") &&
        (artifact_unknown || artifact_none);
    const bool fallback_state =
        m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_WALLET_UNLOCK") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE") ||
        m_paymasterSessionState == QStringLiteral("AUTHORIZED");
    const bool fallback_attempt =
        m_paymasterAttemptState == QStringLiteral("CANDIDATE") ||
        m_paymasterAttemptState == QStringLiteral("QUOTED") ||
        m_paymasterAttemptState == QStringLiteral("QUOTE_EXPIRED") ||
        (m_paymasterAttemptState == QStringLiteral("REJECTED") &&
         m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED"));
    const bool safe_fallback = m_paymasterSessionPersisted && artifact_none &&
                               fallback_state && fallback_attempt;
    const bool signed_state_consistent =
        m_paymasterSessionState == QStringLiteral("AUTHORIZED") ||
        m_paymasterSessionState == QStringLiteral("PENDING_PROVIDER") ||
        m_paymasterSessionState == QStringLiteral("STEMPOOL") ||
        m_paymasterSessionState == QStringLiteral("MEMPOOL") ||
        m_paymasterSessionState == QStringLiteral("FAILED") ||
        m_paymasterSessionState == QStringLiteral("CONFLICTED");
    const bool safe_recovery = m_paymasterSessionPersisted && signed_artifact &&
                               signed_state_consistent && !terminal;
    if (terminal) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::NEW_TRANSFER;
        m_paymasterPrimaryButton->setText(tr("Start a new transfer"));
    } else if (exact_offer_reviewable) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::REVIEW_OFFER;
        m_paymasterPrimaryButton->setText(tr("Review exact offer"));
    } else if (safe_fallback) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::FALLBACK;
        m_paymasterPrimaryButton->setText(
            m_paymasterAttemptState == QStringLiteral("REJECTED")
                ? tr("Check another offer")
                : tr("Try another Paymaster"));
    } else if ((m_paymasterSessionState == QStringLiteral("FAILED") ||
                m_paymasterSessionState == QStringLiteral("CONFLICTED")) && safe_recovery) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::RECOVER;
        m_paymasterPrimaryButton->setText(tr("Start safe recovery"));
    } else {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::REFRESH;
        m_paymasterPrimaryButton->setText(tr("Check current status"));
    }

    const bool have_persisted = m_paymasterSessionPersisted;
    const bool abandon_state =
        m_paymasterSessionState == QStringLiteral("CREATED") ||
        m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_WALLET_UNLOCK") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE") ||
        m_paymasterSessionState == QStringLiteral("FAILED");
    const bool unsigned_state_consistent = abandon_state || fallback_state;
    const bool consistent_artifact_state =
        (artifact_none && unsigned_state_consistent) ||
        (signed_artifact && signed_state_consistent);
    const bool can_retry = have_persisted && consistent_artifact_state && !terminal;
    const bool no_pending_provider_action =
        m_paymasterPendingPhase.isEmpty() || m_paymasterPendingPhase == QStringLiteral("NONE");
    const bool can_fallback = safe_fallback;
    const bool can_abandon = have_persisted && artifact_none && abandon_state &&
                             no_pending_provider_action && !terminal;
    const bool can_recover = safe_recovery;
    const bool can_stop = m_paymasterPollTimer->isActive() &&
                          consistent_artifact_state && !terminal;
    m_retrySessionButton->setVisible(can_retry);
    m_fallbackSessionButton->setVisible(
        can_fallback && m_paymasterPrimaryAction != PaymasterPrimaryAction::FALLBACK);
    m_abandonSessionButton->setVisible(can_abandon);
    m_recoverSessionButton->setVisible(
        can_recover && m_paymasterPrimaryAction != PaymasterPrimaryAction::RECOVER);
    m_cancelQuoteButton->setVisible(can_stop);
    const bool have_more = can_retry || can_fallback || can_abandon || can_recover || can_stop;
    m_paymasterMoreButton->setVisible(have_more);
    if (!have_more) {
        m_paymasterMoreButton->setChecked(false);
        m_paymasterSecondaryActions->hide();
    }
}

void DigiDollarSendWidget::onPaymasterPrimaryAction()
{
    switch (m_paymasterPrimaryAction) {
    case PaymasterPrimaryAction::REVIEW_OFFER:
        executePaymasterTransfer(m_paymasterAddress, m_paymasterAmount, /*allow_unlock=*/false);
        break;
    case PaymasterPrimaryAction::FALLBACK:
        fallbackPaymasterSession();
        break;
    case PaymasterPrimaryAction::RECOVER:
        recoverPaymasterSessionToSelf();
        break;
    case PaymasterPrimaryAction::NEW_TRANSFER:
        m_paymasterPollTimer->stop();
        m_paymasterRequestId.clear();
        m_paymasterSessionId.clear();
        m_paymasterSessionState.clear();
        m_paymasterAttemptState.clear();
        m_paymasterArtifact.clear();
        m_paymasterPendingPhase.clear();
        m_paymasterBroadcastState.clear();
        m_paymasterConfirmationState.clear();
        m_paymasterSessionPersisted = false;
        m_paymasterAddress.clear();
        m_paymasterAmount = 0.0;
        m_paymasterInitialAvailableBalance = 0.0;
        m_sendAllSpendableDD = false;
        if (m_subtractPaymasterFeeCheck) m_subtractPaymasterFeeCheck->setChecked(false);
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterSessionPrivacy.clear();
        m_paymasterRecoveryAuthorizationCommitment.clear();
        m_paymasterRecoveryMaximumServiceFeeCents = 0;
        m_paymasterRecoveryActive = false;
        m_paymasterConfirmationGuard.Reset();
        m_paymasterRecoveryConfirmationGuard.Reset();
        m_paymasterMoreButton->setChecked(false);
        m_paymasterTechnicalButton->setChecked(false);
        m_paymasterStateValue->setText(tr("No active session"));
        m_paymasterIdentityValue->setText(tr("Provider not selected yet"));
        m_paymasterCostValue->setText(tr("No service fee authorized yet"));
        m_paymasterExpiryValue->setText(tr("—"));
        m_addressEdit->clear();
        m_amountEdit->clear();
        if (m_noteEdit) m_noteEdit->clear();
        updatePaymasterFocusMode();
        updateSendButton();
        break;
    case PaymasterPrimaryAction::REFRESH:
        if (m_paymasterSessionPersisted) refreshPaymasterSessionState();
        else pollPaymasterSession();
        break;
    }
}

void DigiDollarSendWidget::showPaymasterExplanation()
{
    QMessageBox::information(
        this, tr("What is a Paymaster?"),
        tr("A Paymaster is an independent provider that supplies the DGB needed for "
           "the DigiByte network fee.\n\n"
           "• User-paid offer: you pay an additional service fee in $DD.\n"
           "• Sponsored offer: the provider pays the network fee and charges no $DD service fee.\n\n"
           "The provider never receives your wallet keys. Your wallet reconstructs and checks "
           "the complete transaction locally, and you see the exact provider and service fee "
           "again before your wallet signs.\n\n"
           "A provider necessarily receives the payment details needed to participate. "
           "Paymaster transport improves pseudonymity but does not provide complete anonymity."));
}

void DigiDollarSendWidget::configureClientSafetyPolicy()
{
    if (!m_walletModel) {
        showWarning(tr("Paymaster service-fee limits"),
                    tr("Select an available wallet before configuring its limits."));
        return;
    }

    QDialog dialog(this);
    // This is a DigiDollar-owned dialog, so give the shared theme a stable
    // selector instead of inheriting the application's blue default dialog
    // palette. The selector also scopes visible spin-box arrows to this modal.
    dialog.setObjectName(QStringLiteral("digiDollarClientSafetyDialog"));
    dialog.setWindowTitle(tr("Set Paymaster service-fee limits"));
    dialog.setMinimumWidth(520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(tr(
        "These wallet-local limits protect your $DD. A Paymaster offer can never charge more "
        "than the lower of these limits and the limit selected for the transfer. Limits are "
        "not shared with providers. Both values must be positive."), &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto* form = new QFormLayout();
    auto* per_transfer = new NoWheelSpinBox(&dialog);
    per_transfer->setObjectName("clientSafetyPerTransferDialog");
    per_transfer->setRange(1, 10000000);
    per_transfer->setValue(static_cast<int>(m_clientSafetyMaximumPerTransaction));
    per_transfer->setSuffix(tr(" cents"));
    auto* per_day = new NoWheelSpinBox(&dialog);
    per_day->setObjectName("clientSafetyPerDayDialog");
    per_day->setRange(1, 10000000);
    per_day->setValue(static_cast<int>(m_clientSafetyMaximumPerDay));
    per_day->setSuffix(tr(" cents"));
    form->addRow(tr("Maximum per transfer:"), per_transfer);
    form->addRow(tr("Maximum in a rolling day:"), per_day);
    layout->addLayout(form);

    auto* recommendation = new QLabel(
        tr("Recommended starting values: 1.00 $DD (100 cents) per transfer and "
           "10.00 $DD (1,000 cents) per rolling day."), &dialog);
    recommendation->setWordWrap(true);
    layout->addWidget(recommendation);
    auto* restore = new QPushButton(tr("Restore recommended limits"), &dialog);
    restore->setObjectName(QStringLiteral("clientSafetyRestoreDefaultsButton"));
    connect(restore, &QPushButton::clicked, &dialog, [per_transfer, per_day] {
        per_transfer->setValue(100);
        per_day->setValue(1000);
    });
    layout->addWidget(restore);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("clientSafetyDialogButtons"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    if (per_day->value() < per_transfer->value()) {
        showWarning(tr("Invalid Paymaster service-fee limits"),
                    tr("The rolling-day limit must be at least as large as the per-transfer limit."));
        return;
    }

    UniValue policy{UniValue::VOBJ};
    policy.pushKV("maximum_service_fee_per_transaction_cents", per_transfer->value());
    policy.pushKV("maximum_service_fee_per_day_cents", per_day->value());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(policy));
    m_configureClientSafetyButton->setEnabled(false);
    m_clientSafetyStatus->setText(tr("Saving wallet-local Paymaster service-fee limits…"));
    QPointer<DigiDollarSendWidget> guard{this};
    WalletModel* requested_model = m_walletModel;
    m_walletModel->executeRpcAsync("setpaymasterclientsafetypolicy", std::move(params),
        [guard, requested_model](UniValue result, QString error) {
            if (!guard || guard->m_walletModel != requested_model) return;
            guard->m_configureClientSafetyButton->setEnabled(true);
            if (!error.isEmpty()) {
                guard->m_clientSafetyStatusKnown = true;
                guard->m_clientSafetyConfigured = false;
                guard->m_clientSafetyError = error;
                guard->updateClientSafetyDisplay();
                guard->showWarning(guard->tr("Paymaster service-fee limits"), error);
                return;
            }
            guard->m_clientSafetyMaximumPerTransaction =
                result.find_value("maximum_service_fee_per_transaction_cents").getInt<qint64>();
            guard->m_clientSafetyMaximumPerDay =
                result.find_value("maximum_service_fee_per_day_cents").getInt<qint64>();
            guard->m_clientSafetyStatusKnown = true;
            guard->m_clientSafetyConfigured = true;
            guard->m_clientSafetyError.clear();
            if (guard->m_feeCapSpin->value() > guard->m_clientSafetyMaximumPerTransaction) {
                guard->m_feeCapSpin->setValue(
                    static_cast<int>(guard->m_clientSafetyMaximumPerTransaction));
            }
            guard->refreshClientSafetyStatus();
        });
}

void DigiDollarSendWidget::refreshClientSafetyStatus()
{
    if (!m_walletModel) {
        m_clientSafetyStatusKnown = false;
        m_clientSafetyConfigured = false;
        updateClientSafetyDisplay();
        return;
    }
    m_clientSafetyStatusKnown = false;
    m_clientSafetyError.clear();
    updateClientSafetyDisplay();
    QPointer<DigiDollarSendWidget> guard{this};
    WalletModel* requested_model = m_walletModel;
    m_walletModel->executeRpcAsync("getpaymasterclientsafetystatus", UniValue{UniValue::VARR},
        [guard, requested_model](UniValue result, QString error) {
            if (!guard || guard->m_walletModel != requested_model) return;
            guard->m_clientSafetyStatusKnown = true;
            guard->m_clientSafetyConfigured = error.isEmpty() &&
                result.find_value("configured").isBool() &&
                result.find_value("configured").get_bool();
            guard->m_clientSafetyError = error;
            const UniValue& policy = result.find_value("policy");
            if (guard->m_clientSafetyConfigured && policy.isObject()) {
                guard->m_clientSafetyMaximumPerTransaction =
                    policy.find_value("maximum_service_fee_per_transaction_cents").getInt<qint64>();
                guard->m_clientSafetyMaximumPerDay =
                    policy.find_value("maximum_service_fee_per_day_cents").getInt<qint64>();
                guard->m_clientSafetyActiveReservations =
                    result.find_value("active_reservations").getInt<qint64>();
                guard->m_clientSafetyReservedCents =
                    result.find_value("reserved_service_fee_cents").getInt<qint64>();
                guard->m_clientSafetySpentTodayCents =
                    result.find_value("spent_service_fee_last_day_cents").getInt<qint64>();
                guard->m_clientSafetyAvailableTodayCents =
                    result.find_value("available_service_fee_today_cents").getInt<qint64>();
                if (guard->m_feeCapSpin->value() > guard->m_clientSafetyMaximumPerTransaction) {
                    guard->m_feeCapSpin->setValue(
                        static_cast<int>(guard->m_clientSafetyMaximumPerTransaction));
                }
            }
            guard->updateClientSafetyDisplay();
            guard->updateFeeDisplay();
            guard->updateSendButton();
        });
}

void DigiDollarSendWidget::updateClientSafetyDisplay()
{
    if (!m_clientSafetyStatus || !m_configureClientSafetyButton) return;
    m_configureClientSafetyButton->setEnabled(m_walletModel && !m_paymasterBusy);
    m_clientSafetyStatus->setToolTip(QString());
    if (!m_walletModel) {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::INFO);
        m_clientSafetyStatus->setText(
            tr("ℹ Select a wallet to check its Paymaster service-fee limits."));
        if (m_clientSafetyDetails) m_clientSafetyDetails->setText(tr("No wallet selected."));
    } else if (!m_clientSafetyStatusKnown) {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::WAITING);
        m_clientSafetyStatus->setText(
            tr("… Checking this wallet's Paymaster service-fee limits…"));
        if (m_clientSafetyDetails) m_clientSafetyDetails->setText(tr("Loading wallet protection details…"));
    } else if (!m_clientSafetyError.isEmpty()) {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::ERR);
        m_clientSafetyStatus->setText(tr(
            "✕ Paymaster unavailable · wallet protection could not be read. "
            "Open Advanced Paymaster settings for technical details."));
        m_clientSafetyStatus->setToolTip(m_clientSafetyError);
        if (m_clientSafetyDetails) m_clientSafetyDetails->setText(m_clientSafetyError);
    } else if (!m_clientSafetyConfigured) {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::ACTION);
        m_clientSafetyStatus->setText(tr(
            "! Action required · Set wallet-local Paymaster service-fee limits before sending. "
            "They cap how much $DD a provider may charge."));
        if (m_clientSafetyDetails) m_clientSafetyDetails->setText(tr("No positive wallet-local Paymaster limits are saved."));
    } else {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::SUCCESS);
        m_clientSafetyStatus->setText(tr(
            "✓ Protection active · maximum %1 per transfer · %2 per rolling day")
            .arg(formatCents(m_clientSafetyMaximumPerTransaction),
                 formatCents(m_clientSafetyMaximumPerDay)));
        m_clientSafetyStatus->setToolTip(tr(
            "Available today: %1\nReserved: %2 in %3 session(s)\nSpent today: %4\n"
            "The transfer-specific limit can only reduce these wallet limits.")
            .arg(formatCents(m_clientSafetyAvailableTodayCents),
                 formatCents(m_clientSafetyReservedCents))
            .arg(m_clientSafetyActiveReservations)
            .arg(formatCents(m_clientSafetySpentTodayCents)));
        if (m_clientSafetyDetails) {
            m_clientSafetyDetails->setText(tr(
                "Available today: %1 · reserved: %2 in %3 session(s) · spent today: %4. "
                "The transfer-specific limit below can only reduce these wallet limits.")
                .arg(formatCents(m_clientSafetyAvailableTodayCents),
                     formatCents(m_clientSafetyReservedCents))
                .arg(m_clientSafetyActiveReservations)
                .arg(formatCents(m_clientSafetySpentTodayCents)));
        }
    }
    m_clientSafetyFrame->setAccessibleDescription(m_clientSafetyStatus->text());
}

void DigiDollarSendWidget::setPaymasterBusy(bool busy)
{
    m_paymasterBusy = busy;
    updateSendButton();
    m_refreshOffersButton->setEnabled(!busy);
    updateClientSafetyDisplay();
    const bool have_persisted_session =
        !m_paymasterRequestId.isEmpty() && m_paymasterSessionPersisted;
    m_retrySessionButton->setEnabled(!busy && have_persisted_session);
    m_fallbackSessionButton->setEnabled(!busy && have_persisted_session);
    m_recoverSessionButton->setEnabled(!busy && have_persisted_session);
    m_abandonSessionButton->setEnabled(!busy && have_persisted_session);
    m_cancelQuoteButton->setEnabled(!busy && have_persisted_session);
    m_paymasterPrimaryButton->setEnabled(!busy);
    updatePaymasterFocusMode();
}

void DigiDollarSendWidget::refreshPaymasterOffers()
{
    if (!m_walletModel || m_paymasterBusy) return;
    const CAmount amount_cents = static_cast<CAmount>(std::llround(m_amountEdit->text().toDouble() * 100));
    if (amount_cents <= 0) {
        DigiDollarStatus::SetBanner(m_offersStatus, DigiDollarStatus::Kind::ACTION);
        m_offersStatus->setText(tr("! Action required · enter a valid $DD amount before requesting Paymaster offers."));
        showWarning(tr("Paymaster offers"), tr("Enter an amount before refreshing offers."));
        return;
    }
    DigiDollarStatus::SetBanner(m_offersStatus, DigiDollarStatus::Kind::WAITING);
    m_offersStatus->setText(tr("… Looking for eligible Paymaster offers…"));
    setPaymasterBusy(true);
    UniValue params{UniValue::VARR};
    params.push_back(amount_cents);
    UniValue preview_options{UniValue::VOBJ};
    preview_options.pushKV(
        "subtract_paymaster_fee_from_amount",
        m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked());
    params.push_back(std::move(preview_options));
    QPointer<DigiDollarSendWidget> guard{this};
    m_walletModel->executeRpcAsync("getpaymasteroffers", std::move(params),
        [guard](UniValue result, QString error) {
            if (!guard) return;
            guard->setPaymasterBusy(false);
            guard->m_offersTable->setRowCount(0);
            guard->m_paymasterPreviewRecipientCents = -1;
            guard->m_paymasterPreviewServiceFeeCents = -1;
            guard->m_paymasterPreviewTotalCents = -1;
            if (!error.isEmpty()) {
                DigiDollarStatus::SetBanner(guard->m_offersStatus, DigiDollarStatus::Kind::ERR);
                guard->m_offersStatus->setText(guard->tr(
                    "✕ Offers are currently unavailable. No provider was selected and no fee was authorized."));
                guard->showWarning(guard->tr("Paymaster offers unavailable"), error);
                return;
            }
            int row{0};
            for (const UniValue& offer : result.getValues()) {
                guard->m_offersTable->insertRow(row);
                const QString provider = QString::fromStdString(offer.find_value("display_name").get_str());
                const QString model = QString::fromStdString(offer.find_value("funding_model").get_str());
                const qint64 fee = offer.find_value("service_fee_cents").getInt<qint64>();
                const qint64 payment = offer.find_value("payment_cents").getInt<qint64>();
                const qint64 total = offer.find_value("user_total_cents").getInt<qint64>();
                if (row == 0) {
                    guard->m_paymasterPreviewRecipientCents = payment;
                    guard->m_paymasterPreviewServiceFeeCents = fee;
                    guard->m_paymasterPreviewTotalCents = total;
                }
                const bool enough = offer.find_value("reputation_sufficient_data").get_bool();
                QString reputation = enough
                    ? guard->tr("%1%").arg(offer.find_value("success_rate_basis_points").getInt<int>() / 100.0, 0, 'f', 2)
                    : guard->tr("New provider — not enough history yet");
                const qint64 expiry = offer.find_value("expires_at").getInt<qint64>();
                const QStringList cells{provider, guard->friendlyFundingModel(model),
                                        guard->formatCents(fee), guard->formatCents(total), reputation,
                                        QDateTime::fromSecsSinceEpoch(expiry).toLocalTime().toString(Qt::ISODate)};
                for (int column = 0; column < cells.size(); ++column) {
                    auto* item = new QTableWidgetItem(cells[column]);
                    item->setToolTip(QString::fromStdString(offer.find_value("provider_id").get_str()) +
                                     QStringLiteral("\n") +
                                     QString::fromStdString(offer.find_value("policy_hash").get_str()));
                    guard->m_offersTable->setItem(row, column, item);
                }
                ++row;
            }
            DigiDollarStatus::SetBanner(
                guard->m_offersStatus,
                row == 0 ? DigiDollarStatus::Kind::WAITING : DigiDollarStatus::Kind::SUCCESS);
            guard->m_offersStatus->setText(row == 0
                ? (guard->m_subtractPaymasterFeeCheck->isChecked()
                       ? guard->tr("… No exact offer is available. Try a sponsored offer, another provider or a slightly different total.")
                       : guard->tr("… No eligible public offer currently matches this amount and your limits."))
                : guard->tr("✓ %1 eligible offer(s) shown as a preview. Core will authenticate, bind and re-confirm the exact selection before signing.").arg(row));
            guard->updateFeeDisplay();
        });
}

UniValue DigiDollarSendWidget::buildPaymasterSendParams(const QString& address, CAmount amount_cents) const
{
    UniValue params{UniValue::VARR};
    params.push_back(address.toStdString());
    params.push_back(amount_cents);
    params.push_back(m_noteEdit ? m_noteEdit->text().trimmed().toStdString() : std::string{});
    params.push_back(0);
    if (m_coinControl && m_coinControl->HasSelected()) {
        UniValue selected{UniValue::VARR};
        for (const COutPoint& outpoint : m_coinControl->ListSelected()) {
            UniValue input{UniValue::VOBJ};
            input.pushKV("txid", outpoint.hash.GetHex());
            input.pushKV("vout", outpoint.n);
            selected.push_back(std::move(input));
        }
        params.push_back(std::move(selected));
    } else {
        params.push_back(UniValue{});
    }
    UniValue options{UniValue::VOBJ};
    options.pushKV("fee_mode", m_feeModeCombo->currentData().toString().toStdString());
    options.pushKV("request_id", m_paymasterRequestId.toStdString());
    options.pushKV("maximum_paymaster_fee_cents", m_feeCapSpin->value());
    options.pushKV("maximum_provider_attempts", m_maxAttemptsSpin->value());
    options.pushKV("privacy", m_privacyCombo->currentData().toString().toStdString());
    options.pushKV("selection", m_selectionCombo->currentData().toString().toStdString());
    options.pushKV(
        "subtract_paymaster_fee_from_amount",
        m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked());
    options.pushKV("send_all_spendable_dd", m_sendAllSpendableDD);
    if (m_paymasterAuthorizationCommitment.isEmpty()) {
        options.pushKV("prepare_only", true);
    } else {
        options.pushKV("authorization_commitment",
                       m_paymasterAuthorizationCommitment.toStdString());
    }
    params.push_back(std::move(options));
    return params;
}

void DigiDollarSendWidget::executePaymasterTransfer(const QString& address, double amount, bool allow_unlock)
{
    // The first call prepares and authenticates an offer only. Signing is never
    // inferred from clicking Send: handlePaymasterResult requires the exact
    // authorization commitment and presents the second confirmation separately.
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty()) return;
    std::shared_ptr<WalletModel::UnlockContext> unlock;
    if (allow_unlock) {
        unlock = m_walletModel->requestUnlockForAsync();
        if (!unlock->isValid()) {
            m_paymasterStateValue->setText(tr("AWAITING_WALLET_UNLOCK"));
            return;
        }
    }
    setPaymasterBusy(true);
    m_paymasterSessionFrame->show();
    m_paymasterStateValue->setText(tr("Contacting Paymaster Network…"));
    const CAmount amount_cents = static_cast<CAmount>(std::llround(amount * 100));
    QPointer<DigiDollarSendWidget> guard{this};
    m_walletModel->executeRpcAsync("senddigidollar", buildPaymasterSendParams(address, amount_cents),
        [guard, address, amount, unlock = std::move(unlock)](UniValue result, QString error) mutable {
            if (guard) guard->handlePaymasterResult(result, error, address, amount);
            unlock.reset();
        });
}

void DigiDollarSendWidget::handlePaymasterResult(const UniValue& result, const QString& error,
                                                 const QString& address, double amount)
{
    setPaymasterBusy(false);
    if (!error.isEmpty()) {
        if (error.contains(QStringLiteral("PAYMASTER_AUTHORIZATION_COMMITMENT_REQUIRED")) ||
            error.contains(QStringLiteral("PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH")) ||
            error.contains(QStringLiteral("PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED")) ||
            error.contains(QStringLiteral("Paymaster quote expired"))) {
            m_paymasterAuthorizationCommitment.clear();
            m_paymasterConfirmationGuard.Reset();
        }
        if ((error.contains(QStringLiteral("PAYMASTER_NO_ELIGIBLE_OFFER")) ||
             error.contains(QStringLiteral("PAYMASTER_NO_EXACT_GROSS_OFFER")) ||
             error.contains(QStringLiteral("PAYMASTER_SWEEP_BALANCE_CHANGED"))) &&
            !m_paymasterSessionPersisted) {
            // Offer selection failed before Core returned a durable session.
            // Do not present retry/recovery actions for a local request id that
            // has no authoritative wallet state behind it. A later Send click
            // must receive a fresh request id so newly announced offers can be
            // considered immediately.
            m_paymasterRequestId.clear();
            m_paymasterSessionId.clear();
            m_paymasterSessionState.clear();
            m_paymasterAttemptState.clear();
            m_paymasterArtifact.clear();
            m_paymasterPendingPhase.clear();
            m_paymasterBroadcastState.clear();
            m_paymasterConfirmationState.clear();
            m_paymasterSessionPersisted = false;
            m_paymasterAuthorizationCommitment.clear();
            m_paymasterConfirmationGuard.Reset();
            m_paymasterStateValue->setText(tr(
                "No Paymaster transfer was created. Any unsigned input reservation was released. Refresh offers and send again."));
            m_paymasterIdentityValue->setText(tr("No provider selected"));
            m_paymasterCostValue->setText(tr("No service fee reserved or authorized"));
            m_paymasterExpiryValue->setText(tr("—"));
            setPaymasterBusy(false);
        } else {
            m_paymasterStateValue->setText(tr("Paymaster error: %1").arg(error));
        }
        m_paymasterPollTimer->stop();
        updatePaymasterFocusMode();
        showBackendError(static_cast<int>(WalletModel::TransactionCreationFailed), error);
        return;
    }
    updatePaymasterSessionView(result);
    // updatePaymasterSessionView() may have learned that Core persisted the
    // session. Re-evaluate the recovery controls with that authoritative fact.
    setPaymasterBusy(false);
    const UniValue& txid = result.find_value("txid");
    const UniValue& final = result.find_value("final");
    const UniValue& direct_status = result.find_value("status");
    const UniValue& result_status = result.find_value("result_status");
    const QString state = QString::fromStdString(result.find_value("session_state").isStr()
                                                     ? result.find_value("session_state").get_str()
                                                     : std::string{});
    if (IsValidatedPaymasterCompletion(
            txid.isStr(), state,
            direct_status.isStr()
                ? QString::fromStdString(direct_status.get_str())
                : QString{},
            result_status.isStr()
                ? QString::fromStdString(result_status.get_str())
                : QString{},
            final.isBool() && final.get_bool())) {
        // A FINAL_COMMITTED result reaches Qt only after Core has reloaded the
        // durable client manifest, validated every final input/output and
        // witness, and accepted or observed the exact transaction. The
        // response echo is presentation metadata at this point; a missing echo
        // must not turn a completed payment into a false authorization error.
        m_paymasterPollTimer->stop();
        const UniValue& payment = result.find_value("payment_cents");
        const double recipient_amount = payment.isNum()
            ? payment.getInt<qint64>() / 100.0
            : amount;
        showSuccess(QString::fromStdString(txid.get_str()), recipient_amount);
        onClearClicked();
        updateBalance();
        return;
    }
    const UniValue& returned_commitment_value = result.find_value("authorization_commitment");
    const QString returned_commitment = returned_commitment_value.isStr()
        ? QString::fromStdString(returned_commitment_value.get_str())
        : QString{};
    if (!m_paymasterAuthorizationCommitment.isEmpty() &&
        (returned_commitment.isEmpty() ||
         returned_commitment != m_paymasterAuthorizationCommitment)) {
        m_paymasterPollTimer->stop();
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterConfirmationGuard.Reset();
        m_paymasterStateValue->setText(
            tr("Paymaster authorization blocked: commitment changed or missing"));
        showWarning(
            tr("Paymaster authorization blocked"),
            tr("The wallet did not return the exact authorization commitment that was "
               "approved. No signature or automatic retry will continue. Review the "
               "current provider offer again."));
        return;
    }
    const UniValue& authorization_required_value =
        result.find_value("authorization_required");
    const bool authorization_required = authorization_required_value.isBool() &&
        authorization_required_value.get_bool();
    if (authorization_required) {
        m_paymasterPollTimer->stop();
        if (returned_commitment.isEmpty()) {
            m_paymasterStateValue->setText(
                tr("Paymaster authorization blocked: missing exact commitment"));
            showWarning(
                tr("Paymaster authorization blocked"),
                tr("The prepared Paymaster transaction did not include its exact client "
                   "authorization commitment. No Qt authorization will continue."));
            return;
        }
        if (!confirmPaymasterSelectionBeforeSigning(result, address)) return;
        m_paymasterAuthorizationCommitment = returned_commitment;
        executePaymasterTransfer(address, amount, /*allow_unlock=*/true);
        return;
    }
    if (state == QStringLiteral("AWAITING_WALLET_UNLOCK")) {
        // Before a quote exists, Core may need the unlocked wallet only for
        // the input-control proof. prepare_only remains set, so this call
        // cannot produce the collaborative transaction signature. If the
        // exact authorization was already approved, the same unlock also
        // permits the separately committed authorize stage.
        m_paymasterPollTimer->stop();
        executePaymasterTransfer(address, amount, /*allow_unlock=*/true);
        return;
    }
    if (!m_paymasterPollTimer->isActive()) m_paymasterPollTimer->start();
    updatePaymasterFocusMode();
}

void DigiDollarSendWidget::updatePaymasterSessionView(const UniValue& result)
{
    m_paymasterSessionFrame->show();
    const UniValue& embedded_session = result.find_value("session");
    const UniValue& session_view = embedded_session.isObject() ? embedded_session : result;
    const auto string_value = [&](const char* key) {
        const UniValue& value = session_view.find_value(key);
        return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
    };
    const QString session = string_value("session_id");
    if (!session.isEmpty()) {
        m_paymasterSessionId = session;
        m_paymasterSessionPersisted = true;
    }
    const QString state = string_value("session_state");
    m_paymasterSessionState = state;
    const QString phase = string_value("pending_phase");
    const QString broadcast = string_value("broadcast_state");
    const QString confirmation = string_value("confirmation_state");
    m_paymasterPendingPhase = phase;
    m_paymasterBroadcastState = broadcast;
    m_paymasterConfirmationState = confirmation;
    const UniValue& artifact = result.find_value("artifact");
    if (artifact.isStr()) m_paymasterArtifact = QString::fromStdString(artifact.get_str());
    const UniValue& attempt = result.find_value("attempt");
    if (attempt.isObject() && attempt.find_value("attempt_state").isStr()) {
        m_paymasterAttemptState =
            QString::fromStdString(attempt.find_value("attempt_state").get_str());
    } else if (result.find_value("attempt_state").isStr()) {
        m_paymasterAttemptState =
            QString::fromStdString(result.find_value("attempt_state").get_str());
    }
    QString status = phase.isEmpty() ? state : tr("%1 — %2").arg(state, phase);
    if (!broadcast.isEmpty() || !confirmation.isEmpty()) {
        status += tr(" · broadcast: %1 · confirmation: %2")
            .arg(broadcast.isEmpty() ? tr("unknown") : broadcast,
                 confirmation.isEmpty() ? tr("unknown") : confirmation);
    }
    m_paymasterStateValue->setText(status);
    QString provider = string_value("provider_id");
    QString policy = string_value("policy_hash");
    if (provider.isEmpty() && attempt.isObject() && attempt.find_value("provider_id").isStr()) {
        provider = QString::fromStdString(attempt.find_value("provider_id").get_str());
    }
    m_paymasterIdentityValue->setText(provider.isEmpty()
        ? tr("Provider not selected yet")
        : tr("Selected provider · %1…").arg(provider.left(12)));
    m_paymasterIdentityValue->setToolTip(
        tr("Provider: %1\nPolicy: %2").arg(provider, policy));
    const UniValue& payment = result.find_value("payment_cents");
    const UniValue& fee = result.find_value("service_fee_cents");
    const UniValue& total = result.find_value("user_total_cents");
    if (payment.isNum() && fee.isNum() && total.isNum()) {
        m_paymasterCostValue->setText(tr("Recipient %1 + service fee %2 = %3 maximum wallet outflow (limit %4)")
            .arg(formatCents(payment.getInt<qint64>()), formatCents(fee.getInt<qint64>()),
                 formatCents(total.getInt<qint64>()), formatCents(m_feeCapSpin->value())));
    }
    const UniValue& expiry = result.find_value("expires_at");
    if (expiry.isNum()) {
        m_paymasterExpiryValue->setText(
            QDateTime::fromSecsSinceEpoch(expiry.getInt<qint64>()).toLocalTime().toString(Qt::ISODate));
    }
    updatePaymasterFocusMode();
}

PaymasterConfirmationSelection DigiDollarSendWidget::paymasterConfirmationSelection(
    const UniValue& result, const QString& address) const
{
    const auto string_value = [&result](const char* key) {
        const UniValue& value = result.find_value(key);
        return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
    };
    PaymasterConfirmationSelection selection;
    selection.provider_id = string_value("provider_id");
    selection.offer_id = string_value("offer_id");
    selection.policy_hash = string_value("policy_hash");
    selection.funding_model = string_value("funding_model");
    selection.recipient = address;
    selection.authorization_commitment = string_value("authorization_commitment");
    const UniValue& payment = result.find_value("payment_cents");
    if (payment.isNum()) selection.payment_cents = payment.getInt<qint64>();
    const UniValue& fee = result.find_value("service_fee_cents");
    if (fee.isNum()) selection.service_fee_cents = fee.getInt<qint64>();
    const UniValue& total = result.find_value("user_total_cents");
    if (total.isNum()) selection.user_total_cents = total.getInt<qint64>();
    return selection;
}

bool DigiDollarSendWidget::confirmPaymasterSelectionBeforeSigning(
    const UniValue& result, const QString& address)
{
    // Display values are taken from Core's bound selection, not recomputed from
    // the currently visible widgets. Any provider, model, recipient, amount, or
    // fee change therefore produces a new commitment and another confirmation.
    const PaymasterConfirmationSelection selection =
        paymasterConfirmationSelection(result, address);
    if (!selection.IsComplete()) {
        m_paymasterStateValue->setText(tr("Paymaster authorization blocked: incomplete exact offer details"));
        showWarning(
            tr("Paymaster authorization blocked"),
            tr("The exact provider, offer, policy, funding model, recipient, amount, "
               "service fee, total outflow or validated authorization commitment was "
               "not returned consistently by the wallet. "
               "No Qt authorization will continue until those details are available."));
        return false;
    }
    if (!m_paymasterConfirmationGuard.RequiresConfirmation(selection)) return true;

    QStringList changed_fields;
    for (const QString& field : m_paymasterConfirmationGuard.ChangedFields(selection)) {
        if (field == QStringLiteral("provider")) changed_fields.push_back(tr("provider"));
        else if (field == QStringLiteral("offer")) changed_fields.push_back(tr("offer"));
        else if (field == QStringLiteral("policy")) changed_fields.push_back(tr("provider policy"));
        else if (field == QStringLiteral("funding_model")) changed_fields.push_back(tr("funding model"));
        else if (field == QStringLiteral("recipient")) changed_fields.push_back(tr("recipient"));
        else if (field == QStringLiteral("amount")) changed_fields.push_back(tr("amount"));
        else if (field == QStringLiteral("service_fee")) changed_fields.push_back(tr("service fee"));
        else if (field == QStringLiteral("total")) changed_fields.push_back(tr("total wallet outflow"));
        else if (field == QStringLiteral("authorization_commitment")) {
            changed_fields.push_back(tr("transaction or capacity commitment"));
        }
    }
    const QString prompt = tr(
        "Review the exact Paymaster offer before your wallet signs:\n\n"
        "Provider: %1\nPayment model: %2\nRecipient: %3\n\n"
        "Total $DD outflow: %6\nRecipient receives: %4\nProvider receives: %5\n"
        "Spendable $DD remaining after this transfer: %9\n\n"
        "Technical authorization commitment: %7\n\n"
        "New or changed fields: %8\n\nContinue to wallet unlock and signature?")
        .arg(selection.provider_id, friendlyFundingModel(selection.funding_model), selection.recipient)
        .arg(formatCents(selection.payment_cents))
        .arg(formatCents(selection.service_fee_cents))
        .arg(formatCents(selection.user_total_cents))
        .arg(selection.authorization_commitment)
        .arg(changed_fields.join(tr(", ")))
        .arg(formatDDAmount(std::max(
            0.0, m_paymasterInitialAvailableBalance -
                     selection.user_total_cents / 100.0)));
    if (QMessageBox::question(this, tr("Confirm exact Paymaster authorization"), prompt,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
        m_paymasterStateValue->setText(tr("Paymaster authorization paused by user"));
        return false;
    }
    return m_paymasterConfirmationGuard.Accept(selection);
}

void DigiDollarSendWidget::pollPaymasterSession()
{
    if (m_paymasterBusy || m_paymasterRequestId.isEmpty()) return;
    if (m_paymasterRecoveryActive) {
        executeAlternativePaymasterRecovery(/*allow_unlock=*/false);
    } else if (!m_paymasterAddress.isEmpty()) {
        executePaymasterTransfer(m_paymasterAddress, m_paymasterAmount, /*allow_unlock=*/false);
    }
}

void DigiDollarSendWidget::refreshPaymasterSessionState()
{
    // Polling observes durable state only. It does not authorize retries,
    // provider changes, cancellation, or recovery; those actions remain gated
    // by the artifact/state fields returned by resolvepaymastersession.
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty() ||
        !m_paymasterSessionPersisted) {
        return;
    }
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(lookup));
    params.push_back("refresh");
    setPaymasterBusy(true);
    QPointer<DigiDollarSendWidget> guard{this};
    m_walletModel->executeRpcAsync("resolvepaymastersession", std::move(params),
        [guard](UniValue result, QString error) {
            if (!guard) return;
            guard->setPaymasterBusy(false);
            if (!error.isEmpty()) {
                guard->showWarning(guard->tr("Paymaster status unavailable"), error);
                return;
            }
            guard->updatePaymasterSessionView(result);
        });
}

void DigiDollarSendWidget::retryPaymasterSession()
{
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty()) return;
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(lookup));
    params.push_back("retry_same");
    setPaymasterBusy(true);
    QPointer<DigiDollarSendWidget> guard{this};
    m_walletModel->executeRpcAsync("resolvepaymastersession", std::move(params),
        [guard](UniValue result, QString error) {
            if (!guard) return;
            guard->setPaymasterBusy(false);
            if (!error.isEmpty()) {
                guard->showWarning(guard->tr("Paymaster recovery"), error);
                return;
            }
            guard->updatePaymasterSessionView(result);
            if (!guard->m_paymasterPollTimer->isActive()) guard->m_paymasterPollTimer->start();
        });
}

void DigiDollarSendWidget::fallbackPaymasterSession()
{
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty()) return;
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(lookup));
    params.push_back("fallback");
    params.push_back(UniValue{UniValue::VOBJ});
    setPaymasterBusy(true);
    m_walletModel->executeRpcAsync("resolvepaymastersession", std::move(params),
        [guard = QPointer<DigiDollarSendWidget>(this)](UniValue result, QString error) {
            if (!guard) return;
            guard->setPaymasterBusy(false);
            if (!error.isEmpty()) {
                guard->m_paymasterStateValue->setText(
                    guard->tr("Paymaster fallback unavailable: %1").arg(error));
                return;
            }
            guard->m_paymasterAuthorizationCommitment.clear();
            guard->m_paymasterConfirmationGuard.Reset();
            guard->m_paymasterRecoveryAuthorizationCommitment.clear();
            guard->m_paymasterRecoveryMaximumServiceFeeCents = 0;
            guard->m_paymasterRecoveryActive = false;
            guard->m_paymasterRecoveryConfirmationGuard.Reset();
            guard->updatePaymasterSessionView(result);
            guard->m_paymasterStateValue->setText(
                guard->tr("Previous unsigned attempt closed; selecting another Paymaster"));
            if (!guard->m_paymasterPollTimer->isActive()) guard->m_paymasterPollTimer->start();
            guard->pollPaymasterSession();
        });
}

void DigiDollarSendWidget::recoverPaymasterSessionToSelf()
{
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty()) return;
    if (m_paymasterSessionPrivacy.isEmpty()) {
        showWarning(
            tr("Paymaster recovery unavailable"),
            tr("The original session privacy profile is not available in this Qt session. "
               "Recovery will not continue because Qt must never guess or downgrade it."));
        return;
    }
    const auto answer = QMessageBox::warning(
        this, tr("Prepare alternative-provider self-recovery"),
        tr("The wallet will first select and authenticate a different recovery provider, then "
           "show the exact wallet returns, provider, fees, privacy profile and Core authorization "
           "commitment before any recovery transaction signature. The original authorized "
           "payment may still confirm first; recovery is not final until confirmed.\n\n"
           "Wallet unlock at this stage may create input-control proofs only. Core's prepare-only "
           "gate cannot create the recovery transaction signature."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    m_paymasterPollTimer->stop();
    m_paymasterRecoveryAuthorizationCommitment.clear();
    m_paymasterRecoveryConfirmationGuard.Reset();
    m_paymasterRecoveryMaximumServiceFeeCents = m_feeCapSpin->value();
    m_paymasterRecoveryActive = true;
    executeAlternativePaymasterRecovery(/*allow_unlock=*/true);
}

void DigiDollarSendWidget::abandonUnsignedPaymasterSession()
{
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty() ||
        !m_paymasterSessionPersisted) {
        return;
    }
    if (QMessageBox::question(
            this, tr("Cancel unsigned Paymaster transfer"),
            tr("Core will release the reserved $DD only if it can prove that no "
               "transaction signature or final transaction exists. If any spending "
               "authorization may exist, cancellation is refused and the protected "
               "recovery choices remain available.\n\nContinue?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    m_paymasterPollTimer->stop();
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(lookup));
    params.push_back("abandon_unsigned");
    params.push_back(UniValue{UniValue::VOBJ});
    setPaymasterBusy(true);
    m_paymasterStateValue->setText(
        tr("Verifying that the unsigned transfer can be canceled safely…"));
    m_walletModel->executeRpcAsync(
        "resolvepaymastersession", std::move(params),
        [guard = QPointer<DigiDollarSendWidget>(this)](UniValue result,
                                                       QString error) {
            if (!guard) return;
            guard->setPaymasterBusy(false);
            if (!error.isEmpty()) {
                guard->m_paymasterStateValue->setText(
                    guard->tr("Cancellation refused; reservations remain protected"));
                guard->showWarning(
                    guard->tr("Paymaster transfer was not canceled"), error);
                return;
            }
            const UniValue& session = result.find_value("session");
            const bool canceled = session.isObject() &&
                session.find_value("session_state").isStr() &&
                session.find_value("session_state").get_str() == "FAILED";
            if (!canceled) {
                guard->m_paymasterStateValue->setText(
                    guard->tr("Cancellation returned an unexpected wallet state"));
                guard->showWarning(
                    guard->tr("Paymaster transfer was not cleared"),
                    guard->tr("Core did not confirm the terminal FAILED state."));
                return;
            }
            guard->m_paymasterRequestId.clear();
            guard->m_paymasterSessionId.clear();
            guard->m_paymasterSessionState.clear();
            guard->m_paymasterAttemptState.clear();
            guard->m_paymasterArtifact.clear();
            guard->m_paymasterPendingPhase.clear();
            guard->m_paymasterBroadcastState.clear();
            guard->m_paymasterConfirmationState.clear();
            guard->m_paymasterSessionPersisted = false;
            guard->m_paymasterAddress.clear();
            guard->m_paymasterAuthorizationCommitment.clear();
            guard->m_paymasterSessionPrivacy.clear();
            guard->m_paymasterRecoveryAuthorizationCommitment.clear();
            guard->m_paymasterAmount = 0.0;
            guard->m_paymasterRecoveryMaximumServiceFeeCents = 0;
            guard->m_paymasterRecoveryActive = false;
            guard->m_paymasterConfirmationGuard.Reset();
            guard->m_paymasterRecoveryConfirmationGuard.Reset();
            guard->m_paymasterStateValue->setText(
                guard->tr("Unsigned transfer canceled; reserved $DD is available again"));
            guard->m_paymasterIdentityValue->setText(guard->tr("—"));
            guard->m_paymasterCostValue->setText(
                guard->tr("No service fee was authorized"));
            guard->m_paymasterExpiryValue->setText(guard->tr("—"));
            guard->updateBalance();
            guard->refreshClientSafetyStatus();
            guard->updatePaymasterFocusMode();
            guard->updateSendButton();
        });
}

UniValue DigiDollarSendWidget::buildAlternativePaymasterRecoveryParams() const
{
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue options{UniValue::VOBJ};
    options.pushKV("maximum_recovery_service_fee_cents",
                   m_paymasterRecoveryMaximumServiceFeeCents);
    if (m_paymasterRecoveryAuthorizationCommitment.isEmpty()) {
        options.pushKV("prepare_only", true);
    } else {
        options.pushKV("recovery_authorization_commitment",
                       m_paymasterRecoveryAuthorizationCommitment.toStdString());
    }
    UniValue params{UniValue::VARR};
    params.push_back(std::move(lookup));
    params.push_back("cancel_to_self");
    params.push_back(std::move(options));
    return params;
}

void DigiDollarSendWidget::executeAlternativePaymasterRecovery(bool allow_unlock)
{
    if (!m_walletModel || m_paymasterBusy || !m_paymasterRecoveryActive ||
        m_paymasterRequestId.isEmpty()) {
        return;
    }
    std::shared_ptr<WalletModel::UnlockContext> unlock;
    if (allow_unlock) {
        unlock = m_walletModel->requestUnlockForAsync();
        if (!unlock->isValid()) {
            m_paymasterStateValue->setText(tr("RECOVERY_AWAITING_WALLET_UNLOCK"));
            m_paymasterRecoveryActive = false;
            return;
        }
    }
    setPaymasterBusy(true);
    m_paymasterStateValue->setText(
        m_paymasterRecoveryAuthorizationCommitment.isEmpty()
            ? tr("Preparing authenticated alternative recovery…")
            : tr("Submitting the exact confirmed recovery authorization…"));
    m_walletModel->executeRpcAsync(
        "resolvepaymastersession", buildAlternativePaymasterRecoveryParams(),
        [guard = QPointer<DigiDollarSendWidget>(this), unlock = std::move(unlock)](
            UniValue result, QString error) mutable {
            unlock.reset();
            if (!guard) return;
            guard->handleAlternativePaymasterRecoveryResult(result, error);
        });
}

PaymasterRecoveryConfirmationSelection
DigiDollarSendWidget::paymasterRecoveryConfirmationSelection(
    const UniValue& recovery) const
{
    const auto string_value = [&recovery](const char* key) {
        const UniValue& value = recovery.find_value(key);
        return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
    };
    const auto number_value = [&recovery](const char* key) {
        const UniValue& value = recovery.find_value(key);
        return value.isNum() ? value.getInt<qint64>() : qint64{-1};
    };

    PaymasterRecoveryConfirmationSelection selection;
    selection.recovery_provider_id = string_value("recovery_provider_id");
    selection.privacy_profile = string_value("privacy_profile");
    selection.offer_id = string_value("offer_id");
    selection.policy_hash = string_value("policy_hash");
    selection.original_commit_key = string_value("original_commit_key");
    selection.original_template_commitment =
        string_value("original_template_commitment");
    selection.authorization_commitment = string_value("authorization_commitment");
    selection.maximum_service_fee_cents =
        number_value("maximum_service_fee_cents");
    selection.service_fee_cents = number_value("service_fee_cents");
    selection.network_fee_satoshis = number_value("network_fee_satoshis");
    selection.expires_at = number_value("expires_at");

    const UniValue& wallet_returns = recovery.find_value("wallet_returns");
    if (wallet_returns.isArray()) {
        int output_index{0};
        for (const UniValue& wallet_return : wallet_returns.getValues()) {
            if (!wallet_return.isObject()) {
                selection.wallet_returns.push_back(QString{});
                continue;
            }
            const UniValue& script = wallet_return.find_value("script_pub_key");
            const UniValue& address = wallet_return.find_value("address");
            const UniValue& amount = wallet_return.find_value("amount_cents");
            if (!script.isStr() || script.get_str().empty() || !amount.isNum()) {
                selection.wallet_returns.push_back(QString{});
                continue;
            }
            selection.wallet_returns.push_back(
                tr("Output %1: %2 cents to %3 (script %4)")
                    .arg(++output_index)
                    .arg(amount.getInt<qint64>())
                    .arg(address.isStr()
                             ? QString::fromStdString(address.get_str())
                             : tr("fresh wallet-owned script"))
                    .arg(QString::fromStdString(script.get_str())));
        }
    }
    return selection;
}

bool DigiDollarSendWidget::confirmPaymasterRecoveryBeforeSigning(
    const PaymasterRecoveryConfirmationSelection& selection)
{
    if (!selection.IsComplete()) {
        blockAlternativePaymasterRecovery(
            tr("Recovery authorization blocked: incomplete exact manifest"),
            tr("Core did not return every exact RecoveryAuthorization field required for "
               "review. No recovery transaction signature or automatic retry will continue."));
        return false;
    }
    if (selection.privacy_profile != m_paymasterSessionPrivacy) {
        blockAlternativePaymasterRecovery(
            tr("Recovery authorization blocked: privacy changed"),
            tr("The recovery privacy profile (%1) does not exactly match the original "
               "session profile (%2). Qt never permits a recovery privacy downgrade or override.")
                .arg(selection.privacy_profile, m_paymasterSessionPrivacy));
        return false;
    }
    if (!m_paymasterRecoveryConfirmationGuard.RequiresConfirmation(selection)) {
        return true;
    }

    QStringList changed_fields;
    for (const QString& field :
         m_paymasterRecoveryConfirmationGuard.ChangedFields(selection)) {
        if (field == QStringLiteral("recovery_provider")) changed_fields.push_back(tr("recovery provider"));
        else if (field == QStringLiteral("privacy")) changed_fields.push_back(tr("privacy"));
        else if (field == QStringLiteral("offer")) changed_fields.push_back(tr("offer"));
        else if (field == QStringLiteral("policy")) changed_fields.push_back(tr("policy"));
        else if (field == QStringLiteral("original_commit")) changed_fields.push_back(tr("original commit"));
        else if (field == QStringLiteral("original_template_commitment")) changed_fields.push_back(tr("original template"));
        else if (field == QStringLiteral("wallet_returns")) changed_fields.push_back(tr("wallet returns"));
        else if (field == QStringLiteral("maximum_service_fee")) changed_fields.push_back(tr("maximum service fee"));
        else if (field == QStringLiteral("service_fee")) changed_fields.push_back(tr("service fee"));
        else if (field == QStringLiteral("network_fee")) changed_fields.push_back(tr("network fee"));
        else if (field == QStringLiteral("expiry")) changed_fields.push_back(tr("expiry"));
        else if (field == QStringLiteral("authorization_commitment")) changed_fields.push_back(tr("authorization commitment"));
    }

    const QString prompt = tr(
        "Review the exact alternative-provider cancel-to-self authorization before any recovery transaction signature:\n\n"
        "Recovery provider: %1\nPrivacy: %2\nOffer: %3\nPolicy: %4\n"
        "Original commit: %5\nOriginal template: %6\n\nWallet-owned returns:\n%7\n\n"
        "Maximum service fee: %8 cents\nService fee: %9 cents\nNetwork fee: %10 satoshis\n"
        "Expires: %11\nAuthorization commitment: %12\n\nChanged fields: %13\n\n"
        "Continue to wallet unlock and the exact recovery signature?")
        .arg(selection.recovery_provider_id, selection.privacy_profile,
             selection.offer_id, selection.policy_hash,
             selection.original_commit_key,
             selection.original_template_commitment,
             selection.wallet_returns.join(QLatin1Char('\n')))
        .arg(selection.maximum_service_fee_cents)
        .arg(selection.service_fee_cents)
        .arg(selection.network_fee_satoshis)
        .arg(QDateTime::fromSecsSinceEpoch(selection.expires_at)
                 .toLocalTime().toString(Qt::ISODate))
        .arg(selection.authorization_commitment)
        .arg(changed_fields.join(tr(", ")));
    if (QMessageBox::question(
            this, tr("Confirm exact Paymaster recovery authorization"), prompt,
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        m_paymasterRecoveryActive = false;
        m_paymasterStateValue->setText(
            tr("Recovery authorization paused by user"));
        return false;
    }
    return m_paymasterRecoveryConfirmationGuard.Accept(selection);
}

void DigiDollarSendWidget::blockAlternativePaymasterRecovery(
    const QString& reason, const QString& detail)
{
    m_paymasterPollTimer->stop();
    m_paymasterRecoveryActive = false;
    m_paymasterStateValue->setText(reason);
    showWarning(tr("Paymaster recovery blocked"), detail);
}

void DigiDollarSendWidget::handleAlternativePaymasterRecoveryResult(
    const UniValue& result, const QString& error)
{
    setPaymasterBusy(false);
    if (!error.isEmpty()) {
        m_paymasterRecoveryActive = false;
        m_paymasterPollTimer->stop();
        m_paymasterStateValue->setText(tr("Paymaster recovery failed: %1").arg(error));
        // Keep the backend's stable error text unchanged so RPC and Qt expose
        // the same recovery/security failure.
        showWarning(tr("Paymaster recovery"), error);
        return;
    }

    const UniValue& session = result.find_value("session");
    if (session.isObject()) updatePaymasterSessionView(session);
    const UniValue& recovery = result.find_value("recovery");
    if (!recovery.isObject()) {
        blockAlternativePaymasterRecovery(
            tr("Recovery authorization blocked: missing recovery manifest"),
            tr("Core did not return the validated alternative recovery object. No "
               "signature or automatic retry will continue."));
        return;
    }

    const PaymasterRecoveryConfirmationSelection selection =
        paymasterRecoveryConfirmationSelection(recovery);
    const QString returned_commitment = selection.authorization_commitment;
    if (!m_paymasterRecoveryAuthorizationCommitment.isEmpty() &&
        (returned_commitment.isEmpty() ||
         returned_commitment != m_paymasterRecoveryAuthorizationCommitment)) {
        blockAlternativePaymasterRecovery(
            tr("Recovery authorization blocked: commitment changed or missing"),
            tr("Core did not return the exact RecoveryAuthorization commitment that was "
               "confirmed. No signature or automatic retry will continue."));
        return;
    }
    const UniValue& expired = recovery.find_value("expired");
    if (expired.isBool() && expired.get_bool()) {
        blockAlternativePaymasterRecovery(
            tr("Recovery authorization expired"),
            tr("The exact alternative recovery authorization expired. Start a fresh "
               "two-stage review; Qt will not reuse the old acceptance."));
        return;
    }

    const UniValue& accepted_value = recovery.find_value("authorization_accepted");
    const bool authorization_accepted = accepted_value.isBool() &&
                                        accepted_value.get_bool();
    if (m_paymasterRecoveryAuthorizationCommitment.isEmpty() &&
        !returned_commitment.isEmpty()) {
        if (!confirmPaymasterRecoveryBeforeSigning(selection)) return;
        m_paymasterRecoveryAuthorizationCommitment = returned_commitment;
        executeAlternativePaymasterRecovery(/*allow_unlock=*/true);
        return;
    }
    if (!m_paymasterRecoveryAuthorizationCommitment.isEmpty() &&
        !authorization_accepted) {
        blockAlternativePaymasterRecovery(
            tr("Recovery authorization was not accepted"),
            tr("The wallet did not atomically accept the exact confirmed recovery "
               "commitment. Qt will not automatically repeat a signing attempt."));
        return;
    }

    const UniValue& phase_value = recovery.find_value("phase");
    const QString phase = phase_value.isStr()
        ? QString::fromStdString(phase_value.get_str())
        : QString{};
    const UniValue& session_final = session.find_value("final");
    if (session_final.isBool() && session_final.get_bool()) {
        m_paymasterRecoveryActive = false;
        m_paymasterPollTimer->stop();
        m_paymasterStateValue->setText(
            tr("Recovery confirmed; reserved inputs are safely resolved"));
        updateBalance();
        return;
    }

    const UniValue& broadcast = result.find_value("broadcast");
    const UniValue& broadcast_error = result.find_value("broadcast_error");
    if (broadcast_error.isStr()) {
        m_paymasterStateValue->setText(
            tr("Recovery %1; broadcast pending: %2")
                .arg(phase, QString::fromStdString(broadcast_error.get_str())));
    } else if (broadcast.isBool() && broadcast.get_bool()) {
        m_paymasterStateValue->setText(
            tr("Recovery broadcast; confirmation pending"));
    } else {
        m_paymasterStateValue->setText(
            phase.isEmpty()
                ? tr("Alternative recovery preparation pending")
                : tr("Alternative recovery: %1").arg(phase));
    }
    if (!m_paymasterPollTimer->isActive()) m_paymasterPollTimer->start();
}

void DigiDollarSendWidget::cancelPaymasterQuote()
{
    m_paymasterPollTimer->stop();
    m_paymasterRecoveryActive = false;
    m_paymasterStateValue->setText(tr("Automatic checks stopped; durable reservations remain protected"));
    updatePaymasterFocusMode();
}

void DigiDollarSendWidget::onUseAvailableBalanceClicked()
{
    if (m_availableBalance <= 0) return;
    if (paymasterModeSelected() && m_coinControl && m_coinControl->HasSelected()) {
        showWarning(
            tr("Wallet emptying needs automatic input selection"),
            tr("Clear the manually selected DigiDollar inputs before emptying the wallet. "
               "Core must bind every confirmed, ordinary spendable input to one exact snapshot."));
        return;
    }
    if (paymasterModeSelected()) {
        m_subtractPaymasterFeeCheck->setChecked(true);
        m_sendAllSpendableDD = true;
    } else {
        m_sendAllSpendableDD = false;
    }
    m_settingSweepAmount = true;
    m_amountEdit->setText(QString::number(m_availableBalance, 'f', 2));
    m_settingSweepAmount = false;
    updateFeeDisplay();
    updateSendButton();
}

void DigiDollarSendWidget::onPasteAddressClicked()
{
    m_addressEdit->setText(QApplication::clipboard()->text());
    onAddressChanged();
}

void DigiDollarSendWidget::onAddressBookClicked()
{
    if (!m_walletModel) return;

    DDAddressBookPage dlg(m_platformStyle, DDAddressBookPage::ForSelection, this);
    dlg.setWalletModel(m_walletModel);
    if (dlg.exec() == QDialog::Accepted) {
        QString address = dlg.getReturnValue();
        if (!address.isEmpty()) {
            m_addressEdit->setText(address);
            onAddressChanged();
        }
    }
}

// REMOVED: applyTheme() method
// All theming is now handled by light.css and dark.css files
// This allows the DigiByte blue theme to work properly

void DigiDollarSendWidget::updateSendButton()
{
    // setupFeeSection() selects the safe default before setupButtonSection()
    // creates the action buttons. Fee-mode updates during that construction
    // phase must not dereference the not-yet-created send button.
    if (!m_sendButton) return;

    bool addressValid = validateAddress();
    bool amountValid = validateAmount();
    bool balanceValid = validateBalance();
    const bool paymaster_ready = !paymasterModeSelected() ||
        (m_clientSafetyStatusKnown && m_clientSafetyConfigured);

    m_sendButton->setEnabled(!m_paymasterBusy && addressValid && amountValid &&
                             balanceValid && paymaster_ready);
    if (m_paymasterBusy) {
        m_sendButton->setToolTip(tr("A Paymaster request is currently being processed"));
    } else if (!addressValid) {
        m_sendButton->setToolTip(tr("Enter a valid DigiDollar recipient address"));
    } else if (!amountValid) {
        m_sendButton->setToolTip(tr("Enter a valid DigiDollar amount greater than zero"));
    } else if (!balanceValid) {
        m_sendButton->setToolTip(tr(
            "Insufficient spendable DigiDollar: this wallet currently has %1 available. A Paymaster supplies only the DGB network fee, not the DigiDollar being sent.")
            .arg(formatDDAmount(m_availableBalance)));
    } else if (paymasterModeSelected() && !paymaster_ready) {
        m_sendButton->setToolTip(m_clientSafetyStatusKnown
            ? tr("Set positive wallet-local Paymaster service-fee limits before sending")
            : tr("Waiting for the wallet's Paymaster service-fee limits"));
    } else {
        m_sendButton->setToolTip(tr("Confirm and send this DigiDollar transaction"));
    }
}

void DigiDollarSendWidget::updateUSDEquivalent()
{
    QString amountText = m_amountEdit->text();
    if (!amountText.isEmpty()) {
        double amount = amountText.toDouble();
        double usdValue = amount * 1.0; // DD should be pegged to $1
        m_usdEquivalentValue->setText(formatUSDAmount(usdValue));
    } else {
        m_usdEquivalentValue->setText("0.00 $USD");
    }
    // REMOVED: All programmatic styling - Let CSS handle theming
}

void DigiDollarSendWidget::updateFeeDisplay()
{
    const QString mode = feeMode();
    const double amount = m_amountEdit->text().toDouble();
    const qint64 amount_cents = static_cast<qint64>(std::llround(amount * 100));
    const bool subtract_fee = paymasterModeSelected() &&
        m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked();
    const bool exact_preview = subtract_fee &&
        m_paymasterPreviewRecipientCents >= 0 &&
        m_paymasterPreviewServiceFeeCents >= 0 &&
        m_paymasterPreviewTotalCents == amount_cents;
    const QString recipient_amount = formatDDAmount(amount);
    const QString maximum_fee = formatCents(m_feeCapSpin->value());
    const QString maximum_outflow = subtract_fee
        ? formatDDAmount(amount)
        : formatDDAmount(amount + m_feeCapSpin->value() / 100.0);
    const QString remaining_balance = formatDDAmount(std::max(0.0, m_availableBalance - amount));
    const QString fallback_recipient = exact_preview
        ? formatCents(m_paymasterPreviewRecipientCents)
        : tr("Calculated from the exact offer before signing");
    const QString fallback_fee = exact_preview
        ? formatCents(m_paymasterPreviewServiceFeeCents)
        : tr("Exact offer required; never more than %1").arg(maximum_fee);
    QString dgb_status;
    if (m_walletModel) {
        const CAmount dgb_balance = m_walletModel->getAvailableDGBBalance();
        const QString readable_balance = QLocale().toString(
            static_cast<double>(dgb_balance) / COIN, 'f', 2);
        dgb_status = tr("Available DGB: %1 DGB").arg(readable_balance);
        if (dgb_balance < COIN / 10) {
            dgb_status += tr(" — currently below the estimated network fee");
        }
    }

    if (mode == QStringLiteral("dgb")) {
        m_feeValue->setText(QStringLiteral("~0.1 DGB"));
        m_feeLabel->setText(tr("Transaction fee:"));
        m_feeModeExplanation->setText(tr(
            "Selected: Own DGB. No Paymaster and no additional $DD service fee will be used."));
        m_feeSummary->setText(tr(
            "<table cellspacing=\"3\">"
            "<tr><td><b>Recipient receives</b></td><td>%1</td></tr>"
            "<tr><td><b>Network fee</b></td><td>Own DGB (estimated ~0.1 DGB)</td></tr>"
            "<tr><td><b>Additional $DD fee</b></td><td>None</td></tr>"
            "<tr><td><b>Maximum wallet outflow</b></td><td>%1</td></tr>"
            "</table>").arg(recipient_amount));
        m_feeSummary->setToolTip(dgb_status);
    } else if (mode == QStringLiteral("auto")) {
        m_feeValue->setText(tr("~0.1 DGB, or at most %1").arg(maximum_fee));
        m_feeLabel->setText(tr("Fee funding:"));
        m_feeModeExplanation->setText(tr(
            "Selected: Automatic. Core first tries to pay approximately 0.1 DGB from this wallet. It looks for "
            "a Paymaster only if suitable DGB fee inputs are insufficient; wallet-lock and "
            "other errors never cause an automatic fallback."));
        if (subtract_fee) {
            m_feeSummary->setText(tr(
                "<table cellspacing=\"3\">"
                "<tr><td><b>Exact total $DD outflow</b></td><td>%1</td></tr>"
                "<tr><td><b>With own DGB</b></td><td>Recipient receives %1; no $DD fee</td></tr>"
                "<tr><td><b>With Paymaster fallback</b></td><td>Recipient receives %2</td></tr>"
                "<tr><td><b>Paymaster service fee</b></td><td>%3</td></tr>"
                "<tr><td><b>Spendable $DD remaining</b></td><td>%4</td></tr>"
                "</table>%5")
                .arg(recipient_amount, fallback_recipient, fallback_fee,
                     remaining_balance,
                     m_sendAllSpendableDD
                         ? tr("<br><b>Wallet emptying:</b> every confirmed, ordinary spendable $DD input will be bound to this exact request.")
                         : QString{}));
        } else {
            m_feeSummary->setText(tr(
                "<table cellspacing=\"3\">"
                "<tr><td><b>Recipient receives</b></td><td>%1</td></tr>"
                "<tr><td><b>Network fee</b></td><td>Own DGB first; Paymaster only if needed</td></tr>"
                "<tr><td><b>Additional $DD fee</b></td><td>None with own DGB; otherwise up to %2</td></tr>"
                "<tr><td><b>Maximum wallet outflow</b></td><td>%3</td></tr>"
                "</table>").arg(recipient_amount, maximum_fee, maximum_outflow));
        }
        m_feeSummary->setToolTip(dgb_status);
    } else {
        m_feeValue->setText(tr("Exact provider quote; at most %1").arg(maximum_fee));
        m_feeLabel->setText(tr("Service fee:"));
        m_feeModeExplanation->setText(tr(
            "Selected: Paymaster. A provider must supply the DGB network fee. A sponsored offer costs no "
            "$DD service fee; a user-paid offer may charge up to the limit shown below. "
            "The exact provider and fee are shown again before signing."));
        if (subtract_fee) {
            m_feeSummary->setText(tr(
                "<table cellspacing=\"3\">"
                "<tr><td><b>Total $DD outflow</b></td><td>%1</td></tr>"
                "<tr><td><b>Recipient receives</b></td><td>%2</td></tr>"
                "<tr><td><b>Provider receives</b></td><td>%3</td></tr>"
                "<tr><td><b>Network fee</b></td><td>Paid in DGB by the selected provider</td></tr>"
                "<tr><td><b>Spendable $DD remaining</b></td><td>%4</td></tr>"
                "</table>%5")
                .arg(recipient_amount, fallback_recipient, fallback_fee,
                     remaining_balance,
                     m_sendAllSpendableDD
                         ? tr("<br><b>Wallet emptying:</b> every confirmed, ordinary spendable $DD input will be bound to this exact request.")
                         : QString{}));
        } else {
            m_feeSummary->setText(tr(
                "<table cellspacing=\"3\">"
                "<tr><td><b>Recipient receives</b></td><td>%1</td></tr>"
                "<tr><td><b>Network fee</b></td><td>Paid in DGB by the selected provider</td></tr>"
                "<tr><td><b>Additional $DD fee</b></td><td>0.00 $DD to %2</td></tr>"
                "<tr><td><b>Maximum wallet outflow</b></td><td>%3</td></tr>"
                "</table>").arg(recipient_amount, maximum_fee, maximum_outflow));
        }
        m_feeSummary->setToolTip(QString());
    }

    QString amountText = m_amountEdit->text();
    if (!amountText.isEmpty()) {
        double amount = amountText.toDouble();
        m_totalValue->setText(exact_preview
            ? formatCents(m_paymasterPreviewRecipientCents)
            : formatDDAmount(amount));
    } else {
        m_totalValue->setText(formatDDAmount(0));
    }

    if (m_privacy) {
        m_feeValue->setText(maskValue(m_feeValue->text()));
        m_totalValue->setText(maskValue(m_totalValue->text()));
        m_feeSummary->setText(maskValue(m_feeSummary->text()));
    }

}

bool DigiDollarSendWidget::validateAddress() const
{
    QString address = m_addressEdit->text();
    int pos = 0;
    QString addressCopy = address;
    return m_addressValidator->validate(addressCopy, pos) == QValidator::Acceptable;
}

bool DigiDollarSendWidget::validateAmount() const
{
    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) return false;

    int pos = 0;
    QString amountCopy = amountText;
    return m_amountValidator->validate(amountCopy, pos) == QValidator::Acceptable;
}

bool DigiDollarSendWidget::validateBalance() const
{
    QString amountText = m_amountEdit->text();
    if (amountText.isEmpty()) return true; // Empty is valid for enabling/disabling

    double amount = amountText.toDouble();
    // The exact Paymaster fee is not known until a quote is authenticated.
    // Validate the recipient amount here and the fee again before signing.
    bool valid = amount <= m_availableBalance && amount > 0;

    // Note: Cannot call updateAmountValidation() from const method

    return valid;
}

QString DigiDollarSendWidget::formatDDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $DD";
}

QString DigiDollarSendWidget::formatCents(qint64 cents) const
{
    return tr("%1 $DD (%2 cents)").arg(QString::number(cents / 100.0, 'f', 2)).arg(cents);
}

QString DigiDollarSendWidget::friendlyFundingModel(const QString& model) const
{
    if (model == QStringLiteral("user_paid")) return tr("Service fee in $DD");
    if (model == QStringLiteral("sponsored")) return tr("Sponsored by provider");
    return model.isEmpty() ? tr("Not specified") : model;
}

QString DigiDollarSendWidget::formatUSDAmount(double amount) const
{
    return QString::number(amount, 'f', 2) + " $USD";
}

// PHASE 7.3: Error display helper
void DigiDollarSendWidget::showError(const QString& title, const QString& message)
{
    QMessageBox msgBox(this);
    msgBox.setIcon(QMessageBox::Critical);
    msgBox.setWindowTitle(title);
    msgBox.setText(message);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();

    // Log for debugging
    LogPrintf("DigiDollar GUI Error: %s - %s\n",
              title.toStdString(), message.toStdString());
}

// PHASE 7.3: Warning display helper
void DigiDollarSendWidget::showWarning(const QString& title, const QString& message)
{
    QMessageBox msgBox(this);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle(title);
    msgBox.setText(message);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();

    // Log for debugging
    LogPrintf("DigiDollar GUI Warning: %s - %s\n",
              title.toStdString(), message.toStdString());
}

// PHASE 7.3: Wallet state validation
bool DigiDollarSendWidget::checkWalletState()
{
    if (!m_walletModel) {
        showError(tr("Wallet Error"),
                  tr("Wallet is not available.\n\nPlease ensure your wallet is properly loaded."));
        return false;
    }

    // NOTE: Wallet unlock is handled in onSendClicked() where the UnlockContext
    // stays in scope through executeTransfer(). Do NOT unlock here — the context
    // would die when checkWalletState() returns, re-locking before the transaction.

    // Check if DigiDollar wallet initialized (balance check serves as proxy)
    if (m_availableBalance < 0) {
        showError(tr("DigiDollar Not Available"),
                 tr("DigiDollar wallet is not initialized.\n\n"
                    "This may indicate a wallet initialization error."));
        return false;
    }

    return true;
}

// PHASE 7.2: Enhanced confirmation dialog with 3-second countdown
// This matches the DGB send confirmation flow exactly
bool DigiDollarSendWidget::showConfirmationDialog(const QString& address, double amount)
{
    const QString mode = feeMode();
    const bool paymaster_only = mode == QStringLiteral("paymaster");
    const bool automatic = mode == QStringLiteral("auto");
    const bool subtract_fee = paymasterModeSelected() &&
        m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked();
    const QString title = paymaster_only
        ? tr("Request a Paymaster offer")
        : automatic ? tr("Confirm automatic fee funding")
                    : tr("Confirm send DigiDollar");
    QString question_string;

    question_string.append(paymaster_only
        ? tr("Do you want Core to prepare this Paymaster transfer?")
        : tr("Do you want to send this DigiDollar transaction?"));
    question_string.append("<br /><span style='font-size:10pt;'>");
    question_string.append(paymaster_only
        ? tr("This step requests and verifies an offer. It does not sign or broadcast a transaction.")
        : tr("Please review the transfer before continuing."));
    question_string.append("</span>");

    question_string.append("<hr /><b>");
    question_string.append(paymaster_only ? tr("Planned transfer") : tr("Transfer"));
    question_string.append("</b><br />");
    if (subtract_fee) {
        question_string.append(tr("Exact maximum $DD outflow: %1").arg(formatDDAmount(amount)));
        question_string.append("<br />");
        if (automatic) {
            question_string.append(tr(
                "Recipient receives the full %1 when this wallet can pay in DGB. "
                "Only a Paymaster fallback deducts its exact service fee.")
                .arg(formatDDAmount(amount)));
        } else {
            question_string.append(tr(
                "The exact recipient amount is calculated from the authenticated offer. "
                "Recipient amount plus provider fee must equal %1 exactly.")
                .arg(formatDDAmount(amount)));
        }
        if (m_sendAllSpendableDD) {
            question_string.append("<br /><b>");
            question_string.append(tr(
                "Wallet emptying is enabled: every confirmed, ordinary spendable $DD input "
                "must still equal this total when Core reserves it."));
            question_string.append("</b>");
        }
    } else {
        question_string.append(tr("Recipient receives: %1").arg(formatDDAmount(amount)));
    }
    question_string.append("<br /><span style='font-family:monospace;'>");
    question_string.append(address.toHtmlEscaped());
    question_string.append("</span>");

    if (m_coinControl && m_coinControl->HasSelected()) {
        const int selected_count = static_cast<int>(m_coinControl->ListSelected().size());
        const CAmount selected_amount = selectedDigiDollarAmount();
        question_string.append("<hr /><b>");
        question_string.append(tr("Selected $DD inputs"));
        question_string.append("</b>: ");
        question_string.append(tr("%1 input(s), %2 selected")
            .arg(selected_count)
            .arg(formatDDAmount(selected_amount / 100.0)));
    }

    question_string.append("<hr /><b>");
    question_string.append(tr("Selected fee method"));
    question_string.append("</b><br />");
    if (automatic) {
        qint64 effective_fee_cap = m_feeCapSpin->value();
        if (m_clientSafetyStatusKnown && m_clientSafetyConfigured) {
            effective_fee_cap = std::min(effective_fee_cap,
                                         m_clientSafetyMaximumPerTransaction);
            effective_fee_cap = std::min(effective_fee_cap,
                                         m_clientSafetyAvailableTodayCents);
        }
        question_string.append(tr("Automatic: use this wallet's DGB first."));
        question_string.append("<br />");
        question_string.append(tr(
            "Only if suitable DGB fee inputs are insufficient will Core request a Paymaster offer."));
        question_string.append("<br />");
        question_string.append(tr("Paymaster fee ceiling: %1").arg(formatCents(effective_fee_cap)));
        if (subtract_fee) {
            question_string.append("<br />");
            question_string.append(tr(
                "On Paymaster fallback, that exact fee is deducted from the total above; "
                "it is never added beyond the authorized outflow."));
        }
        question_string.append("<br />");
        question_string.append(tr(
            "This is a safety ceiling, not an expected fee. If Paymaster fallback is needed, "
            "a separate confirmation shows the exact provider, payment model, service fee and total before signing."));
    } else if (paymaster_only) {
        qint64 effective_fee_cap = m_feeCapSpin->value();
        if (m_clientSafetyStatusKnown && m_clientSafetyConfigured) {
            effective_fee_cap = std::min(effective_fee_cap,
                                         m_clientSafetyMaximumPerTransaction);
            effective_fee_cap = std::min(effective_fee_cap,
                                         m_clientSafetyAvailableTodayCents);
        }
        question_string.append(tr("Paymaster required: a provider must supply the DGB network fee."));
        question_string.append("<br />");
        question_string.append(tr("Maximum permitted service fee for this request: %1")
                                   .arg(formatCents(effective_fee_cap)));
        if (subtract_fee) {
            question_string.append("<br />");
            question_string.append(tr(
                "The exact fee will be deducted from the entered total. Offers that cannot "
                "produce an exact cent-level split are rejected before inputs are reserved."));
        }
        question_string.append("<br /><span style='color:#aa0000; font-weight:bold;'>");
        question_string.append(tr("No service fee is authorized by this step."));
        question_string.append("</span><br />");
        question_string.append(tr(
            "Core now authenticates and selects an eligible offer within that ceiling. "
            "Before any payment signature, a separate confirmation shows the exact provider, "
            "payment model, service fee and maximum wallet outflow."));
    } else {
        question_string.append(tr("Own DGB: this wallet pays the estimated ~0.1 DGB network fee."));
        question_string.append("<br />");
        question_string.append(tr("No Paymaster and no additional $DD service fee will be used."));
    }

    question_string.append("<hr />");
    question_string.append(paymaster_only
        ? tr("Next action: request and verify an exact Paymaster offer.")
        : automatic ? tr("Next action: use DGB if possible, otherwise prepare an exact Paymaster offer.")
                    : tr("Next action: unlock the wallet if necessary, then sign and broadcast."));

    const QString confirm_button_text = paymaster_only
        ? tr("Find offer")
        : automatic ? tr("Continue") : tr("Send");
    auto confirmationDialog = new DDSendConfirmationDialog(
        title,
        question_string,
        QString{},
        DD_SEND_CONFIRM_DELAY,
        confirm_button_text,
        this
    );
    confirmationDialog->setObjectName(paymaster_only
        ? QStringLiteral("paymasterOfferRequestConfirmation")
        : QStringLiteral("digiDollarSendConfirmation"));
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);

    int result = confirmationDialog->exec();

    if (result == QMessageBox::Yes) {
        if (paymaster_only) {
            LogPrintf("DigiDollar: User approved preparing a Paymaster offer for %f DD to %s after 3-second review\n",
                      amount, address.toStdString());
        } else {
            LogPrintf("DigiDollar: User confirmed transfer of %f DD to %s after 3-second review\n",
                      amount, address.toStdString());
        }
        return true;
    } else {
        LogPrintf("DigiDollar: User cancelled the send preparation\n");
        return false;
    }
}

// PHASE 7.3: Execute transfer with progress indicator and error handling
void DigiDollarSendWidget::executeTransfer(const QString& address, double amount)
{
    // Show progress dialog
    QProgressDialog progress(tr("Sending DigiDollar..."),
                            tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0); // Show immediately
    progress.setCancelButton(nullptr); // No cancel during transfer
    progress.show();
    QApplication::processEvents(); // Force update

    // Disable UI during transfer
    m_sendButton->setEnabled(false);
    m_addressEdit->setEnabled(false);
    m_amountEdit->setEnabled(false);
    m_clearButton->setEnabled(false);
    m_useAvailableBalanceButton->setEnabled(false);

    CAmount amountCents = static_cast<CAmount>(std::llround(amount * 100));
    QString note = m_noteEdit ? m_noteEdit->text().trimmed() : QString();

    std::vector<COutPoint> selectedInputs;
    const std::vector<COutPoint>* presetInputs = nullptr;
    if (m_coinControl && m_coinControl->HasSelected()) {
        selectedInputs = m_coinControl->ListSelected();
        presetInputs = &selectedInputs;
    }
    WalletModel::DigiDollarSendResult result = m_walletModel->sendDigiDollar(address, amountCents, note, presetInputs);

    // Close progress dialog
    progress.close();

    // Re-enable UI
    m_sendButton->setEnabled(true);
    m_addressEdit->setEnabled(true);
    m_amountEdit->setEnabled(true);
    m_clearButton->setEnabled(true);
    m_useAvailableBalanceButton->setEnabled(true);

    // Handle result
    if (result.status == WalletModel::OK) {
        // Success!
        showSuccess(result.txid, amount);
        onClearClicked();
        updateBalance(); // Refresh balance display
    } else {
        // Error occurred - map to user-friendly message
        showBackendError(static_cast<int>(result.status), result.reasonFailed);
    }
}

// PHASE 7.3: Success notification
QString DigiDollarSendWidget::buildSuccessMessage(const QString& txid, double amount) const
{
    return tr(
        "<b style='font-size: 14px; color: green;'>✓ DigiDollar Transfer Broadcast</b><br/><br/>"
        "<table cellpadding='4' style='font-size: 12px;'>"
        "<tr><td><b>Amount Sent:</b></td><td align='right'>%1</td></tr>"
        "<tr><td><b>Transaction ID:</b></td><td style='font-family: monospace; font-size: 10px;'>%2</td></tr>"
        "</table><br/>"
        "<span style='color: #666; font-size: 11px;'>"
        "Your transaction has been broadcast to the network.<br/>"
        "It is pending until miners include it in a block and the block confirms."
        "</span>"
    ).arg(formatDDAmount(amount))
     .arg(txid);
}

void DigiDollarSendWidget::showSuccess(const QString& txid, double amount)
{
    QString successMsg = buildSuccessMessage(txid, amount);

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Transfer Broadcast"));
    msgBox.setText(successMsg);
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();

    LogPrintf("DigiDollar: Transfer broadcast - txid: %s\n", txid.toStdString());
}

// PHASE 7.3: Backend error message mapping
void DigiDollarSendWidget::showBackendError(int status, const QString& reasonFailed)
{
    QString errorTitle;
    QString errorMessage;

    // Map backend errors to user-friendly messages
    switch (status) {
    case WalletModel::InvalidAddress:
        errorTitle = tr("Invalid Address");
        errorMessage = tr(
            "The recipient address is invalid.\n\n"
            "Please check the address format and try again.\n\n"
            "Technical details: %1"
        ).arg(reasonFailed);
        break;

    case WalletModel::InvalidAmount:
        errorTitle = tr("Invalid Amount");
        errorMessage = tr(
            "The transfer amount is invalid.\n\n"
            "Please ensure the amount is:\n"
            "• Greater than 0\n"
            "• Within the maximum limit\n"
            "• Properly formatted\n\n"
            "Technical details: %1"
        ).arg(reasonFailed);
        break;

    case WalletModel::AmountExceedsBalance:
        errorTitle = tr("Insufficient Balance");
        errorMessage = tr(
            "You don't have enough DigiDollar for this transfer.\n\n"
            "%1\n\n"
            "Please:\n"
            "• Enter a smaller amount, or\n"
            "• Add more $DD to your wallet"
        ).arg(reasonFailed);
        break;

    case WalletModel::TransactionCreationFailed:
        errorTitle = tr("Transaction Failed");

        // Parse specific error types
        if (reasonFailed.contains(QStringLiteral("PAYMASTER_CLIENT_SAFETY"))) {
            errorTitle = tr("Paymaster service-fee limits required");
            errorMessage = tr(
                "This wallet does not yet have valid Paymaster service-fee limits.\n\n"
                "Return to Network fee and select Set service-fee limits. No provider "
                "can be used until positive wallet-local limits are saved.\n\n"
                "Technical details: %1").arg(reasonFailed);
        } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_NO_EXACT_GROSS_OFFER"))) {
            errorTitle = tr("No exact Paymaster split available");
            errorMessage = tr(
                "No current offer can split this total exactly into a recipient amount and "
                "the provider's rounded service fee. No $DD was reserved or signed.\n\n"
                "Try a sponsored offer, another provider, or change the total by one cent.\n\n"
                "Technical details: %1").arg(reasonFailed);
        } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_SWEEP_BALANCE_CHANGED"))) {
            errorTitle = tr("Spendable $DD balance changed");
            errorMessage = tr(
                "The confirmed, ordinary spendable $DD inputs changed after Wallet emptying "
                "was previewed. Core stopped before authorizing a different total.\n\n"
                "Refresh the balance and start Wallet emptying again. Reserved, pending and "
                "Paymaster-pool $DD remain untouched.\n\nTechnical details: %1")
                .arg(reasonFailed);
        } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_NO_ELIGIBLE_OFFER"))) {
            errorTitle = tr("No suitable Paymaster available");
            errorMessage = tr(
                "No currently advertised provider matches this amount, fee limit and "
                "privacy selection.\n\nTry again later, raise only the limit you are "
                "comfortable paying, or select Own DGB.\n\nTechnical details: %1")
                .arg(reasonFailed);
        } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_INSUFFICIENT_USER_DD"))) {
            errorTitle = tr("Not enough $DD for the selected offer");
            errorMessage = tr(
                "The recipient amount is available, but this wallet cannot also cover the "
                "exact user-paid Paymaster service fee. No transaction was signed.\n\n"
                "Choose a lower-fee or sponsored offer, send a smaller amount, or select "
                "Own DGB.\n\nTechnical details: %1").arg(reasonFailed);
        } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_SAFETY_LIMIT_EXHAUSTED"))) {
            errorTitle = tr("Paymaster daily limit reached");
            errorMessage = tr(
                "This wallet's service-fee budget is already reserved or spent for the "
                "current rolling day. Existing sessions remain protected.\n\n"
                "Technical details: %1").arg(reasonFailed);
        } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_NODE_NOT_READY")) ||
                   reasonFailed.contains(QStringLiteral("PAYMASTER_TXINDEX_NOT_READY"))) {
            errorTitle = tr("Paymaster is not ready");
            errorMessage = tr(
                "The node is not currently ready for Paymaster transfers. Check node "
                "synchronization, txindex and Paymaster prerequisites, or select Own DGB.\n\n"
                "Technical details: %1").arg(reasonFailed);
        } else if (reasonFailed.contains("Insufficient DGB", Qt::CaseInsensitive)) {
            errorTitle = tr("Not enough DGB for the network fee");
            errorMessage = tr(
                "This wallet cannot currently fund the DigiByte network fee with suitable "
                "confirmed DGB inputs. Add DGB or deliberately select a Paymaster option.\n\n"
                "Technical details: %1").arg(reasonFailed);
        } else if (reasonFailed.contains("locked", Qt::CaseInsensitive)) {
            errorMessage = tr(
                "Your wallet is locked.\n\n"
                "Please unlock your wallet to send DigiDollar.\n\n"
                "Go to Settings > Unlock Wallet"
            );
        } else if (reasonFailed.contains("coin selection", Qt::CaseInsensitive) ||
                   reasonFailed.contains("SelectDDCoins", Qt::CaseInsensitive)) {
            errorMessage = tr(
                "Unable to select DigiDollar for transfer.\n\n"
                "This may occur if:\n"
                "• Your $DD is locked in pending transactions\n"
                "• The requested amount requires too many inputs\n\n"
                "Please try:\n"
                "• Waiting for pending transactions to confirm\n"
                "• Sending a smaller amount\n\n"
                "Technical details: %1"
            ).arg(reasonFailed);
        } else if (reasonFailed.contains("signing", Qt::CaseInsensitive)) {
            errorMessage = tr(
                "Transaction signing failed.\n\n"
                "Please ensure:\n"
                "• Your wallet is unlocked\n"
                "• You have the required private keys\n\n"
                "Technical details: %1"
            ).arg(reasonFailed);
        } else if (reasonFailed.contains("mempool", Qt::CaseInsensitive) ||
                   reasonFailed.contains("broadcast", Qt::CaseInsensitive)) {
            errorMessage = tr(
                "Transaction was rejected by the network.\n\n"
                "This may occur if:\n"
                "• Network fees are too low\n"
                "• The transaction conflicts with another\n"
                "• Network connectivity issues\n\n"
                "Please try:\n"
                "• Waiting a few moments and trying again\n"
                "• Checking your network connection\n\n"
                "Technical details: %1"
            ).arg(reasonFailed);
        } else {
            // Generic transaction failure
            errorMessage = tr(
                "Failed to create or send the transaction.\n\n"
                "Technical details: %1\n\n"
                "If this problem persists, please check:\n"
                "• Wallet synchronization status\n"
                "• Network connection\n"
                "• Available DigiDollar balance"
            ).arg(reasonFailed);
        }
        break;

    default:
        errorTitle = tr("Transfer Error");
        errorMessage = tr(
            "An unexpected error occurred during transfer.\n\n"
            "Error details: %1\n\n"
            "Please try again or contact support if the issue persists."
        ).arg(reasonFailed);
        break;
    }

    showError(errorTitle, errorMessage);
}

void DigiDollarSendWidget::updateAddressValidation()
{
    QString address = m_addressEdit->text();
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
    QString errorColor = isDarkTheme ? "#f44336" : "#dc3545";

    if (address.isEmpty()) {
        m_addressValidationLabel->setText(tr("Enter a valid DigiDollar address for this network"));
        DigiDollarStatus::SetText(m_addressValidationLabel, DigiDollarStatus::Kind::INFO);
        m_addressEdit->setStyleSheet("");
    } else if (validateAddress()) {
        m_addressValidationLabel->setText(tr("✓ Valid DigiDollar address"));
        DigiDollarStatus::SetText(m_addressValidationLabel, DigiDollarStatus::Kind::SUCCESS);
        m_addressEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(successColor));
    } else {
        m_addressValidationLabel->setText(tr("✗ Invalid DigiDollar address for this network"));
        DigiDollarStatus::SetText(m_addressValidationLabel, DigiDollarStatus::Kind::ERR);
        m_addressEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(errorColor));
    }
}

void DigiDollarSendWidget::updateAmountValidation()
{
    QString amountText = m_amountEdit->text();
    QPalette palette = QApplication::palette();
    int lightness = palette.color(QPalette::WindowText).lightness();
    bool isDarkTheme = lightness > 127;

    QString successColor = isDarkTheme ? "#4caf50" : "#28a745";
    QString warningColor = isDarkTheme ? "#ff9800" : "#ffc107";
    QString errorColor = isDarkTheme ? "#f44336" : "#dc3545";

    if (!amountText.isEmpty()) {
        bool isValid = validateAmount();
        bool hasBalance = validateBalance();

        if (!isValid) {
            // Invalid format
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(errorColor));
        } else if (!hasBalance) {
            // Valid format but insufficient balance
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(warningColor));
        } else {
            // Valid and sufficient balance
            m_amountEdit->setStyleSheet(QString("QLineEdit { border: 2px solid %1; }").arg(successColor));
        }
    } else {
        m_amountEdit->setStyleSheet("");
    }
}

// DDSendConfirmationDialog implementation
// Uses the DGB send confirmation countdown while allowing a context-specific
// action label for prepare-only Paymaster requests.
DDSendConfirmationDialog::DDSendConfirmationDialog(const QString& title, const QString& text,
                                                     const QString& informative_text,
                                                     int secDelay,
                                                     const QString& confirm_button_text,
                                                     QWidget* parent)
    : QMessageBox(parent), secDelay(secDelay)
{
    setIcon(QMessageBox::Question);
    setWindowTitle(title); // On macOS, the window title is ignored (as required by the macOS Guidelines).
    setText(text);
    setInformativeText(informative_text);
    setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    confirmButtonText = confirm_button_text.isEmpty()
        ? yesButton->text() : confirm_button_text;
    updateButtons();
    connect(&countDownTimer, &QTimer::timeout, this, &DDSendConfirmationDialog::countDown);
}

int DDSendConfirmationDialog::exec()
{
    updateButtons();
    countDownTimer.start(1s);
    return QMessageBox::exec();
}

void DDSendConfirmationDialog::countDown()
{
    secDelay--;
    updateButtons();

    if (secDelay <= 0) {
        countDownTimer.stop();
    }
}

void DDSendConfirmationDialog::updateButtons()
{
    if (secDelay > 0) {
        // Disable button and show countdown
        yesButton->setEnabled(false);
        yesButton->setText(confirmButtonText + QString(" (%1)").arg(secDelay));
    } else {
        // Enable button and remove countdown
        yesButton->setEnabled(true);
        yesButton->setText(confirmButtonText);
    }
}

// DigiDollarAddressValidator implementation
DigiDollarAddressValidator::DigiDollarAddressValidator(QObject* parent) :
    QValidator(parent)
{
}

QValidator::State DigiDollarAddressValidator::validate(QString& input, int& pos) const
{
    Q_UNUSED(pos)

    if (input.isEmpty()) {
        return QValidator::Intermediate;
    }

    if (isValidDDAddress(input)) {
        return QValidator::Acceptable;
    }

    // Check if it could become valid with more characters
    if (input.length() < 3) {
        if (input.startsWith("D") || input.startsWith("T") || input.startsWith("R")) {
            return QValidator::Intermediate;
        }
    } else if (input.length() < 42) {
        if (input.startsWith("DD") || input.startsWith("TD") || input.startsWith("RD")) {
            return QValidator::Intermediate;
        }
    }

    return QValidator::Invalid;
}

bool DigiDollarAddressValidator::isValidDDAddress(const QString& address) const
{
    return CDigiDollarAddress::IsValidDigiDollarAddressForCurrentNetwork(address.toStdString());
}

void DigiDollarSendWidget::onCoinControlButtonClicked()
{
    if (!m_walletModel) {
        return;
    }

    // Initialize coin control if not already done
    if (!m_coinControl) {
        m_coinControl = std::make_unique<wallet::DDCoinControl>();
    }

    // Open the coin control dialog
    // Note: platformStyle would typically come from the main window
    DigiDollarCoinControlDialog dlg(*m_coinControl, m_walletModel, nullptr, this);
    dlg.exec();

    // Update labels after dialog closes
    updateCoinControlLabels();
}

void DigiDollarSendWidget::updateCoinControlLabels()
{
    if (!m_coinControl || !m_walletModel) {
        // No coin control active, show automatic selection state.
        if (m_coinControlQuantityLabel) {
            m_coinControlQuantityLabel->setText(tr("automatically selected"));
            m_coinControlQuantityLabel->setVisible(true);
        }
        if (m_coinControlAmountLabel) {
            m_coinControlAmountLabel->clear();
            m_coinControlAmountLabel->setVisible(true);
        }
        updateFeeDisplay();
        return;
    }

    if (!m_coinControl->HasSelected()) {
        if (m_coinControlQuantityLabel) {
            m_coinControlQuantityLabel->setText(tr("automatically selected"));
            m_coinControlQuantityLabel->setVisible(true);
        }
        if (m_coinControlAmountLabel) {
            m_coinControlAmountLabel->clear();
            m_coinControlAmountLabel->setVisible(true);
        }
        updateFeeDisplay();
        return;
    }

    // Count selected inputs and calculate total amount
    std::vector<COutPoint> selectedInputs = m_coinControl->ListSelected();
    int nQuantity = selectedInputs.size();
    const CAmount selectedAmount = selectedDigiDollarAmount();

    // Show quantity label
    if (m_coinControlQuantityLabel) {
        m_coinControlQuantityLabel->setText(tr("Quantity: %1").arg(nQuantity));
        m_coinControlQuantityLabel->setVisible(true);
    }

    if (m_coinControlAmountLabel) {
        m_coinControlAmountLabel->setText(tr("Amount: %1").arg(formatDDAmount(selectedAmount / 100.0)));
        m_coinControlAmountLabel->setVisible(true);
    }

    updateFeeDisplay();
}

CAmount DigiDollarSendWidget::selectedDigiDollarAmount() const
{
    if (!m_coinControl || !m_walletModel) return 0;

    DigiDollarWallet* ddWallet = m_walletModel->getDigiDollarWallet();
    if (!ddWallet) return 0;

    CAmount amount = 0;
    for (const COutPoint& outpoint : m_coinControl->ListSelected()) {
        const CAmount dd_amount = ddWallet->GetDDFromUTXO(outpoint);
        if (dd_amount > 0) amount += dd_amount;
    }
    return amount;
}

void DigiDollarSendWidget::setSelectedDigiDollarInputsForTesting(const std::vector<COutPoint>& inputs)
{
    if (!m_coinControl) {
        m_coinControl = std::make_unique<wallet::DDCoinControl>();
    }
    m_coinControl->UnSelectAll();
    for (const COutPoint& input : inputs) {
        m_coinControl->Select(input);
    }
    updateCoinControlLabels();
}

void DigiDollarSendWidget::setAvailableDigiDollarBalanceForTesting(CAmount balance_cents)
{
    m_availableBalance = static_cast<double>(balance_cents) / 100.0;
    m_availableBalanceValue->setText(formatDDAmount(m_availableBalance));
    m_useAvailableBalanceButton->setEnabled(balance_cents > 0);
    updateAmountValidation();
    updateFeeDisplay();
    updateSendButton();
}

WalletModel::DigiDollarSendResult DigiDollarSendWidget::sendDigiDollarForTesting(const QString& address, CAmount amount, const QString& comment)
{
    std::vector<COutPoint> selectedInputs;
    const std::vector<COutPoint>* presetInputs = nullptr;
    if (m_coinControl && m_coinControl->HasSelected()) {
        selectedInputs = m_coinControl->ListSelected();
        presetInputs = &selectedInputs;
    }
    return m_walletModel->sendDigiDollar(address, amount, comment, presetInputs);
}

QString DigiDollarSendWidget::successMessageForTesting(const QString& txid, double amount) const
{
    return buildSuccessMessage(txid, amount);
}

void DigiDollarSendWidget::setPaymasterSessionForTesting(
    const QString& state, const QString& artifact, bool persisted,
    const QString& address, double amount, const QString& attempt_state,
    const QString& pending_phase)
{
    m_paymasterRequestId = QStringLiteral("00000000-0000-4000-8000-000000000001");
    m_paymasterSessionState = state;
    m_paymasterArtifact = artifact;
    m_paymasterAttemptState = attempt_state;
    m_paymasterPendingPhase = pending_phase;
    m_paymasterSessionPersisted = persisted;
    m_paymasterAddress = address;
    m_paymasterAmount = amount;
    updatePaymasterFocusMode();
}

void DigiDollarSendWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    updateBalance();
    updateUSDEquivalent();
    updateFeeDisplay();
    if (m_privacy) {
        m_usdEquivalentValue->setText(maskValue(formatUSDAmount(0)));
        m_feeValue->setText(maskValue(m_feeValue->text()));
        m_totalValue->setText(maskValue(formatDDAmount(0)));
        m_feeSummary->setText(maskValue(m_feeSummary->text()));
    }
}

QString DigiDollarSendWidget::maskValue(const QString& value) const
{
    QString masked = value;
    for (int i = 0; i < masked.size(); ++i) {
        if (masked[i].isDigit()) {
            masked[i] = '#';
        }
    }
    return masked;
}

// AmountValidator implementation
AmountValidator::AmountValidator(double min, double max, int maxDecimals, QObject* parent) :
    QValidator(parent), m_min(min), m_max(max), m_maxDecimals(maxDecimals)
{
}

QValidator::State AmountValidator::validate(QString& input, int& pos) const
{
    Q_UNUSED(pos)

    if (input.isEmpty()) {
        return QValidator::Intermediate;
    }

    // Check for negative numbers
    if (input.startsWith("-")) {
        return QValidator::Invalid;
    }

    // Check for valid number format
    QRegularExpression numRegex("^\\d*\\.?\\d*$");
    if (!numRegex.match(input).hasMatch()) {
        return QValidator::Invalid;
    }

    // Check decimal places (max m_maxDecimals, default 8)
    int decimalPos = input.indexOf('.');
    if (decimalPos != -1) {
        if (input.length() - decimalPos - 1 > m_maxDecimals) {
            return QValidator::Invalid;
        }
    }

    // Convert to double and check range
    bool ok;
    double value = input.toDouble(&ok);
    if (!ok) {
        return QValidator::Intermediate;
    }

    if (value > m_max) {
        return QValidator::Invalid;
    }

    if (value < m_min) {
        // User may still be typing (e.g. "1" on the way to "100")
        // Return Intermediate so Qt allows the keystroke but the
        // submit button stays disabled until the value is in range.
        return QValidator::Intermediate;
    }

    return QValidator::Acceptable;
}

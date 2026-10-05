// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_TEST_PAYMASTERWIDGETTESTS_H
#define DIGIBYTE_QT_TEST_PAYMASTERWIDGETTESTS_H

#include <QObject>

namespace interfaces { class Node; }

class PaymasterWidgetTests : public QObject
{
    Q_OBJECT
public:
    explicit PaymasterWidgetTests(interfaces::Node& node) : m_node(node) {}

private Q_SLOTS:
    void paymasterGuidedTaskLayout_data();
    void paymasterGuidedTaskLayout();
    void paymasterGuidedCapitalTasks_data();
    void paymasterGuidedCapitalTasks();
    void paymasterGuidedRestoreHasOneApproval_data();
    void paymasterGuidedRestoreHasOneApproval();
    void paymasterOperationControllerRecoversWithoutDuplicateApproval();
    void paymasterOperationFundingReviewIsBounded();
    void paymasterDeferredStartIntentIsWalletScoped();
    void paymasterFiveDestinationsAndVisibleTasks();
    void paymasterOperatorOverviewGuidesAndFailsClosed();
    void paymasterPreparationFeeRecovery();
    void paymasterStartIntentNeedsReserveApproval();
    void paymasterPreparationContinuesAfterRefresh_data();
    void paymasterPreparationContinuesAfterRefresh();
    void paymasterProviderCommandLocksPages();
    void paymasterNodeConnectionEditor_data();
    void paymasterNodeConnectionEditor();
    void paymasterOperatingUnlock_data();
    void paymasterOperatingUnlock();
    void paymasterPoolPreparationStoredFeeDiagnostics();
    void paymasterPoolPreparationDiagnostics_data();
    void paymasterPoolPreparationDiagnostics();
    void paymasterPoolPreparationCancellation_data();
    void paymasterPoolPreparationCancellation();
    void paymasterConnectionLayout_data();
    void paymasterConnectionLayout();
    void paymasterOperatorDelayedStartup_data();
    void paymasterOperatorDelayedStartup();
    void paymasterLiquidityPolicyDefaultsAndApprovalGuard();
    void paymasterLiquidityPolicySavePersistsVisibleValues();
    void paymasterLiquiditySaveAcrossRefresh_data();
    void paymasterLiquiditySaveAcrossRefresh();
    void paymasterLiquidityMaintenanceStatesAreReadable();
    void paymasterExternalReadinessIsSeparatedFromConfiguration();
    void paymasterPendingStartUsesLiveStatusInsteadOfStaleModal();
    void paymasterCarrierWithdrawalActionsFailClosed();
    void paymasterCarrierWithdrawalPreviewsArePlanBound();
    void paymasterSafetyControlsDefaultFailClosed();
    void paymasterSafetyPolicyDisplaysFiniteDisabledSemantics();
    void paymasterInjectedRpcCoversConfigurationWorkflows();
    void paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows();
    void paymasterManualActivityUsesExpectedRpcAndReadableStates();
    void paymasterFinancesAndBackupWorkflow();
    void paymasterOverviewAutostart_data();
    void paymasterOverviewAutostart();
    void paymasterOverviewRefill_data();
    void paymasterOverviewRefill();
    void paymasterCapitalOverview_data();
    void paymasterCapitalOverview();
    void paymasterOverviewFinanceWalletAndPrivacyBinding();
    void paymasterOperatorWorkTransitions_data();
    void paymasterOperatorWorkTransitions();
    void paymasterOperatorPollingPreservesDraftsAndThrottlesFinance();
    void paymasterOperatorBackgroundRefresh_data();
    void paymasterOperatorBackgroundRefresh();
    void paymasterStopAndRelease_data();
    void paymasterStopAndRelease();
    void paymasterHomeStartIsSeparateFromSettings();
    void paymasterRetirementLateResults_data();
    void paymasterRetirementLateResults();
    void paymasterClientConfirmationRequiresObservation();
    void paymasterConfirmationGuardDetectsMaterialChanges();
    void paymasterRecoveryConfirmationGuardDetectsMaterialChanges();
    void paymasterClientOfferPreviewUsesExactRpcAndPlainText();
    void paymasterClientOfferPreviewInvalidatesAndHandlesFailures();
    void paymasterClientAuthorizationIsTwoStageAndFailClosed();
    void paymasterClientLiveSendProgressesAcrossAsyncPhases_data();
    void paymasterClientLiveSendProgressesAcrossAsyncPhases();
    void paymasterClientSessionActionMatrix_data();
    void paymasterClientSessionActionMatrix();
    void paymasterClientSessionRpcActionsAreBound();
    void paymasterClientFallbackContinues_data();
    void paymasterClientFallbackContinues();
    void paymasterClientSessionDiscoveryFailures_data();
    void paymasterClientSessionDiscoveryFailures();
    void paymasterClientMutationDialogWalletBinding_data();
    void paymasterClientMutationDialogWalletBinding();
    void paymasterClientReviewCancellation_data();
    void paymasterClientReviewCancellation();
    void paymasterClientOfferAutomaticRefresh();
    void paymasterClientOfferCards_data();
    void paymasterClientOfferCards();
    void paymasterAppNumberFormat();
    void paymasterClientPreparationRequiresCurrentOffer_data();
    void paymasterClientPreparationRequiresCurrentOffer();
    void paymasterClientFundingBalanceChanges();
    void paymasterFeeAmountsAndPercentages();
    void paymasterOfferPolicyTypedValues_data();
    void paymasterOfferPolicyTypedValues();
    void paymasterOfferFormAlignment_data();
    void paymasterOfferFormAlignment();
    void paymasterClientReleasedInputsCanBeReservedAgain();
    void paymasterClientUncreatedRequestReturnsToCompose_data();
    void paymasterClientUncreatedRequestReturnsToCompose();
    void paymasterClientCoreCancellationRoundTrip_data();
    void paymasterClientCoreCancellationRoundTrip();
    void paymasterOperatorConfirmationWalletBinding_data();
    void paymasterOperatorConfirmationWalletBinding();
    void paymasterClientRestartCreatedSessionWithoutRecipientIsReadOnly();
    void paymasterClientRecipientlessCancellation_data();
    void paymasterClientRecipientlessCancellation();
    void paymasterClientMultipleRestartSessionsRequireSelection();
    void paymasterClientRecoveryExpiryIsFailClosed();
    void paymasterClientLiveRecoveryProgresses_data();
    void paymasterClientLiveRecoveryProgresses();
    void paymasterClientDelayedCallbackIgnoresWalletClose();
    void paymasterClientUnlockLeaseSurvivesWalletModelClose();
    void paymasterClientDatabaseReadErrorIsActionable();
    void paymasterClientControlsAreAccessible();
    void paymasterOfferTableRendersGreenTheme();
    void paymasterGuidedSetupRendersConsistentTheme();
    void paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep_data();
    void paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep();

private:
    interfaces::Node& m_node;
};

#endif // DIGIBYTE_QT_TEST_PAYMASTERWIDGETTESTS_H

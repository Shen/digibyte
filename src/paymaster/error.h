// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_PAYMASTER_ERROR_H
#define DIGIBYTE_PAYMASTER_ERROR_H

#include <algorithm>
#include <stdexcept>
#include <string>

namespace DigiDollar::Paymaster {
enum class ErrorKind { WALLET, INVALID_INPUT, DESERIALIZATION, UNLOCK, REJECTED, INTERNAL, GENERAL, NODE_CAPACITY, INSUFFICIENT_FUNDS, INVALID_ADDRESS };
enum class ErrorDisposition { RETRY_BEFORE_EXECUTION, RECONCILE, USER_DECISION, PERMANENT };

inline const char* ErrorDispositionName(ErrorDisposition disposition)
{
    switch (disposition) {
    case ErrorDisposition::RETRY_BEFORE_EXECUTION: return "retry_before_execution";
    case ErrorDisposition::RECONCILE: return "reconcile";
    case ErrorDisposition::USER_DECISION: return "user_decision";
    case ErrorDisposition::PERMANENT: return "permanent";
    }
    return "reconcile";
}

/** An execution failure is ambiguous by default: callers must reconcile durable
 * state before attempting another mutation. Only the failing operation can
 * explicitly assert that no authorization or execution has started. */
class ApplicationError : public std::runtime_error {
public:
    const ErrorKind kind;
    const ErrorDisposition disposition;
    const std::string code;
    const bool session_absent;

    ApplicationError(ErrorKind category, const std::string& reason,
                     ErrorDisposition recovery = ErrorDisposition::RECONCILE,
                     bool verified_session_absent = false)
        : std::runtime_error(reason.empty() ? "PAYMASTER_APPLICATION_ERROR" : reason), kind{category}, disposition{recovery},
          code{IsCode(reason) ? reason : "PAYMASTER_APPLICATION_ERROR"},
          session_absent{verified_session_absent} {}

    ApplicationError(ErrorKind category, const std::string& stable_code, const std::string& message,
                     ErrorDisposition recovery, bool verified_session_absent = false)
        : std::runtime_error(message.empty() ? stable_code : message), kind{category}, disposition{recovery},
          code{IsCode(stable_code) ? stable_code : "PAYMASTER_APPLICATION_ERROR"},
          session_absent{verified_session_absent} {}

private:
    static bool IsCode(const std::string& reason)
    {
        return reason.rfind("PAYMASTER_", 0) == 0 && reason.size() <= 128 &&
            std::all_of(reason.begin(), reason.end(), [](unsigned char ch) {
                return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
            });
    }
};
} // namespace DigiDollar::Paymaster
#endif // DIGIBYTE_PAYMASTER_ERROR_H

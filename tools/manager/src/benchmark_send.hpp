#pragma once

#include <string>
#include <string_view>

namespace anvil::manager {

// Send (docs/spec/benchmark-v1.md §9): the report goes to the receiver only on
// the player's click, after the preview showed its exact bytes. No automatic
// retries: the player clicks again.
inline constexpr const wchar_t* kReceiverHost = L"bench.happydwarf.dev";
inline constexpr const wchar_t* kReceiverPath = L"/reports";
inline constexpr const char* kPrivacyUrl = "https://github.com/happy-dwarf-team/anvil/blob/main/docs/benchmark-privacy.md";
inline constexpr const char* kPrivacyAddress = "privacy@happydwarf.dev";

// How a Send ended.
struct SendOutcome {
    int status = 0;       // the HTTP status; 0 when the request did not get an answer
    std::string body;     // the answer, at most 64 KB
    std::string error;    // the network error, with status 0
};

// The receiver's {"message": "..."}; "" when the body is not one.
std::string receiver_message(std::string_view body);

// 201 or 200: the report is on the server; Send becomes a disabled "Sent".
inline bool send_accepted(const SendOutcome& outcome) { return outcome.status == 201 || outcome.status == 200; }

// The line under Send and Copy (§9's table).
std::string send_outcome_text(const SendOutcome& outcome);

}  // namespace anvil::manager

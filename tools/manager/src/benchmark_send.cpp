#include "benchmark_send.hpp"

#include "json.hpp"

namespace anvil::manager {

std::string receiver_message(std::string_view body) {
    std::string error;
    const auto doc = parse_json(body, error);
    if (!doc || !(*doc)["message"].is_string()) return "";
    std::string message = (*doc)["message"].as_string();
    if (message.size() > 300) message.resize(300);
    return message;
}

std::string send_outcome_text(const SendOutcome& o) {
    if (o.status == 0) return "The report was not sent: " + o.error + ". You can copy it instead.";
    if (o.status == 201) return "Sent. Thank you.";
    if (o.status == 200) return "This report has already arrived.";
    if (o.status == 429 || o.status == 503) return "The receiver is busy. Try again later.";
    const std::string message = receiver_message(o.body);
    if (o.status == 413 || o.status == 422) return message.empty() ? "The receiver refused the report." : message;
    return "The receiver answered " + std::to_string(o.status) + (message.empty() ? std::string(".") : ": " + message);
}

}  // namespace anvil::manager

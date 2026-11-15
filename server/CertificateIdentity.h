#pragma once

#include "base/xmq.h"
#include <string>

namespace xmq {

/// Reverify a TLS peer for client authentication and bind one certificate field to MQTT username.
/// Empty identityField disables binding; an empty crlFile disables revocation checking.
/// Throws on any malformed, ambiguous, untrusted, expired or revoked input.
XMQ_EXPORT void verifyCertificateIdentity(const std::string& chainPem, const std::string& caFile,
                               const std::string& crlFile, const std::string& identityField,
                               const std::string& username, int verifyDepth);

} // namespace xmq

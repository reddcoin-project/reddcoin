// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CA_STORE_H
#define BITCOIN_NODE_CA_STORE_H

#include <string>

//! Forward declared so callers do not have to include the OpenSSL headers, and
//! so this header stays free of the Boost.Asio wrapper around them.
typedef struct ssl_ctx_st SSL_CTX;

namespace node {
/**
 * Load the host platform's trusted root certificates into ssl_ctx.
 *
 * This exists because the depends build cannot use OpenSSL's default verify
 * paths. depends configures OpenSSL with
 * `--openssldir=$(host_prefix)/etc/openssl`, which bakes the *builder's*
 * absolute path into libcrypto. On the machine that later runs the binary that
 * directory does not exist, so SSL_CTX_set_default_verify_paths() installs no
 * trust anchors at all and every verification fails. That is true of release
 * builds on every host, not only Windows and macOS.
 *
 * The anchors therefore come from the operating system instead:
 *
 *   - Windows: OpenSSL's own winstore loader, which reads the system ROOT
 *     certificate store.
 *   - macOS: the system trust store via Security.framework.
 *   - Everything else: SSL_CERT_FILE / SSL_CERT_DIR when set, otherwise the
 *     first of the well-known distribution bundle locations that exists.
 *
 * Taking the anchors from the platform rather than shipping a bundle means the
 * trust list follows the operating system's own updates and revocations, and
 * leaves no CA list in this repository that somebody has to remember to
 * refresh.
 *
 * @return An empty string on success, otherwise a description of why no
 *         anchors could be loaded. Callers must treat a non-empty return as
 *         fatal to the connection: continuing without anchors would leave
 *         verification enabled but trusting nothing, or worse, be papered over
 *         by disabling verification.
 */
std::string LoadTrustedCACertificates(SSL_CTX* ssl_ctx);
} // namespace node

#endif // BITCOIN_NODE_CA_STORE_H

// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/ca_store.h>

#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdlib>
#include <string>

#if defined(WIN32)
#if OPENSSL_VERSION_NUMBER < 0x30000000L
#error "Windows builds need OpenSSL 3.0 or later for the winstore certificate loader; build against depends."
#endif
#elif defined(MAC_OSX)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <sys/stat.h>
#endif

namespace {
#if defined(MAC_OSX)
//! Add one DER-encoded certificate to a store.
//!
//! A certificate the store already holds is not a failure. Platform trust
//! stores can list the same root more than once, and OpenSSL reports the
//! duplicate as an error that would otherwise abort the whole load.
bool AddDerCertificate(X509_STORE* store, const unsigned char* der, long der_len)
{
    // d2i_X509 advances the pointer it is given, so hand it a copy.
    const unsigned char* pos{der};
    X509* cert{d2i_X509(nullptr, &pos, der_len)};
    if (cert == nullptr) {
        ERR_clear_error();
        return false;
    }

    bool ok{true};
    if (X509_STORE_add_cert(store, cert) != 1) {
        ok = ERR_GET_REASON(ERR_peek_last_error()) == X509_R_CERT_ALREADY_IN_HASH_TABLE;
        ERR_clear_error();
    }

    // X509_STORE_add_cert takes its own reference, so drop ours either way.
    X509_free(cert);
    return ok;
}
#endif // MAC_OSX

#if !defined(WIN32) && !defined(MAC_OSX)
//! Bundle locations used by the distributions this is likely to run on. The
//! first one that exists wins; they hold the same set of public roots.
const char* const CA_FILE_CANDIDATES[]{
    "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Gentoo, Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL, CentOS
    "/etc/ssl/ca-bundle.pem",                            // openSUSE
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS 7 and later
    "/etc/ssl/cert.pem",                                 // FreeBSD, OpenBSD, Alpine
};

//! Hashed certificate directories, used only if no bundle file is found.
const char* const CA_DIR_CANDIDATES[]{
    "/etc/ssl/certs",
    "/etc/pki/tls/certs",
};

bool PathExists(const char* path)
{
    struct stat sb;
    return ::stat(path, &sb) == 0;
}
#endif // !WIN32 && !MAC_OSX
} // namespace

std::string node::LoadTrustedCACertificates(SSL_CTX* ssl_ctx)
{
    if (ssl_ctx == nullptr) return "No TLS context to load certificates into";

#if defined(WIN32)
    // OpenSSL 3.2 and later expose the Windows ROOT store through the OSSL_STORE
    // URI below. It reads the same anchors the rest of the system trusts, so it
    // picks up enterprise roots and administrator revocations without any
    // enumeration code here. depends must not be configured with no-winstore.
    if (SSL_CTX_load_verify_store(ssl_ctx, "org.openssl.winstore://") != 1) {
        ERR_clear_error();
        return "Could not read the Windows system certificate store";
    }
    return "";

#elif defined(MAC_OSX)
    // OpenSSL has no equivalent loader for the macOS keychain, so the anchors
    // are copied across one at a time.
    CFArrayRef anchors{nullptr};
    if (SecTrustCopyAnchorCertificates(&anchors) != errSecSuccess || anchors == nullptr) {
        return "Could not read the macOS system trust store";
    }

    X509_STORE* store{SSL_CTX_get_cert_store(ssl_ctx)};
    if (store == nullptr) {
        CFRelease(anchors);
        return "No TLS certificate store to load anchors into";
    }

    long loaded{0};
    const CFIndex count{CFArrayGetCount(anchors)};
    for (CFIndex i = 0; i < count; ++i) {
        const void* elem{CFArrayGetValueAtIndex(anchors, i)};
        if (elem == nullptr) continue;
        SecCertificateRef cert{reinterpret_cast<SecCertificateRef>(const_cast<void*>(elem))};

        CFDataRef der{SecCertificateCopyData(cert)};
        if (der == nullptr) continue;
        if (AddDerCertificate(store, CFDataGetBytePtr(der), static_cast<long>(CFDataGetLength(der)))) {
            ++loaded;
        }
        CFRelease(der);
    }
    CFRelease(anchors);

    if (loaded == 0) return "The macOS system trust store contained no usable certificates";
    return "";

#else
    // An explicit environment setting is honoured first, so a container or a
    // distribution that keeps its bundle somewhere unusual can point at it
    // without a rebuild. These are the same two variables OpenSSL's own default
    // verify paths consult.
    const char* const env_file{std::getenv("SSL_CERT_FILE")};
    const char* const env_dir{std::getenv("SSL_CERT_DIR")};
    if ((env_file != nullptr && *env_file != '\0') || (env_dir != nullptr && *env_dir != '\0')) {
        if (SSL_CTX_load_verify_locations(ssl_ctx,
                                          (env_file != nullptr && *env_file != '\0') ? env_file : nullptr,
                                          (env_dir != nullptr && *env_dir != '\0') ? env_dir : nullptr) == 1) {
            return "";
        }
        ERR_clear_error();
        return "SSL_CERT_FILE or SSL_CERT_DIR is set but no certificates could be read from it";
    }

    for (const char* const candidate : CA_FILE_CANDIDATES) {
        if (!PathExists(candidate)) continue;
        if (SSL_CTX_load_verify_locations(ssl_ctx, candidate, nullptr) == 1) return "";
        ERR_clear_error();
    }

    for (const char* const candidate : CA_DIR_CANDIDATES) {
        if (!PathExists(candidate)) continue;
        if (SSL_CTX_load_verify_locations(ssl_ctx, nullptr, candidate) == 1) return "";
        ERR_clear_error();
    }

    // Last resort: OpenSSL's compiled-in location, which is what a build
    // against a system OpenSSL rather than against depends will normally
    // succeed with.
    //
    // Whether it exists has to be checked here. SSL_CTX_set_default_verify_paths
    // only registers the lookups and reports success without touching the
    // filesystem, so trusting its return value would report an empty trust
    // store as a working one, which is precisely the depends case this function
    // exists to catch.
    const char* const default_file{X509_get_default_cert_file()};
    const char* const default_dir{X509_get_default_cert_dir()};
    if ((default_file != nullptr && PathExists(default_file)) ||
        (default_dir != nullptr && PathExists(default_dir))) {
        if (SSL_CTX_set_default_verify_paths(ssl_ctx) == 1) return "";
        ERR_clear_error();
    }

    return "No CA certificate bundle was found in any of the usual locations; "
           "set SSL_CERT_FILE or SSL_CERT_DIR to point at one";
#endif
}

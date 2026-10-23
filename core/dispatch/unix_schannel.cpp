// secur32's "unix side" (dlls/secur32/secur32_priv.h, enum schan_funcs): the TLS engine behind Wine's PE schannel.
// Wine's own unix side is GnuTLS; here every session is an SSPI SChannel context of the HOST (the Windows process on
// the runners, the Wine process on linux-cross), driven by the same hand-over Wine's PE side already does: it passes
// whole TLS records in SecBuffers and takes the produced records back (handshake tokens, encrypted application
// data), and the application's sockets stay its own. Nothing here touches a socket.
//
// Certificate validation is the PE side's and the application's (it reads the peer certificate with
// QueryContextAttributes and calls crypt32): the host credentials are SCH_CRED_MANUAL_CRED_VALIDATION, so the host
// does not validate the chain a second time and nothing is accepted or rejected on the host's trust store.
//
// Limits (answered with an error, never faked): server credentials and client certificates (the PE side would have
// to import the key into the host), unique channel bindings, DTLS. The protocol set is the host's default.
//
// Reached from guest threads in the host view (UnixCallDispatcher); no function-local statics with dynamic init.
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include "internal.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace win10xbox::dispatch::detail {
namespace {

using Session64 = std::uint64_t;
constexpr NTSTATUS STATUS_INTERNAL_ERROR = static_cast<NTSTATUS>(0xC00000E5);
constexpr NTSTATUS STATUS_NOT_IMPLEMENTED = static_cast<NTSTATUS>(0xC0000002);
constexpr NTSTATUS STATUS_NOT_SUPPORTED = static_cast<NTSTATUS>(0xC00000BB);

struct HostCred { CredHandle handle; };

struct Session {
    HostCred* cred = nullptr;
    CtxtHandle ctx{};
    bool have_ctx = false;
    std::wstring target;
    std::vector<unsigned char> alpn;   // SEC_APPLICATION_PROTOCOLS bytes, sent with the first token
    // A handshake step whose output did not fit the caller's buffer: Wine's PE side grows the buffer and repeats the
    // call with the same input, but SSPI has consumed that input already, so the result is kept and replayed.
    bool pend = false;
    SECURITY_STATUS pend_ss = SEC_E_OK;
    ULONG pend_in = 0;
    std::vector<unsigned char> pend_out;
};

// ---- the parameter blocks of secur32_priv.h (same layout, 64-bit pointers) ----------------------------------------
struct SchanCredentials { ULONG credential_use; DWORD enabled_protocols; std::uint64_t credentials; };
struct AllocCredParams {
    SchanCredentials* c; ULONG cert_encoding; ULONG cert_size; BYTE* cert_blob; ULONG key_size; BYTE* key_blob;
};
struct CreateSessionParams { SchanCredentials* cred; Session64* session; };
struct FreeCredParams { SchanCredentials* c; };
struct SessionParams { Session64 session; };
struct GetAppProtocolParams { Session64 session; SecPkgContext_ApplicationProtocol* protocol; };
struct GetConnInfoParams { Session64 session; SecPkgContext_ConnectionInfo* info; };
struct GetCipherInfoParams { Session64 session; SecPkgContext_CipherInfo* info; };
struct GetPeerCertParams { Session64 session; BYTE* buffer; ULONG* bufsize; ULONG* retcount; };
struct GetBindingParams { Session64 session; void* buffer; ULONG* bufsize; };
enum ControlToken { kCtlNone, kCtlShutdown, kCtlAlert };
struct HandshakeParams {
    Session64 session; SecBufferDesc* input; ULONG input_size; SecBufferDesc* output; ULONG* input_offset;
    int* output_buffer_idx; ULONG* output_offset; int control_token; unsigned alert_type; unsigned alert_number;
};
struct RecvParams { Session64 session; SecBufferDesc* input; ULONG input_size; void* buffer; ULONG* length; };
struct SendParams {
    Session64 session; SecBufferDesc* output; const void* buffer; ULONG length; int* output_buffer_idx;
    ULONG* output_offset;
};
struct SetAlpnParams { Session64 session; unsigned char* buffer; unsigned buflen; };
struct SetMtuParams { Session64 session; unsigned mtu; };
struct SetTargetParams { Session64 session; const char* target; };
struct SetDtlsTimeoutsParams { Session64 session; unsigned retrans; unsigned total; };

constexpr ULONG kIscFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION |
                            ISC_REQ_EXTENDED_ERROR;

inline Session* Sess(Session64 h) { return reinterpret_cast<Session*>(h); }

// ---- process / credentials ------------------------------------------------------------------------------------

NTSTATUS WINAPI U_schan_ok(void*) { return 0; }

// Bit set of the protocols this engine can run: TLS 1.0 - 1.3 for clients (servers are not supported, see above).
NTSTATUS WINAPI U_schan_get_enabled_protocols(void*) {
    return static_cast<NTSTATUS>(SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_2_CLIENT |
                                 SP_PROT_TLS1_3_CLIENT);
}

NTSTATUS WINAPI U_schan_allocate_certificate_credentials(void* args) {
    auto* p = static_cast<AllocCredParams*>(args);
    if (p->c->credential_use != SECPKG_CRED_OUTBOUND) return STATUS_NOT_IMPLEMENTED;   // no server side
    if (p->cert_blob) return STATUS_NOT_IMPLEMENTED;                                   // no client certificate
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_NO_SERVERNAME_CHECK;
    auto* hc = new HostCred{};
    TimeStamp ts;
    SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                                                   nullptr, &sc, nullptr, nullptr, &hc->handle, &ts);
    if (ss != SEC_E_OK) { delete hc; return STATUS_INTERNAL_ERROR; }
    p->c->credentials = reinterpret_cast<std::uint64_t>(hc);
    return 0;
}

NTSTATUS WINAPI U_schan_free_certificate_credentials(void* args) {
    auto* p = static_cast<FreeCredParams*>(args);
    auto* hc = reinterpret_cast<HostCred*>(p->c->credentials);
    if (hc) { FreeCredentialsHandle(&hc->handle); delete hc; }
    p->c->credentials = 0;
    return 0;
}

// ---- sessions ------------------------------------------------------------------------------------------------

NTSTATUS WINAPI U_schan_create_session(void* args) {
    auto* p = static_cast<CreateSessionParams*>(args);
    *p->session = 0;
    if (p->cred->credential_use != SECPKG_CRED_OUTBOUND || !p->cred->credentials) return STATUS_INTERNAL_ERROR;
    auto* s = new Session{};
    s->cred = reinterpret_cast<HostCred*>(p->cred->credentials);
    *p->session = reinterpret_cast<Session64>(s);
    return 0;
}

NTSTATUS WINAPI U_schan_dispose_session(void* args) {
    Session* s = Sess(static_cast<SessionParams*>(args)->session);
    if (s->have_ctx) DeleteSecurityContext(&s->ctx);
    delete s;
    return 0;
}

NTSTATUS WINAPI U_schan_set_session_target(void* args) {
    auto* p = static_cast<SetTargetParams*>(args);
    Session* s = Sess(p->session);
    s->target.clear();
    for (const char* c = p->target; c && *c; ++c) s->target.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    return 0;
}

NTSTATUS WINAPI U_schan_set_application_protocols(void* args) {
    auto* p = static_cast<SetAlpnParams*>(args);
    Sess(p->session)->alpn.assign(p->buffer, p->buffer + p->buflen);
    return 0;
}

NTSTATUS WINAPI U_schan_dtls_unsupported(void*) { return STATUS_NOT_SUPPORTED; }

// ---- handshake -------------------------------------------------------------------------------------------------

NTSTATUS WINAPI U_schan_handshake(void* args) {
    auto* p = static_cast<HandshakeParams*>(args);
    Session* s = Sess(p->session);
    *p->input_offset = 0;
    *p->output_buffer_idx = -1;
    *p->output_offset = 0;

    SecBuffer out_buf{0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out_buf};
    SECURITY_STATUS ss;
    ULONG attr = 0;
    TimeStamp ts;

    if (s->pend) {
        ss = s->pend_ss;
        *p->input_offset = s->pend_in;
    } else {
        if (p->control_token) {
            if (!s->have_ctx) return SEC_E_INVALID_HANDLE;
            DWORD shutdown = SCHANNEL_SHUTDOWN;
            SCHANNEL_ALERT_TOKEN alert{SCHANNEL_ALERT, p->alert_type, p->alert_number};
            SecBuffer ctl;
            if (p->control_token == kCtlShutdown) { ctl = {sizeof shutdown, SECBUFFER_TOKEN, &shutdown}; }
            else { ctl = {sizeof alert, SECBUFFER_TOKEN, &alert}; }
            SecBufferDesc ctl_desc{SECBUFFER_VERSION, 1, &ctl};
            ss = ApplyControlToken(&s->ctx, &ctl_desc);
            if (ss != SEC_E_OK) return ss;
            ss = InitializeSecurityContextW(&s->cred->handle, &s->ctx, s->target.empty() ? nullptr : s->target.data(),
                                            kIscFlags, 0, 0, nullptr, 0, &s->ctx, &out_desc, &attr, &ts);
        } else {
            SecBuffer in[3] = {};
            SecBufferDesc in_desc{SECBUFFER_VERSION, 0, in};
            std::vector<unsigned char> data;
            ULONG used = 0;
            if (p->input && p->input->cBuffers && p->input_size) {
                const SecBuffer& src = p->input->pBuffers[0];
                used = p->input_size < src.cbBuffer ? p->input_size : src.cbBuffer;
                data.assign(static_cast<unsigned char*>(src.pvBuffer), static_cast<unsigned char*>(src.pvBuffer) + used);
                in[0] = {used, SECBUFFER_TOKEN, data.data()};
                in[1] = {0, SECBUFFER_EMPTY, nullptr};
                in_desc.cBuffers = 2;
            }
            if (!s->have_ctx && !s->alpn.empty()) {
                in[in_desc.cBuffers++] = {static_cast<ULONG>(s->alpn.size()), SECBUFFER_APPLICATION_PROTOCOLS,
                                          s->alpn.data()};
            }
            ss = InitializeSecurityContextW(&s->cred->handle, s->have_ctx ? &s->ctx : nullptr,
                                            s->target.empty() ? nullptr : s->target.data(), kIscFlags, 0, 0,
                                            in_desc.cBuffers ? &in_desc : nullptr, 0, &s->ctx, &out_desc, &attr, &ts);
            if (!FAILED(ss)) s->have_ctx = true;
            if (ss != SEC_E_INCOMPLETE_MESSAGE && in_desc.cBuffers >= 2 && used) {
                ULONG extra = (in[1].BufferType == SECBUFFER_EXTRA) ? in[1].cbBuffer : 0;
                *p->input_offset = used - extra;
            }
        }

        s->pend_ss = ss;
        s->pend_in = *p->input_offset;
        s->pend_out.clear();
        if (out_buf.pvBuffer) {
            if (out_buf.cbBuffer)
                s->pend_out.assign(static_cast<unsigned char*>(out_buf.pvBuffer),
                                   static_cast<unsigned char*>(out_buf.pvBuffer) + out_buf.cbBuffer);
            FreeContextBuffer(out_buf.pvBuffer);
        }
    }

    if (!s->pend_out.empty() && p->output && p->output->cBuffers) {
        SecBuffer& dst = p->output->pBuffers[0];
        if (s->pend_out.size() > dst.cbBuffer) {
            s->pend = true;
            return SEC_E_BUFFER_TOO_SMALL;
        }
        std::memcpy(dst.pvBuffer, s->pend_out.data(), s->pend_out.size());
        *p->output_buffer_idx = 0;
        *p->output_offset = static_cast<ULONG>(s->pend_out.size());
    }
    s->pend = false;
    s->pend_out.clear();
    return ss;
}

// ---- record protection -------------------------------------------------------------------------------------------

bool StreamSizes(Session* s, SecPkgContext_StreamSizes* sizes) {
    return s->have_ctx && QueryContextAttributesW(&s->ctx, SECPKG_ATTR_STREAM_SIZES, sizes) == SEC_E_OK;
}

NTSTATUS WINAPI U_schan_send(void* args) {
    auto* p = static_cast<SendParams*>(args);
    Session* s = Sess(p->session);
    SecPkgContext_StreamSizes sz;
    if (!StreamSizes(s, &sz)) return SEC_E_INVALID_HANDLE;
    std::vector<unsigned char> wire;   // the records, back to back
    const unsigned char* src = static_cast<const unsigned char*>(p->buffer);
    ULONG left = p->length;
    do {
        ULONG n = left < sz.cbMaximumMessage ? left : sz.cbMaximumMessage;
        std::vector<unsigned char> rec(sz.cbHeader + n + sz.cbTrailer);
        if (n) std::memcpy(rec.data() + sz.cbHeader, src, n);
        SecBuffer b[4] = {{sz.cbHeader, SECBUFFER_STREAM_HEADER, rec.data()},
                          {n, SECBUFFER_DATA, rec.data() + sz.cbHeader},
                          {sz.cbTrailer, SECBUFFER_STREAM_TRAILER, rec.data() + sz.cbHeader + n},
                          {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        SECURITY_STATUS ss = EncryptMessage(&s->ctx, 0, &d, 0);
        if (ss != SEC_E_OK) return ss;
        for (int i = 0; i < 3; ++i) {
            const auto* q = static_cast<const unsigned char*>(b[i].pvBuffer);
            wire.insert(wire.end(), q, q + b[i].cbBuffer);
        }
        src += n;
        left -= n;
    } while (left);

    // Fill the caller's buffers in order, each up to its cbBuffer (Wine's push_adapter does the same).
    size_t pos = 0;
    int last = -1;
    ULONG last_off = 0;
    for (ULONG i = 0; i < p->output->cBuffers && pos < wire.size(); ++i) {
        SecBuffer& dst = p->output->pBuffers[i];
        size_t n = wire.size() - pos < dst.cbBuffer ? wire.size() - pos : dst.cbBuffer;
        std::memcpy(dst.pvBuffer, wire.data() + pos, n);
        pos += n;
        last = static_cast<int>(i);
        last_off = static_cast<ULONG>(n);
    }
    if (pos < wire.size()) return SEC_E_BUFFER_TOO_SMALL;
    *p->output_buffer_idx = last;
    *p->output_offset = last_off;
    return SEC_E_OK;
}

NTSTATUS WINAPI U_schan_recv(void* args) {
    auto* p = static_cast<RecvParams*>(args);
    Session* s = Sess(p->session);
    if (!s->have_ctx) return SEC_E_INVALID_HANDLE;
    const SecBuffer& src = p->input->pBuffers[0];
    ULONG n = p->input_size < src.cbBuffer ? p->input_size : src.cbBuffer;
    std::vector<unsigned char> rec(static_cast<unsigned char*>(src.pvBuffer),
                                   static_cast<unsigned char*>(src.pvBuffer) + n);
    SecBuffer b[4] = {{n, SECBUFFER_DATA, rec.data()}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr},
                      {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc d{SECBUFFER_VERSION, 4, b};
    SECURITY_STATUS ss = DecryptMessage(&s->ctx, &d, 0, nullptr);
    *p->length = 0;
    if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE) return ss;
    for (auto& x : b) {
        if (x.BufferType != SECBUFFER_DATA) continue;
        ULONG m = x.cbBuffer;
        if (m > n) m = n;
        std::memcpy(p->buffer, x.pvBuffer, m);
        *p->length = m;
        break;
    }
    return ss;
}

// ---- queries -------------------------------------------------------------------------------------------------

NTSTATUS WINAPI U_schan_get_max_message_size(void* args) {
    SecPkgContext_StreamSizes sz;
    return StreamSizes(Sess(static_cast<SessionParams*>(args)->session), &sz) ? static_cast<NTSTATUS>(sz.cbMaximumMessage) : 0;
}

NTSTATUS WINAPI U_schan_get_session_cipher_block_size(void* args) {
    SecPkgContext_StreamSizes sz;
    return StreamSizes(Sess(static_cast<SessionParams*>(args)->session), &sz) ? static_cast<NTSTATUS>(sz.cbBlockSize) : 0;
}

NTSTATUS WINAPI U_schan_get_connection_info(void* args) {
    auto* p = static_cast<GetConnInfoParams*>(args);
    Session* s = Sess(p->session);
    if (!s->have_ctx) return SEC_E_INVALID_HANDLE;
    return QueryContextAttributesW(&s->ctx, SECPKG_ATTR_CONNECTION_INFO, p->info);
}

NTSTATUS WINAPI U_schan_get_cipher_info(void* args) {
    auto* p = static_cast<GetCipherInfoParams*>(args);
    Session* s = Sess(p->session);
    if (!s->have_ctx) return SEC_E_INVALID_HANDLE;
    return QueryContextAttributesW(&s->ctx, SECPKG_ATTR_CIPHER_INFO, p->info);
}

NTSTATUS WINAPI U_schan_get_application_protocol(void* args) {
    auto* p = static_cast<GetAppProtocolParams*>(args);
    Session* s = Sess(p->session);
    std::memset(p->protocol, 0, sizeof *p->protocol);
    if (!s->have_ctx) return SEC_E_OK;
    SECURITY_STATUS ss = QueryContextAttributesW(&s->ctx, SECPKG_ATTR_APPLICATION_PROTOCOL, p->protocol);
    if (ss != SEC_E_OK) std::memset(p->protocol, 0, sizeof *p->protocol);
    return SEC_E_OK;
}

NTSTATUS WINAPI U_schan_get_unique_channel_binding(void*) { return SEC_E_UNSUPPORTED_FUNCTION; }

// CALG_RSA_SIGN / CALG_ECDSA from the leaf certificate's key algorithm, as Wine's unix side reports it.
NTSTATUS WINAPI U_schan_get_key_signature_algorithm(void* args) {
    Session* s = Sess(static_cast<SessionParams*>(args)->session);
    if (!s->have_ctx) return 0;
    PCCERT_CONTEXT cert = nullptr;
    if (QueryContextAttributesW(&s->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert) return 0;
    const char* oid = cert->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId;
    NTSTATUS alg = 0;
    if (oid && !std::strcmp(oid, "1.2.840.113549.1.1.1")) alg = CALG_RSA_SIGN;
    else if (oid && !std::strcmp(oid, "1.2.840.10045.2.1")) alg = CALG_ECDSA;
    CertFreeCertificateContext(cert);
    return alg;
}

// The peer's chain as the server sent it: ULONG sizes[count], then the DER blobs; the leaf comes first.
NTSTATUS WINAPI U_schan_get_session_peer_certificate(void* args) {
    auto* p = static_cast<GetPeerCertParams*>(args);
    Session* s = Sess(p->session);
    if (!s->have_ctx) return SEC_E_INVALID_HANDLE;
    PCCERT_CONTEXT leaf = nullptr;
    if (QueryContextAttributesW(&s->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &leaf) != SEC_E_OK || !leaf)
        return SEC_E_INTERNAL_ERROR;
    std::vector<std::vector<unsigned char>> certs;
    certs.emplace_back(leaf->pbCertEncoded, leaf->pbCertEncoded + leaf->cbCertEncoded);
    if (leaf->hCertStore) {
        PCCERT_CONTEXT c = nullptr;
        while ((c = CertEnumCertificatesInStore(leaf->hCertStore, c)) != nullptr) {
            if (c->cbCertEncoded == leaf->cbCertEncoded &&
                !std::memcmp(c->pbCertEncoded, leaf->pbCertEncoded, c->cbCertEncoded))
                continue;
            certs.emplace_back(c->pbCertEncoded, c->pbCertEncoded + c->cbCertEncoded);
        }
    }
    CertFreeCertificateContext(leaf);
    size_t size = certs.size() * sizeof(ULONG);
    for (auto& c : certs) size += c.size();
    if (!p->buffer || *p->bufsize < size) { *p->bufsize = static_cast<ULONG>(size); return SEC_E_BUFFER_TOO_SMALL; }
    auto* sizes = reinterpret_cast<ULONG*>(p->buffer);
    BYTE* out = p->buffer + certs.size() * sizeof(ULONG);
    for (size_t i = 0; i < certs.size(); ++i) {
        sizes[i] = static_cast<ULONG>(certs[i].size());
        std::memcpy(out, certs[i].data(), certs[i].size());
        out += certs[i].size();
    }
    *p->bufsize = static_cast<ULONG>(size);
    *p->retcount = static_cast<ULONG>(certs.size());
    return SEC_E_OK;
}

}  // namespace

// In enum schan_funcs order.
const UnixCallFn kSchannelFuncs[] = {
    &U_schan_ok,                                 // process_attach
    &U_schan_ok,                                 // process_detach
    &U_schan_allocate_certificate_credentials,
    &U_schan_create_session,
    &U_schan_dispose_session,
    &U_schan_free_certificate_credentials,
    &U_schan_get_application_protocol,
    &U_schan_get_cipher_info,
    &U_schan_get_connection_info,
    &U_schan_get_enabled_protocols,
    &U_schan_get_key_signature_algorithm,
    &U_schan_get_max_message_size,
    &U_schan_get_session_cipher_block_size,
    &U_schan_get_session_peer_certificate,
    &U_schan_get_unique_channel_binding,
    &U_schan_handshake,
    &U_schan_recv,
    &U_schan_send,
    &U_schan_set_application_protocols,
    &U_schan_dtls_unsupported,                   // set_dtls_mtu
    &U_schan_set_session_target,
    &U_schan_dtls_unsupported,                   // set_dtls_timeouts
};
const std::size_t kSchannelFuncCount = sizeof(kSchannelFuncs) / sizeof(kSchannelFuncs[0]);

// Loads what the host's SChannel fetches lazily (schannel.dll, ncrypt, bcrypt, crypt32 providers) by building one
// client credential and producing a ClientHello. Must run before the PEB take-over, like afd::PrimeHost; failures
// are ignored (a host without SChannel simply has no TLS).
void PrimeSchannelHost() {
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    CredHandle cred;
    TimeStamp ts;
    if (AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc,
                                  nullptr, nullptr, &cred, &ts) != SEC_E_OK)
        return;
    CtxtHandle ctx;
    SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc od{SECBUFFER_VERSION, 1, &out};
    ULONG attr = 0;
    wchar_t host[] = L"localhost";
    if (InitializeSecurityContextW(&cred, nullptr, host, kIscFlags, 0, 0, nullptr, 0, &ctx, &od, &attr, &ts) ==
        SEC_I_CONTINUE_NEEDED) {
        if (out.pvBuffer) FreeContextBuffer(out.pvBuffer);
        DeleteSecurityContext(&ctx);
    }
    FreeCredentialsHandle(&cred);
}

}  // namespace win10xbox::dispatch::detail

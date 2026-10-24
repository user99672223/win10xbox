// bcrypt's unix library: the symmetric ciphers (docs/slices/crypto-rsaaes.md).
//
// Wine's bcrypt does hashes, HMAC and the key derivations in PE code and sends the ciphers and the asymmetric keys to
// its unix side (gnutls). The layer has no gnutls: with no unix library BCryptGenerateSymmetricKey answers
// STATUS_NOT_IMPLEMENTED ("no encryption support") for every algorithm. This is the unix side for AES-128/192/256 in ECB,
// CBC and CFB8 (the modes Wine's gnutls.c maps), self-contained so that it behaves the same on every host. 3DES, RC4, GCM,
// CCM and the asymmetric keys stay STATUS_NOT_IMPLEMENTED.
//
// The struct key prefix below mirrors dlls/bcrypt/bcrypt_internal.h (wine-11.9); the PE side keeps the chaining vector in
// key->u.s.vector and calls the unix side one block at a time, so the unix side holds only the expanded key and the chain.
#include "internal.h"
#include <cstdint>
#include <cstring>
#include <new>

namespace win10xbox::dispatch {
namespace detail {
namespace {

struct BcryptKeySymmetric {
    int mode;                  // enum chain_mode: CBC, ECB, CFB, CCM, GCM
    ULONG block_size;
    UCHAR* vector;
    ULONG vector_len;
    UCHAR* secret;
    unsigned secret_len;
    // CRITICAL_SECTION follows in the PE layout; never touched here.
};
struct BcryptKey {
    ULONG magic;
    int alg_id;                // enum alg_id: 3DES, AES, RC4, ...
    std::uint64_t priv[3];     // private data for the backend
    BcryptKeySymmetric s;
};
constexpr int kAlgAes = 1;
enum { kModeCbc = 0, kModeEcb = 1, kModeCfb = 2 };

struct EncryptParams { BcryptKey* key; const UCHAR* input; unsigned input_len; UCHAR* output; ULONG output_len; };
struct SetAuthParams { BcryptKey* key; UCHAR* auth_data; ULONG len; };
struct GetTagParams { BcryptKey* key; UCHAR* tag; ULONG len; };

constexpr NTSTATUS kNotImplemented = static_cast<NTSTATUS>(0xC0000002);
constexpr NTSTATUS kNotSupported = static_cast<NTSTATUS>(0xC00000BB);
constexpr NTSTATUS kNoMemory = static_cast<NTSTATUS>(0xC0000017);
constexpr NTSTATUS kInvalidParameter = static_cast<NTSTATUS>(0xC000000D);

// ---- AES ----------------------------------------------------------------------------------------------------------
std::uint8_t g_sbox[256], g_inv[256];
SRWLOCK g_tables_lock = SRWLOCK_INIT;
bool g_tables_ready = false;

std::uint8_t Rotl8(std::uint8_t x, int n) { return static_cast<std::uint8_t>((x << n) | (x >> (8 - n))); }
std::uint8_t XTime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
std::uint8_t GMul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = XTime(a);
        b >>= 1;
    }
    return r;
}

void EnsureTables() {
    AcquireSRWLockExclusive(&g_tables_lock);
    if (!g_tables_ready) {
        std::uint8_t p = 1, q = 1;
        do {
            p = static_cast<std::uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q = static_cast<std::uint8_t>(q ^ (q << 1));
            q = static_cast<std::uint8_t>(q ^ (q << 2));
            q = static_cast<std::uint8_t>(q ^ (q << 4));
            if (q & 0x80) q ^= 0x09;
            std::uint8_t x = static_cast<std::uint8_t>(q ^ Rotl8(q, 1) ^ Rotl8(q, 2) ^ Rotl8(q, 3) ^ Rotl8(q, 4));
            g_sbox[p] = static_cast<std::uint8_t>(x ^ 0x63);
        } while (p != 1);
        g_sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) g_inv[g_sbox[i]] = static_cast<std::uint8_t>(i);
        g_tables_ready = true;
    }
    ReleaseSRWLockExclusive(&g_tables_lock);
}

struct AesState {
    std::uint8_t rk[240];
    int rounds;
    std::uint8_t chain[16];
};

void ExpandKey(AesState* a, const UCHAR* key, unsigned len) {
    const unsigned nk = len / 4;
    a->rounds = static_cast<int>(nk) + 6;
    std::memcpy(a->rk, key, len);
    std::uint8_t rcon = 1;
    for (unsigned i = nk; i < 4 * (a->rounds + 1); ++i) {
        std::uint8_t t[4];
        std::memcpy(t, a->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            std::uint8_t t0 = t[0];
            t[0] = static_cast<std::uint8_t>(g_sbox[t[1]] ^ rcon);
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[t0];
            rcon = XTime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; ++k) t[k] = g_sbox[t[k]];
        }
        for (int k = 0; k < 4; ++k) a->rk[4 * i + k] = static_cast<std::uint8_t>(a->rk[4 * (i - nk) + k] ^ t[k]);
    }
}

void AddRoundKey(std::uint8_t* s, const std::uint8_t* rk) { for (int i = 0; i < 16; ++i) s[i] ^= rk[i]; }

void EncryptBlock(const AesState* a, const std::uint8_t* in, std::uint8_t* out) {
    std::uint8_t s[16], t[16];
    std::memcpy(s, in, 16);
    AddRoundKey(s, a->rk);
    for (int r = 1; r <= a->rounds; ++r) {
        for (int i = 0; i < 16; ++i) s[i] = g_sbox[s[i]];
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) t[4 * c + row] = s[4 * ((c + row) % 4) + row];
        if (r != a->rounds) {
            for (int c = 0; c < 4; ++c) {
                const std::uint8_t* x = t + 4 * c;
                s[4 * c + 0] = static_cast<std::uint8_t>(XTime(x[0]) ^ (XTime(x[1]) ^ x[1]) ^ x[2] ^ x[3]);
                s[4 * c + 1] = static_cast<std::uint8_t>(x[0] ^ XTime(x[1]) ^ (XTime(x[2]) ^ x[2]) ^ x[3]);
                s[4 * c + 2] = static_cast<std::uint8_t>(x[0] ^ x[1] ^ XTime(x[2]) ^ (XTime(x[3]) ^ x[3]));
                s[4 * c + 3] = static_cast<std::uint8_t>((XTime(x[0]) ^ x[0]) ^ x[1] ^ x[2] ^ XTime(x[3]));
            }
        } else {
            std::memcpy(s, t, 16);
        }
        AddRoundKey(s, a->rk + 16 * r);
    }
    std::memcpy(out, s, 16);
}

void DecryptBlock(const AesState* a, const std::uint8_t* in, std::uint8_t* out) {
    std::uint8_t s[16], t[16];
    std::memcpy(s, in, 16);
    AddRoundKey(s, a->rk + 16 * a->rounds);
    for (int r = a->rounds - 1; r >= 0; --r) {
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) t[4 * ((c + row) % 4) + row] = s[4 * c + row];
        for (int i = 0; i < 16; ++i) s[i] = g_inv[t[i]];
        AddRoundKey(s, a->rk + 16 * r);
        if (r != 0) {
            for (int c = 0; c < 4; ++c) {
                std::uint8_t x[4];
                std::memcpy(x, s + 4 * c, 4);
                s[4 * c + 0] = static_cast<std::uint8_t>(GMul(x[0], 14) ^ GMul(x[1], 11) ^ GMul(x[2], 13) ^ GMul(x[3], 9));
                s[4 * c + 1] = static_cast<std::uint8_t>(GMul(x[0], 9) ^ GMul(x[1], 14) ^ GMul(x[2], 11) ^ GMul(x[3], 13));
                s[4 * c + 2] = static_cast<std::uint8_t>(GMul(x[0], 13) ^ GMul(x[1], 9) ^ GMul(x[2], 14) ^ GMul(x[3], 11));
                s[4 * c + 3] = static_cast<std::uint8_t>(GMul(x[0], 11) ^ GMul(x[1], 13) ^ GMul(x[2], 9) ^ GMul(x[3], 14));
            }
        }
    }
    std::memcpy(out, s, 16);
}

AesState*& StateOf(BcryptKey* k) { return reinterpret_cast<AesState*&>(k->priv[0]); }

// The expanded key and the chain, built at the first use after a reset (the chain starts at the key's vector).
NTSTATUS Init(BcryptKey* k, AesState** out) {
    if (k->alg_id != kAlgAes) return kNotSupported;
    if (k->s.mode != kModeCbc && k->s.mode != kModeEcb && k->s.mode != kModeCfb) return kNotSupported;
    if (k->s.secret_len != 16 && k->s.secret_len != 24 && k->s.secret_len != 32) return kNotSupported;
    AesState*& st = StateOf(k);
    if (!st) {
        EnsureTables();
        st = new (std::nothrow) AesState;
        if (!st) return kNoMemory;
        std::memset(st, 0, sizeof *st);
        ExpandKey(st, k->s.secret, k->s.secret_len);
        if (k->s.vector && k->s.vector_len >= 16) std::memcpy(st->chain, k->s.vector, 16);
    }
    *out = st;
    return 0;
}

NTSTATUS Crypt(BcryptKey* k, const UCHAR* in, unsigned len, UCHAR* out, ULONG out_len, bool encrypt) {
    if (!k || !in || !out || out_len < len) return kInvalidParameter;
    AesState* a;
    if (NTSTATUS s = Init(k, &a)) return s;
    if (k->s.mode == kModeCfb) {
        for (unsigned i = 0; i < len; ++i) {
            std::uint8_t ks[16];
            EncryptBlock(a, a->chain, ks);
            std::uint8_t c = encrypt ? static_cast<std::uint8_t>(in[i] ^ ks[0]) : in[i];
            std::uint8_t p = static_cast<std::uint8_t>(in[i] ^ ks[0]);
            std::memmove(a->chain, a->chain + 1, 15);
            a->chain[15] = c;
            out[i] = p;   // encrypt: in ^ ks = ciphertext; decrypt: in ^ ks = plaintext
        }
        return 0;
    }
    if (len % 16) return kInvalidParameter;
    for (unsigned off = 0; off < len; off += 16) {
        std::uint8_t blk[16];
        if (k->s.mode == kModeEcb) {
            if (encrypt) EncryptBlock(a, in + off, out + off); else DecryptBlock(a, in + off, out + off);
        } else if (encrypt) {
            for (int i = 0; i < 16; ++i) blk[i] = static_cast<std::uint8_t>(in[off + i] ^ a->chain[i]);
            EncryptBlock(a, blk, out + off);
            std::memcpy(a->chain, out + off, 16);
        } else {
            std::uint8_t cipher[16];
            std::memcpy(cipher, in + off, 16);   // in and out may alias
            DecryptBlock(a, cipher, blk);
            for (int i = 0; i < 16; ++i) out[off + i] = static_cast<std::uint8_t>(blk[i] ^ a->chain[i]);
            std::memcpy(a->chain, cipher, 16);
        }
    }
    return 0;
}

void Drop(BcryptKey* k) {
    AesState*& st = StateOf(k);
    if (st) { std::memset(st, 0, sizeof *st); delete st; st = nullptr; }
}

NTSTATUS WINAPI U_bcrypt_ok(void*) { return 0; }
NTSTATUS WINAPI U_bcrypt_not_implemented(void*) { return kNotImplemented; }
NTSTATUS WINAPI U_bcrypt_vector_reset(void* args) { if (args) Drop(static_cast<BcryptKey*>(args)); return 0; }
NTSTATUS WINAPI U_bcrypt_set_auth_data(void*) { return kNotSupported; }   // GCM / CCM only
NTSTATUS WINAPI U_bcrypt_get_tag(void*) { return kNotSupported; }
NTSTATUS WINAPI U_bcrypt_encrypt(void* args) {
    auto* p = static_cast<EncryptParams*>(args);
    return Crypt(p->key, p->input, p->input_len, p->output, p->output_len, true);
}
NTSTATUS WINAPI U_bcrypt_decrypt(void* args) {
    auto* p = static_cast<EncryptParams*>(args);
    return Crypt(p->key, p->input, p->input_len, p->output, p->output_len, false);
}
NTSTATUS WINAPI U_bcrypt_destroy(void* args) { if (args) Drop(static_cast<BcryptKey*>(args)); return 0; }

}  // namespace

// enum key_funcs of dlls/bcrypt/bcrypt_internal.h.
extern const UnixCallFn kBcryptFuncs[] = {
    &U_bcrypt_ok,               // process_attach
    &U_bcrypt_ok,               // process_detach
    &U_bcrypt_vector_reset,     // key_symmetric_vector_reset
    &U_bcrypt_set_auth_data,    // key_symmetric_set_auth_data
    &U_bcrypt_encrypt,          // key_symmetric_encrypt
    &U_bcrypt_decrypt,          // key_symmetric_decrypt
    &U_bcrypt_get_tag,          // key_symmetric_get_tag
    &U_bcrypt_destroy,          // key_symmetric_destroy
    &U_bcrypt_not_implemented,  // key_asymmetric_generate
    &U_bcrypt_not_implemented,  // key_asymmetric_decrypt
    &U_bcrypt_not_implemented,  // key_asymmetric_encrypt
    &U_bcrypt_not_implemented,  // key_asymmetric_duplicate
    &U_bcrypt_not_implemented,  // key_asymmetric_sign
    &U_bcrypt_not_implemented,  // key_asymmetric_verify
    &U_bcrypt_ok,               // key_asymmetric_destroy
    &U_bcrypt_not_implemented,  // key_asymmetric_export
    &U_bcrypt_not_implemented,  // key_asymmetric_import
    &U_bcrypt_not_implemented,  // key_asymmetric_derive_key
};
extern const std::size_t kBcryptFuncCount = sizeof kBcryptFuncs / sizeof kBcryptFuncs[0];

}  // namespace detail
}  // namespace win10xbox::dispatch

/* CryptoAPI and CNG through the layer (docs/slices/crypto-rsaaes.md). Every step prints its result and GetLastError();
 * the same program run natively on Windows is the reference. Exit 42 = every step passed, 1 = a step failed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

static void step(const char* name, BOOL ok, DWORD err) {
    printf("cryptapi: %s %s lasterror=0x%08lx\n", name, ok ? "ok" : "FAILED", (unsigned long)err);
    if (!ok) g_fail++;
    fflush(stdout);
}
#define STEP(name, expr) do { SetLastError(0); BOOL ok_ = (expr) ? TRUE : FALSE; DWORD e_ = GetLastError(); step(name, ok_, e_); } while (0)

/* SHA-256("abc") */
static const BYTE kSha256Abc[32] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

static const BYTE kKey[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                              16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
/* FIPS-197 C.3: AES-256, key 00..1f, plaintext 00112233445566778899aabbccddeeff */
static const BYTE kFipsPlain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
static const BYTE kFipsCipher[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
static const char kPlain[] = "The quick brown fox jumps over the lazy dog";

static void acquire(const char* name, LPCWSTR prov, DWORD type) {
    HCRYPTPROV h = 0;
    SetLastError(0);
    BOOL ok = CryptAcquireContextW(&h, NULL, prov, type, CRYPT_VERIFYCONTEXT);
    step(name, ok, GetLastError());
    if (ok) CryptReleaseContext(h, 0);
}

static void capi(void) {
    HCRYPTPROV prov = 0;
    acquire("acquire PROV_RSA_AES verifycontext", MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES);
    acquire("acquire PROV_RSA_FULL verifycontext", NULL, PROV_RSA_FULL);

    /* A named container: create, reopen, delete. Remove a leftover of an earlier run first. */
    LPCWSTR cont = L"win10xbox_cryptapi";
    CryptAcquireContextW(&prov, cont, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET);
    prov = 0;
    STEP("container create", CryptAcquireContextW(&prov, cont, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET));
    if (prov) CryptReleaseContext(prov, 0);
    prov = 0;
    STEP("container reopen", CryptAcquireContextW(&prov, cont, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, 0));
    if (prov) CryptReleaseContext(prov, 0);
    prov = 0;
    STEP("container delete", CryptAcquireContextW(&prov, cont, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET));

    prov = 0;
    if (!CryptAcquireContextW(&prov, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        step("provider for the rest of capi", FALSE, GetLastError());
        return;
    }

    BYTE rnd[32], zero[32];
    memset(rnd, 0, sizeof rnd);
    memset(zero, 0, sizeof zero);
    STEP("CryptGenRandom", CryptGenRandom(prov, sizeof rnd, rnd) && memcmp(rnd, zero, sizeof rnd) != 0);

    HCRYPTHASH hash = 0;
    BYTE dig[32];
    DWORD dl = sizeof dig;
    STEP("CryptCreateHash SHA_256", CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash));
    if (hash) {
        STEP("CryptHashData", CryptHashData(hash, (const BYTE*)"abc", 3, 0));
        memset(dig, 0, sizeof dig);
        STEP("CryptGetHashParam", CryptGetHashParam(hash, HP_HASHVAL, dig, &dl, 0) && dl == 32 && memcmp(dig, kSha256Abc, 32) == 0);
        CryptDestroyHash(hash);
    }

    struct { BLOBHEADER h; DWORD n; BYTE k[32]; } blob;
    blob.h.bType = PLAINTEXTKEYBLOB;
    blob.h.bVersion = CUR_BLOB_VERSION;
    blob.h.reserved = 0;
    blob.h.aiKeyAlg = CALG_AES_256;
    blob.n = 32;
    memcpy(blob.k, kKey, 32);
    HCRYPTKEY key = 0;
    STEP("CryptImportKey AES-256", CryptImportKey(prov, (BYTE*)&blob, sizeof blob, 0, 0, &key));
    if (key) {
        BYTE buf[64];
        DWORD len = 16;
        memcpy(buf, kFipsPlain, 16);   /* one block, not final: no padding, zero IV = the raw block cipher */
        STEP("CryptEncrypt AES-256 FIPS-197 vector", CryptEncrypt(key, 0, FALSE, 0, buf, &len, sizeof buf) && len == 16 && memcmp(buf, kFipsCipher, 16) == 0);
        CryptDestroyKey(key);
        key = 0;
        STEP("CryptImportKey AES-256 again", CryptImportKey(prov, (BYTE*)&blob, sizeof blob, 0, 0, &key));
        len = (DWORD)sizeof kPlain;
        memset(buf, 0, sizeof buf);
        memcpy(buf, kPlain, len);
        STEP("CryptEncrypt AES-256", CryptEncrypt(key, 0, TRUE, 0, buf, &len, sizeof buf) && len == 48 && memcmp(buf, kPlain, 16) != 0);
        STEP("CryptDecrypt AES-256", CryptDecrypt(key, 0, TRUE, 0, buf, &len) && len == sizeof kPlain && memcmp(buf, kPlain, len) == 0);
        CryptDestroyKey(key);
    }
    CryptReleaseContext(prov, 0);
}

static void cng(void) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BYTE dig[32];
    NTSTATUS s;

    s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    step("BCryptOpenAlgorithmProvider SHA256", s >= 0, (DWORD)s);
    if (s >= 0) {
        s = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
        step("BCryptCreateHash", s >= 0, (DWORD)s);
        if (s >= 0) {
            s = BCryptHashData(hash, (PUCHAR)"abc", 3, 0);
            step("BCryptHashData", s >= 0, (DWORD)s);
            memset(dig, 0, sizeof dig);
            s = BCryptFinishHash(hash, dig, sizeof dig, 0);
            step("BCryptFinishHash", s >= 0 && memcmp(dig, kSha256Abc, 32) == 0, (DWORD)s);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }

    alg = NULL;
    s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    step("BCryptOpenAlgorithmProvider AES", s >= 0, (DWORD)s);
    if (s < 0) return;
    s = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    step("BCryptSetProperty CBC", s >= 0, (DWORD)s);
    BCRYPT_KEY_HANDLE key = NULL;
    s = BCryptGenerateSymmetricKey(alg, &key, NULL, 0, (PUCHAR)kKey, 32, 0);
    step("BCryptGenerateSymmetricKey", s >= 0, (DWORD)s);
    if (s >= 0) {
        BYTE iv[16], iv2[16], ct[64], pt[64];
        BCRYPT_ALG_HANDLE ecb = NULL;
        BCRYPT_KEY_HANDLE ekey = NULL;
        ULONG k = 0;
        s = BCryptOpenAlgorithmProvider(&ecb, BCRYPT_AES_ALGORITHM, NULL, 0);
        if (s >= 0) s = BCryptSetProperty(ecb, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        if (s >= 0) s = BCryptGenerateSymmetricKey(ecb, &ekey, NULL, 0, (PUCHAR)kKey, 32, 0);
        if (s >= 0) s = BCryptEncrypt(ekey, (PUCHAR)kFipsPlain, 16, NULL, NULL, 0, ct, sizeof ct, &k, 0);
        step("BCryptEncrypt AES-256 ECB FIPS-197 vector", s >= 0 && k == 16 && memcmp(ct, kFipsCipher, 16) == 0, (DWORD)s);
        if (ekey) BCryptDestroyKey(ekey);
        if (ecb) BCryptCloseAlgorithmProvider(ecb, 0);
        ULONG n = 0, m = 0;
        memset(iv, 7, 16);
        memcpy(iv2, iv, 16);
        s = BCryptEncrypt(key, (PUCHAR)kPlain, sizeof kPlain, NULL, iv, 16, ct, sizeof ct, &n, BCRYPT_BLOCK_PADDING);
        step("BCryptEncrypt", s >= 0 && n == 48, (DWORD)s);
        s = BCryptDecrypt(key, ct, n, NULL, iv2, 16, pt, sizeof pt, &m, BCRYPT_BLOCK_PADDING);
        step("BCryptDecrypt", s >= 0 && m == sizeof kPlain && memcmp(pt, kPlain, m) == 0, (DWORD)s);
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
}

int main(void) {
    capi();
    cng();
    printf("cryptapi: done failures=%d\n", g_fail);
    fflush(stdout);
    return g_fail ? 1 : 42;
}

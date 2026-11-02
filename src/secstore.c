/* SPDX-License-Identifier: AGPL-3.0-only */
/* The OS secret store: see secstore.h. */
#include "secstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tests set LUME_SECSTORE_SERVICE so they never read or write the user's real entries */
static const char *svc(const char *service) { const char *e = getenv("LUME_SECSTORE_SERVICE"); return e && *e ? e : service; }
static void wipe(void *p, size_t n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }
void secstore_free(char *s) { if (!s) return; wipe(s, strlen(s)); free(s); }
bool secstore_has(const char *service, const char *account) { char *s = secstore_get(service, account); bool ok = s && *s; secstore_free(s); return ok; }

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

const char *secstore_name(void) { return "macOS Keychain"; }
bool secstore_available(char *why, int n) { (void)why; (void)n; return true; }
static CFMutableDictionaryRef query(const char *service, const char *account) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef s = CFStringCreateWithCString(NULL, service, kCFStringEncodingUTF8), a = CFStringCreateWithCString(NULL, account, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, s);
    CFDictionarySetValue(q, kSecAttrAccount, a);
    CFRelease(s); CFRelease(a);
    return q;
}
static void os_err(OSStatus st, const char *what, char *why, int n) {
    CFStringRef m = SecCopyErrorMessageString(st, NULL);
    char b[256] = ""; if (m) { CFStringGetCString(m, b, sizeof b, kCFStringEncodingUTF8); CFRelease(m); }
    snprintf(why, (size_t)n, "%s: %s (%d)", what, b[0] ? b : "Keychain error", (int)st);
}
bool secstore_set(const char *service, const char *account, const char *secret, char *why, int n) {
    service = svc(service);
    CFMutableDictionaryRef q = query(service, account);
    CFDataRef d = CFDataCreate(NULL, (const UInt8 *)secret, (CFIndex)strlen(secret));
    /* replace: update when it exists, add otherwise */
    CFMutableDictionaryRef upd = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(upd, kSecValueData, d);
    OSStatus st = SecItemUpdate(q, upd);
    if (st == errSecItemNotFound) {
        CFDictionarySetValue(q, kSecValueData, d);
        CFStringRef label = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s (%s)"), service, account);
        CFDictionarySetValue(q, kSecAttrLabel, label); CFRelease(label);
        CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked);   /* only while the user's session is unlocked */
        st = SecItemAdd(q, NULL);
    }
    CFRelease(upd); CFRelease(d); CFRelease(q);
    if (st != errSecSuccess) { os_err(st, "The key could not be saved in the Keychain", why, n); return false; }
    return true;
}
char *secstore_get(const char *service, const char *account) {
    service = svc(service);
    CFMutableDictionaryRef q = query(service, account);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef out = NULL;
    OSStatus st = SecItemCopyMatching(q, &out);
    CFRelease(q);
    if (st != errSecSuccess || !out) return NULL;
    CFDataRef d = out; CFIndex L = CFDataGetLength(d);
    char *s = malloc((size_t)L + 1);
    if (s) { memcpy(s, CFDataGetBytePtr(d), (size_t)L); s[L] = 0; }
    CFRelease(out);
    return s;
}
bool secstore_delete(const char *service, const char *account) {
    service = svc(service);
    CFMutableDictionaryRef q = query(service, account);
    OSStatus st = SecItemDelete(q);
    CFRelease(q);
    return st == errSecSuccess || st == errSecItemNotFound;
}

#elif defined(_WIN32)
#include <windows.h>
#include <wincred.h>

const char *secstore_name(void) { return "Windows Credential Manager"; }
bool secstore_available(char *why, int n) { (void)why; (void)n; return true; }
static void target(const char *service, const char *account, wchar_t *out, int n) {
    char t[512]; snprintf(t, sizeof t, "%s/%s", service, account);
    MultiByteToWideChar(CP_UTF8, 0, t, -1, out, n);
}
bool secstore_set(const char *service, const char *account, const char *secret, char *why, int n) {
    service = svc(service);
    wchar_t tg[512]; target(service, account, tg, 512);
    CREDENTIALW c; memset(&c, 0, sizeof c);
    c.Type = CRED_TYPE_GENERIC; c.TargetName = tg;
    c.CredentialBlobSize = (DWORD)strlen(secret); c.CredentialBlob = (LPBYTE)secret;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;   /* this user, this machine (not roamed) */
    wchar_t user[128]; MultiByteToWideChar(CP_UTF8, 0, account, -1, user, 128); c.UserName = user;
    if (!CredWriteW(&c, 0)) { snprintf(why, (size_t)n, "The key could not be saved in the Credential Manager (error %lu)", (unsigned long)GetLastError()); return false; }
    return true;
}
char *secstore_get(const char *service, const char *account) {
    service = svc(service);
    wchar_t tg[512]; target(service, account, tg, 512);
    PCREDENTIALW c = NULL;
    if (!CredReadW(tg, CRED_TYPE_GENERIC, 0, &c)) return NULL;
    char *s = malloc(c->CredentialBlobSize + 1);
    if (s) { memcpy(s, c->CredentialBlob, c->CredentialBlobSize); s[c->CredentialBlobSize] = 0; }
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return s;
}
bool secstore_delete(const char *service, const char *account) {
    service = svc(service);
    wchar_t tg[512]; target(service, account, tg, 512);
    return CredDeleteW(tg, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
}

#else   /* Linux and other Unix: libsecret (Secret Service), loaded at run time */
#include <dlfcn.h>

/* the parts of libsecret's SecretSchema ABI Lume uses (stable since libsecret 0.7) */
typedef struct { const char *name; int type; } LsAttr;
typedef struct { const char *name; int flags; LsAttr attributes[32]; int reserved; void *r1, *r2, *r3, *r4, *r5, *r6, *r7; } LsSchema;
typedef int (*store_fn)(const LsSchema *, const char *collection, const char *label, const char *password, void *cancel, void **error, ...);
typedef char *(*lookup_fn)(const LsSchema *, void *cancel, void **error, ...);
typedef int (*clear_fn)(const LsSchema *, void *cancel, void **error, ...);
typedef void (*pwfree_fn)(char *);
static void *lib; static store_fn p_store; static lookup_fn p_lookup; static clear_fn p_clear; static pwfree_fn p_free; static bool tried;
static const LsSchema SCHEMA = { "app.lume.data.Secret", 0, { { "service", 0 }, { "account", 0 }, { NULL, 0 } }, 0, 0, 0, 0, 0, 0, 0, 0 };
static bool load(void) {
    if (tried) return lib != NULL;
    tried = true;
    lib = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
    p_store = (store_fn)dlsym(lib, "secret_password_store_sync");
    p_lookup = (lookup_fn)dlsym(lib, "secret_password_lookup_sync");
    p_clear = (clear_fn)dlsym(lib, "secret_password_clear_sync");
    p_free = (pwfree_fn)dlsym(lib, "secret_password_free");
    if (!p_store || !p_lookup || !p_clear || !p_free) { dlclose(lib); lib = NULL; return false; }
    return true;
}
const char *secstore_name(void) { return "Secret Service (GNOME Keyring / KWallet)"; }
bool secstore_available(char *why, int n) {
    if (load()) return true;
    snprintf(why, (size_t)n, "No secret store on this system (libsecret / GNOME Keyring or KWallet not found): Lume does not keep keys in files. Install gnome-keyring (or use the ANTHROPIC_API_KEY environment variable).");
    return false;
}
bool secstore_set(const char *service, const char *account, const char *secret, char *why, int n) {
    service = svc(service);
    if (!secstore_available(why, n)) return false;
    char label[256]; snprintf(label, sizeof label, "%s (%s)", service, account);
    if (!p_store(&SCHEMA, NULL, label, secret, NULL, NULL, "service", service, "account", account, NULL)) { snprintf(why, (size_t)n, "The key could not be saved in the Secret Service (is the keyring unlocked?)"); return false; }
    return true;
}
char *secstore_get(const char *service, const char *account) {
    service = svc(service);
    if (!load()) return NULL;
    char *s = p_lookup(&SCHEMA, NULL, NULL, "service", service, "account", account, NULL);
    if (!s) return NULL;
    char *r = strdup(s); p_free(s);   /* secret_password_free wipes its copy */
    return r;
}
bool secstore_delete(const char *service, const char *account) {
    service = svc(service);
    if (!load()) return true;
    p_clear(&SCHEMA, NULL, NULL, "service", service, "account", account, NULL);
    return true;
}
#endif

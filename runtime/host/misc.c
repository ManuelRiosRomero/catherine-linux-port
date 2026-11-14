// Registry (one key), COM basics, small ADVAPI32/RPCRT4/COMCTL32 bits.
#define _GNU_SOURCE
#include "host.h"
#include <string.h>
#include <strings.h>
#include <sys/random.h>

#define ADV "ADVAPI32.dll"
#define OLE "ole32.dll"

// ---------- registry: only HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0 exists ----------

#define HKEY_CPU 0xC0DE0001u
static void cpu_info(char *name, size_t cap, uint32_t *mhz) {
    char line[256];
    FILE *f = fopen("/proc/cpuinfo", "r");
    snprintf(name, cap, "x86 CPU");
    *mhz = 2800;
    while (f && fgets(line, sizeof line, f)) {
        char *v = strchr(line, ':');
        if (!v) continue;
        v += 2;
        v[strcspn(v, "\n")] = 0;
        if (!strncmp(line, "model name", 10)) snprintf(name, cap, "%s", v);
        else if (!strncmp(line, "cpu MHz", 7)) { *mhz = (uint32_t)atof(v); break; }
    }
    if (f) fclose(f);
}
IMP(ADV, RegOpenKeyExA) {   // (hkey, subkey, options, sam, result*)
    const char *k = ARG(c, 1) ? GSTR(ARG(c, 1)) : "";
    if (ARG(c, 0) == 0x80000002u && !strcasecmp(k, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0")) {
        U32(ARG(c, 4)) = HKEY_CPU;
        ret_std(c, 0, 5);
        return;
    }
    ret_std(c, 2, 5);   // ERROR_FILE_NOT_FOUND
}
IMP(ADV, RegOpenKeyA) { ret_std(c, 2, 3); }   // (hkey, subkey, result*): d3dx9's debug settings key: absent
IMP(ADV, RegQueryValueExA) {   // (hkey, name, reserved, type*, data, size*)
    char name[128];
    uint32_t mhz;
    cpu_info(name, sizeof name, &mhz);
    const char *v = ARG(c, 1) ? GSTR(ARG(c, 1)) : "";
    uint32_t type, len, data = ARG(c, 4), sz = ARG(c, 5);
    uint8_t buf[256];
    if (ARG(c, 0) != HKEY_CPU) { ret_std(c, 6, 6); return; }   // ERROR_INVALID_HANDLE
    if (!strcasecmp(v, "~MHz")) { type = 4; len = 4; memcpy(buf, &mhz, 4); }   // REG_DWORD
    else if (!strcasecmp(v, "ProcessorNameString")) { type = 1; len = strlen(name) + 1; memcpy(buf, name, len); }   // REG_SZ
    else { ret_std(c, 2, 6); return; }
    if (ARG(c, 3)) U32(ARG(c, 3)) = type;
    uint32_t have = sz ? U32(sz) : 0;
    if (sz) U32(sz) = len;
    if (data && have < len) { ret_std(c, 234, 6); return; }    // ERROR_MORE_DATA
    if (data) memcpy(GM(data), buf, len);
    ret_std(c, 0, 6);
}
IMPX(ADV, "RegCloseKey", RegCloseKey_) { ret_std(c, 0, 1); }

// ---------- crypto: random only for now (the rest when the game reaches it) ----------

IMP(ADV, CryptAcquireContextA) { U32(ARG(c, 0)) = 0xC0DE0002u; ret_std(c, 1, 5); }
IMP(ADV, CryptReleaseContext) { ret_std(c, 1, 2); }
IMP(ADV, CryptGenRandom) { getrandom(GM(ARG(c, 2)), ARG(c, 1), 0); ret_std(c, 1, 3); }

// ---------- COM ----------

IMP(OLE, CoInitialize) { ret_std(c, 0, 1); }
IMP(OLE, CoInitializeEx) { ret_std(c, 0, 2); }
IMP(OLE, CoUninitialize) { ret_std(c, 0, 0); }
IMP(OLE, CoTaskMemFree) { gfree(ARG(c, 0)); ret_std(c, 0, 1); }
IMP(OLE, PropVariantClear) { memset(GM(ARG(c, 0)), 0, 16); ret_std(c, 0, 1); }
IMP(OLE, CoSetProxyBlanket) { ret_std(c, 0, 8); }
// ponytail: no COM classes yet (XAudio2, Media Foundation are replaced natively); log the CLSID so we see who asks
IMP(OLE, CoCreateInstance) {   // (clsid, outer, ctx, iid, out*)
    const uint8_t *g = GM(ARG(c, 0));
    fprintf(stderr, "[com] CoCreateInstance {%02x%02x%02x%02x-...}: class not registered\n", g[3], g[2], g[1], g[0]);
    U32(ARG(c, 4)) = 0;
    ret_std(c, 0x80040154u, 5);   // REGDB_E_CLASSNOTREG
}
// Gamebryo's D3D10 renderer is linked in but never created (the game makes a D3D9 device)
IMP("d3dx10_43.dll", D3DX10SaveTextureToFileA) { die("d3dx10: the D3D10 renderer is not supported (the game uses D3D9)"); }
IMP("d3dx10_43.dll", D3DX10LoadTextureFromTexture) { die("d3dx10: the D3D10 renderer is not supported (the game uses D3D9)"); }

// BSTR: length in bytes at p-4, then the UTF-16 text and a NUL; the pointer handed out is p
IMPX("OLEAUT32.dll", "#2", SysAllocString) {
    uint32_t s = ARG(c, 0), n = 0;
    if (!s) { ret_std(c, 0, 1); return; }
    while (V(GM(s + 2 * n), uint16_t)) n++;
    uint32_t p = gmalloc(4 + 2 * n + 2);
    U32(p) = 2 * n;
    memcpy(GM(p + 4), GM(s), 2 * n + 2);
    ret_std(c, p + 4, 1);
}
IMPX("OLEAUT32.dll", "#6", SysFreeString) { if (ARG(c, 0)) gfree(ARG(c, 0) - 4); ret_std(c, 0, 1); }
// the only certificate code is statically linked TLS, unreachable while the network is down (net.c): nothing to free
IMP("CRYPT32.dll", CertFreeCertificateContext) { ret_std(c, 1, 1); }
IMP("RPCRT4.dll", UuidCreate) { getrandom(GM(ARG(c, 0)), 16, 0); ret_std(c, 0, 1); }
IMP("COMCTL32.dll", InitCommonControlsEx) { ret_std(c, 1, 1); }

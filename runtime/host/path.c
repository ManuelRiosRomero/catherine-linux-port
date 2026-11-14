// Windows path -> host path. The working directory is the game folder (the host is started from it).
//   relative, "C:\Game\..." -> game folder (C:\Game is what GetModuleFileName reports)
//   any other absolute path   -> ~/.local/share/catherine-port/<drive letter>/<rest>   (saves, settings)
// Every component is matched case-insensitively: Windows ignores case, ext4 on the Steam Deck does not.
#define _GNU_SOURCE
#include "host.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static void ensure_dir(const char *d) {      // mkdir -p
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", d);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

void host_path(const char *win, char *out, size_t cap) {
    char rel[1024];
    const char *p = win;
    out[0] = 0;
    if (p[0] && p[1] == ':') {
        if (!strncasecmp(p + 2, "\\Game", 5) && (p[7] == '\\' || p[7] == '/' || !p[7])) p += 7 + (p[7] != 0);
        else {
            const char *home = getenv("HOME");
            snprintf(out, cap, "%s/.local/share/catherine-port/%c", home ? home : ".", (char)(p[0] | 0x20));
            ensure_dir(out);
            p += 2;
        }
    }
    snprintf(rel, sizeof rel, "%s", p);
    for (char *q = rel; *q; q++) if (*q == '\\') *q = '/';

    // walk components, fixing case where the exact name doesn't exist
    size_t n = strlen(out);
    char *save = NULL;   // strtok_r: the game resolves paths from several threads at once
    for (char *tok = strtok_r(rel, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", n ? out : ".");
        char cand[1024];
        snprintf(cand, sizeof cand, "%s%s%s", out, n ? "/" : "", tok);
        struct stat st;
        if (stat(cand, &st)) {
            DIR *d = opendir(dir);
            for (struct dirent *e; d && (e = readdir(d));)
                if (!strcasecmp(e->d_name, tok)) { snprintf(cand, sizeof cand, "%s%s%s", out, n ? "/" : "", e->d_name); break; }
            if (d) closedir(d);
        }
        snprintf(out, cap, "%s", cand);
        n = strlen(out);
    }
    if (!n) snprintf(out, cap, ".");
    // Steam Cloud: started from Steam, catherine-launch.sh points CATH_CLOUD_SAVE at the Proton prefix's SaveGame.bin
    // (the file Steam syncs with the PC). Only that one file goes there; settings stay the port's own. Test runs fake
    // Gold prizes (CATH_SKIP), so they must never write the real save.
    const char *cloud = getenv("CATH_CLOUD_SAVE"), *test = getenv("CATH_TEST_SAVE");
    size_t on = strlen(out), sn = strlen("/TheEccentricApe/Catherine/SaveGame.bin");
    // CATH_TEST_SAVE=<file>: a throwaway copy of the port's save, so a test may save into a slot without touching it
    if (test && *test && on > sn && !strcasecmp(out + on - sn, "/TheEccentricApe/Catherine/SaveGame.bin")
        && !strstr(test, "/Steam/")) {
        snprintf(out, cap, "%s", test);
        return;
    }
    if (cloud && *cloud && on > sn && !strcasecmp(out + on - sn, "/TheEccentricApe/Catherine/SaveGame.bin")) {
        if (getenv("CATH_SKIP")) die("CATH_SKIP with CATH_CLOUD_SAVE: test runs must not touch the real save");
        snprintf(out, cap, "%s", cloud);
        return;
    }
    // The player's real saves live in Steam's folders (the Proton prefix in compatdata, Steam Cloud in userdata). Apart from
    // the one file above, the port keeps its own under ~/.local/share/catherine-port and must never read or write those.
    if (strstr(out, "/Steam/userdata/") || strstr(out, "/compatdata/")) die("refusing path in Steam's save folders: %s", out);
}

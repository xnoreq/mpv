/*
 * Get path to config dir/file.
 *
 * Return Values:
 *   Returns the pointer to the ALLOCATED buffer containing the
 *   zero terminated path string. This buffer has to be FREED
 *   by the caller.
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "config.h"
#include "mpvcore/mp_msg.h"
#include "mpvcore/path.h"
#include "talloc.h"
#include "osdep/io.h"

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <windows.h>
#include <shlobj.h>
#endif

#ifdef CONFIG_MACOSX_BUNDLE
#include "osdep/macosx_bundle.h"
#endif

#define SUPPORT_OLD_CONFIG 1
#define ALWAYS_LOCAL_APPDATA 1

typedef char *(*lookup_fun)(const char *[]);
static const lookup_fun config_lookup_functions[] = {
    mp_find_user_config_file_array,
#ifdef CONFIG_MACOSX_BUNDLE
    get_bundled_path,
#endif
    mp_find_global_config_file_array,
    NULL
};

char *mp_find_config_file_array(const char *path[])
{
    for (int i = 0; config_lookup_functions[i] != NULL; i++) {
        char *p = config_lookup_functions[i](path);
        if (!p) continue;

        if (mp_path_exists(p))
            return p;

        talloc_free(p);
    }
    return NULL;
}

typedef enum {Config = 0, Cache, Runtime, config_type_count} config_type;

static inline char *mpv_home(const void *talloc_ctx, const config_type type) {
    char *mpvhome = getenv("MPV_HOME");
    if (mpvhome)
        switch (type) {
        case Config:
            return talloc_strdup(talloc_ctx, mpvhome);
            break;
        case Cache:
            return mp_path_join(talloc_ctx, bstr0(mpvhome), bstr0("cache"));
            break;
        case Runtime:
            return NULL;
            break;
        }

    return NULL;
}

static inline bool is_absolute(const struct bstr p) {
    if (p.len < 1)
        return false;

#if HAVE_DOS_PATHS
    // NOTE: X: without a / or \ is a *relative* path
    if (p.len > 2 && p.start[1] == ':')
        return p.start[2] == '\\' || p.start[2] == '/';
    return p.start[0] == '\\' || p.start[0] == '/';
#else
    return p.start[0] == '/';
#endif
}

#if !defined(_WIN32) || defined(__CYGWIN__)
static inline struct bstr find_config_dir(const void *talloc_ctx, const config_type type) {
    char *confdir = mpv_home(talloc_ctx, type);
    if (confdir)
        return bstr0(confdir);

    char *homedir = getenv("HOME");

    const char *xdg_env =
        type == Config  ? "XDG_CONFIG_HOME" :
        type == Cache   ? "XDG_CACHE_HOME"  :
        type == Runtime ? "XDG_RUNTIME_DIR" : NULL;

    /* first, we discover the new config dir's path */
    char *tmp = talloc_new(NULL);
    struct bstr ret = bstr0(NULL);

    /* spec requires that the paths on XDG_* envvars are absolute or ignored */
    if ((confdir = getenv(xdg_env)) != NULL && is_absolute(bstr0(confdir))) {
        if (type == Runtime)
            mkdir(confdir, 0700);
        else
            mkdir(confdir, 0777);

        confdir = mp_path_mkdirs(tmp, bstr0(confdir), bstr0("mpv"), bstr0(""));
    } else if (type == Runtime) {
        uid_t uid = getuid();

        const char *run = "/tmp";
        if (mp_path_isdir("/run"))
            run = "/run";
        else if (mp_path_isdir("/var/run"))
            run = "/var/run";

        confdir = talloc_asprintf(tmp, "%s/mpv-%d", run, uid);
        mkdir(confdir, 0700);
    } else {
        if (homedir == NULL)
            goto exit;
        const char *dir =
            type == Config ? ".config" :
            type == Cache  ? ".cache"  : NULL;
        confdir = mp_path_mkdirs(tmp, bstr0(homedir), bstr0(dir), bstr0("mpv"), bstr0(""));
    }

#if SUPPORT_OLD_CONFIG
    /* check for the old config dir -- we only accept it if it's a real dir */
    if (type != Runtime) {
        char *olddir = mp_path_join(tmp, bstr0(homedir), bstr0(".mpv"));
        struct stat st;
        if (lstat(olddir, &st) == 0 && S_ISDIR(st.st_mode)) {
            static int warned = 0;
            if (!warned++)
                mp_msg(MSGT_GLOBAL, MSGL_WARN,
                       "The default config directory changed. "
                       "Migrate to the new directory with: mv %s %s\n",
                       olddir, confdir);
            confdir = olddir;
        }
    }
#endif

    ret = bstr0(talloc_strdup(talloc_ctx, confdir));
exit:
    talloc_free(tmp);
    return ret;
}

#else /* windows version */

static inline struct bstr find_config_dir(const void *talloc_ctx, const config_type type) {
    char *confdir = mpv_home(talloc_ctx, type);
    if (confdir)
        return bstr0(confdir);

    char *tmp = talloc_new(NULL);

    /* get the exe's path */
    /* windows xp bug: exename might not be 0-terminated; give the buffer an extra 0 wchar */
    wchar_t exename[MAX_PATH+1] = {0};
    GetModuleFileNameW(NULL, exename, MAX_PATH);
    struct bstr exedir = mp_dirname(mp_to_utf8(tmp, exename));
    confdir = mp_path_join(tmp, exedir, bstr0("mpv"));

    /* check if we have an exe-local confdir */
    if (!(ALWAYS_LOCAL_APPDATA && type == Cache) &&
        mp_path_exists(confdir) && mp_path_isdir(confdir)) {
        if (type == Cache) {
            confdir = mp_path_mkdirs(talloc_ctx, bstr0(confdir), bstr0("cache"), bstr0(""));
        } else {
            confdir = talloc_strdup(talloc_ctx, confdir);
        }
    } else {
        wchar_t appdata[MAX_PATH];
        DWORD flags =
            type == Config ? CSIDL_APPDATA       :
            type == Cache  ? CSIDL_LOCAL_APPDATA : 0;

        if (SUCCEEDED(SHGetFolderPathW(NULL,
                                       flags|CSIDL_FLAG_CREATE,
                                       NULL,
                                       SHGFP_TYPE_CURRENT,
                                       appdata))) {
            char *u8appdata = mp_to_utf8(tmp, appdata);

            confdir = mp_path_mkdirs(talloc_ctx, bstr0(u8appdata), bstr0("mpv"), bstr0(""));
        } else {
            confdir = NULL;
        }
    }
    talloc_free(tmp);
    return bstr0(confdir);
}

#endif

static inline char *find_user_file(const config_type type, const char *path[]) {
    static void *config_dir_ctx = NULL;
    if (config_dir_ctx == NULL) {
        config_dir_ctx = talloc_new(NULL);
        talloc_set_name_const(config_dir_ctx, "Config dirs");
    }

    static struct bstr config_dirs[config_type_count] = {{0,0}};
    if (config_dirs[type].len == 0)
        config_dirs[type] = find_config_dir(config_dir_ctx, type);

    void *tmp = talloc_new(NULL);

    char *msg = talloc_asprintf(tmp, "find_user_file(%d", type);
    for (int i = 0; path[i] != NULL; i++)
        msg = talloc_asprintf(msg, "%s, '%s'", msg, path[i]);

    struct bstr *paths = mp_prepend_and_bstr0(tmp, config_dirs[type], path);

    char *buf = NULL;
    if (type == Runtime)
        buf = mp_path_join_array(config_dir_ctx, paths);
    else
        buf = mp_path_mkdirs_array(config_dir_ctx, paths);

    msg = talloc_asprintf(msg, "%s, NULL) -> '%s'", msg, buf);
    mp_msg(MSGT_GLOBAL, MSGL_WARN, "%s\n", msg);

    talloc_free(tmp);
    return buf;
}

char *mp_find_user_config_file_array(const char *path[])
{
    return find_user_file(Config, path);
}


char *mp_find_user_cache_file_array(const char *path[])
{
    return find_user_file(Cache, path);
}

char *mp_find_user_runtime_file_array(const char *path[])
{
    return find_user_file(Runtime, path);
    exit(0);
}

char *mp_find_global_config_file_array(const char *path[])
{
    struct bstr *paths = mp_prepend_and_bstr0(NULL, bstr0(MPLAYER_CONFDIR), path);
    char *ret = mp_path_join_array(NULL, paths);
    talloc_free(paths);
    return ret;
}

char *mp_basename(const char *path)
{
    char *s;

#if HAVE_DOS_PATHS
    s = strrchr(path, '\\');
    if (s)
        path = s + 1;
    s = strrchr(path, ':');
    if (s)
        path = s + 1;
#endif
    s = strrchr(path, '/');
    return s ? s + 1 : (char *)path;
}

struct bstr mp_dirname(const char *path)
{
    struct bstr ret = {
        (uint8_t *)path, mp_basename(path) - path
    };
    if (ret.len == 0)
        return bstr0(".");
    return ret;
}

char *mp_splitext(const char *path, bstr *root)
{
    assert(path);
    const char *split = strrchr(path, '.');
    if (!split)
        split = path + strlen(path);
    if (root)
        *root = (bstr){.start = (char *)path, .len = path - split};
    return (char *)split;
}

static inline bool ends_with_separator(const struct bstr p)  {
    if (p.len < 1)
        return false;

    int endchar = p.start[p.len - 1];
#if HAVE_DOS_PATHS
    // "X:" is a relative path. We treat it as having a separator
    // to avoid adding a \ to it, which would turn it into an absolute one.
    return endchar == '/' || endchar == '\\' ||
           p.len == 2 && endchar == ':';
#else
    return endchar == '/';
#endif
}

static void mkdir_cb(const char *path, const struct bstr *rest) {
    if (rest[0].start == NULL) // this is the last component
        return;
    mkdir(path, 0777);
}

#define mp_path_join_cb(talloc_ctx, cb, ...) \
    mp_path_join_cb_array((talloc_ctx), (cb), (const struct bstr[]){__VA_ARGS__, bstr0(NULL)})
static inline char *mp_path_join_cb_array(
    const void *talloc_ctx, void (*cb)(const char* path, const struct bstr *rest), const struct bstr path[])
{
    if (path[0].start == NULL) {
        char *ret = talloc_strdup(talloc_ctx, "");
        if (cb) cb(ret, &path[0]);
        return ret;
    }
    if (path[1].start == NULL) {
        char *ret = bstrdup0(talloc_ctx, path[0]);
        if (cb) cb(ret, &path[1]);
        return ret;
    }
    if (path[2].start == NULL) {
        if (is_absolute(path[1])) {
            char *ret = bstrdup0(talloc_ctx, path[1]);
            if (cb) cb(ret, &path[2]);
            return ret;
        }
    }

    char *tmp = talloc_new(NULL);

    char *p = NULL;
    for (int i = 0; path[i].start != NULL; i++) {
        const struct bstr np = path[i];
        if (is_absolute(np)) {
            // discard the path accumulated so far
            if (i > 0)
                mp_msg(MSGT_GLOBAL, MSGL_WARN,
                       "Joining path with absolute path: %.*s\n", BSTR_P(np));
            talloc_free(p);
            p = bstrdup0(tmp, np);
        } else {
#if HAVE_DOS_PATHS
            if (i > 0 && np.len > 1 && np.start[1] == ':') {
                mp_msg(MSGT_GLOBAL, MSGL_FATAL,
                       "Joining path with drive-relative path: %.*s\n", BSTR_P(np));
                abort();
            }
#endif

            char *sep = "";
            if (!ends_with_separator(bstr0(p)))
#if HAVE_DOS_PATHS
                sep = "\\";
#else
                sep = "/";
#endif
            p = talloc_asprintf(tmp, "%s%s%.*s",
                                p, sep, BSTR_P(np));
        }

        if (cb) cb(p, &path[i+1]);
    }

    p = talloc_strdup(talloc_ctx, p);
    talloc_free(tmp);
    return p;
}

// path[] is (and needs to be) bstr0(NULL)-terminated.
char *mp_path_join_array(const void *talloc_ctx, const struct bstr path[])
{
    return mp_path_join_cb_array(talloc_ctx, NULL, path);
}

char *mp_path_mkdirs_array(const void *talloc_ctx, const struct bstr path[])
{
    return mp_path_join_cb_array(talloc_ctx, mkdir_cb, path);
}

char *mp_getcwd(const void *talloc_ctx)
{
    char *wd = talloc_array(talloc_ctx, char, 20);
    while (getcwd(wd, talloc_get_size(wd)) == NULL) {
        if (errno != ERANGE) {
            talloc_free(wd);
            return NULL;
        }
        wd = talloc_realloc(talloc_ctx, wd, char, talloc_get_size(wd) * 2);
    }
    return wd;
}

bool mp_path_exists(const char *path)
{
    struct stat st;
    return mp_stat(path, &st) == 0;
}

bool mp_path_isdir(const char *path)
{
    struct stat st;
    return mp_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

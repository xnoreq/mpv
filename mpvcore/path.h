/*
 * Get path to config dir/file.
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

#ifndef MPLAYER_PATH_H
#define MPLAYER_PATH_H

#include <stdbool.h>
#include "mpvcore/bstr.h"


// Search for the input filename in several paths. These include user and global
// config locations by default. Some platforms may implement additional platform
// related lookups (i.e.: OSX inside an application bundle).
#define mp_find_config_file(...) \
    mp_find_config_file_array((const char *[]){__VA_ARGS__, NULL})
char *mp_find_config_file_array(const char *path[]);

// Search for the input filename in the global configuration location.
char *mp_find_global_config_file_array(const char *path[]);

// Search for the input filename in the user configuration location.
#define mp_find_user_config_file(...) \
    mp_find_user_config_file_array((const char *[]){__VA_ARGS__, NULL})
char *mp_find_user_config_file_array(const char *path[]);

// Search for the input filename in the user cache location.
#define mp_find_user_cache_file(...) \
    mp_find_user_cache_file_array((const char *[]){__VA_ARGS__, NULL})
char *mp_find_user_cache_file_array(const char *path[]);

#define mp_find_user_runtime_file(...) \
    mp_find_user_runtime_file_array((const char *[]){__VA_ARGS__, NULL})
char *mp_find_user_runtime_file_array(const char *path[]);

// Return pointer to filename part of path

char *mp_basename(const char *path);

/* Return file extension, including the '.'. If root is not NULL, set it to the
 * part of the path without extension. So: path == root + returnvalue
 * Don't consider it a file extension if the only '.' is the first character.
 * Return "" if no extension.
 */
char *mp_splitext(const char *path, bstr *root);

/* Return struct bstr referencing directory part of path, or if that
 * would be empty, ".".
 */
struct bstr mp_dirname(const char *path);

/* Join path components and return a newly allocated string
 * for the result. The system's path separator is inserted between
 * the components if needed.
 * If a path is absolute, the value of the previous paths are ignored.
 */
#define mp_path_join(talloc_ctx, ...) \
    mp_path_join_array((talloc_ctx), (const struct bstr[]){__VA_ARGS__, bstr0(NULL)})
char *mp_path_join_array(const void *talloc_ctx, const struct bstr path[]);

/* Generates a path in the same manner as mp_path_join, but calls mkdir
 * with mode 0777 for each component in the path, except for the last (presumed to be a file).
 * Returns the full generated path.
 */
#define mp_path_mkdirs(talloc_ctx, ...) \
    mp_path_mkdirs_array((talloc_ctx), (const struct bstr[]){__VA_ARGS__, bstr0(NULL)})
char *mp_path_mkdirs_array(const void *talloc_ctx, const struct bstr path[]);

char *mp_getcwd(const void *talloc_ctx);

bool mp_path_exists(const char *path);
bool mp_path_isdir(const char *path);

static inline struct bstr *mp_prepend_and_bstr0(const void *talloc_ctx, struct bstr prefix, const char *rest[])
{
    int count = 0;
    for (; rest[count++] != NULL;) {} // just count

    struct bstr *ret = talloc_array(talloc_ctx, struct bstr, count + 1);
    ret[0] = prefix;
    for (int i = 0; i < count; i++)
        ret[1 + i] = bstr0(rest[i]);
    ret[count] = bstr0(NULL);
    return ret;
}

#endif /* MPLAYER_PATH_H */

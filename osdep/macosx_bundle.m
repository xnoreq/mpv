/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#import <Foundation/Foundation.h>
#include "osdep/macosx_bundle.h"
#include "mpvcore/path.h"
#include "talloc.h"

char *get_bundled_path(const char *path[])
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  NSString *path = [[NSBundle mainBundle] resourcePath];

  void *tmp = talloc_new(NULL);
  char *rv = mp_path_join_array(NULL, mp_prepend_and_bstr0(tmp, bstr0([path UTF8String]), path));
  talloc_free(tmp);
  [pool release];
  return rv;
}

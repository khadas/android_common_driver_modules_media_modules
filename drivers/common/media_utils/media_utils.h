/*
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Description:
 */
#ifndef _MEDIA_FILE_H_
#define _MEDIA_FILE_H_
inline void *aml_media_mem_alloc(size_t size, gfp_t flags);
inline void aml_media_mem_free(const void *addr);

ssize_t media_write(struct file *, const void *, size_t, loff_t *);
ssize_t media_read(struct file *, void *, size_t, loff_t *);
struct file *media_open(const char *, int, umode_t);
int media_close(struct file *, fl_owner_t);
#endif

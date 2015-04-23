/* System Data internals
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

extern int sysdata_verify_sig(const void *data, unsigned long *_len);
int data_verify_pkcs7(const void *data, unsigned long len,
		      const void *raw_pkcs7, size_t pkcs7_len);

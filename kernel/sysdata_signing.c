/* System Data signature checker
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <keys/system_keyring.h>
#include <crypto/public_key.h>
#include <crypto/pkcs7.h>
#include "sysdata-internal.h"

/*
 * System Data signature information block.
 *
 * The constituents of the signature section are, in order:
 *
 *	- Signer's name
 *	- Key identifier
 *	- Signature data
 *	- Information block
 */
struct sysdata_signature {
	u8	algo;		/* Public-key crypto algorithm [0] */
	u8	hash;		/* Digest algorithm [0] */
	u8	id_type;	/* Key identifier type [PKEY_ID_PKCS7] */
	u8	signer_len;	/* Length of signer's name [0] */
	u8	key_id_len;	/* Length of key identifier [0] */
	u8	__pad[3];
	__be32	sig_len;	/* Length of signature data */
};

/*
 * Verify a PKCS#7-based signature on system data.
 */
static int data_verify_pkcs7(const void *data, unsigned long len,
			     const void *raw_pkcs7, size_t pkcs7_len)
{
	struct pkcs7_message *pkcs7;
	bool trusted;
	int ret;

	pkcs7 = pkcs7_parse_message(raw_pkcs7, pkcs7_len);
	if (IS_ERR(pkcs7))
		return PTR_ERR(pkcs7);

	/* The data should be detached - so we need to supply it. */
	if (pkcs7_supply_detached_data(pkcs7, data, len) < 0) {
		pr_err("PKCS#7 signature with non-detached data\n");
		ret = -EBADMSG;
		goto error;
	}

	ret = pkcs7_verify(pkcs7);
	if (ret < 0)
		goto error;

	ret = pkcs7_validate_trust(pkcs7, system_trusted_keyring, &trusted);
	if (ret < 0)
		goto error;

	if (!trusted) {
		pr_err("PKCS#7 signature not signed with a trusted key\n");
		ret = -ENOKEY;
	}

error:
	pkcs7_free_message(pkcs7);
	pr_devel("<==%s() = %d\n", __func__, ret);
	return ret;
}

/*
 * Verify the signature on system data.
 */
int sysdata_verify_sig(const void *data, unsigned long *_len)
{
	struct sysdata_signature ds;
	size_t len = *_len, sig_len;

	pr_devel("==>%s(,%zu)\n", __func__, len);

	if (len <= sizeof(ds))
		return -EBADMSG;

	memcpy(&ds, data + (len - sizeof(ds)), sizeof(ds));
	len -= sizeof(ds);

	sig_len = be32_to_cpu(ds.sig_len);
	if (sig_len >= len)
		return -EBADMSG;
	len -= sig_len;
	*_len = len;

	if (ds.id_type != PKEY_ID_PKCS7) {
		pr_err("Module is not signed with expected PKCS#7 message\n");
		return -ENOPKG;
	}

	if (ds.algo != 0 ||
	    ds.hash != 0 ||
	    ds.signer_len != 0 ||
	    ds.key_id_len != 0 ||
	    ds.__pad[0] != 0 ||
	    ds.__pad[1] != 0 ||
	    ds.__pad[2] != 0) {
		pr_err("PKCS#7 signature info has unexpected non-zero params\n");
		return -EBADMSG;
	}

	return data_verify_pkcs7(data, len, data + len, sig_len);
}

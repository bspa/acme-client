/*	$Id$ */
/*
 * Copyright (c) 2016 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHORS DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/stat.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
# include <sandbox.h>
#endif

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/engine.h>

#include "extern.h"

#define MARKER "-----BEGIN CERTIFICATE-----"

/*
 * Convert an X509 certificate to a buffer of "sz".
 * We don't guarantee that it's nil-terminated.
 * Returns NULL on failure.
 */
static char *
x509buf(X509 *x, size_t *sz)
{
	BIO	*bio;
	char	*p;
	int	 ssz;

	/* Convert X509 to PEM in BIO. */

	if (NULL == (bio = BIO_new(BIO_s_mem()))) {
		dowarnx("BIO_new");
		return(NULL);
	} else if ( ! PEM_write_bio_X509(bio, x)) {
		dowarnx("PEM_write_bio_X509");
		BIO_free(bio);
		return(NULL);
	}

	/* Now convert bio to string. */

	if (NULL == (p = malloc(bio->num_write))) {
		dowarn("malloc");
		BIO_free(bio);
		return(NULL);
	} 

	ssz = BIO_read(bio, p, bio->num_write);
	if (ssz < 0 || (unsigned)ssz != bio->num_write) {
		dowarnx("BIO_read");
		BIO_free(bio);
		return(NULL);
	}

	*sz = ssz;
	BIO_free(bio);
	return(p);
}

int
certproc(int netsock, int filesock, uid_t uid, gid_t gid)
{
	char		*csr, *chain, *url;
	unsigned char	*csrcp, *chaincp;
	size_t		 csrsz, chainsz;
	int		 i, rc, idx;
	X509		*x, *chainx;
	X509_EXTENSION	*ext;
	const X509V3_EXT_METHOD* method;
	void		*entries;
	STACK_OF(CONF_VALUE) *val;
	CONF_VALUE	*nval;

	ext = NULL;
	idx = -1;
	method = NULL;
	chain = csr = url = NULL;
	rc = 0;
	x = chainx = NULL;

	/* File-system and sandbox jailing. */

#ifdef __APPLE__
	if (-1 == sandbox_init(kSBXProfileNoNetwork, 
 	    SANDBOX_NAMED, NULL)) {
		dowarn("sandbox_init");
		goto error;
	}
#endif
	if (-1 == chroot(PATH_VAR_EMPTY)) {
		dowarn("%s: chroot", PATH_VAR_EMPTY);
		goto error;
	} else if (-1 == chdir("/")) {
		dowarn("/: chdir");
		goto error;
	}

	/* Pre-pledge due to file access attempts. */

	ERR_load_crypto_strings();

#if defined(__OpenBSD__) && OpenBSD >= 201605
	if (-1 == pledge("stdio", NULL)) {
		dowarn("pledge");
		goto error;
	}
#endif
	if ( ! dropprivs(uid, gid))
		doerrx("dropprivs");

	/*
	 * Wait until we receive the DER encoded (signed) certificate
	 * from the network process.
	 */

	if (NULL == (csr = readbuf(netsock, COMM_CSR, &csrsz)))
		goto error;

	csrcp = (u_char *)csr;
	x = d2i_X509(NULL, (const u_char **)&csrcp, csrsz);
	if (NULL == x) {
		dowarnx("d2i_X509");
		goto error;
	}

	/*
	 * Extract the CA Issuers from its NID.
	 * I have no idea what I'm doing.
	 */

	idx = X509_get_ext_by_NID(x, NID_info_access, idx);
	if (idx >= 0 && NULL != (ext = X509_get_ext(x, idx)))
		method = X509V3_EXT_get(ext);

	entries = X509_get_ext_d2i(x, NID_info_access, 0, 0);
	if (NULL != method && NULL != entries) {
		val = method->i2v(method, entries, 0);
		for (i = 0; i < sk_CONF_VALUE_num(val); i++) {
			nval = sk_CONF_VALUE_value(val, i);
			if (strcmp(nval->name, "CA Issuers - URI"))
				continue;
			url = strdup(nval->value);
			if (NULL == url) {
				dowarn("strdup");
				goto error;
			}
			break;
		}
	}

	if (NULL == url) {
		dowarnx("no CA issuer registered with certificate");
		goto error;
	}

	/* Write the CA issuer to the netsock. */

	if ( ! writestr(netsock, COMM_ISSUER, url))
		goto error;

	/* Read the full-chain back from the netsock. */

	if (NULL == (chain = readbuf(netsock, COMM_CHAIN, &chainsz)))
		goto error;

	/*
	 * Then check if the chain is PEM-encoded by looking to see if
	 * it begins with the PEM marker.
	 * If so, ship it as-is; otherwise, convert to a PEM encoded
	 * buffer and ship that.
	 */

	if (chainsz <= strlen(MARKER) ||
	    strncmp(chain, MARKER, strlen(MARKER))) {
		chaincp = (u_char *)chain;
		chainx = d2i_X509(NULL, 
			(const u_char **)&chaincp, chainsz);
		if (NULL == chainx) {
			dowarnx("d2i_X509");
			goto error;
		}
		free(chain);
		if (NULL == (chain = x509buf(chainx, &chainsz)))
			goto error;
	} 
	
	if ( ! writebuf(filesock, COMM_CHAIN, chain, chainsz))
		goto error;

	/* Next, convert the X509 to a buffer and send that. */

	free(chain);
	if (NULL == (chain = x509buf(x, &chainsz)))
		goto error;
	if ( ! writebuf(filesock, COMM_CSR, chain, chainsz))
		goto error;

	rc = 1;
error:
	if (NULL != x)
		X509_free(x);
	if (NULL != chainx)
		X509_free(chainx);
	free(csr);
	free(url);
	free(chain);
	close(netsock);
	close(filesock);
	ERR_print_errors_fp(stderr);
	ERR_free_strings();
	return(rc);
}

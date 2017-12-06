/*
 * Copyright (C) 2015 Aaron Jones <aaronmdjones@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "atheme.h"

#ifdef HAVE_OPENSSL

#ifdef HAVE_LIBIDN
#include <stringprep.h>
#endif /* HAVE_LIBIDN */

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "pbkdf2v2.h"

#define ATHEME_SASLPREP_MAXLEN (PASSLEN + 1)

static const char salt_chars[62] = PBKDF2_FN_BASE62;

static unsigned int pbkdf2v2_rounds = PBKDF2_ITERCNT_DEF;

unsigned int pbkdf2v2_digest = PBKDF2_DIGEST_DEF;

static bool
atheme_pbkdf2v2_determine_prf(struct pbkdf2v2_parameters *const restrict parsed)
{
	switch (parsed->a)
	{
		case PBKDF2_PRF_SCRAM_SHA1:
		case PBKDF2_PRF_SCRAM_SHA1_S64:
			parsed->scram = true;
			/* FALLTHROUGH */
		case PBKDF2_PRF_HMAC_SHA1:
		case PBKDF2_PRF_HMAC_SHA1_S64:
			parsed->md = EVP_sha1();
			parsed->dl = SHA_DIGEST_LENGTH;
			break;

		case PBKDF2_PRF_SCRAM_SHA2_256:
		case PBKDF2_PRF_SCRAM_SHA2_256_S64:
			parsed->scram = true;
			/* FALLTHROUGH */
		case PBKDF2_PRF_HMAC_SHA2_256:
		case PBKDF2_PRF_HMAC_SHA2_256_S64:
			parsed->md = EVP_sha256();
			parsed->dl = SHA256_DIGEST_LENGTH;
			break;

		case PBKDF2_PRF_SCRAM_SHA2_512:
		case PBKDF2_PRF_SCRAM_SHA2_512_S64:
			parsed->scram = true;
			/* FALLTHROUGH */
		case PBKDF2_PRF_HMAC_SHA2_512:
		case PBKDF2_PRF_HMAC_SHA2_512_S64:
			parsed->md = EVP_sha512();
			parsed->dl = SHA512_DIGEST_LENGTH;
			break;

		default:
			(void) slog(LG_DEBUG, "%s: PRF ID '%u' unknown", __func__, parsed->a);
			return false;
	}

	switch (parsed->a)
	{
		case PBKDF2_PRF_HMAC_SHA1_S64:
		case PBKDF2_PRF_HMAC_SHA2_256_S64:
		case PBKDF2_PRF_HMAC_SHA2_512_S64:
		case PBKDF2_PRF_SCRAM_SHA1_S64:
		case PBKDF2_PRF_SCRAM_SHA2_256_S64:
		case PBKDF2_PRF_SCRAM_SHA2_512_S64:
			parsed->salt64 = true;
			break;
	}

	if (! parsed->md)
	{
		(void) slog(LG_ERROR, "%s: parsed->md is NULL", __func__);
		return false;
	}

#ifndef HAVE_LIBIDN
	if (parsed->scram)
	{
		(void) slog(LG_INFO, "%s: encountered SCRAM format hash, but GNU libidn is unavailable", __func__);
		(void) slog(LG_INFO, "%s: user logins may fail if they have exotic password characters", __func__);
	}
#endif /* !HAVE_LIBIDN */

	return true;
}

static inline bool
atheme_pbkdf2v2_parameters_sane(const struct pbkdf2v2_parameters *const restrict parsed)
{
	if (parsed->sl < PBKDF2_SALTLEN_MIN || parsed->sl > PBKDF2_SALTLEN_MAX)
	{
		(void) slog(LG_ERROR, "%s: salt '%s' length %zu out of range", __func__, parsed->salt, parsed->sl);
		return false;
	}
	if (parsed->c < PBKDF2_ITERCNT_MIN || parsed->c > PBKDF2_ITERCNT_MAX)
	{
		(void) slog(LG_ERROR, "%s: iteration count '%u' out of range", __func__, parsed->c);
		return false;
	}

	return true;
}

static bool
atheme_pbkdf2v2_scram_derive(const struct pbkdf2v2_parameters *const parsed,
                             unsigned char csk[EVP_MAX_MD_SIZE],
                             unsigned char chk[EVP_MAX_MD_SIZE])
{
	unsigned char cck[EVP_MAX_MD_SIZE];

	if (csk && ! HMAC(parsed->md, parsed->cdg, (int) parsed->dl, ServerKeyStr, sizeof ServerKeyStr, csk, NULL))
	{
		(void) slog(LG_ERROR, "%s: HMAC() failed for csk", __func__);
		return false;
	}
	if (chk && ! HMAC(parsed->md, parsed->cdg, (int) parsed->dl, ClientKeyStr, sizeof ClientKeyStr, cck, NULL))
	{
		(void) slog(LG_ERROR, "%s: HMAC() failed for cck", __func__);
		return false;
	}
	if (chk && EVP_Digest(cck, parsed->dl, chk, NULL, parsed->md, NULL) != 1)
	{
		(void) slog(LG_ERROR, "%s: EVP_Digest(cck) failed for chk", __func__);
		return false;
	}

	return true;
}

#ifdef HAVE_LIBIDN

/* **********************************************************************************************
 * These 2 functions are provided for modules/saslserv/scram-sha (RFC 5802, RFC 7677, RFC 4013) *
 * The second function is also used by *this* module for password normalization (in SCRAM mode) *
 *                                                                                              *
 * Prototypes for them appear first, to avoid `-Wmissing-prototypes' diagnostics (under Clang)  *
 *                                                                                              *
 * Constant-but-unused function pointers for them appear last, so that the compiler can verify  *
 * their signatures match the typedefs in include/pbkdf2v2.h (necessary for bug-free inter-     *
 * module function calls)                                                                       *
 ********************************************************************************************** */

bool atheme_pbkdf2v2_scram_dbextract(const char *restrict, struct pbkdf2v2_parameters *restrict);
const char *atheme_pbkdf2v2_scram_normalize(const char *restrict);

bool
atheme_pbkdf2v2_scram_dbextract(const char *const restrict parameters,
                                struct pbkdf2v2_parameters *const restrict parsed)
{
	char salt64[0x8000];
	char ssk64[0x8000];
	char shk64[0x8000];
	char sdg64[0x8000];

	(void) memset(parsed, 0x00, sizeof *parsed);

	if (sscanf(parameters, PBKDF2_FS_LOADHASH, &parsed->a, &parsed->c, salt64, ssk64, shk64) == 5)
	{
		(void) slog(LG_DEBUG, "%s: matched PBKDF2_FS_LOADHASH (SCRAM-SHA)", __func__);
		goto parsed;
	}
	if (sscanf(parameters, PBKDF2_FN_LOADHASH, &parsed->a, &parsed->c, salt64, sdg64) == 4)
	{
		(void) slog(LG_DEBUG, "%s: matched PBKDF2_FN_LOADHASH (HMAC-SHA)", __func__);
		goto parsed;
	}

	(void) slog(LG_DEBUG, "%s: could not extract necessary information from database", __func__);
	return false;

parsed:

	if (! atheme_pbkdf2v2_determine_prf(parsed))
		// This function logs messages on failure
		return false;

	if (parsed->salt64)
	{
		if ((parsed->sl = base64_decode(salt64, parsed->salt, sizeof parsed->salt)) == (size_t) -1)
		{
			(void) slog(LG_ERROR, "%s: base64_decode('%s') for salt failed", __func__, salt64);
			return false;
		}

		if (! atheme_pbkdf2v2_parameters_sane(parsed))
			// This function logs messages on failure
			return false;
	}
	else
	{
		parsed->sl = strlen(salt64);

		if (! atheme_pbkdf2v2_parameters_sane(parsed))
			// This function logs messages on failure
			return false;

		(void) memcpy(parsed->salt, salt64, parsed->sl);
	}

	// Ensure that the SCRAM-SHA module knows which one of 2 possible algorithms we're using
	switch (parsed->a)
	{
		case PBKDF2_PRF_HMAC_SHA1:
		case PBKDF2_PRF_HMAC_SHA1_S64:
		case PBKDF2_PRF_SCRAM_SHA1:
		case PBKDF2_PRF_SCRAM_SHA1_S64:
			parsed->a = PBKDF2_PRF_SCRAM_SHA1;
			break;

		case PBKDF2_PRF_HMAC_SHA2_256:
		case PBKDF2_PRF_HMAC_SHA2_256_S64:
		case PBKDF2_PRF_SCRAM_SHA2_256:
		case PBKDF2_PRF_SCRAM_SHA2_256_S64:
			parsed->a = PBKDF2_PRF_SCRAM_SHA2_256;
			break;

		default:
			(void) slog(LG_DEBUG, "%s: unsupported PRF '%u'", __func__, parsed->a);
			return false;
	}

	if (parsed->scram)
	{
		if (base64_decode(ssk64, parsed->ssk, sizeof parsed->ssk) != parsed->dl)
		{
			(void) slog(LG_ERROR, "%s: base64_decode('%s') for ssk failed", __func__, ssk64);
			return false;
		}
		if (base64_decode(shk64, parsed->shk, sizeof parsed->shk) != parsed->dl)
		{
			(void) slog(LG_ERROR, "%s: base64_decode('%s') for ssk failed", __func__, shk64);
			return false;
		}
	}
	else
	{
		// atheme_pbkdf2v2_scram_derive() uses parsed->cdg; not parsed->sdg
		if (base64_decode(sdg64, parsed->cdg, sizeof parsed->cdg) != parsed->dl)
		{
			(void) slog(LG_ERROR, "%s: base64_decode('%s') for sdg failed", __func__, sdg64);
			return false;
		}

		if (! atheme_pbkdf2v2_scram_derive(parsed, parsed->ssk, parsed->shk))
			// This function logs messages on failure
			return false;

		(void) slog(LG_INFO, "%s: attempting SCRAM-SHA login with regular PBKDF2 credentials", __func__);
	}

	return true;
}

const char *
atheme_pbkdf2v2_scram_normalize(const char *const restrict input)
{
	static char buf[ATHEME_SASLPREP_MAXLEN];

	(void) memset(buf, 0x00, ATHEME_SASLPREP_MAXLEN);

	if (snprintf(buf, ATHEME_SASLPREP_MAXLEN, "%s", input) >= ATHEME_SASLPREP_MAXLEN)
	{
		(void) slog(LG_DEBUG, "%s: snprintf(3) would have overflowed result buffer (BUG)", __func__);
		return NULL;
	}

	const int ret = stringprep(buf, ATHEME_SASLPREP_MAXLEN, (Stringprep_profile_flags) 0, stringprep_saslprep);

	if (ret != STRINGPREP_OK)
	{
		(void) slog(LG_DEBUG, "%s: %s", __func__, stringprep_strerror((Stringprep_rc) ret));
		return NULL;
	}

	return buf;
}

static const atheme_pbkdf2v2_scram_dbextract_fn __attribute__((unused)) ex_fn_ptr = &atheme_pbkdf2v2_scram_dbextract;
static const atheme_pbkdf2v2_scram_normalize_fn __attribute__((unused)) nm_fn_ptr = &atheme_pbkdf2v2_scram_normalize;

/* **********************************************************************************************
 * End external functions                                                                       *
 ********************************************************************************************** */

#endif /* HAVE_LIBIDN */

static bool
atheme_pbkdf2v2_compute(const char *restrict password, const char *const restrict parameters,
                        struct pbkdf2v2_parameters *const restrict parsed, const bool verifying)
{
	char salt64[0x2000];
	char sdg64[0x2000];
	char ssk64[0x2000];
	char shk64[0x2000];

	(void) memset(parsed, 0x00, sizeof *parsed);

	bool matched_ssk_shk = false;

	if (verifying)
	{
		if (sscanf(parameters, PBKDF2_FS_LOADHASH, &parsed->a, &parsed->c, salt64, ssk64, shk64) == 5)
		{
			(void) slog(LG_DEBUG, "%s: matched PBKDF2_FS_LOADHASH (SCRAM-SHA)", __func__);
			matched_ssk_shk = true;
			goto parsed;
		}
		if (sscanf(parameters, PBKDF2_FN_LOADHASH, &parsed->a, &parsed->c, salt64, sdg64) == 4)
		{
			(void) slog(LG_DEBUG, "%s: matched PBKDF2_FN_LOADHASH (HMAC-SHA)", __func__);
			goto parsed;
		}

		(void) slog(LG_DEBUG, "%s: no sscanf(3) was successful", __func__);
	}
	else
	{
		if (sscanf(parameters, PBKDF2_FN_LOADSALT, &parsed->a, &parsed->c, salt64) == 3)
		{
			(void) slog(LG_DEBUG, "%s: matched PBKDF2_FN_LOADSALT (Encrypting)", __func__);
			goto parsed;
		}

		(void) slog(LG_ERROR, "%s: no sscanf(3) was successful (BUG?)", __func__);
	}

	return false;

parsed:

	if (! atheme_pbkdf2v2_determine_prf(parsed))
		// This function logs messages on failure
		return false;

#ifdef HAVE_LIBIDN
	if (parsed->scram && ((password = atheme_pbkdf2v2_scram_normalize(password)) == NULL))
		// This function logs messages on failure
		return false;
#endif /* HAVE_LIBIDN */

	if (parsed->salt64)
	{
		if ((parsed->sl = base64_decode(salt64, parsed->salt, sizeof parsed->salt)) == (size_t) -1)
		{
			(void) slog(LG_ERROR, "%s: base64_decode('%s') for salt failed", __func__, salt64);
			return false;
		}

		if (! atheme_pbkdf2v2_parameters_sane(parsed))
			// This function logs messages on failure
			return false;
	}
	else
	{
		parsed->sl = strlen(salt64);

		if (! atheme_pbkdf2v2_parameters_sane(parsed))
			// This function logs messages on failure
			return false;

		(void) memcpy(parsed->salt, salt64, parsed->sl);
	}

	const size_t pl = strlen(password);

	if (! pl)
	{
		(void) slog(LG_ERROR, "%s: password length == 0", __func__);
		return false;
	}

	if (matched_ssk_shk)
	{
		if (base64_decode(ssk64, parsed->ssk, sizeof parsed->ssk) != parsed->dl)
		{
			(void) slog(LG_ERROR, "%s: base64_decode('%s') for ssk failed", __func__, ssk64);
			return false;
		}
		if (base64_decode(shk64, parsed->shk, sizeof parsed->shk) != parsed->dl)
		{
			(void) slog(LG_ERROR, "%s: base64_decode('%s') for shk failed", __func__, shk64);
			return false;
		}
	}
	else if (verifying)
	{
		if (base64_decode(sdg64, parsed->sdg, sizeof parsed->sdg) != parsed->dl)
		{
			(void) slog(LG_ERROR, "%s: base64_decode('%s') for sdg failed", __func__, sdg64);
			return false;
		}
	}

	const int ret = PKCS5_PBKDF2_HMAC(password, (int) pl, parsed->salt, (int) parsed->sl, (int) parsed->c,
	                                  parsed->md, (int) parsed->dl, parsed->cdg);
	if (ret != 1)
	{
		(void) slog(LG_ERROR, "%s: PKCS5_PBKDF2_HMAC() failed", __func__);
		return false;
	}

	return true;
}

static const char *
atheme_pbkdf2v2_salt(void)
{
	unsigned char rawsalt[PBKDF2_SALTLEN_DEF];
	(void) arc4random_buf(rawsalt, sizeof rawsalt);

	char salt[PBKDF2_SALTLEN_MAX * 3];
	if (base64_encode(rawsalt, sizeof rawsalt, salt, sizeof salt) == (size_t) -1)
	{
		(void) slog(LG_ERROR, "%s: base64_encode() failed (BUG)", __func__);
		return NULL;
	}

	static char res[PASSLEN];
	if (snprintf(res, PASSLEN, PBKDF2_FN_SAVESALT, pbkdf2v2_digest, pbkdf2v2_rounds, salt) >= PASSLEN)
	{
		(void) slog(LG_ERROR, "%s: snprintf(3) would have overflowed result buffer (BUG)", __func__);
		return NULL;
	}

	return res;
}

static const char *
atheme_pbkdf2v2_crypt(const char *const restrict password, const char *const restrict parameters)
{
	struct pbkdf2v2_parameters parsed;

	if (! atheme_pbkdf2v2_compute(password, parameters, &parsed, false))
		// This function logs messages on failure
		return NULL;

	static char res[PASSLEN];

	if (parsed.scram)
	{
		unsigned char csk[EVP_MAX_MD_SIZE];
		unsigned char chk[EVP_MAX_MD_SIZE];
		char csk64[EVP_MAX_MD_SIZE * 3];
		char chk64[EVP_MAX_MD_SIZE * 3];

		if (! atheme_pbkdf2v2_scram_derive(&parsed, csk, chk))
			// This function logs messages on failure
			return NULL;

		if (base64_encode(csk, parsed.dl, csk64, sizeof csk64) == (size_t) -1)
		{
			(void) slog(LG_ERROR, "%s: base64_encode() failed for csk", __func__);
			return NULL;
		}
		if (base64_encode(chk, parsed.dl, chk64, sizeof chk64) == (size_t) -1)
		{
			(void) slog(LG_ERROR, "%s: base64_encode() failed for chk", __func__);
			return NULL;
		}
		if (snprintf(res, PASSLEN, PBKDF2_FS_SAVEHASH, parsed.a, parsed.c, parsed.salt, csk64, chk64) >= PASSLEN)
		{
			(void) slog(LG_ERROR, "%s: snprintf() would have overflowed result buffer (BUG)", __func__);
			return NULL;
		}
	}
	else
	{
		char cdg64[EVP_MAX_MD_SIZE * 3];

		if (base64_encode(parsed.cdg, parsed.dl, cdg64, sizeof cdg64) == (size_t) -1)
		{
			(void) slog(LG_ERROR, "%s: base64_encode() failed for cdg", __func__);
			return NULL;
		}
		if (snprintf(res, PASSLEN, PBKDF2_FN_SAVEHASH, parsed.a, parsed.c, parsed.salt, cdg64) >= PASSLEN)
		{
			(void) slog(LG_ERROR, "%s: snprintf(3) would have overflowed result buffer (BUG)", __func__);
			return NULL;
		}
	}

	return res;
}

static bool
atheme_pbkdf2v2_verify(const char *const restrict password, const char *const restrict parameters)
{
	struct pbkdf2v2_parameters parsed;

	if (! atheme_pbkdf2v2_compute(password, parameters, &parsed, true))
		// This function logs messages on failure
		return false;

	if (parsed.scram)
	{
		unsigned char csk[EVP_MAX_MD_SIZE];

		if (! atheme_pbkdf2v2_scram_derive(&parsed, csk, NULL))
			// This function logs messages on failure
			return false;

		if (memcmp(parsed.ssk, csk, parsed.dl) != 0)
		{
			(void) slog(LG_DEBUG, "%s: memcmp(3) mismatch on ssk (invalid password?)", __func__);
			return false;
		}
	}
	else
	{
		if (memcmp(parsed.sdg, parsed.cdg, parsed.dl) != 0)
		{
			(void) slog(LG_DEBUG, "%s: memcmp(3) mismatch on sdg (invalid password?)", __func__);
			return false;
		}
	}

	return true;
}

static bool
atheme_pbkdf2v2_recrypt(const char *const restrict parameters)
{
	unsigned int prf;
	unsigned int iter;
	char salt[0x2000];

	if (sscanf(parameters, PBKDF2_FN_LOADSALT, &prf, &iter, salt) != 3)
	{
		(void) slog(LG_ERROR, "%s: no sscanf(3) was successful (BUG?)", __func__);
		return false;
	}
	if (prf != pbkdf2v2_digest)
	{
		(void) slog(LG_DEBUG, "%s: prf (%u) != default (%u)", __func__, prf, pbkdf2v2_digest);
		return true;
	}
	if (iter != pbkdf2v2_rounds)
	{
		(void) slog(LG_DEBUG, "%s: rounds (%u) != default (%u)", __func__, iter, pbkdf2v2_rounds);
		return true;
	}
	if (strlen(salt) != PBKDF2_SALTLEN_DEF)
	{
		(void) slog(LG_DEBUG, "%s: salt length is different", __func__);
		return true;
	}

	return false;
}

static int
c_ci_pbkdf2v2_digest(mowgli_config_file_entry_t *const restrict ce)
{
	if (!ce->vardata)
	{
		conf_report_warning(ce, "no parameter for configuration option");
		return 0;
	}

	if (!strcasecmp(ce->vardata, "SHA1"))
		pbkdf2v2_digest = PBKDF2_PRF_HMAC_SHA1_S64;
	else if (!strcasecmp(ce->vardata, "SHA256"))
		pbkdf2v2_digest = PBKDF2_PRF_HMAC_SHA2_256_S64;
	else if (!strcasecmp(ce->vardata, "SHA512"))
		pbkdf2v2_digest = PBKDF2_PRF_HMAC_SHA2_512_S64;
#ifdef HAVE_LIBIDN
	else if (!strcasecmp(ce->vardata, "SCRAM-SHA1"))
		pbkdf2v2_digest = PBKDF2_PRF_SCRAM_SHA1_S64;
	else if (!strcasecmp(ce->vardata, "SCRAM-SHA256"))
		pbkdf2v2_digest = PBKDF2_PRF_SCRAM_SHA2_256_S64;
/*	// No specification
	else if (!strcasecmp(ce->vardata, "SCRAM-SHA512"))
		pbkdf2v2_digest = PBKDF2_PRF_SCRAM_SHA2_512_S64;
*/
#endif /* HAVE_LIBIDN */
	else
	{
		conf_report_warning(ce, "invalid parameter for configuration option -- using default");
		pbkdf2v2_digest = PBKDF2_DIGEST_DEF;
	}

	return 0;
}

static crypt_impl_t crypto_pbkdf2v2_impl = {

	.id         = "pbkdf2v2",
	.salt       = &atheme_pbkdf2v2_salt,
	.crypt      = &atheme_pbkdf2v2_crypt,
	.verify     = &atheme_pbkdf2v2_verify,
	.recrypt    = &atheme_pbkdf2v2_recrypt,
};

static mowgli_list_t pbkdf2v2_conf_table;

static void
crypto_pbkdf2v2_modinit(module_t __attribute__((unused)) *const restrict m)
{
	(void) crypt_register(&crypto_pbkdf2v2_impl);

	(void) add_subblock_top_conf("PBKDF2V2", &pbkdf2v2_conf_table);
	(void) add_conf_item("DIGEST", &pbkdf2v2_conf_table, c_ci_pbkdf2v2_digest);
	(void) add_uint_conf_item("ROUNDS", &pbkdf2v2_conf_table, 0, &pbkdf2v2_rounds,
	                          PBKDF2_ITERCNT_MIN, PBKDF2_ITERCNT_MAX, PBKDF2_ITERCNT_DEF);
}

static void
crypto_pbkdf2v2_moddeinit(const module_unload_intent_t __attribute__((unused)) intent)
{
	(void) del_conf_item("DIGEST", &pbkdf2v2_conf_table);
	(void) del_conf_item("ROUNDS", &pbkdf2v2_conf_table);
	(void) del_top_conf("PBKDF2V2");

	(void) crypt_unregister(&crypto_pbkdf2v2_impl);
}

DECLARE_MODULE_V1(PBKDF2V2_CRYPTO_MODULE_NAME, false, crypto_pbkdf2v2_modinit, crypto_pbkdf2v2_moddeinit,
                  PACKAGE_VERSION, "Aaron Jones <aaronmdjones@gmail.com>");

#endif /* HAVE_OPENSSL */

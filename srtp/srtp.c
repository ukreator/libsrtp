/*
 * srtp.c
 *
 * the secure real-time transport protocol
 *
 * David A. McGrew
 * Cisco Systems, Inc.
 */
/*
 *	
 * Copyright (c) 2001-2006, Cisco Systems, Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * 
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 * 
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "srtp.h"
#include "srtp_priv.h"
#include "crypto_types.h"
#include "err.h"
#include "ekt.h"             /* for SRTP Encrypted Key Transport */
#include "alloc.h"           /* for srtp_crypto_alloc()          */
#ifdef OPENSSL
#include "aes_gcm_ossl.h"    /* for AES GCM mode  */
#endif

#include <limits.h>
#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#elif defined(HAVE_WINSOCK2_H)
# include <winsock2.h>
#endif


/* the debug module for srtp */

srtp_debug_module_t mod_srtp = {
  0,                  /* debugging is off by default */
  "srtp"              /* printable name for module   */
};

#define octets_in_rtp_header   12
#define uint32s_in_rtp_header  3
#define octets_in_rtcp_header  8
#define uint32s_in_rtcp_header 2
#define octets_in_rtp_extn_hdr 4

static srtp_err_status_t
srtp_validate_rtp_header(void *rtp_hdr, int *pkt_octet_len) {
  srtp_hdr_t *hdr = (srtp_hdr_t *)rtp_hdr;

  /* Check RTP header length */
  int rtp_header_len = octets_in_rtp_header + 4 * hdr->cc;
  if (hdr->x == 1)
    rtp_header_len += octets_in_rtp_extn_hdr;

  if (*pkt_octet_len < rtp_header_len)
    return srtp_err_status_bad_param;

  /* Verifing profile length. */
  if (hdr->x == 1) {
    srtp_hdr_xtnd_t *xtn_hdr =
      (srtp_hdr_xtnd_t *)((uint32_t *)hdr + uint32s_in_rtp_header + hdr->cc);
    int profile_len = ntohs(xtn_hdr->length);
    rtp_header_len += profile_len * 4;
    /* profile length counts the number of 32-bit words */
    if (*pkt_octet_len < rtp_header_len)
      return srtp_err_status_bad_param;
  }
  return srtp_err_status_ok;
}

const char *srtp_get_version_string ()
{
    /*
     * Simply return the autotools generated string
     */
    return SRTP_VER_STRING;
}

unsigned int srtp_get_version ()
{
    unsigned int major = 0, minor = 0, micro = 0;
    unsigned int rv = 0;
    int parse_rv;

    /*
     * Parse the autotools generated version 
     */
    parse_rv = sscanf(SRTP_VERSION, "%u.%u.%u", &major, &minor, &micro);
    if (parse_rv != 3) {
	/*
	 * We're expected to parse all 3 version levels.
	 * If not, then this must not be an official release.
	 * Return all zeros on the version
	 */
	return (0);
    }

    /* 
     * We allow 8 bits for the major and minor, while
     * allowing 16 bits for the micro.  16 bits for the micro
     * may be beneficial for a continuous delivery model 
     * in the future.
     */
    rv |= (major & 0xFF) << 24;
    rv |= (minor & 0xFF) << 16;
    rv |= micro & 0xFF;
    return rv;
}

srtp_err_status_t
srtp_stream_alloc(srtp_stream_ctx_t **str_ptr,
		  const srtp_policy_t *p) {
  srtp_stream_ctx_t *str;
  srtp_err_status_t stat;

  /*
   * This function allocates the stream context, rtp and rtcp ciphers
   * and auth functions, and key limit structure.  If there is a
   * failure during allocation, we free all previously allocated
   * memory and return a failure code.  The code could probably 
   * be improved, but it works and should be clear.
   */

  /* allocate srtp stream and set str_ptr */
  str = (srtp_stream_ctx_t *) srtp_crypto_alloc(sizeof(srtp_stream_ctx_t));
  if (str == NULL)
    return srtp_err_status_alloc_fail;
  *str_ptr = str;  
  
  /* allocate cipher */
  stat = srtp_crypto_kernel_alloc_cipher(p->rtp.cipher_type, 
				    &str->rtp_cipher, 
				    p->rtp.cipher_key_len,
				    p->rtp.auth_tag_len); 
  if (stat) {
    srtp_crypto_free(str);
    return stat;
  }

  /* allocate auth function */
  stat = srtp_crypto_kernel_alloc_auth(p->rtp.auth_type, 
				  &str->rtp_auth,
				  p->rtp.auth_key_len, 
				  p->rtp.auth_tag_len); 
  if (stat) {
    srtp_cipher_dealloc(str->rtp_cipher);
    srtp_crypto_free(str);
    return stat;
  }
  
  /* allocate key limit structure */
  str->limit = (srtp_key_limit_ctx_t*) srtp_crypto_alloc(sizeof(srtp_key_limit_ctx_t));
  if (str->limit == NULL) {
    auth_dealloc(str->rtp_auth);
    srtp_cipher_dealloc(str->rtp_cipher);
    srtp_crypto_free(str); 
    return srtp_err_status_alloc_fail;
  }

  /*
   * ...and now the RTCP-specific initialization - first, allocate
   * the cipher 
   */
  stat = srtp_crypto_kernel_alloc_cipher(p->rtcp.cipher_type, 
				    &str->rtcp_cipher, 
				    p->rtcp.cipher_key_len, 
				    p->rtcp.auth_tag_len); 
  if (stat) {
    auth_dealloc(str->rtp_auth);
    srtp_cipher_dealloc(str->rtp_cipher);
    srtp_crypto_free(str->limit);
    srtp_crypto_free(str);
    return stat;
  }

  /* allocate auth function */
  stat = srtp_crypto_kernel_alloc_auth(p->rtcp.auth_type, 
				  &str->rtcp_auth,
				  p->rtcp.auth_key_len, 
				  p->rtcp.auth_tag_len); 
  if (stat) {
    srtp_cipher_dealloc(str->rtcp_cipher);
    auth_dealloc(str->rtp_auth);
    srtp_cipher_dealloc(str->rtp_cipher);
    srtp_crypto_free(str->limit);
    srtp_crypto_free(str);
   return stat;
  }  

  /* allocate ekt data associated with stream */
  stat = srtp_ekt_alloc(&str->ekt, p->ekt);
  if (stat) {
    auth_dealloc(str->rtcp_auth);
    srtp_cipher_dealloc(str->rtcp_cipher);
    auth_dealloc(str->rtp_auth);
    srtp_cipher_dealloc(str->rtp_cipher);
    srtp_crypto_free(str->limit);
    srtp_crypto_free(str);
   return stat;    
  }

  return srtp_err_status_ok;
}

srtp_err_status_t
srtp_stream_dealloc(srtp_t session, srtp_stream_ctx_t *stream) { 
  srtp_err_status_t status;
  
  /*
   * we use a conservative deallocation strategy - if any deallocation
   * fails, then we report that fact without trying to deallocate
   * anything else
   */

  /* deallocate cipher, if it is not the same as that in template */
  if (session->stream_template
      && stream->rtp_cipher == session->stream_template->rtp_cipher) {
    /* do nothing */
  } else {
    status = srtp_cipher_dealloc(stream->rtp_cipher); 
    if (status) 
      return status;
  }

  /* deallocate auth function, if it is not the same as that in template */
  if (session->stream_template
      && stream->rtp_auth == session->stream_template->rtp_auth) {
    /* do nothing */
  } else {
    status = auth_dealloc(stream->rtp_auth);
    if (status)
      return status;
  }

  /* deallocate key usage limit, if it is not the same as that in template */
  if (session->stream_template
      && stream->limit == session->stream_template->limit) {
    /* do nothing */
  } else {
    srtp_crypto_free(stream->limit);
  }   

  /* 
   * deallocate rtcp cipher, if it is not the same as that in
   * template 
   */
  if (session->stream_template
      && stream->rtcp_cipher == session->stream_template->rtcp_cipher) {
    /* do nothing */
  } else {
    status = srtp_cipher_dealloc(stream->rtcp_cipher); 
    if (status) 
      return status;
  }

  /*
   * deallocate rtcp auth function, if it is not the same as that in
   * template 
   */
  if (session->stream_template
      && stream->rtcp_auth == session->stream_template->rtcp_auth) {
    /* do nothing */
  } else {
    status = auth_dealloc(stream->rtcp_auth);
    if (status)
      return status;
  }

  status = srtp_rdbx_dealloc(&stream->rtp_rdbx);
  if (status)
    return status;

  /* DAM - need to deallocate EKT here */

  /*
   * zeroize the salt value
   */
  memset(stream->salt, 0, SRTP_AEAD_SALT_LEN);
  memset(stream->c_salt, 0, SRTP_AEAD_SALT_LEN);

  
  /* deallocate srtp stream context */
  srtp_crypto_free(stream);

  return srtp_err_status_ok;
}


/*
 * srtp_stream_clone(stream_template, new) allocates a new stream and
 * initializes it using the cipher and auth of the stream_template
 * 
 * the only unique data in a cloned stream is the replay database and
 * the SSRC
 */

srtp_err_status_t
srtp_stream_clone(const srtp_stream_ctx_t *stream_template, 
		  uint32_t ssrc, 
		  srtp_stream_ctx_t **str_ptr) {
  srtp_err_status_t status;
  srtp_stream_ctx_t *str;

  debug_print(mod_srtp, "cloning stream (SSRC: 0x%08x)", ssrc);

  /* allocate srtp stream and set str_ptr */
  str = (srtp_stream_ctx_t *) srtp_crypto_alloc(sizeof(srtp_stream_ctx_t));
  if (str == NULL)
    return srtp_err_status_alloc_fail;
  *str_ptr = str;  

  /* set cipher and auth pointers to those of the template */
  str->rtp_cipher  = stream_template->rtp_cipher;
  str->rtp_auth    = stream_template->rtp_auth;
  str->rtcp_cipher = stream_template->rtcp_cipher;
  str->rtcp_auth   = stream_template->rtcp_auth;

  /* set key limit to point to that of the template */
  status = srtp_key_limit_clone(stream_template->limit, &str->limit);
  if (status) { 
    srtp_crypto_free(*str_ptr);
    *str_ptr = NULL;
    return status;
  }

  /* initialize replay databases */
  status = srtp_rdbx_init(&str->rtp_rdbx,
		     srtp_rdbx_get_window_size(&stream_template->rtp_rdbx));
  if (status) {
    srtp_crypto_free(*str_ptr);
    *str_ptr = NULL;
    return status;
  }
  srtp_rdb_init(&str->rtcp_rdb);
  str->allow_repeat_tx = stream_template->allow_repeat_tx;
  
  /* set ssrc to that provided */
  str->ssrc = ssrc;

  /* set direction and security services */
  str->direction     = stream_template->direction;
  str->rtp_services  = stream_template->rtp_services;
  str->rtcp_services = stream_template->rtcp_services;

  /* set pointer to EKT data associated with stream */
  str->ekt = stream_template->ekt;

  /* Copy the salt values */
  memcpy(str->salt, stream_template->salt, SRTP_AEAD_SALT_LEN);
  memcpy(str->c_salt, stream_template->c_salt, SRTP_AEAD_SALT_LEN);

  /* defensive coding */
  str->next = NULL;

  return srtp_err_status_ok;
}


/*
 * key derivation functions, internal to libSRTP
 *
 * srtp_kdf_t is a key derivation context
 *
 * srtp_kdf_init(&kdf, cipher_id, k, keylen) initializes kdf to use cipher
 * described by cipher_id, with the master key k with length in octets keylen.
 * 
 * srtp_kdf_generate(&kdf, l, kl, keylen) derives the key
 * corresponding to label l and puts it into kl; the length
 * of the key in octets is provided as keylen.  this function
 * should be called once for each subkey that is derived.
 *
 * srtp_kdf_clear(&kdf) zeroizes and deallocates the kdf state
 */

typedef enum {
  label_rtp_encryption  = 0x00,
  label_rtp_msg_auth    = 0x01,
  label_rtp_salt        = 0x02,
  label_rtcp_encryption = 0x03,
  label_rtcp_msg_auth   = 0x04,
  label_rtcp_salt       = 0x05
} srtp_prf_label;


/*
 * srtp_kdf_t represents a key derivation function.  The SRTP
 * default KDF is the only one implemented at present.
 */

typedef struct { 
  srtp_cipher_t *cipher;    /* cipher used for key derivation  */  
} srtp_kdf_t;

srtp_err_status_t
srtp_kdf_init(srtp_kdf_t *kdf, srtp_cipher_type_id_t cipher_id, const uint8_t *key, int length) {

  srtp_err_status_t stat;
  stat = srtp_crypto_kernel_alloc_cipher(cipher_id, &kdf->cipher, length, 0);
  if (stat)
    return stat;

  stat = srtp_cipher_init(kdf->cipher, key);
  if (stat) {
    srtp_cipher_dealloc(kdf->cipher);
    return stat;
  }

  return srtp_err_status_ok;
}

srtp_err_status_t
srtp_kdf_generate(srtp_kdf_t *kdf, srtp_prf_label label,
		  uint8_t *key, unsigned int length) {

  v128_t nonce;
  srtp_err_status_t status;
  
  /* set eigth octet of nonce to <label>, set the rest of it to zero */
  v128_set_to_zero(&nonce);
  nonce.v8[7] = label;
 
  status = srtp_cipher_set_iv(kdf->cipher, (const uint8_t*)&nonce, direction_encrypt);
  if (status)
    return status;
  
  /* generate keystream output */
  octet_string_set_to_zero(key, length);
  status = srtp_cipher_encrypt(kdf->cipher, key, &length);
  if (status)
    return status;

  return srtp_err_status_ok;
}

srtp_err_status_t
srtp_kdf_clear(srtp_kdf_t *kdf) {
  srtp_err_status_t status;
  status = srtp_cipher_dealloc(kdf->cipher);
  if (status)
    return status;
  kdf->cipher = NULL;

  return srtp_err_status_ok;  
}

/*
 *  end of key derivation functions 
 */

#define MAX_SRTP_KEY_LEN 256


/* Get the base key length corresponding to a given combined key+salt
 * length for the given cipher.
 * Assumption is that for AES-ICM a key length < 30 is Ismacryp using
 * AES-128 and short salts; everything else uses a salt length of 14.
 * TODO: key and salt lengths should be separate fields in the policy.  */
static inline int base_key_length(const srtp_cipher_type_t *cipher, int key_length)
{
  switch (cipher->id) {
  case SRTP_AES_128_ICM:
  case SRTP_AES_192_ICM:
  case SRTP_AES_256_ICM:
    /* The legacy modes are derived from
     * the configured key length on the policy */
    return key_length - 14;
    break;
  case SRTP_AES_128_GCM:
    return 16;
    break;
  case SRTP_AES_256_GCM:
    return 32;
    break;
  default:
    return key_length;
    break;
  }
}

srtp_err_status_t
srtp_stream_init_keys(srtp_stream_ctx_t *srtp, const void *key) {
  srtp_err_status_t stat;
  srtp_kdf_t kdf;
  uint8_t tmp_key[MAX_SRTP_KEY_LEN];
  int kdf_keylen = 30, rtp_keylen, rtcp_keylen;
  int rtp_base_key_len, rtp_salt_len;
  int rtcp_base_key_len, rtcp_salt_len;

  /* If RTP or RTCP have a key length > AES-128, assume matching kdf. */
  /* TODO: kdf algorithm, master key length, and master salt length should
   * be part of srtp_policy_t. */
  rtp_keylen = srtp_cipher_get_key_length(srtp->rtp_cipher);
  rtcp_keylen = srtp_cipher_get_key_length(srtp->rtcp_cipher);
  rtp_base_key_len = base_key_length(srtp->rtp_cipher->type, rtp_keylen);
  rtp_salt_len = rtp_keylen - rtp_base_key_len;

  if (rtp_keylen > kdf_keylen) {
    kdf_keylen = 46;  /* AES-CTR mode is always used for KDF */
  }

  if (rtcp_keylen > kdf_keylen) {
    kdf_keylen = 46;  /* AES-CTR mode is always used for KDF */
  }

  debug_print(mod_srtp, "srtp key len: %d", rtp_keylen);
  debug_print(mod_srtp, "srtcp key len: %d", rtcp_keylen);
  debug_print(mod_srtp, "base key len: %d", rtp_base_key_len);
  debug_print(mod_srtp, "kdf key len: %d", kdf_keylen);
  debug_print(mod_srtp, "rtp salt len: %d", rtp_salt_len);

  /* 
   * Make sure the key given to us is 'zero' appended.  GCM
   * mode uses a shorter master SALT (96 bits), but still relies on 
   * the legacy CTR mode KDF, which uses a 112 bit master SALT.
   */
  memset(tmp_key, 0x0, MAX_SRTP_KEY_LEN);
  memcpy(tmp_key, key, (rtp_base_key_len + rtp_salt_len));

  /* initialize KDF state     */
  stat = srtp_kdf_init(&kdf, SRTP_AES_ICM, (const uint8_t *)tmp_key, kdf_keylen);
  if (stat) {
    return srtp_err_status_init_fail;
  }
  
  /* generate encryption key  */
  stat = srtp_kdf_generate(&kdf, label_rtp_encryption, 
			   tmp_key, rtp_base_key_len);
  if (stat) {
    /* zeroize temp buffer */
    octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
    return srtp_err_status_init_fail;
  }
  debug_print(mod_srtp, "cipher key: %s", 
	      srtp_octet_string_hex_string(tmp_key, rtp_base_key_len));

  /* 
   * if the cipher in the srtp context uses a salt, then we need
   * to generate the salt value
   */
  if (rtp_salt_len > 0) {
    debug_print(mod_srtp, "found rtp_salt_len > 0, generating salt", NULL);

    /* generate encryption salt, put after encryption key */
    stat = srtp_kdf_generate(&kdf, label_rtp_salt, 
			     tmp_key + rtp_base_key_len, rtp_salt_len);
    if (stat) {
      /* zeroize temp buffer */
      octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
      return srtp_err_status_init_fail;
    }
    memcpy(srtp->salt, tmp_key + rtp_base_key_len, SRTP_AEAD_SALT_LEN);
  }
  if (rtp_salt_len > 0) {
    debug_print(mod_srtp, "cipher salt: %s",
		srtp_octet_string_hex_string(tmp_key + rtp_base_key_len, rtp_salt_len));
  }

  /* initialize cipher */
  stat = srtp_cipher_init(srtp->rtp_cipher, tmp_key);
  if (stat) {
    /* zeroize temp buffer */
    octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
    return srtp_err_status_init_fail;
  }

  /* generate authentication key */
  stat = srtp_kdf_generate(&kdf, label_rtp_msg_auth,
			   tmp_key, srtp_auth_get_key_length(srtp->rtp_auth));
  if (stat) {
    /* zeroize temp buffer */
    octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
    return srtp_err_status_init_fail;
  }
  debug_print(mod_srtp, "auth key:   %s",
	      srtp_octet_string_hex_string(tmp_key, 
				      srtp_auth_get_key_length(srtp->rtp_auth))); 

  /* initialize auth function */
  stat = auth_init(srtp->rtp_auth, tmp_key);
  if (stat) {
    /* zeroize temp buffer */
    octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
    return srtp_err_status_init_fail;
  }

  /*
   * ...now initialize SRTCP keys
   */

  rtcp_base_key_len = base_key_length(srtp->rtcp_cipher->type, rtcp_keylen);
  rtcp_salt_len = rtcp_keylen - rtcp_base_key_len;
  debug_print(mod_srtp, "rtcp salt len: %d", rtcp_salt_len);
  
  /* generate encryption key  */
  stat = srtp_kdf_generate(&kdf, label_rtcp_encryption, 
			   tmp_key, rtcp_base_key_len);
  if (stat) {
    /* zeroize temp buffer */
    octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
    return srtp_err_status_init_fail;
  }

  /* 
   * if the cipher in the srtp context uses a salt, then we need
   * to generate the salt value
   */
  if (rtcp_salt_len > 0) {
    debug_print(mod_srtp, "found rtcp_salt_len > 0, generating rtcp salt",
		NULL);

    /* generate encryption salt, put after encryption key */
    stat = srtp_kdf_generate(&kdf, label_rtcp_salt, 
			     tmp_key + rtcp_base_key_len, rtcp_salt_len);
    if (stat) {
      /* zeroize temp buffer */
      octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
      return srtp_err_status_init_fail;
    }
    memcpy(srtp->c_salt, tmp_key + rtcp_base_key_len, SRTP_AEAD_SALT_LEN);
  }
  debug_print(mod_srtp, "rtcp cipher key: %s", 
	      srtp_octet_string_hex_string(tmp_key, rtcp_base_key_len));  
  if (rtcp_salt_len > 0) {
    debug_print(mod_srtp, "rtcp cipher salt: %s",
		srtp_octet_string_hex_string(tmp_key + rtcp_base_key_len, rtcp_salt_len));
  }

  /* initialize cipher */
  stat = srtp_cipher_init(srtp->rtcp_cipher, tmp_key);
  if (stat) {
    /* zeroize temp buffer */
    octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
    return srtp_err_status_init_fail;
  }

  /* generate authentication key */
  stat = srtp_kdf_generate(&kdf, label_rtcp_msg_auth,
			   tmp_key, srtp_auth_get_key_length(srtp->rtcp_auth));
  if (stat) {
    /* zeroize temp buffer */
    octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
    return srtp_err_status_init_fail;
  }

  debug_print(mod_srtp, "rtcp auth key:   %s",
	      srtp_octet_string_hex_string(tmp_key, 
		     srtp_auth_get_key_length(srtp->rtcp_auth))); 

  /* initialize auth function */
  stat = auth_init(srtp->rtcp_auth, tmp_key);
  if (stat) {
    /* zeroize temp buffer */
    octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);
    return srtp_err_status_init_fail;
  }

  /* clear memory then return */
  stat = srtp_kdf_clear(&kdf);
  octet_string_set_to_zero(tmp_key, MAX_SRTP_KEY_LEN);  
  if (stat)
    return srtp_err_status_init_fail;

  return srtp_err_status_ok;
}

srtp_err_status_t
srtp_stream_init(srtp_stream_ctx_t *srtp, 
		  const srtp_policy_t *p) {
  srtp_err_status_t err;

   debug_print(mod_srtp, "initializing stream (SSRC: 0x%08x)", 
	       p->ssrc.value);

   /* initialize replay database */
   /* window size MUST be at least 64.  MAY be larger.  Values more than
    * 2^15 aren't meaningful due to how extended sequence numbers are
    * calculated.   Let a window size of 0 imply the default value. */

   if (p->window_size != 0 && (p->window_size < 64 || p->window_size >= 0x8000))
     return srtp_err_status_bad_param;

   if (p->window_size != 0)
     err = srtp_rdbx_init(&srtp->rtp_rdbx, p->window_size);
   else
     err = srtp_rdbx_init(&srtp->rtp_rdbx, 128);
   if (err) return err;

   /* initialize key limit to maximum value */
#ifdef NO_64BIT_MATH
{
   uint64_t temp;
   temp = make64(UINT_MAX,UINT_MAX);
   srtp_key_limit_set(srtp->limit, temp);
}
#else
   srtp_key_limit_set(srtp->limit, 0xffffffffffffLL);
#endif

   /* set the SSRC value */
   srtp->ssrc = htonl(p->ssrc.value);

   /* set the security service flags */
   srtp->rtp_services  = p->rtp.sec_serv;
   srtp->rtcp_services = p->rtcp.sec_serv;

   /*
    * set direction to unknown - this flag gets checked in srtp_protect(),
    * srtp_unprotect(), srtp_protect_rtcp(), and srtp_unprotect_rtcp(), and 
    * gets set appropriately if it is set to unknown.
    */
   srtp->direction = dir_unknown;

   /* initialize SRTCP replay database */
   srtp_rdb_init(&srtp->rtcp_rdb);

   /* initialize allow_repeat_tx */
   /* guard against uninitialized memory: allow only 0 or 1 here */
   if (p->allow_repeat_tx != 0 && p->allow_repeat_tx != 1) {
     srtp_rdbx_dealloc(&srtp->rtp_rdbx);
     return srtp_err_status_bad_param;
   }
   srtp->allow_repeat_tx = p->allow_repeat_tx;

   /* DAM - no RTCP key limit at present */

   /* initialize keys */
   err = srtp_stream_init_keys(srtp, p->key);
   if (err) {
     srtp_rdbx_dealloc(&srtp->rtp_rdbx);
     return err;
   }

   /* 
    * if EKT is in use, then initialize the EKT data associated with
    * the stream
    */
   err = srtp_ekt_stream_init_from_policy(srtp->ekt, p->ekt);
   if (err) {
     srtp_rdbx_dealloc(&srtp->rtp_rdbx);
     return err;
   }

   return srtp_err_status_ok;  
 }


 /*
  * srtp_event_reporter is an event handler function that merely
  * reports the events that are reported by the callbacks
  */

 void
 srtp_event_reporter(srtp_event_data_t *data) {

   srtp_err_report(srtp_err_level_warning, "srtp: in stream 0x%x: ", 
	      data->stream->ssrc);

   switch(data->event) {
   case event_ssrc_collision:
     srtp_err_report(srtp_err_level_warning, "\tSSRC collision\n");
     break;
   case event_key_soft_limit:
     srtp_err_report(srtp_err_level_warning, "\tkey usage soft limit reached\n");
     break;
   case event_key_hard_limit:
     srtp_err_report(srtp_err_level_warning, "\tkey usage hard limit reached\n");
     break;
   case event_packet_index_limit:
     srtp_err_report(srtp_err_level_warning, "\tpacket index limit reached\n");
     break;
   default:
     srtp_err_report(srtp_err_level_warning, "\tunknown event reported to handler\n");
   }
 }

 /*
  * srtp_event_handler is a global variable holding a pointer to the
  * event handler function; this function is called for any unexpected
  * event that needs to be handled out of the SRTP data path.  see
  * srtp_event_t in srtp.h for more info
  *
  * it is okay to set srtp_event_handler to NULL, but we set 
  * it to the srtp_event_reporter.
  */

 static srtp_event_handler_func_t *srtp_event_handler = srtp_event_reporter;

 srtp_err_status_t
 srtp_install_event_handler(srtp_event_handler_func_t func) {

   /* 
    * note that we accept NULL arguments intentionally - calling this
    * function with a NULL arguments removes an event handler that's
    * been previously installed
    */

   /* set global event handling function */
   srtp_event_handler = func;
   return srtp_err_status_ok;
 }

/*
 * AEAD uses a new IV formation method.  This function implements
 * section 9.1 from draft-ietf-avtcore-srtp-aes-gcm-07.txt.  The
 * calculation is defined as, where (+) is the xor operation:
 *
 *
 *              0  0  0  0  0  0  0  0  0  0  1  1
 *              0  1  2  3  4  5  6  7  8  9  0  1
 *            +--+--+--+--+--+--+--+--+--+--+--+--+
 *            |00|00|    SSRC   |     ROC   | SEQ |---+
 *            +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *                                                    |
 *            +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *            |         Encryption Salt           |->(+)
 *            +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *                                                    |
 *            +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *            |       Initialization Vector       |<--+
 *            +--+--+--+--+--+--+--+--+--+--+--+--+*
 *
 * Input:  *stream - pointer to SRTP stream context, used to retrieve
 *                   the SALT 
 *         *iv     - Pointer to receive the calculated IV
 *         *seq    - The ROC and SEQ value to use for the
 *                   IV calculation.
 *         *hdr    - The RTP header, used to get the SSRC value
 *
 */
static void srtp_calc_aead_iv(srtp_stream_ctx_t *stream, v128_t *iv, 
	                      srtp_xtd_seq_num_t *seq, srtp_hdr_t *hdr)
{
    v128_t	in;
    v128_t	salt;

#ifdef NO_64BIT_MATH
    uint32_t local_roc = ((high32(*seq) << 16) |
                         (low32(*seq) >> 16));
    uint16_t local_seq = (uint16_t) (low32(*seq));
#else
    uint32_t local_roc = (uint32_t)(*seq >> 16);
    uint16_t local_seq = (uint16_t) *seq;
#endif

    memset(&in, 0, sizeof(v128_t));
    memset(&salt, 0, sizeof(v128_t));

    in.v16[5] = htons(local_seq);
    local_roc = htonl(local_roc);
    memcpy(&in.v16[3], &local_roc, sizeof(local_roc));

    /*
     * Copy in the RTP SSRC value
     */
    memcpy(&in.v8[2], &hdr->ssrc, 4);
    debug_print(mod_srtp, "Pre-salted RTP IV = %s\n", v128_hex_string(&in));

    /*
     * Get the SALT value from the context
     */
    memcpy(salt.v8, stream->salt, SRTP_AEAD_SALT_LEN);
    debug_print(mod_srtp, "RTP SALT = %s\n", v128_hex_string(&salt));

    /*
     * Finally, apply tyhe SALT to the input
     */
    v128_xor(iv, &in, &salt);
}


/*
 * This function handles outgoing SRTP packets while in AEAD mode,
 * which currently supports AES-GCM encryption.  All packets are
 * encrypted and authenticated.
 */
static srtp_err_status_t
srtp_protect_aead (srtp_ctx_t *ctx, srtp_stream_ctx_t *stream, 
	           void *rtp_hdr, unsigned int *pkt_octet_len)
{
    srtp_hdr_t *hdr = (srtp_hdr_t*)rtp_hdr;
    uint32_t *enc_start;        /* pointer to start of encrypted portion  */
    unsigned int enc_octet_len = 0; /* number of octets in encrypted portion  */
    srtp_xtd_seq_num_t est;          /* estimated xtd_seq_num_t of *hdr        */
    int delta;                  /* delta of local pkt idx and that in hdr */
    srtp_err_status_t status;
    uint32_t tag_len;
    v128_t iv;
    unsigned int aad_len;

    debug_print(mod_srtp, "function srtp_protect_aead", NULL);

    /*
     * update the key usage limit, and check it to make sure that we
     * didn't just hit either the soft limit or the hard limit, and call
     * the event handler if we hit either.
     */
    switch (srtp_key_limit_update(stream->limit)) {
    case srtp_key_event_normal:
        break;
    case srtp_key_event_hard_limit:
        srtp_handle_event(ctx, stream, event_key_hard_limit);
        return srtp_err_status_key_expired;
    case srtp_key_event_soft_limit:
    default:
        srtp_handle_event(ctx, stream, event_key_soft_limit);
        break;
    }

    /* get tag length from stream */
    tag_len = srtp_auth_get_tag_length(stream->rtp_auth);

    /*
     * find starting point for encryption and length of data to be
     * encrypted - the encrypted portion starts after the rtp header
     * extension, if present; otherwise, it starts after the last csrc,
     * if any are present
     */
     enc_start = (uint32_t*)hdr + uint32s_in_rtp_header + hdr->cc;
     if (hdr->x == 1) {
         srtp_hdr_xtnd_t *xtn_hdr = (srtp_hdr_xtnd_t*)enc_start;
         enc_start += (ntohs(xtn_hdr->length) + 1);
     }
     if (!((uint8_t*)enc_start < (uint8_t*)hdr + *pkt_octet_len))
         return srtp_err_status_parse_err;
     enc_octet_len = (unsigned int)(*pkt_octet_len -
                                    ((uint8_t*)enc_start - (uint8_t*)hdr));

    /*
     * estimate the packet index using the start of the replay window
     * and the sequence number from the header
     */
    delta = srtp_rdbx_estimate_index(&stream->rtp_rdbx, &est, ntohs(hdr->seq));
    status = srtp_rdbx_check(&stream->rtp_rdbx, delta);
    if (status) {
	if (status != srtp_err_status_replay_fail || !stream->allow_repeat_tx) {
	    return status;  /* we've been asked to reuse an index */
	}
    } else {
	srtp_rdbx_add_index(&stream->rtp_rdbx, delta);
    }

#ifdef NO_64BIT_MATH
    debug_print2(mod_srtp, "estimated packet index: %08x%08x",
                 high32(est), low32(est));
#else
    debug_print(mod_srtp, "estimated packet index: %016llx", est);
#endif

    /*
     * AEAD uses a new IV formation method
     */
    srtp_calc_aead_iv(stream, &iv, &est, hdr);
    status = srtp_cipher_set_iv(stream->rtp_cipher, (const uint8_t*)&iv, direction_encrypt);
    if (status) {
        return srtp_err_status_cipher_fail;
    }

    /* shift est, put into network byte order */
#ifdef NO_64BIT_MATH
    est = be64_to_cpu(make64((high32(est) << 16) |
                             (low32(est) >> 16),
                             low32(est) << 16));
#else
    est = be64_to_cpu(est << 16);
#endif

    /*
     * Set the AAD over the RTP header 
     */
    aad_len = (uint8_t *)enc_start - (uint8_t *)hdr;
    status = srtp_cipher_set_aad(stream->rtp_cipher, (uint8_t*)hdr, aad_len);
    if (status) {
        return ( srtp_err_status_cipher_fail);
    }

    /* Encrypt the payload  */
    status = srtp_cipher_encrypt(stream->rtp_cipher,
                            (uint8_t*)enc_start, &enc_octet_len);
    if (status) {
        return srtp_err_status_cipher_fail;
    }
    /*
     * If we're doing GCM, we need to get the tag
     * and append that to the output
     */
    status = srtp_cipher_get_tag(stream->rtp_cipher, 
                            (uint8_t*)enc_start+enc_octet_len, &tag_len);
    if (status) {
	return ( srtp_err_status_cipher_fail);
    }
    enc_octet_len += tag_len;

    /* increase the packet length by the length of the auth tag */
    *pkt_octet_len += tag_len;

    return srtp_err_status_ok;
}


/*
 * This function handles incoming SRTP packets while in AEAD mode,
 * which currently supports AES-GCM encryption.  All packets are
 * encrypted and authenticated.  Note, the auth tag is at the end
 * of the packet stream and is automatically checked by GCM
 * when decrypting the payload.
 */
static srtp_err_status_t
srtp_unprotect_aead (srtp_ctx_t *ctx, srtp_stream_ctx_t *stream, int delta, 
	             srtp_xtd_seq_num_t est, void *srtp_hdr, unsigned int *pkt_octet_len)
{
    srtp_hdr_t *hdr = (srtp_hdr_t*)srtp_hdr;
    uint32_t *enc_start;        /* pointer to start of encrypted portion  */
    unsigned int enc_octet_len = 0; /* number of octets in encrypted portion */
    v128_t iv;
    srtp_err_status_t status;
    int tag_len;
    unsigned int aad_len;

    debug_print(mod_srtp, "function srtp_unprotect_aead", NULL);

#ifdef NO_64BIT_MATH
    debug_print2(mod_srtp, "estimated u_packet index: %08x%08x", high32(est), low32(est));
#else
    debug_print(mod_srtp, "estimated u_packet index: %016llx", est);
#endif

    /* get tag length from stream */
    tag_len = srtp_auth_get_tag_length(stream->rtp_auth);

    /*
     * AEAD uses a new IV formation method 
     */
    srtp_calc_aead_iv(stream, &iv, &est, hdr);
    status = srtp_cipher_set_iv(stream->rtp_cipher, (const uint8_t*)&iv, direction_decrypt);
    if (status) {
        return srtp_err_status_cipher_fail;
    }

    /*
     * find starting point for decryption and length of data to be
     * decrypted - the encrypted portion starts after the rtp header
     * extension, if present; otherwise, it starts after the last csrc,
     * if any are present
     */
    enc_start = (uint32_t*)hdr + uint32s_in_rtp_header + hdr->cc;
    if (hdr->x == 1) {
        srtp_hdr_xtnd_t *xtn_hdr = (srtp_hdr_xtnd_t*)enc_start;
        enc_start += (ntohs(xtn_hdr->length) + 1);
    }
    if (!((uint8_t*)enc_start < (uint8_t*)hdr + *pkt_octet_len))
        return srtp_err_status_parse_err;
    /*
     * We pass the tag down to the cipher when doing GCM mode 
     */
    enc_octet_len = (unsigned int)(*pkt_octet_len - 
                                   ((uint8_t*)enc_start - (uint8_t*)hdr));

    /*
     * Sanity check the encrypted payload length against
     * the tag size.  It must always be at least as large
     * as the tag length.
     */
    if (enc_octet_len < (unsigned int) tag_len) {
        return srtp_err_status_cipher_fail;
    }

    /*
     * update the key usage limit, and check it to make sure that we
     * didn't just hit either the soft limit or the hard limit, and call
     * the event handler if we hit either.
     */
    switch (srtp_key_limit_update(stream->limit)) {
    case srtp_key_event_normal:
        break;
    case srtp_key_event_soft_limit:
        srtp_handle_event(ctx, stream, event_key_soft_limit);
        break;
    case srtp_key_event_hard_limit:
        srtp_handle_event(ctx, stream, event_key_hard_limit);
        return srtp_err_status_key_expired;
    default:
        break;
    }

    /*
     * Set the AAD for AES-GCM, which is the RTP header
     */
    aad_len = (uint8_t *)enc_start - (uint8_t *)hdr;
    status = srtp_cipher_set_aad(stream->rtp_cipher, (uint8_t*)hdr, aad_len);
    if (status) {
        return ( srtp_err_status_cipher_fail);
    }

    /* Decrypt the ciphertext.  This also checks the auth tag based 
     * on the AAD we just specified above */
    status = srtp_cipher_decrypt(stream->rtp_cipher, (uint8_t*)enc_start, &enc_octet_len);
    if (status) {
        return status;
    }

    /*
     * verify that stream is for received traffic - this check will
     * detect SSRC collisions, since a stream that appears in both
     * srtp_protect() and srtp_unprotect() will fail this test in one of
     * those functions.
     *
     * we do this check *after* the authentication check, so that the
     * latter check will catch any attempts to fool us into thinking
     * that we've got a collision
     */
    if (stream->direction != dir_srtp_receiver) {
        if (stream->direction == dir_unknown) {
            stream->direction = dir_srtp_receiver;
        } else {
            srtp_handle_event(ctx, stream, event_ssrc_collision);
        }
    }

    /*
     * if the stream is a 'provisional' one, in which the template context
     * is used, then we need to allocate a new stream at this point, since
     * the authentication passed
     */
    if (stream == ctx->stream_template) {
        srtp_stream_ctx_t *new_stream;

        /*
         * allocate and initialize a new stream
         *
         * note that we indicate failure if we can't allocate the new
         * stream, and some implementations will want to not return
         * failure here
         */
        status = srtp_stream_clone(ctx->stream_template, hdr->ssrc, &new_stream);
        if (status) {
            return status;
        }

        /* add new stream to the head of the stream_list */
        new_stream->next = ctx->stream_list;
        ctx->stream_list = new_stream;

        /* set stream (the pointer used in this function) */
        stream = new_stream;
    }

    /*
     * the message authentication function passed, so add the packet
     * index into the replay database
     */
    srtp_rdbx_add_index(&stream->rtp_rdbx, delta);

    /* decrease the packet length by the length of the auth tag */
    *pkt_octet_len -= tag_len;

    return srtp_err_status_ok;
}




 srtp_err_status_t
 srtp_protect(srtp_ctx_t *ctx, void *rtp_hdr, int *pkt_octet_len) {
   srtp_hdr_t *hdr = (srtp_hdr_t *)rtp_hdr;
   uint32_t *enc_start;        /* pointer to start of encrypted portion  */
   uint32_t *auth_start;       /* pointer to start of auth. portion      */
   unsigned int enc_octet_len = 0; /* number of octets in encrypted portion  */
   srtp_xtd_seq_num_t est;          /* estimated xtd_seq_num_t of *hdr        */
   int delta;                  /* delta of local pkt idx and that in hdr */
   uint8_t *auth_tag = NULL;   /* location of auth_tag within packet     */
   srtp_err_status_t status;   
   int tag_len;
   srtp_stream_ctx_t *stream;
   uint32_t prefix_len;

   debug_print(mod_srtp, "function srtp_protect", NULL);

  /* we assume the hdr is 32-bit aligned to start */

  /* Verify RTP header */
  status = srtp_validate_rtp_header(rtp_hdr, pkt_octet_len);
  if (status)
    return status;

   /* check the packet length - it must at least contain a full header */
   if (*pkt_octet_len < octets_in_rtp_header)
     return srtp_err_status_bad_param;

   /*
    * look up ssrc in srtp_stream list, and process the packet with
    * the appropriate stream.  if we haven't seen this stream before,
    * there's a template key for this srtp_session, and the cipher
    * supports key-sharing, then we assume that a new stream using
    * that key has just started up
    */
   stream = srtp_get_stream(ctx, hdr->ssrc);
   if (stream == NULL) {
     if (ctx->stream_template != NULL) {
       srtp_stream_ctx_t *new_stream;

       /* allocate and initialize a new stream */
       status = srtp_stream_clone(ctx->stream_template, 
				  hdr->ssrc, &new_stream); 
       if (status)
	 return status;

       /* add new stream to the head of the stream_list */
       new_stream->next = ctx->stream_list;
       ctx->stream_list = new_stream;

       /* set direction to outbound */
       new_stream->direction = dir_srtp_sender;

       /* set stream (the pointer used in this function) */
       stream = new_stream;
     } else {
       /* no template stream, so we return an error */
       return srtp_err_status_no_ctx;
     } 
   }

   /* 
    * verify that stream is for sending traffic - this check will
    * detect SSRC collisions, since a stream that appears in both
    * srtp_protect() and srtp_unprotect() will fail this test in one of
    * those functions.
    */
  if (stream->direction != dir_srtp_sender) {
     if (stream->direction == dir_unknown) {
       stream->direction = dir_srtp_sender;
     } else {
       srtp_handle_event(ctx, stream, event_ssrc_collision);
     }
  }

   /*
    * Check if this is an AEAD stream (GCM mode).  If so, then dispatch
    * the request to our AEAD handler.
    */
  if (stream->rtp_cipher->algorithm == SRTP_AES_128_GCM ||
      stream->rtp_cipher->algorithm == SRTP_AES_256_GCM) {
      return srtp_protect_aead(ctx, stream, rtp_hdr, (unsigned int*)pkt_octet_len);
  }

  /* 
   * update the key usage limit, and check it to make sure that we
   * didn't just hit either the soft limit or the hard limit, and call
   * the event handler if we hit either.
   */
  switch(srtp_key_limit_update(stream->limit)) {
  case srtp_key_event_normal:
    break;
  case srtp_key_event_soft_limit: 
    srtp_handle_event(ctx, stream, event_key_soft_limit);
    break; 
  case srtp_key_event_hard_limit:
    srtp_handle_event(ctx, stream, event_key_hard_limit);
	return srtp_err_status_key_expired;
  default:
    break;
  }

   /* get tag length from stream */
   tag_len = srtp_auth_get_tag_length(stream->rtp_auth); 

   /*
    * find starting point for encryption and length of data to be
    * encrypted - the encrypted portion starts after the rtp header
    * extension, if present; otherwise, it starts after the last csrc,
    * if any are present
    *
    * if we're not providing confidentiality, set enc_start to NULL
    */
   if (stream->rtp_services & sec_serv_conf) {
     enc_start = (uint32_t *)hdr + uint32s_in_rtp_header + hdr->cc;  
     if (hdr->x == 1) {
       srtp_hdr_xtnd_t *xtn_hdr = (srtp_hdr_xtnd_t *)enc_start;
       enc_start += (ntohs(xtn_hdr->length) + 1);
       if (!((uint8_t*)enc_start < (uint8_t*)hdr + *pkt_octet_len))
         return srtp_err_status_parse_err;
     }
     enc_octet_len = (unsigned int)(*pkt_octet_len -
                                    ((uint8_t*)enc_start - (uint8_t*)hdr));
   } else {
     enc_start = NULL;
   }

   /* 
    * if we're providing authentication, set the auth_start and auth_tag
    * pointers to the proper locations; otherwise, set auth_start to NULL
    * to indicate that no authentication is needed
    */
   if (stream->rtp_services & sec_serv_auth) {
     auth_start = (uint32_t *)hdr;
     auth_tag = (uint8_t *)hdr + *pkt_octet_len;
   } else {
     auth_start = NULL;
     auth_tag = NULL;
   }

   /*
    * estimate the packet index using the start of the replay window   
    * and the sequence number from the header
    */
   delta = srtp_rdbx_estimate_index(&stream->rtp_rdbx, &est, ntohs(hdr->seq));
   status = srtp_rdbx_check(&stream->rtp_rdbx, delta);
   if (status) {
     if (status != srtp_err_status_replay_fail || !stream->allow_repeat_tx)
       return status;  /* we've been asked to reuse an index */
   }
   else
     srtp_rdbx_add_index(&stream->rtp_rdbx, delta);

#ifdef NO_64BIT_MATH
   debug_print2(mod_srtp, "estimated packet index: %08x%08x", 
		high32(est),low32(est));
#else
   debug_print(mod_srtp, "estimated packet index: %016llx", est);
#endif

   /* 
    * if we're using rindael counter mode, set nonce and seq 
    */
   if (stream->rtp_cipher->type->id == SRTP_AES_ICM ||
       stream->rtp_cipher->type->id == SRTP_AES_256_ICM) {
     v128_t iv;

     iv.v32[0] = 0;
     iv.v32[1] = hdr->ssrc;
#ifdef NO_64BIT_MATH
     iv.v64[1] = be64_to_cpu(make64((high32(est) << 16) | (low32(est) >> 16),
								 low32(est) << 16));
#else
     iv.v64[1] = be64_to_cpu(est << 16);
#endif
     status = srtp_cipher_set_iv(stream->rtp_cipher, (const uint8_t*)&iv, direction_encrypt);

   } else {  
     v128_t iv;

     /* otherwise, set the index to est */  
#ifdef NO_64BIT_MATH
     iv.v32[0] = 0;
     iv.v32[1] = 0;
#else
     iv.v64[0] = 0;
#endif
     iv.v64[1] = be64_to_cpu(est);
     status = srtp_cipher_set_iv(stream->rtp_cipher, (const uint8_t*)&iv, direction_encrypt);
   }
   if (status)
     return srtp_err_status_cipher_fail;

   /* shift est, put into network byte order */
#ifdef NO_64BIT_MATH
   est = be64_to_cpu(make64((high32(est) << 16) |
						 (low32(est) >> 16),
						 low32(est) << 16));
#else
   est = be64_to_cpu(est << 16);
#endif
   
   /* 
    * if we're authenticating using a universal hash, put the keystream
    * prefix into the authentication tag
    */
   if (auth_start) {
     
    prefix_len = srtp_auth_get_prefix_length(stream->rtp_auth);    
    if (prefix_len) {
      status = srtp_cipher_output(stream->rtp_cipher, auth_tag, &prefix_len);
      if (status)
	return srtp_err_status_cipher_fail;
      debug_print(mod_srtp, "keystream prefix: %s", 
		  srtp_octet_string_hex_string(auth_tag, prefix_len));
    }
  }

  /* if we're encrypting, exor keystream into the message */
  if (enc_start) {
    status = srtp_cipher_encrypt(stream->rtp_cipher, 
			        (uint8_t *)enc_start, &enc_octet_len);
    if (status)
      return srtp_err_status_cipher_fail;
  }

  /*
   *  if we're authenticating, run authentication function and put result
   *  into the auth_tag 
   */
  if (auth_start) {        

    /* initialize auth func context */
    status = auth_start(stream->rtp_auth);
    if (status) return status;

    /* run auth func over packet */
    status = auth_update(stream->rtp_auth, 
			 (uint8_t *)auth_start, *pkt_octet_len);
    if (status) return status;
    
    /* run auth func over ROC, put result into auth_tag */
    debug_print(mod_srtp, "estimated packet index: %016llx", est);
    status = auth_compute(stream->rtp_auth, (uint8_t *)&est, 4, auth_tag); 
    debug_print(mod_srtp, "srtp auth tag:    %s", 
		srtp_octet_string_hex_string(auth_tag, tag_len));
    if (status)
      return srtp_err_status_auth_fail;   

  }

  if (auth_tag) {

    /* increase the packet length by the length of the auth tag */
    *pkt_octet_len += tag_len;
  }

  return srtp_err_status_ok;  
}


srtp_err_status_t
srtp_unprotect(srtp_ctx_t *ctx, void *srtp_hdr, int *pkt_octet_len) {
  srtp_hdr_t *hdr = (srtp_hdr_t *)srtp_hdr;
  uint32_t *enc_start;      /* pointer to start of encrypted portion  */
  uint32_t *auth_start;     /* pointer to start of auth. portion      */
  unsigned int enc_octet_len = 0;/* number of octets in encrypted portion */
  uint8_t *auth_tag = NULL; /* location of auth_tag within packet     */
  srtp_xtd_seq_num_t est;        /* estimated xtd_seq_num_t of *hdr        */
  int delta;                /* delta of local pkt idx and that in hdr */
  v128_t iv;
  srtp_err_status_t status;
  srtp_stream_ctx_t *stream;
  uint8_t tmp_tag[SRTP_MAX_TAG_LEN];
  uint32_t tag_len, prefix_len;

  debug_print(mod_srtp, "function srtp_unprotect", NULL);

  /* we assume the hdr is 32-bit aligned to start */

  /* Verify RTP header */
  status = srtp_validate_rtp_header(srtp_hdr, pkt_octet_len);
  if (status)
    return status;

  /* check the packet length - it must at least contain a full header */
  if (*pkt_octet_len < octets_in_rtp_header)
    return srtp_err_status_bad_param;

  /*
   * look up ssrc in srtp_stream list, and process the packet with 
   * the appropriate stream.  if we haven't seen this stream before,
   * there's only one key for this srtp_session, and the cipher
   * supports key-sharing, then we assume that a new stream using
   * that key has just started up
   */
  stream = srtp_get_stream(ctx, hdr->ssrc);
  if (stream == NULL) {
    if (ctx->stream_template != NULL) {
      stream = ctx->stream_template;
      debug_print(mod_srtp, "using provisional stream (SSRC: 0x%08x)",
		  hdr->ssrc);
      
      /* 
       * set estimated packet index to sequence number from header,
       * and set delta equal to the same value
       */
#ifdef NO_64BIT_MATH
      est = (srtp_xtd_seq_num_t) make64(0,ntohs(hdr->seq));
      delta = low32(est);
#else
      est = (srtp_xtd_seq_num_t) ntohs(hdr->seq);
      delta = (int)est;
#endif
    } else {
      
      /*
       * no stream corresponding to SSRC found, and we don't do
       * key-sharing, so return an error
       */
      return srtp_err_status_no_ctx;
    }
  } else {
  
    /* estimate packet index from seq. num. in header */
    delta = srtp_rdbx_estimate_index(&stream->rtp_rdbx, &est, ntohs(hdr->seq));
    
    /* check replay database */
    status = srtp_rdbx_check(&stream->rtp_rdbx, delta);
    if (status)
      return status;
  }

#ifdef NO_64BIT_MATH
  debug_print2(mod_srtp, "estimated u_packet index: %08x%08x", high32(est),low32(est));
#else
  debug_print(mod_srtp, "estimated u_packet index: %016llx", est);
#endif

  /*
   * Check if this is an AEAD stream (GCM mode).  If so, then dispatch
   * the request to our AEAD handler.
   */
  if (stream->rtp_cipher->algorithm == SRTP_AES_128_GCM ||
      stream->rtp_cipher->algorithm == SRTP_AES_256_GCM) {
      return srtp_unprotect_aead(ctx, stream, delta, est, srtp_hdr, (unsigned int*)pkt_octet_len);
  }

  /* get tag length from stream */
  tag_len = srtp_auth_get_tag_length(stream->rtp_auth); 

  /* 
   * set the cipher's IV properly, depending on whatever cipher we
   * happen to be using
   */
  if (stream->rtp_cipher->type->id == SRTP_AES_ICM ||
      stream->rtp_cipher->type->id == SRTP_AES_256_ICM) {

    /* aes counter mode */
    iv.v32[0] = 0;
    iv.v32[1] = hdr->ssrc;  /* still in network order */
#ifdef NO_64BIT_MATH
    iv.v64[1] = be64_to_cpu(make64((high32(est) << 16) | (low32(est) >> 16),
			         low32(est) << 16));
#else
    iv.v64[1] = be64_to_cpu(est << 16);
#endif
    status = srtp_cipher_set_iv(stream->rtp_cipher, (const uint8_t*)&iv, direction_decrypt);
  } else {  
    
    /* no particular format - set the iv to the pakcet index */  
#ifdef NO_64BIT_MATH
    iv.v32[0] = 0;
    iv.v32[1] = 0;
#else
    iv.v64[0] = 0;
#endif
    iv.v64[1] = be64_to_cpu(est);
    status = srtp_cipher_set_iv(stream->rtp_cipher, (const uint8_t*)&iv, direction_decrypt);
  }
  if (status)
    return srtp_err_status_cipher_fail;

  /* shift est, put into network byte order */
#ifdef NO_64BIT_MATH
  est = be64_to_cpu(make64((high32(est) << 16) |
					    (low32(est) >> 16),
					    low32(est) << 16));
#else
  est = be64_to_cpu(est << 16);
#endif

  /*
   * find starting point for decryption and length of data to be
   * decrypted - the encrypted portion starts after the rtp header
   * extension, if present; otherwise, it starts after the last csrc,
   * if any are present
   *
   * if we're not providing confidentiality, set enc_start to NULL
   */
  if (stream->rtp_services & sec_serv_conf) {
    enc_start = (uint32_t *)hdr + uint32s_in_rtp_header + hdr->cc;  
    if (hdr->x == 1) {
      srtp_hdr_xtnd_t *xtn_hdr = (srtp_hdr_xtnd_t *)enc_start;
      enc_start += (ntohs(xtn_hdr->length) + 1);
    }  
    if (!((uint8_t*)enc_start < (uint8_t*)hdr + *pkt_octet_len))
      return srtp_err_status_parse_err;
    enc_octet_len = (uint32_t)(*pkt_octet_len - tag_len -
                               ((uint8_t*)enc_start - (uint8_t*)hdr));
  } else {
    enc_start = NULL;
  }

  /* 
   * if we're providing authentication, set the auth_start and auth_tag
   * pointers to the proper locations; otherwise, set auth_start to NULL
   * to indicate that no authentication is needed
   */
  if (stream->rtp_services & sec_serv_auth) {
    auth_start = (uint32_t *)hdr;
    auth_tag = (uint8_t *)hdr + *pkt_octet_len - tag_len;
  } else {
    auth_start = NULL;
    auth_tag = NULL;
  } 

  /*
   * if we expect message authentication, run the authentication
   * function and compare the result with the value of the auth_tag
   */
  if (auth_start) {        

    /* 
     * if we're using a universal hash, then we need to compute the
     * keystream prefix for encrypting the universal hash output
     *
     * if the keystream prefix length is zero, then we know that
     * the authenticator isn't using a universal hash function
     */  
    if (stream->rtp_auth->prefix_len != 0) {
      
      prefix_len = srtp_auth_get_prefix_length(stream->rtp_auth);    
      status = srtp_cipher_output(stream->rtp_cipher, tmp_tag, &prefix_len);
      debug_print(mod_srtp, "keystream prefix: %s", 
		  srtp_octet_string_hex_string(tmp_tag, prefix_len));
      if (status)
	return srtp_err_status_cipher_fail;
    } 

    /* initialize auth func context */
    status = auth_start(stream->rtp_auth);
    if (status) return status;
 
    /* now compute auth function over packet */
    status = auth_update(stream->rtp_auth, (uint8_t *)auth_start,  
			 *pkt_octet_len - tag_len);

    /* run auth func over ROC, then write tmp tag */
    status = auth_compute(stream->rtp_auth, (uint8_t *)&est, 4, tmp_tag);  

    debug_print(mod_srtp, "computed auth tag:    %s", 
		srtp_octet_string_hex_string(tmp_tag, tag_len));
    debug_print(mod_srtp, "packet auth tag:      %s", 
		srtp_octet_string_hex_string(auth_tag, tag_len));
    if (status)
      return srtp_err_status_auth_fail;   

    if (octet_string_is_eq(tmp_tag, auth_tag, tag_len))
      return srtp_err_status_auth_fail;
  }

  /* 
   * update the key usage limit, and check it to make sure that we
   * didn't just hit either the soft limit or the hard limit, and call
   * the event handler if we hit either.
   */
  switch(srtp_key_limit_update(stream->limit)) {
  case srtp_key_event_normal:
    break;
  case srtp_key_event_soft_limit: 
    srtp_handle_event(ctx, stream, event_key_soft_limit);
    break; 
  case srtp_key_event_hard_limit:
    srtp_handle_event(ctx, stream, event_key_hard_limit);
    return srtp_err_status_key_expired;
  default:
    break;
  }

  /* if we're decrypting, add keystream into ciphertext */
  if (enc_start) {
    status = srtp_cipher_decrypt(stream->rtp_cipher, (uint8_t *)enc_start, &enc_octet_len);
    if (status)
      return srtp_err_status_cipher_fail;
  }

  /* 
   * verify that stream is for received traffic - this check will
   * detect SSRC collisions, since a stream that appears in both
   * srtp_protect() and srtp_unprotect() will fail this test in one of
   * those functions.
   *
   * we do this check *after* the authentication check, so that the
   * latter check will catch any attempts to fool us into thinking
   * that we've got a collision
   */
  if (stream->direction != dir_srtp_receiver) {
    if (stream->direction == dir_unknown) {
      stream->direction = dir_srtp_receiver;
    } else {
      srtp_handle_event(ctx, stream, event_ssrc_collision);
    }
  }

  /* 
   * if the stream is a 'provisional' one, in which the template context
   * is used, then we need to allocate a new stream at this point, since
   * the authentication passed
   */
  if (stream == ctx->stream_template) {  
    srtp_stream_ctx_t *new_stream;

    /* 
     * allocate and initialize a new stream 
     * 
     * note that we indicate failure if we can't allocate the new
     * stream, and some implementations will want to not return
     * failure here
     */
    status = srtp_stream_clone(ctx->stream_template, hdr->ssrc, &new_stream); 
    if (status)
      return status;
    
    /* add new stream to the head of the stream_list */
    new_stream->next = ctx->stream_list;
    ctx->stream_list = new_stream;
    
    /* set stream (the pointer used in this function) */
    stream = new_stream;
  }
  
  /* 
   * the message authentication function passed, so add the packet
   * index into the replay database 
   */
  srtp_rdbx_add_index(&stream->rtp_rdbx, delta);

  /* decrease the packet length by the length of the auth tag */
  *pkt_octet_len -= tag_len;

  return srtp_err_status_ok;  
}

srtp_err_status_t
srtp_init() {
  srtp_err_status_t status;

  /* initialize crypto kernel */
  status = srtp_crypto_kernel_init();
  if (status) 
    return status;

  /* load srtp debug module into the kernel */
  status = srtp_crypto_kernel_load_debug_module(&mod_srtp);
  if (status)
    return status;

  return srtp_err_status_ok;
}

srtp_err_status_t
srtp_shutdown() {
  srtp_err_status_t status;

  /* shut down crypto kernel */
  status = srtp_crypto_kernel_shutdown();
  if (status) 
    return status;

  /* shutting down crypto kernel frees the srtp debug module as well */

  return srtp_err_status_ok;
}


/* 
 * The following code is under consideration for removal.  See
 * SRTP_MAX_TRAILER_LEN 
 */
#if 0

/*
 * srtp_get_trailer_length(&a) returns the number of octets that will
 * be added to an RTP packet by the SRTP processing.  This value
 * is constant for a given srtp_stream_t (i.e. between initializations).
 */

int
srtp_get_trailer_length(const srtp_stream_t s) {
  return srtp_auth_get_tag_length(s->rtp_auth);
}

#endif

/*
 * srtp_get_stream(ssrc) returns a pointer to the stream corresponding
 * to ssrc, or NULL if no stream exists for that ssrc
 *
 * this is an internal function 
 */

srtp_stream_ctx_t *
srtp_get_stream(srtp_t srtp, uint32_t ssrc) {
  srtp_stream_ctx_t *stream;

  /* walk down list until ssrc is found */
  stream = srtp->stream_list;
  while (stream != NULL) {
    if (stream->ssrc == ssrc)
      return stream;
    stream = stream->next;
  }
  
  /* we haven't found our ssrc, so return a null */
  return NULL;
}

srtp_err_status_t
srtp_dealloc(srtp_t session) {
  srtp_stream_ctx_t *stream;
  srtp_err_status_t status;

  /*
   * we take a conservative deallocation strategy - if we encounter an
   * error deallocating a stream, then we stop trying to deallocate
   * memory and just return an error
   */

  /* walk list of streams, deallocating as we go */
  stream = session->stream_list;
  while (stream != NULL) {
    srtp_stream_t next = stream->next;
    status = srtp_stream_dealloc(session, stream);
    if (status)
      return status;
    stream = next;
  }
  
  /* deallocate stream template, if there is one */
  if (session->stream_template != NULL) {
    status = auth_dealloc(session->stream_template->rtcp_auth); 
    if (status) 
      return status; 
    status = srtp_cipher_dealloc(session->stream_template->rtcp_cipher); 
    if (status) 
      return status; 
    srtp_crypto_free(session->stream_template->limit);
    status = srtp_cipher_dealloc(session->stream_template->rtp_cipher); 
    if (status) 
      return status; 
    status = auth_dealloc(session->stream_template->rtp_auth);
    if (status)
      return status;
    status = srtp_rdbx_dealloc(&session->stream_template->rtp_rdbx);
    if (status)
      return status;
    srtp_crypto_free(session->stream_template);
  }

  /* deallocate session context */
  srtp_crypto_free(session);

  return srtp_err_status_ok;
}


srtp_err_status_t
srtp_add_stream(srtp_t session, 
		const srtp_policy_t *policy)  {
  srtp_err_status_t status;
  srtp_stream_t tmp;

  /* sanity check arguments */
  if ((session == NULL) || (policy == NULL) || (policy->key == NULL))
    return srtp_err_status_bad_param;

  /* allocate stream  */
  status = srtp_stream_alloc(&tmp, policy);
  if (status) {
    return status;
  }
  
  /* initialize stream  */
  status = srtp_stream_init(tmp, policy);
  if (status) {
    srtp_crypto_free(tmp);
    return status;
  }
  
  /* 
   * set the head of the stream list or the template to point to the
   * stream that we've just alloced and init'ed, depending on whether
   * or not it has a wildcard SSRC value or not
   *
   * if the template stream has already been set, then the policy is
   * inconsistent, so we return a bad_param error code
   */
  switch (policy->ssrc.type) {
  case (ssrc_any_outbound):
    if (session->stream_template) {
      return srtp_err_status_bad_param;
    }
    session->stream_template = tmp;
    session->stream_template->direction = dir_srtp_sender;
    break;
  case (ssrc_any_inbound):
    if (session->stream_template) {
      return srtp_err_status_bad_param;
    }
    session->stream_template = tmp;
    session->stream_template->direction = dir_srtp_receiver;
    break;
  case (ssrc_specific):
    tmp->next = session->stream_list;
    session->stream_list = tmp;
    break;
  case (ssrc_undefined):
  default:
    srtp_crypto_free(tmp);
    return srtp_err_status_bad_param;
  }
    
  return srtp_err_status_ok;
}


srtp_err_status_t
srtp_create(srtp_t *session,               /* handle for session     */ 
	    const srtp_policy_t *policy) { /* SRTP policy (list)     */
  srtp_err_status_t stat;
  srtp_ctx_t *ctx;

  /* sanity check arguments */
  if (session == NULL)
    return srtp_err_status_bad_param;

  /* allocate srtp context and set ctx_ptr */
  ctx = (srtp_ctx_t *) srtp_crypto_alloc(sizeof(srtp_ctx_t));
  if (ctx == NULL)
    return srtp_err_status_alloc_fail;
  *session = ctx;

  /* 
   * loop over elements in the policy list, allocating and
   * initializing a stream for each element
   */
  ctx->stream_template = NULL;
  ctx->stream_list = NULL;
  ctx->user_data = NULL;
  while (policy != NULL) {    

    stat = srtp_add_stream(ctx, policy);
    if (stat) {
      /* clean up everything */
      srtp_dealloc(*session);
      return stat;
    }    

    /* set policy to next item in list  */
    policy = policy->next;
  }

  return srtp_err_status_ok;
}


srtp_err_status_t
srtp_remove_stream(srtp_t session, uint32_t ssrc) {
  srtp_stream_ctx_t *stream, *last_stream;
  srtp_err_status_t status;

  /* sanity check arguments */
  if (session == NULL)
    return srtp_err_status_bad_param;

  /* will be compared against values in network order in the stream list */
  ssrc = htonl(ssrc);
  
  /* find stream in list; complain if not found */
  last_stream = stream = session->stream_list;
  while ((stream != NULL) && (ssrc != stream->ssrc)) {
    last_stream = stream;
    stream = stream->next;
  }
  if (stream == NULL)
    return srtp_err_status_no_ctx;

  /* remove stream from the list */
  if (last_stream == stream)
    /* stream was first in list */
    session->stream_list = stream->next;
  else
    last_stream->next = stream->next;

  /* deallocate the stream */
  status = srtp_stream_dealloc(session, stream);
  if (status)
    return status;

  return srtp_err_status_ok;
}


/*
 * the default policy - provides a convenient way for callers to use
 * the default security policy
 * 
 * this policy is that defined in the current SRTP internet draft.
 *
 */

/* 
 * NOTE: cipher_key_len is really key len (128 bits) plus salt len
 *  (112 bits)
 */
/* There are hard-coded 16's for base_key_len in the key generation code */

void
srtp_crypto_policy_set_rtp_default(srtp_crypto_policy_t *p) {

  p->cipher_type     = SRTP_AES_ICM;           
  p->cipher_key_len  = 30;                /* default 128 bits per RFC 3711 */
  p->auth_type       = SRTP_HMAC_SHA1;             
  p->auth_key_len    = 20;                /* default 160 bits per RFC 3711 */
  p->auth_tag_len    = 10;                /* default 80 bits per RFC 3711 */
  p->sec_serv        = sec_serv_conf_and_auth;
  
}

void
srtp_crypto_policy_set_rtcp_default(srtp_crypto_policy_t *p) {

  p->cipher_type     = SRTP_AES_ICM;           
  p->cipher_key_len  = 30;                 /* default 128 bits per RFC 3711 */
  p->auth_type       = SRTP_HMAC_SHA1;             
  p->auth_key_len    = 20;                 /* default 160 bits per RFC 3711 */
  p->auth_tag_len    = 10;                 /* default 80 bits per RFC 3711 */
  p->sec_serv        = sec_serv_conf_and_auth;
  
}

void
srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(srtp_crypto_policy_t *p) {

  /*
   * corresponds to RFC 4568
   *
   * note that this crypto policy is intended for SRTP, but not SRTCP
   */

  p->cipher_type     = SRTP_AES_ICM;           
  p->cipher_key_len  = 30;                /* 128 bit key, 112 bit salt */
  p->auth_type       = SRTP_HMAC_SHA1;             
  p->auth_key_len    = 20;                /* 160 bit key               */
  p->auth_tag_len    = 4;                 /* 32 bit tag                */
  p->sec_serv        = sec_serv_conf_and_auth;
  
}


void
srtp_crypto_policy_set_aes_cm_128_null_auth(srtp_crypto_policy_t *p) {

  /*
   * corresponds to RFC 4568
   *
   * note that this crypto policy is intended for SRTP, but not SRTCP
   */

  p->cipher_type     = SRTP_AES_ICM;           
  p->cipher_key_len  = 30;                /* 128 bit key, 112 bit salt */
  p->auth_type       = SRTP_NULL_AUTH;             
  p->auth_key_len    = 0; 
  p->auth_tag_len    = 0; 
  p->sec_serv        = sec_serv_conf;
  
}


void
srtp_crypto_policy_set_null_cipher_hmac_sha1_80(srtp_crypto_policy_t *p) {

  /*
   * corresponds to RFC 4568
   */

  p->cipher_type     = SRTP_NULL_CIPHER;           
  p->cipher_key_len  = 0;
  p->auth_type       = SRTP_HMAC_SHA1;             
  p->auth_key_len    = 20; 
  p->auth_tag_len    = 10; 
  p->sec_serv        = sec_serv_auth;
  
}

void
srtp_crypto_policy_set_null_cipher_hmac_null(srtp_crypto_policy_t *p) {

  /*
   * Should only be used for testing
   */

  p->cipher_type     = SRTP_NULL_CIPHER;           
  p->cipher_key_len  = 0;
  p->auth_type       = SRTP_NULL_AUTH;             
  p->auth_key_len    = 0; 
  p->auth_tag_len    = 0; 
  p->sec_serv        = sec_serv_none;
  
}


void
srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(srtp_crypto_policy_t *p) {

  /*
   * corresponds to draft-ietf-avt-big-aes-03.txt
   */

  p->cipher_type     = SRTP_AES_ICM;           
  p->cipher_key_len  = 46;
  p->auth_type       = SRTP_HMAC_SHA1;             
  p->auth_key_len    = 20;                /* default 160 bits per RFC 3711 */
  p->auth_tag_len    = 10;                /* default 80 bits per RFC 3711 */
  p->sec_serv        = sec_serv_conf_and_auth;
}


void
srtp_crypto_policy_set_aes_cm_256_hmac_sha1_32(srtp_crypto_policy_t *p) {

  /*
   * corresponds to draft-ietf-avt-big-aes-03.txt
   *
   * note that this crypto policy is intended for SRTP, but not SRTCP
   */

  p->cipher_type     = SRTP_AES_ICM;           
  p->cipher_key_len  = 46;
  p->auth_type       = SRTP_HMAC_SHA1;             
  p->auth_key_len    = 20;                /* default 160 bits per RFC 3711 */
  p->auth_tag_len    = 4;                 /* default 80 bits per RFC 3711 */
  p->sec_serv        = sec_serv_conf_and_auth;
}

/*
 * AES-256 with no authentication.
 */
void
srtp_crypto_policy_set_aes_cm_256_null_auth (srtp_crypto_policy_t *p)
{
    p->cipher_type     = SRTP_AES_ICM;
    p->cipher_key_len  = 46;
    p->auth_type       = SRTP_NULL_AUTH;
    p->auth_key_len    = 0;
    p->auth_tag_len    = 0;
    p->sec_serv        = sec_serv_conf;
}

#ifdef OPENSSL
/*
 * AES-128 GCM mode with 8 octet auth tag. 
 */
void
srtp_crypto_policy_set_aes_gcm_128_8_auth(srtp_crypto_policy_t *p) {
  p->cipher_type     = SRTP_AES_128_GCM;           
  p->cipher_key_len  = SRTP_AES_128_GCM_KEYSIZE_WSALT; 
  p->auth_type       = SRTP_NULL_AUTH; /* GCM handles the auth for us */            
  p->auth_key_len    = 0; 
  p->auth_tag_len    = 8;   /* 8 octet tag length */
  p->sec_serv        = sec_serv_conf_and_auth;
}

/*
 * AES-256 GCM mode with 8 octet auth tag. 
 */
void
srtp_crypto_policy_set_aes_gcm_256_8_auth(srtp_crypto_policy_t *p) {
  p->cipher_type     = SRTP_AES_256_GCM;           
  p->cipher_key_len  = SRTP_AES_256_GCM_KEYSIZE_WSALT; 
  p->auth_type       = SRTP_NULL_AUTH; /* GCM handles the auth for us */ 
  p->auth_key_len    = 0; 
  p->auth_tag_len    = 8;   /* 8 octet tag length */
  p->sec_serv        = sec_serv_conf_and_auth;
}

/*
 * AES-128 GCM mode with 8 octet auth tag, no RTCP encryption. 
 */
void
srtp_crypto_policy_set_aes_gcm_128_8_only_auth(srtp_crypto_policy_t *p) {
  p->cipher_type     = SRTP_AES_128_GCM;           
  p->cipher_key_len  = SRTP_AES_128_GCM_KEYSIZE_WSALT; 
  p->auth_type       = SRTP_NULL_AUTH; /* GCM handles the auth for us */ 
  p->auth_key_len    = 0; 
  p->auth_tag_len    = 8;   /* 8 octet tag length */
  p->sec_serv        = sec_serv_auth;  /* This only applies to RTCP */
}

/*
 * AES-256 GCM mode with 8 octet auth tag, no RTCP encryption. 
 */
void
srtp_crypto_policy_set_aes_gcm_256_8_only_auth(srtp_crypto_policy_t *p) {
  p->cipher_type     = SRTP_AES_256_GCM;           
  p->cipher_key_len  = SRTP_AES_256_GCM_KEYSIZE_WSALT; 
  p->auth_type       = SRTP_NULL_AUTH; /* GCM handles the auth for us */ 
  p->auth_key_len    = 0; 
  p->auth_tag_len    = 8;   /* 8 octet tag length */
  p->sec_serv        = sec_serv_auth;  /* This only applies to RTCP */
}

/*
 * AES-128 GCM mode with 16 octet auth tag. 
 */
void
srtp_crypto_policy_set_aes_gcm_128_16_auth(srtp_crypto_policy_t *p) {
  p->cipher_type     = SRTP_AES_128_GCM;           
  p->cipher_key_len  = SRTP_AES_128_GCM_KEYSIZE_WSALT; 
  p->auth_type       = SRTP_NULL_AUTH; /* GCM handles the auth for us */            
  p->auth_key_len    = 0; 
  p->auth_tag_len    = 16;   /* 16 octet tag length */
  p->sec_serv        = sec_serv_conf_and_auth;
}

/*
 * AES-256 GCM mode with 16 octet auth tag. 
 */
void
srtp_crypto_policy_set_aes_gcm_256_16_auth(srtp_crypto_policy_t *p) {
  p->cipher_type     = SRTP_AES_256_GCM;           
  p->cipher_key_len  = SRTP_AES_256_GCM_KEYSIZE_WSALT; 
  p->auth_type       = SRTP_NULL_AUTH; /* GCM handles the auth for us */ 
  p->auth_key_len    = 0; 
  p->auth_tag_len    = 16;   /* 16 octet tag length */
  p->sec_serv        = sec_serv_conf_and_auth;
}

#endif

/* 
 * secure rtcp functions
 */

/*
 * AEAD uses a new IV formation method.  This function implements
 * section 10.1 from draft-ietf-avtcore-srtp-aes-gcm-07.txt.  The
 * calculation is defined as, where (+) is the xor operation:
 *
 *                0  1  2  3  4  5  6  7  8  9 10 11
 *               +--+--+--+--+--+--+--+--+--+--+--+--+
 *               |00|00|    SSRC   |00|00|0+SRTCP Idx|---+
 *               +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *                                                       |
 *               +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *               |         Encryption Salt           |->(+)
 *               +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *                                                       |
 *               +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *               |       Initialization Vector       |<--+
 *               +--+--+--+--+--+--+--+--+--+--+--+--+*
 *
 * Input:  *stream - pointer to SRTP stream context, used to retrieve
 *                   the SALT 
 *         *iv     - Pointer to recieve the calculated IV
 *         seq_num - The SEQ value to use for the IV calculation.
 *         *hdr    - The RTP header, used to get the SSRC value
 *
 */
static void srtp_calc_aead_iv_srtcp(srtp_stream_ctx_t *stream, v128_t *iv, 
                                    uint32_t seq_num, srtcp_hdr_t *hdr)
{
    v128_t	in;
    v128_t	salt;

    memset(&in, 0, sizeof(v128_t));
    memset(&salt, 0, sizeof(v128_t));

    in.v16[0] = 0;
    memcpy(&in.v16[1], &hdr->ssrc, 4); /* still in network order! */
    in.v16[3] = 0;
    in.v32[2] = 0x7FFFFFFF & htonl(seq_num); /* bit 32 is suppose to be zero */

    debug_print(mod_srtp, "Pre-salted RTCP IV = %s\n", v128_hex_string(&in));

    /*
     * Get the SALT value from the context
     */
    memcpy(salt.v8, stream->c_salt, 12);
    debug_print(mod_srtp, "RTCP SALT = %s\n", v128_hex_string(&salt));

    /*
     * Finally, apply the SALT to the input
     */
    v128_xor(iv, &in, &salt);
}

/*
 * This code handles AEAD ciphers for outgoing RTCP.  We currently support
 * AES-GCM mode with 128 or 256 bit keys. 
 */
static srtp_err_status_t
srtp_protect_rtcp_aead (srtp_t ctx, srtp_stream_ctx_t *stream, 
                        void *rtcp_hdr, unsigned int *pkt_octet_len)
{
    srtcp_hdr_t *hdr = (srtcp_hdr_t*)rtcp_hdr;
    uint32_t *enc_start;        /* pointer to start of encrypted portion  */
    uint32_t *trailer;          /* pointer to start of trailer            */
    unsigned int enc_octet_len = 0; /* number of octets in encrypted portion */
    uint8_t *auth_tag = NULL;   /* location of auth_tag within packet     */
    srtp_err_status_t status;
    uint32_t tag_len;
    uint32_t seq_num;
    v128_t iv;
    uint32_t tseq;

    /* get tag length from stream context */
    tag_len = srtp_auth_get_tag_length(stream->rtcp_auth);

    /*
     * set encryption start and encryption length - if we're not
     * providing confidentiality, set enc_start to NULL
     */
    enc_start = (uint32_t*)hdr + uint32s_in_rtcp_header;
    enc_octet_len = *pkt_octet_len - octets_in_rtcp_header;

    /* NOTE: hdr->length is not usable - it refers to only the first
           RTCP report in the compound packet! */
    /* NOTE: trailer is 32-bit aligned because RTCP 'packets' are always
           multiples of 32-bits (RFC 3550 6.1) */
    trailer = (uint32_t*)((char*)enc_start + enc_octet_len + tag_len);

    if (stream->rtcp_services & sec_serv_conf) {
        *trailer = htonl(SRTCP_E_BIT); /* set encrypt bit */
    } else {
        enc_start = NULL;
        enc_octet_len = 0;
        /* 0 is network-order independant */
        *trailer = 0x00000000; /* set encrypt bit */
    }

    /*
     * set the auth_tag pointer to the proper location, which is after
     * the payload, but before the trailer
     * (note that srtpc *always* provides authentication, unlike srtp)
     */
    /* Note: This would need to change for optional mikey data */
    auth_tag = (uint8_t*)hdr + *pkt_octet_len;

    /*
     * check sequence number for overruns, and copy it into the packet
     * if its value isn't too big
     */
    status = srtp_rdb_increment(&stream->rtcp_rdb);
    if (status) {
        return status;
    }
    seq_num = srtp_rdb_get_value(&stream->rtcp_rdb);
    *trailer |= htonl(seq_num);
    debug_print(mod_srtp, "srtcp index: %x", seq_num);

    /*
     * Calculating the IV and pass it down to the cipher 
     */
    srtp_calc_aead_iv_srtcp(stream, &iv, seq_num, hdr);
    status = srtp_cipher_set_iv(stream->rtcp_cipher, (const uint8_t*)&iv, direction_encrypt);
    if (status) {
        return srtp_err_status_cipher_fail;
    }

    /*
     * Set the AAD for GCM mode
     */
    if (enc_start) {
	/*
	 * If payload encryption is enabled, then the AAD consist of
	 * the RTCP header and the seq# at the end of the packet
	 */
	status = srtp_cipher_set_aad(stream->rtcp_cipher, (uint8_t*)hdr, octets_in_rtcp_header);
	if (status) {
	    return ( srtp_err_status_cipher_fail);
	}
    } else {
	/*
	 * Since payload encryption is not enabled, we must authenticate
	 * the entire packet as described in section 10.3 in revision 07
	 * of the draft.
	 */
	status = srtp_cipher_set_aad(stream->rtcp_cipher, (uint8_t*)hdr, *pkt_octet_len);
	if (status) {
	    return ( srtp_err_status_cipher_fail);
	}
    }
    /* 
     * put the idx# into network byte order and process it as AAD
     */
    tseq = htonl(*trailer);
    status = srtp_cipher_set_aad(stream->rtcp_cipher, (uint8_t*)&tseq, sizeof(srtcp_trailer_t));
    if (status) {
        return ( srtp_err_status_cipher_fail);
    }

    /* if we're encrypting, exor keystream into the message */
    if (enc_start) {
        status = srtp_cipher_encrypt(stream->rtcp_cipher,
                                    (uint8_t*)enc_start, &enc_octet_len);
        if (status) {
            return srtp_err_status_cipher_fail;
        }
	/*
	 * Get the tag and append that to the output
	 */
	status = srtp_cipher_get_tag(stream->rtcp_cipher, (uint8_t*)auth_tag, &tag_len);
	if (status) {
	    return ( srtp_err_status_cipher_fail);
	}
	enc_octet_len += tag_len;
    } else {
	/*
	 * Even though we're not encrypting the payload, we need
	 * to run the cipher to get the auth tag.
	 */
	unsigned int nolen = 0;
        status = srtp_cipher_encrypt(stream->rtcp_cipher, NULL, &nolen);
        if (status) {
            return srtp_err_status_cipher_fail;
        }
	/*
	 * Get the tag and append that to the output
	 */
	status = srtp_cipher_get_tag(stream->rtcp_cipher, (uint8_t*)auth_tag, &tag_len);
	if (status) {
	    return ( srtp_err_status_cipher_fail);
	}
	enc_octet_len += tag_len;
    }

    /* increase the packet length by the length of the auth tag and seq_num*/
    *pkt_octet_len += (tag_len + sizeof(srtcp_trailer_t));

    return srtp_err_status_ok;
}

/*
 * This function handles incoming SRTCP packets while in AEAD mode,
 * which currently supports AES-GCM encryption.  Note, the auth tag is 
 * at the end of the packet stream and is automatically checked by GCM
 * when decrypting the payload.
 */
static srtp_err_status_t
srtp_unprotect_rtcp_aead (srtp_t ctx, srtp_stream_ctx_t *stream, 
                          void *srtcp_hdr, unsigned int *pkt_octet_len)
{
    srtcp_hdr_t *hdr = (srtcp_hdr_t*)srtcp_hdr;
    uint32_t *enc_start;        /* pointer to start of encrypted portion  */
    uint32_t *trailer;          /* pointer to start of trailer            */
    unsigned int enc_octet_len = 0; /* number of octets in encrypted portion */
    uint8_t *auth_tag = NULL;   /* location of auth_tag within packet     */
    srtp_err_status_t status;
    int tag_len;
    unsigned int tmp_len;
    uint32_t seq_num;
    v128_t iv;
    uint32_t tseq;

    /* get tag length from stream context */
    tag_len = srtp_auth_get_tag_length(stream->rtcp_auth);

    /*
     * set encryption start, encryption length, and trailer
     */
    /* index & E (encryption) bit follow normal data.  hdr->len
           is the number of words (32-bit) in the normal packet minus 1 */
    /* This should point trailer to the word past the end of the
           normal data. */
    /* This would need to be modified for optional mikey data */
    /*
     * NOTE: trailer is 32-bit aligned because RTCP 'packets' are always
     *	 multiples of 32-bits (RFC 3550 6.1)
     */
    trailer = (uint32_t*)((char*)hdr + *pkt_octet_len - sizeof(srtcp_trailer_t));
    /*
     * We pass the tag down to the cipher when doing GCM mode 
     */
    enc_octet_len = *pkt_octet_len - (octets_in_rtcp_header + 
                                      sizeof(srtcp_trailer_t));
    auth_tag = (uint8_t*)hdr + *pkt_octet_len - tag_len - sizeof(srtcp_trailer_t);

    if (*((unsigned char*)trailer) & SRTCP_E_BYTE_BIT) {
        enc_start = (uint32_t*)hdr + uint32s_in_rtcp_header;
    } else {
        enc_octet_len = 0;
        enc_start = NULL; /* this indicates that there's no encryption */
    }

    /*
     * check the sequence number for replays
     */
    /* this is easier than dealing with bitfield access */
    seq_num = ntohl(*trailer) & SRTCP_INDEX_MASK;
    debug_print(mod_srtp, "srtcp index: %x", seq_num);
    status = srtp_rdb_check(&stream->rtcp_rdb, seq_num);
    if (status) {
        return status;
    }

    /*
     * Calculate and set the IV
     */
    srtp_calc_aead_iv_srtcp(stream, &iv, seq_num, hdr);
    status = srtp_cipher_set_iv(stream->rtcp_cipher, (const uint8_t*)&iv, direction_decrypt);
    if (status) {
        return srtp_err_status_cipher_fail;
    }

    /*
     * Set the AAD for GCM mode
     */
    if (enc_start) {
	/*
	 * If payload encryption is enabled, then the AAD consist of
	 * the RTCP header and the seq# at the end of the packet
	 */
	status = srtp_cipher_set_aad(stream->rtcp_cipher, (uint8_t*)hdr, octets_in_rtcp_header);
	if (status) {
	    return ( srtp_err_status_cipher_fail);
	}
    } else {
	/*
	 * Since payload encryption is not enabled, we must authenticate
	 * the entire packet as described in section 10.3 in revision 07
	 * of the draft.
	 */
	status = srtp_cipher_set_aad(stream->rtcp_cipher, (uint8_t*)hdr, 
			            (*pkt_octet_len - tag_len - sizeof(srtcp_trailer_t)));
	if (status) {
	    return ( srtp_err_status_cipher_fail);
	}
    }

    /* 
     * put the idx# into network byte order, and process it as AAD 
     */
    tseq = htonl(*trailer);
    status = srtp_cipher_set_aad(stream->rtcp_cipher, (uint8_t*)&tseq, sizeof(srtcp_trailer_t));
    if (status) {
	return ( srtp_err_status_cipher_fail);
    }

    /* if we're decrypting, exor keystream into the message */
    if (enc_start) {
        status = srtp_cipher_decrypt(stream->rtcp_cipher, (uint8_t*)enc_start, &enc_octet_len);
        if (status) {
            return status;
        }
    } else {
	/*
	 * Still need to run the cipher to check the tag
	 */
	tmp_len = tag_len;
        status = srtp_cipher_decrypt(stream->rtcp_cipher, (uint8_t*)auth_tag, &tmp_len);
        if (status) {
            return status;
        }
    }

    /* decrease the packet length by the length of the auth tag and seq_num*/
    *pkt_octet_len -= (tag_len + sizeof(srtcp_trailer_t));

    /*
     * verify that stream is for received traffic - this check will
     * detect SSRC collisions, since a stream that appears in both
     * srtp_protect() and srtp_unprotect() will fail this test in one of
     * those functions.
     *
     * we do this check *after* the authentication check, so that the
     * latter check will catch any attempts to fool us into thinking
     * that we've got a collision
     */
    if (stream->direction != dir_srtp_receiver) {
        if (stream->direction == dir_unknown) {
            stream->direction = dir_srtp_receiver;
        } else {
            srtp_handle_event(ctx, stream, event_ssrc_collision);
        }
    }

    /*
     * if the stream is a 'provisional' one, in which the template context
     * is used, then we need to allocate a new stream at this point, since
     * the authentication passed
     */
    if (stream == ctx->stream_template) {
        srtp_stream_ctx_t *new_stream;

        /*
         * allocate and initialize a new stream
         *
         * note that we indicate failure if we can't allocate the new
         * stream, and some implementations will want to not return
         * failure here
         */
        status = srtp_stream_clone(ctx->stream_template, hdr->ssrc, &new_stream);
        if (status) {
            return status;
        }

        /* add new stream to the head of the stream_list */
        new_stream->next = ctx->stream_list;
        ctx->stream_list = new_stream;

        /* set stream (the pointer used in this function) */
        stream = new_stream;
    }

    /* we've passed the authentication check, so add seq_num to the rdb */
    srtp_rdb_add_index(&stream->rtcp_rdb, seq_num);

    return srtp_err_status_ok;
}

srtp_err_status_t 
srtp_protect_rtcp(srtp_t ctx, void *rtcp_hdr, int *pkt_octet_len) {
  srtcp_hdr_t *hdr = (srtcp_hdr_t *)rtcp_hdr;
  uint32_t *enc_start;      /* pointer to start of encrypted portion  */
  uint32_t *auth_start;     /* pointer to start of auth. portion      */
  uint32_t *trailer;        /* pointer to start of trailer            */
  unsigned int enc_octet_len = 0;/* number of octets in encrypted portion */
  uint8_t *auth_tag = NULL; /* location of auth_tag within packet     */
  srtp_err_status_t status;   
  int tag_len;
  srtp_stream_ctx_t *stream;
  uint32_t prefix_len;
  uint32_t seq_num;

  /* we assume the hdr is 32-bit aligned to start */

  /* check the packet length - it must at least contain a full header */
  if (*pkt_octet_len < octets_in_rtcp_header)
    return srtp_err_status_bad_param;

  /*
   * look up ssrc in srtp_stream list, and process the packet with 
   * the appropriate stream.  if we haven't seen this stream before,
   * there's only one key for this srtp_session, and the cipher
   * supports key-sharing, then we assume that a new stream using
   * that key has just started up
   */
  stream = srtp_get_stream(ctx, hdr->ssrc);
  if (stream == NULL) {
    if (ctx->stream_template != NULL) {
      srtp_stream_ctx_t *new_stream;
      
      /* allocate and initialize a new stream */
      status = srtp_stream_clone(ctx->stream_template,
				 hdr->ssrc, &new_stream); 
      if (status)
	return status;
      
      /* add new stream to the head of the stream_list */
      new_stream->next = ctx->stream_list;
      ctx->stream_list = new_stream;
      
      /* set stream (the pointer used in this function) */
      stream = new_stream;
    } else {
      /* no template stream, so we return an error */
      return srtp_err_status_no_ctx;
    } 
  }
  
  /* 
   * verify that stream is for sending traffic - this check will
   * detect SSRC collisions, since a stream that appears in both
   * srtp_protect() and srtp_unprotect() will fail this test in one of
   * those functions.
   */
  if (stream->direction != dir_srtp_sender) {
    if (stream->direction == dir_unknown) {
      stream->direction = dir_srtp_sender;
    } else {
      srtp_handle_event(ctx, stream, event_ssrc_collision);
    }
  }  

  /*
   * Check if this is an AEAD stream (GCM mode).  If so, then dispatch
   * the request to our AEAD handler.
   */
  if (stream->rtp_cipher->algorithm == SRTP_AES_128_GCM ||
      stream->rtp_cipher->algorithm == SRTP_AES_256_GCM) {
      return srtp_protect_rtcp_aead(ctx, stream, rtcp_hdr, (unsigned int*)pkt_octet_len);
  }

  /* get tag length from stream context */
  tag_len = srtp_auth_get_tag_length(stream->rtcp_auth); 

  /*
   * set encryption start and encryption length - if we're not
   * providing confidentiality, set enc_start to NULL
   */
  enc_start = (uint32_t *)hdr + uint32s_in_rtcp_header;  
  enc_octet_len = *pkt_octet_len - octets_in_rtcp_header;

  /* all of the packet, except the header, gets encrypted */
  /* NOTE: hdr->length is not usable - it refers to only the first
	 RTCP report in the compound packet! */
  /* NOTE: trailer is 32-bit aligned because RTCP 'packets' are always
	 multiples of 32-bits (RFC 3550 6.1) */
  trailer = (uint32_t *) ((char *)enc_start + enc_octet_len);

  if (stream->rtcp_services & sec_serv_conf) {
    *trailer = htonl(SRTCP_E_BIT);     /* set encrypt bit */    
  } else {
    enc_start = NULL;
    enc_octet_len = 0;
	/* 0 is network-order independant */
    *trailer = 0x00000000;     /* set encrypt bit */    
  }

  /* 
   * set the auth_start and auth_tag pointers to the proper locations
   * (note that srtpc *always* provides authentication, unlike srtp)
   */
  /* Note: This would need to change for optional mikey data */
  auth_start = (uint32_t *)hdr;
  auth_tag = (uint8_t *)hdr + *pkt_octet_len + sizeof(srtcp_trailer_t); 

  /* perform EKT processing if needed */
  srtp_ekt_write_data(stream->ekt, auth_tag, tag_len, pkt_octet_len, 
		      srtp_rdbx_get_packet_index(&stream->rtp_rdbx));

  /* 
   * check sequence number for overruns, and copy it into the packet
   * if its value isn't too big
   */
  status = srtp_rdb_increment(&stream->rtcp_rdb);
  if (status)
    return status;
  seq_num = srtp_rdb_get_value(&stream->rtcp_rdb);
  *trailer |= htonl(seq_num);
  debug_print(mod_srtp, "srtcp index: %x", seq_num);

  /* 
   * if we're using rindael counter mode, set nonce and seq 
   */
  if (stream->rtcp_cipher->type->id == SRTP_AES_ICM) {
    v128_t iv;
    
    iv.v32[0] = 0;
    iv.v32[1] = hdr->ssrc;  /* still in network order! */
    iv.v32[2] = htonl(seq_num >> 16);
    iv.v32[3] = htonl(seq_num << 16);
    status = srtp_cipher_set_iv(stream->rtcp_cipher, (const uint8_t*)&iv, direction_encrypt);

  } else {  
    v128_t iv;
    
    /* otherwise, just set the index to seq_num */  
    iv.v32[0] = 0;
    iv.v32[1] = 0;
    iv.v32[2] = 0;
    iv.v32[3] = htonl(seq_num);
    status = srtp_cipher_set_iv(stream->rtcp_cipher, (const uint8_t*)&iv, direction_encrypt);
  }
  if (status)
    return srtp_err_status_cipher_fail;

  /* 
   * if we're authenticating using a universal hash, put the keystream
   * prefix into the authentication tag
   */
  
  /* if auth_start is non-null, then put keystream into tag  */
  if (auth_start) {

    /* put keystream prefix into auth_tag */
    prefix_len = srtp_auth_get_prefix_length(stream->rtcp_auth);    
    status = srtp_cipher_output(stream->rtcp_cipher, auth_tag, &prefix_len);

    debug_print(mod_srtp, "keystream prefix: %s", 
		srtp_octet_string_hex_string(auth_tag, prefix_len));

    if (status)
      return srtp_err_status_cipher_fail;
  }

  /* if we're encrypting, exor keystream into the message */
  if (enc_start) {
    status = srtp_cipher_encrypt(stream->rtcp_cipher, 
		  	        (uint8_t *)enc_start, &enc_octet_len);
    if (status)
      return srtp_err_status_cipher_fail;
  }

  /* initialize auth func context */
  auth_start(stream->rtcp_auth);

  /* 
   * run auth func over packet (including trailer), and write the
   * result at auth_tag 
   */
  status = auth_compute(stream->rtcp_auth, 
			(uint8_t *)auth_start, 
			(*pkt_octet_len) + sizeof(srtcp_trailer_t), 
			auth_tag);
  debug_print(mod_srtp, "srtcp auth tag:    %s", 
	      srtp_octet_string_hex_string(auth_tag, tag_len));
  if (status)
    return srtp_err_status_auth_fail;   
    
  /* increase the packet length by the length of the auth tag and seq_num*/
  *pkt_octet_len += (tag_len + sizeof(srtcp_trailer_t));
    
  return srtp_err_status_ok;  
}


srtp_err_status_t 
srtp_unprotect_rtcp(srtp_t ctx, void *srtcp_hdr, int *pkt_octet_len) {
  srtcp_hdr_t *hdr = (srtcp_hdr_t *)srtcp_hdr;
  uint32_t *enc_start;      /* pointer to start of encrypted portion  */
  uint32_t *auth_start;     /* pointer to start of auth. portion      */
  uint32_t *trailer;        /* pointer to start of trailer            */
  unsigned int enc_octet_len = 0;/* number of octets in encrypted portion */
  uint8_t *auth_tag = NULL; /* location of auth_tag within packet     */
  uint8_t tmp_tag[SRTP_MAX_TAG_LEN];
  uint8_t tag_copy[SRTP_MAX_TAG_LEN];
  srtp_err_status_t status;   
  unsigned int auth_len;
  int tag_len;
  srtp_stream_ctx_t *stream;
  uint32_t prefix_len;
  uint32_t seq_num;
  int e_bit_in_packet;     /* whether the E-bit was found in the packet */
  int sec_serv_confidentiality; /* whether confidentiality was requested */

  /* we assume the hdr is 32-bit aligned to start */

  /* check that the length value is sane; we'll check again once we
     know the tag length, but we at least want to know that it is
     a positive value */
  if (*pkt_octet_len < octets_in_rtcp_header + sizeof(srtcp_trailer_t))
    return srtp_err_status_bad_param;

  /*
   * look up ssrc in srtp_stream list, and process the packet with 
   * the appropriate stream.  if we haven't seen this stream before,
   * there's only one key for this srtp_session, and the cipher
   * supports key-sharing, then we assume that a new stream using
   * that key has just started up
   */
  stream = srtp_get_stream(ctx, hdr->ssrc);
  if (stream == NULL) {
    if (ctx->stream_template != NULL) {
      stream = ctx->stream_template;

      /* 
       * check to see if stream_template has an EKT data structure, in
       * which case we initialize the template using the EKT policy
       * referenced by that data (which consists of decrypting the
       * master key from the EKT field)
       *
       * this function initializes a *provisional* stream, and this
       * stream should not be accepted until and unless the packet
       * passes its authentication check
       */ 
      if (stream->ekt != NULL) {
	status = srtp_stream_init_from_ekt(stream, srtcp_hdr, *pkt_octet_len);
	if (status)
	  return status;
      }

      debug_print(mod_srtp, "srtcp using provisional stream (SSRC: 0x%08x)", 
		  hdr->ssrc);
    } else {
      /* no template stream, so we return an error */
      return srtp_err_status_no_ctx;
    } 
  }
  
  /* get tag length from stream context */
  tag_len = srtp_auth_get_tag_length(stream->rtcp_auth);

  /* check the packet length - it must contain at least a full RTCP
     header, an auth tag (if applicable), and the SRTCP encrypted flag
     and 31-bit index value */
  if (*pkt_octet_len < (int) (octets_in_rtcp_header + tag_len + sizeof(srtcp_trailer_t))) {
    return srtp_err_status_bad_param;
  }

  /*
   * Check if this is an AEAD stream (GCM mode).  If so, then dispatch
   * the request to our AEAD handler.
   */
  if (stream->rtp_cipher->algorithm == SRTP_AES_128_GCM ||
      stream->rtp_cipher->algorithm == SRTP_AES_256_GCM) {
      return srtp_unprotect_rtcp_aead(ctx, stream, srtcp_hdr, (unsigned int*)pkt_octet_len);
  }

  sec_serv_confidentiality = stream->rtcp_services == sec_serv_conf ||
      stream->rtcp_services == sec_serv_conf_and_auth;

  /*
   * set encryption start, encryption length, and trailer
   */
  enc_octet_len = *pkt_octet_len - 
                  (octets_in_rtcp_header + tag_len + sizeof(srtcp_trailer_t));
  /* index & E (encryption) bit follow normal data.  hdr->len
	 is the number of words (32-bit) in the normal packet minus 1 */
  /* This should point trailer to the word past the end of the
	 normal data. */
  /* This would need to be modified for optional mikey data */
  /*
   * NOTE: trailer is 32-bit aligned because RTCP 'packets' are always
   *	 multiples of 32-bits (RFC 3550 6.1)
   */
  trailer = (uint32_t *) ((char *) hdr +
      *pkt_octet_len -(tag_len + sizeof(srtcp_trailer_t)));
  e_bit_in_packet =
      (*((unsigned char *) trailer) & SRTCP_E_BYTE_BIT) == SRTCP_E_BYTE_BIT;
  if (e_bit_in_packet != sec_serv_confidentiality) {
    return srtp_err_status_cant_check;
  }
  if (sec_serv_confidentiality) {
    enc_start = (uint32_t *)hdr + uint32s_in_rtcp_header;  
  } else {
    enc_octet_len = 0;
    enc_start = NULL; /* this indicates that there's no encryption */
  }

  /* 
   * set the auth_start and auth_tag pointers to the proper locations
   * (note that srtcp *always* uses authentication, unlike srtp)
   */
  auth_start = (uint32_t *)hdr;
  auth_len = *pkt_octet_len - tag_len;
  auth_tag = (uint8_t *)hdr + auth_len;

  /* 
   * if EKT is in use, then we make a copy of the tag from the packet,
   * and then zeroize the location of the base tag
   *
   * we first re-position the auth_tag pointer so that it points to
   * the base tag
   */
  if (stream->ekt) {
    auth_tag -= srtp_ekt_octets_after_base_tag(stream->ekt);
    memcpy(tag_copy, auth_tag, tag_len);
    octet_string_set_to_zero(auth_tag, tag_len);
    auth_tag = tag_copy;
    auth_len += tag_len;
  }

  /* 
   * check the sequence number for replays
   */
  /* this is easier than dealing with bitfield access */
  seq_num = ntohl(*trailer) & SRTCP_INDEX_MASK;
  debug_print(mod_srtp, "srtcp index: %x", seq_num);
  status = srtp_rdb_check(&stream->rtcp_rdb, seq_num);
  if (status)
    return status;

  /* 
   * if we're using aes counter mode, set nonce and seq 
   */
  if (stream->rtcp_cipher->type->id == SRTP_AES_ICM) {
    v128_t iv;

    iv.v32[0] = 0;
    iv.v32[1] = hdr->ssrc; /* still in network order! */
    iv.v32[2] = htonl(seq_num >> 16);
    iv.v32[3] = htonl(seq_num << 16);
    status = srtp_cipher_set_iv(stream->rtcp_cipher, (const uint8_t*)&iv, direction_decrypt);

  } else {  
    v128_t iv;
    
    /* otherwise, just set the index to seq_num */  
    iv.v32[0] = 0;
    iv.v32[1] = 0;
    iv.v32[2] = 0;
    iv.v32[3] = htonl(seq_num);
    status = srtp_cipher_set_iv(stream->rtcp_cipher, (const uint8_t*)&iv, direction_decrypt);

  }
  if (status)
    return srtp_err_status_cipher_fail;

  /* initialize auth func context */
  auth_start(stream->rtcp_auth);

  /* run auth func over packet, put result into tmp_tag */
  status = auth_compute(stream->rtcp_auth, (uint8_t *)auth_start,  
			auth_len, tmp_tag);
  debug_print(mod_srtp, "srtcp computed tag:       %s", 
	      srtp_octet_string_hex_string(tmp_tag, tag_len));
  if (status)
    return srtp_err_status_auth_fail;   
  
  /* compare the tag just computed with the one in the packet */
  debug_print(mod_srtp, "srtcp tag from packet:    %s", 
	      srtp_octet_string_hex_string(auth_tag, tag_len));  
  if (octet_string_is_eq(tmp_tag, auth_tag, tag_len))
    return srtp_err_status_auth_fail;

  /* 
   * if we're authenticating using a universal hash, put the keystream
   * prefix into the authentication tag
   */
  prefix_len = srtp_auth_get_prefix_length(stream->rtcp_auth);    
  if (prefix_len) {
    status = srtp_cipher_output(stream->rtcp_cipher, auth_tag, &prefix_len);
    debug_print(mod_srtp, "keystream prefix: %s", 
		srtp_octet_string_hex_string(auth_tag, prefix_len));
    if (status)
      return srtp_err_status_cipher_fail;
  }

  /* if we're decrypting, exor keystream into the message */
  if (enc_start) {
    status = srtp_cipher_decrypt(stream->rtcp_cipher, (uint8_t *)enc_start, &enc_octet_len);
    if (status)
      return srtp_err_status_cipher_fail;
  }

  /* decrease the packet length by the length of the auth tag and seq_num */
  *pkt_octet_len -= (tag_len + sizeof(srtcp_trailer_t));

  /*
   * if EKT is in effect, subtract the EKT data out of the packet
   * length
   */
  *pkt_octet_len -= srtp_ekt_octets_after_base_tag(stream->ekt);

  /* 
   * verify that stream is for received traffic - this check will
   * detect SSRC collisions, since a stream that appears in both
   * srtp_protect() and srtp_unprotect() will fail this test in one of
   * those functions.
   *
   * we do this check *after* the authentication check, so that the
   * latter check will catch any attempts to fool us into thinking
   * that we've got a collision
   */
  if (stream->direction != dir_srtp_receiver) {
    if (stream->direction == dir_unknown) {
      stream->direction = dir_srtp_receiver;
    } else {
      srtp_handle_event(ctx, stream, event_ssrc_collision);
    }
  }

  /* 
   * if the stream is a 'provisional' one, in which the template context
   * is used, then we need to allocate a new stream at this point, since
   * the authentication passed
   */
  if (stream == ctx->stream_template) {  
    srtp_stream_ctx_t *new_stream;

    /* 
     * allocate and initialize a new stream 
     * 
     * note that we indicate failure if we can't allocate the new
     * stream, and some implementations will want to not return
     * failure here
     */
    status = srtp_stream_clone(ctx->stream_template, hdr->ssrc, &new_stream); 
    if (status)
      return status;
    
    /* add new stream to the head of the stream_list */
    new_stream->next = ctx->stream_list;
    ctx->stream_list = new_stream;
    
    /* set stream (the pointer used in this function) */
    stream = new_stream;
  }

  /* we've passed the authentication check, so add seq_num to the rdb */
  srtp_rdb_add_index(&stream->rtcp_rdb, seq_num);
    
    
  return srtp_err_status_ok;  
}


/*
 * user data within srtp_t context
 */

void
srtp_set_user_data(srtp_t ctx, void *data) {
  ctx->user_data = data;
}

void*
srtp_get_user_data(srtp_t ctx) {
  return ctx->user_data;
}


/*
 * dtls keying for srtp 
 */

srtp_err_status_t
srtp_crypto_policy_set_from_profile_for_rtp(srtp_crypto_policy_t *policy, 
				            srtp_profile_t profile) {

  /* set SRTP policy from the SRTP profile in the key set */
  switch(profile) {
  case srtp_profile_aes128_cm_sha1_80:
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(policy);
    break;
  case srtp_profile_aes128_cm_sha1_32:
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(policy);
    break;
  case srtp_profile_null_sha1_80:
    srtp_crypto_policy_set_null_cipher_hmac_sha1_80(policy);
    break;
  case srtp_profile_aes256_cm_sha1_80:
    srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(policy);
    break;
  case srtp_profile_aes256_cm_sha1_32:
    srtp_crypto_policy_set_aes_cm_256_hmac_sha1_32(policy);
    break;
    /* the following profiles are not (yet) supported */
  case srtp_profile_null_sha1_32:
  default:
    return srtp_err_status_bad_param;
  }

  return srtp_err_status_ok;
}

srtp_err_status_t
srtp_crypto_policy_set_from_profile_for_rtcp(srtp_crypto_policy_t *policy, 
					     srtp_profile_t profile) {

  /* set SRTP policy from the SRTP profile in the key set */
  switch(profile) {
  case srtp_profile_aes128_cm_sha1_80:
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(policy);
    break;
  case srtp_profile_aes128_cm_sha1_32:
    /* We do not honor the 32-bit auth tag request since
     * this is not compliant with RFC 3711 */
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(policy);
    break;
  case srtp_profile_null_sha1_80:
    srtp_crypto_policy_set_null_cipher_hmac_sha1_80(policy);
    break;
  case srtp_profile_aes256_cm_sha1_80:
    srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(policy);
    break;
  case srtp_profile_aes256_cm_sha1_32:
    /* We do not honor the 32-bit auth tag request since
     * this is not compliant with RFC 3711 */
    srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(policy);
    break;
    /* the following profiles are not (yet) supported */
  case srtp_profile_null_sha1_32:
  default:
    return srtp_err_status_bad_param;
  }

  return srtp_err_status_ok;
}

void srtp_append_salt_to_key(uint8_t *key, unsigned int bytes_in_key, uint8_t *salt, unsigned int bytes_in_salt) {
  memcpy(key + bytes_in_key, salt, bytes_in_salt);
}

unsigned int
srtp_profile_get_master_key_length(srtp_profile_t profile) {

  switch(profile) {
  case srtp_profile_aes128_cm_sha1_80:
    return 16;
    break;
  case srtp_profile_aes128_cm_sha1_32:
    return 16;
    break;
  case srtp_profile_null_sha1_80:
    return 16;
    break;
  case srtp_profile_aes256_cm_sha1_80:
    return 32;
    break;
  case srtp_profile_aes256_cm_sha1_32:
    return 32;
    break;
    /* the following profiles are not (yet) supported */
  case srtp_profile_null_sha1_32:
  default:
    return 0;  /* indicate error by returning a zero */
  }
}

unsigned int
srtp_profile_get_master_salt_length(srtp_profile_t profile) {

  switch(profile) {
  case srtp_profile_aes128_cm_sha1_80:
    return 14;
    break;
  case srtp_profile_aes128_cm_sha1_32:
    return 14;
    break;
  case srtp_profile_null_sha1_80:
    return 14;
    break;
  case srtp_profile_aes256_cm_sha1_80:
    return 14;
    break;
  case srtp_profile_aes256_cm_sha1_32:
    return 14;
    break;
    /* the following profiles are not (yet) supported */
  case srtp_profile_null_sha1_32:
  default:
    return 0;  /* indicate error by returning a zero */
  }
}

/*
 * SRTP debug interface
 */
srtp_err_status_t srtp_set_debug_module(char *mod_name, int v)
{
    return srtp_crypto_kernel_set_debug_module(mod_name, v);
}

srtp_err_status_t srtp_list_debug_modules(void)
{
    return srtp_crypto_kernel_list_debug_modules();
}


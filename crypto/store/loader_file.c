/*
 * Copyright 2016-2017 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <string.h>

#include <openssl/bio.h>
#include <openssl/dsa.h>         /* For d2i_DSAPrivateKey */
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>      /* For the PKCS8 stuff o.O */
#include <openssl/rsa.h>         /* For d2i_RSAPrivateKey */
#include <openssl/safestack.h>
#include <openssl/store.h>
#include <openssl/ui.h>
#include <openssl/x509.h>        /* For the PKCS8 stuff o.O */
#include "internal/asn1_int.h"
#include "store_locl.h"

#include "e_os.h"

/*
 *  Password prompting
 */

static char *file_get_pass(const UI_METHOD *ui_method, char *pass,
                           size_t maxsize, const char *prompt_info, void *data)
{
    UI *ui = UI_new();
    char *prompt = NULL;

    if (ui == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_FILE_GET_PASS, ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    if (ui_method != NULL)
        UI_set_method(ui, ui_method);
    UI_add_user_data(ui, data);

    if ((prompt = UI_construct_prompt(ui, "pass phrase",
                                      prompt_info)) == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_FILE_GET_PASS, ERR_R_MALLOC_FAILURE);
        pass = NULL;
    } else if (!UI_add_input_string(ui, prompt, UI_INPUT_FLAG_DEFAULT_PWD,
                                    pass, 0, maxsize - 1)) {
        OSSL_STOREerr(OSSL_STORE_F_FILE_GET_PASS, ERR_R_UI_LIB);
        pass = NULL;
    } else {
        switch (UI_process(ui)) {
        case -2:
            OSSL_STOREerr(OSSL_STORE_F_FILE_GET_PASS,
                          OSSL_STORE_R_UI_PROCESS_INTERRUPTED_OR_CANCELLED);
            pass = NULL;
            break;
        case -1:
            OSSL_STOREerr(OSSL_STORE_F_FILE_GET_PASS, ERR_R_UI_LIB);
            pass = NULL;
            break;
        default:
            break;
        }
    }

    OPENSSL_free(prompt);
    UI_free(ui);
    return pass;
}

struct pem_pass_data {
    const UI_METHOD *ui_method;
    void *data;
    const char *prompt_info;
};
static int file_fill_pem_pass_data(struct pem_pass_data *pass_data,
                                   const char *prompt_info,
                                   const UI_METHOD *ui_method, void *ui_data)
{
    if (pass_data == NULL)
        return 0;
    pass_data->ui_method = ui_method;
    pass_data->data = ui_data;
    pass_data->prompt_info = prompt_info;
    return 1;
}
static int file_get_pem_pass(char *buf, int num, int w, void *data)
{
    struct pem_pass_data *pass_data = data;
    char *pass = file_get_pass(pass_data->ui_method, buf, num,
                               pass_data->prompt_info, pass_data->data);

    return pass == NULL ? 0 : strlen(pass);
}

/*
 *  The file scheme handlers
 */

/*-
 * The try_decode function is called to check if the blob of data can
 * be used by this handler, and if it can, decodes it into a supported
 * OpenSSL type and returns a OSSL_STORE_INFO with the decoded data.
 * Input:
 *    pem_name:     If this blob comes from a PEM file, this holds
 *                  the PEM name.  If it comes from another type of
 *                  file, this is NULL.
 *    pem_header:   If this blob comes from a PEM file, this holds
 *                  the PEM headers.  If it comes from another type of
 *                  file, this is NULL.
 *    blob:         The blob of data to match with what this handler
 *                  can use.
 *    len:          The length of the blob.
 *    handler_ctx:  For a handler marked repeatable, this pointer can
 *                  be used to create a context for the handler.  IT IS
 *                  THE HANDLER'S RESPONSIBILITY TO CREATE AND DESTROY
 *                  THIS CONTEXT APPROPRIATELY, i.e. create on first call
 *                  and destroy when about to return NULL.
 *    ui_method:    Application UI method for getting a password, pin
 *                  or any other interactive data.
 *    ui_data:      Application data to be passed to ui_method when
 *                  it's called.
 * Output:
 *    a OSSL_STORE_INFO
 */
typedef OSSL_STORE_INFO *(*file_try_decode_fn)(const char *pem_name,
                                               const char *pem_header,
                                               const unsigned char *blob,
                                               size_t len, void **handler_ctx,
                                               const UI_METHOD *ui_method,
                                               void *ui_data);
/*
 * The eof function should return 1 if there's no more data to be found
 * with the handler_ctx, otherwise 0.  This is only used when the handler is
 * marked repeatable.
 */
typedef int (*file_eof_fn)(void *handler_ctx);
/*
 * The destroy_ctx function is used to destroy the handler_ctx that was
 * intiated by a repeatable try_decode fuction.  This is only used when
 * the handler is marked repeatable.
 */
typedef void (*file_destroy_ctx_fn)(void **handler_ctx);

typedef struct file_handler_st {
    const char *name;
    file_try_decode_fn try_decode;
    file_eof_fn eof;
    file_destroy_ctx_fn destroy_ctx;

    /* flags */
    int repeatable;
} FILE_HANDLER;

static OSSL_STORE_INFO *try_decode_PKCS12(const char *pem_name,
                                          const char *pem_header,
                                          const unsigned char *blob,
                                          size_t len, void **pctx,
                                          const UI_METHOD *ui_method,
                                          void *ui_data)
{
    OSSL_STORE_INFO *store_info = NULL;
    STACK_OF(OSSL_STORE_INFO) *ctx = *pctx;

    if (ctx == NULL) {
        /* Initial parsing */
        PKCS12 *p12;
        int ok = 0;

        if (pem_name != NULL)
            /* No match, there is no PEM PKCS12 tag */
            return NULL;

        if ((p12 = d2i_PKCS12(NULL, &blob, len)) != NULL) {
            char *pass = NULL;
            char tpass[PEM_BUFSIZE];
            EVP_PKEY *pkey = NULL;
            X509 *cert = NULL;
            STACK_OF(X509) *chain = NULL;

            if (PKCS12_verify_mac(p12, "", 0)
                || PKCS12_verify_mac(p12, NULL, 0)) {
                pass = "";
            } else {
                if ((pass = file_get_pass(ui_method, tpass, PEM_BUFSIZE,
                                          "PKCS12 import password",
                                          ui_data)) == NULL) {
                    OSSL_STOREerr(OSSL_STORE_F_TRY_DECODE_PKCS12,
                                  OSSL_STORE_R_PASSPHRASE_CALLBACK_ERROR);
                    goto p12_end;
                }
                if (!PKCS12_verify_mac(p12, pass, strlen(pass))) {
                    OSSL_STOREerr(OSSL_STORE_F_TRY_DECODE_PKCS12,
                                  OSSL_STORE_R_ERROR_VERIFYING_PKCS12_MAC);
                    goto p12_end;
                }
            }

            if (PKCS12_parse(p12, pass, &pkey, &cert, &chain)) {
                OSSL_STORE_INFO *si_pkey = NULL;
                OSSL_STORE_INFO *si_cert = NULL;
                OSSL_STORE_INFO *si_ca = NULL;

                if ((ctx = sk_OSSL_STORE_INFO_new_null()) != NULL
                    && (si_pkey = OSSL_STORE_INFO_new_PKEY(pkey)) != NULL
                    && sk_OSSL_STORE_INFO_push(ctx, si_pkey) != 0
                    && (si_cert = OSSL_STORE_INFO_new_CERT(cert)) != NULL
                    && sk_OSSL_STORE_INFO_push(ctx, si_cert) != 0) {
                    ok = 1;
                    si_pkey = NULL;
                    si_cert = NULL;

                    while(sk_X509_num(chain) > 0) {
                        X509 *ca = sk_X509_value(chain, 0);

                        if ((si_ca = OSSL_STORE_INFO_new_CERT(ca)) == NULL
                            || sk_OSSL_STORE_INFO_push(ctx, si_ca) == 0) {
                            ok = 0;
                            break;
                        }
                        si_ca = NULL;
                        (void)sk_X509_shift(chain);
                    }
                }
                if (!ok) {
                    OSSL_STORE_INFO_free(si_ca);
                    OSSL_STORE_INFO_free(si_cert);
                    OSSL_STORE_INFO_free(si_pkey);
                    sk_OSSL_STORE_INFO_pop_free(ctx, OSSL_STORE_INFO_free);
                    EVP_PKEY_free(pkey);
                    X509_free(cert);
                    sk_X509_pop_free(chain, X509_free);
                    ctx = NULL;
                }
                *pctx = ctx;
            }
        }
     p12_end:
        PKCS12_free(p12);
        if (!ok)
            return NULL;
    }

    if (ctx != NULL)
        store_info = sk_OSSL_STORE_INFO_shift(ctx);

    return store_info;
}
static int eof_PKCS12(void *ctx_)
{
    STACK_OF(OSSL_STORE_INFO) *ctx = ctx_;

    return ctx == NULL || sk_OSSL_STORE_INFO_num(ctx) == 0;
}
static void destroy_ctx_PKCS12(void **pctx)
{
    STACK_OF(OSSL_STORE_INFO) *ctx = *pctx;

    sk_OSSL_STORE_INFO_pop_free(ctx, OSSL_STORE_INFO_free);
    *pctx = NULL;
}
static FILE_HANDLER PKCS12_handler = {
    "PKCS12",
    try_decode_PKCS12,
    eof_PKCS12,
    destroy_ctx_PKCS12,
    1                            /* repeatable */
};

int pem_check_suffix(const char *pem_str, const char *suffix);
static OSSL_STORE_INFO *try_decode_PrivateKey(const char *pem_name,
                                              const char *pem_header,
                                              const unsigned char *blob,
                                              size_t len, void **pctx,
                                              const UI_METHOD *ui_method,
                                              void *ui_data)
{
    OSSL_STORE_INFO *store_info = NULL;
    EVP_PKEY *pkey = NULL;
    const EVP_PKEY_ASN1_METHOD *ameth = NULL;

    if (pem_name != NULL) {
        int slen;

        if ((slen = pem_check_suffix(pem_name, "PRIVATE KEY")) > 0
            && (ameth = EVP_PKEY_asn1_find_str(NULL, pem_name, slen)) != NULL)
            pkey = d2i_PrivateKey(ameth->pkey_id, NULL, &blob, len);
    } else {
        int i;

        for (i = 0; i < EVP_PKEY_asn1_get_count(); i++) {
            ameth = EVP_PKEY_asn1_get0(i);
            if (ameth->pkey_flags & ASN1_PKEY_ALIAS)
                continue;
            pkey = d2i_PrivateKey(ameth->pkey_id, NULL, &blob, len);
            if (pkey != NULL)
                break;
        }
    }
    if (pkey == NULL)
        /* No match */
        return NULL;

    store_info = OSSL_STORE_INFO_new_PKEY(pkey);
    if (store_info == NULL)
        EVP_PKEY_free(pkey);

    return store_info;
}
static FILE_HANDLER PrivateKey_handler = {
    "PrivateKey",
    try_decode_PrivateKey
};

static OSSL_STORE_INFO *try_decode_PUBKEY(const char *pem_name,
                                          const char *pem_header,
                                          const unsigned char *blob,
                                          size_t len, void **pctx,
                                          const UI_METHOD *ui_method,
                                          void *ui_data)
{
    OSSL_STORE_INFO *store_info = NULL;
    EVP_PKEY *pkey = NULL;

    if (pem_name != NULL && strcmp(pem_name, PEM_STRING_PUBLIC) != 0)
        /* No match */
        return NULL;

    if ((pkey = d2i_PUBKEY(NULL, &blob, len)) != NULL)
        store_info = OSSL_STORE_INFO_new_PKEY(pkey);

    return store_info;
}
static FILE_HANDLER PUBKEY_handler = {
    "PUBKEY",
    try_decode_PUBKEY
};

static OSSL_STORE_INFO *try_decode_params(const char *pem_name,
                                          const char *pem_header,
                                          const unsigned char *blob,
                                          size_t len, void **pctx,
                                          const UI_METHOD *ui_method,
                                          void *ui_data)
{
    OSSL_STORE_INFO *store_info = NULL;
    EVP_PKEY *pkey = EVP_PKEY_new();
    const EVP_PKEY_ASN1_METHOD *ameth = NULL;
    int ok = 0;

    if (pkey == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_TRY_DECODE_PARAMS, ERR_R_EVP_LIB);
        return NULL;
    }

    if (pem_name != NULL) {
        int slen;

        if ((slen = pem_check_suffix(pem_name, "PARAMETERS")) > 0
            && EVP_PKEY_set_type_str(pkey, pem_name, slen)
            && (ameth = EVP_PKEY_get0_asn1(pkey)) != NULL
            && ameth->param_decode != NULL
            && ameth->param_decode(pkey, &blob, len))
            ok = 1;
    } else {
        int i;

        for (i = 0; i < EVP_PKEY_asn1_get_count(); i++) {
            ameth = EVP_PKEY_asn1_get0(i);
            if (ameth->pkey_flags & ASN1_PKEY_ALIAS)
                continue;
            if (EVP_PKEY_set_type(pkey, ameth->pkey_id)
                && (ameth = EVP_PKEY_get0_asn1(pkey)) != NULL
                && ameth->param_decode != NULL
                && ameth->param_decode(pkey, &blob, len)) {
                ok = 1;
                break;
            }
        }
    }

    if (ok)
        store_info = OSSL_STORE_INFO_new_PARAMS(pkey);
    if (store_info == NULL)
        EVP_PKEY_free(pkey);

    return store_info;
}
static FILE_HANDLER params_handler = {
    "params",
    try_decode_params
};

static OSSL_STORE_INFO *try_decode_X509Certificate(const char *pem_name,
                                                   const char *pem_header,
                                                   const unsigned char *blob,
                                                   size_t len, void **pctx,
                                                   const UI_METHOD *ui_method,
                                                   void *ui_data)
{
    OSSL_STORE_INFO *store_info = NULL;
    X509 *cert = NULL;

    /*
     * In most cases, we can try to interpret the serialized data as a trusted
     * cert (X509 + X509_AUX) and fall back to reading it as a normal cert
     * (just X509), but if the PEM name specifically declares it as a trusted
     * cert, then no fallback should be engaged.  |ignore_trusted| tells if
     * the fallback can be used (1) or not (0).
     */
    int ignore_trusted = 1;

    if (pem_name != NULL) {
        if (strcmp(pem_name, PEM_STRING_X509_TRUSTED) == 0)
            ignore_trusted = 0;
        else if (strcmp(pem_name, PEM_STRING_X509_OLD) != 0
                 && strcmp(pem_name, PEM_STRING_X509) != 0)
            /* No match */
            return NULL;
    }

    if ((cert = d2i_X509_AUX(NULL, &blob, len)) != NULL
        || (ignore_trusted && (cert = d2i_X509(NULL, &blob, len)) != NULL))
        store_info = OSSL_STORE_INFO_new_CERT(cert);

    if (store_info == NULL)
        X509_free(cert);

    return store_info;
}
static FILE_HANDLER X509Certificate_handler = {
    "X509Certificate",
    try_decode_X509Certificate
};

static OSSL_STORE_INFO *try_decode_X509CRL(const char *pem_name,
                                           const char *pem_header,
                                           const unsigned char *blob,
                                           size_t len, void **pctx,
                                           const UI_METHOD *ui_method,
                                           void *ui_data)
{
    OSSL_STORE_INFO *store_info = NULL;
    X509_CRL *crl = NULL;

    if (pem_name != NULL
        && strcmp(pem_name, PEM_STRING_X509_CRL) != 0)
        /* No match */
        return NULL;

    if ((crl = d2i_X509_CRL(NULL, &blob, len)) != NULL)
        store_info = OSSL_STORE_INFO_new_CRL(crl);

    if (store_info == NULL)
        X509_CRL_free(crl);

    return store_info;
}
static FILE_HANDLER X509CRL_handler = {
    "X509CRL",
    try_decode_X509CRL
};

static const FILE_HANDLER *file_handlers[] = {
    &PKCS12_handler,
    &X509Certificate_handler,
    &X509CRL_handler,
    &params_handler,
    &PUBKEY_handler,
    &PrivateKey_handler,
};


/*
 *  The loader itself
 */

struct ossl_store_loader_ctx_st {
    BIO *file;
    int is_pem;
    int errcnt;

    /* The following are used when the handler is marked as repeatable */
    const FILE_HANDLER *last_handler;
    void *last_handler_ctx;
};

static OSSL_STORE_LOADER_CTX *file_open(const OSSL_STORE_LOADER *loader,
                                        const char *uri,
                                        const UI_METHOD *ui_method,
                                        void *ui_data)
{
    BIO *buff = NULL;
    char peekbuf[4096];
    OSSL_STORE_LOADER_CTX *ctx = NULL;
    const char *path = NULL;

    if (strncasecmp(uri, "file:", 5) == 0) {
        if (strncmp(&uri[5], "//localhost/", 12) == 0) {
            path = &uri[16];
        } else if (strncmp(&uri[5], "///", 3) == 0) {
            path = &uri[7];
        } else if (strncmp(&uri[5], "//", 2) != 0) {
            path = &uri[5];
        } else {
            OSSL_STOREerr(OSSL_STORE_F_FILE_OPEN,
                          OSSL_STORE_R_URI_AUTHORITY_UNSUPPORED);
            return NULL;
        }

        /*
         * If the scheme "file" was an explicit part of the URI, the path must
         * be absolute.  So says RFC 8089
         */
        if (path[0] != '/') {
            OSSL_STOREerr(OSSL_STORE_F_FILE_OPEN,
                          OSSL_STORE_R_PATH_MUST_BE_ABSOLUTE);
            return NULL;
        }

#ifdef _WIN32
        /* Windows file: URIs with a drive letter start with a / */
        if (path[0] == '/' && path[2] == ':' && path[3] == '/')
            path++;
#endif
    } else {
        path = uri;
    }


    ctx = OPENSSL_zalloc(sizeof(*ctx));
    if (ctx == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_FILE_OPEN, ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    if ((buff = BIO_new(BIO_f_buffer())) == NULL)
        goto err;
    if ((ctx->file = BIO_new_file(path, "rb")) == NULL) {
        goto err;
    }
    ctx->file = BIO_push(buff, ctx->file);
    if (BIO_buffer_peek(ctx->file, peekbuf, sizeof(peekbuf)-1) > 0) {
        peekbuf[sizeof(peekbuf)-1] = '\0';
        if (strstr(peekbuf, "-----BEGIN ") != NULL)
            ctx->is_pem = 1;
    }

    return ctx;
 err:
    if (buff != NULL)
        BIO_free(buff);
    OPENSSL_free(ctx);
    return NULL;
}

static OSSL_STORE_INFO *file_load_try_decode(OSSL_STORE_LOADER_CTX *ctx,
                                             const char *pem_name,
                                             const char *pem_header,
                                             unsigned char *data, size_t len,
                                             const UI_METHOD *ui_method,
                                             void *ui_data, int *matchcount)
{
    OSSL_STORE_INFO *result = NULL;
    BUF_MEM *new_mem = NULL;
    char *new_pem_name = NULL;
    int t = 0;

 again:
    {
        size_t i = 0;
        void *handler_ctx = NULL;
        const FILE_HANDLER **matching_handlers =
            OPENSSL_zalloc(sizeof(*matching_handlers)
                           * OSSL_NELEM(file_handlers));

        if (matching_handlers == NULL) {
            OSSL_STOREerr(OSSL_STORE_F_FILE_LOAD_TRY_DECODE,
                          ERR_R_MALLOC_FAILURE);
            goto err;
        }

        *matchcount = 0;
        for (i = 0; i < OSSL_NELEM(file_handlers); i++) {
            const FILE_HANDLER *handler = file_handlers[i];
            void *tmp_handler_ctx = NULL;
            OSSL_STORE_INFO *tmp_result =
                handler->try_decode(pem_name, pem_header, data, len,
                                    &tmp_handler_ctx, ui_method, ui_data);

            if (tmp_result == NULL) {
                OSSL_STOREerr(OSSL_STORE_F_FILE_LOAD_TRY_DECODE,
                              OSSL_STORE_R_IS_NOT_A);
                ERR_add_error_data(1, handler->name);
            } else {
                if (matching_handlers)
                    matching_handlers[*matchcount] = handler;

                if (handler_ctx)
                    handler->destroy_ctx(&handler_ctx);
                handler_ctx = tmp_handler_ctx;

                if (++*matchcount == 1) {
                    result = tmp_result;
                    tmp_result = NULL;
                } else {
                    /* more than one match => ambiguous, kill any result */
                    OSSL_STORE_INFO_free(result);
                    OSSL_STORE_INFO_free(tmp_result);
                    if (handler->destroy_ctx != NULL)
                        handler->destroy_ctx(&handler_ctx);
                    handler_ctx = NULL;
                    result = NULL;
                }
            }
        }

        if (*matchcount > 1)
            OSSL_STOREerr(OSSL_STORE_F_FILE_LOAD_TRY_DECODE,
                          OSSL_STORE_R_AMBIGUOUS_CONTENT_TYPE);
        if (*matchcount == 0)
            OSSL_STOREerr(OSSL_STORE_F_FILE_LOAD_TRY_DECODE,
                          OSSL_STORE_R_UNSUPPORTED_CONTENT_TYPE);
        else if (matching_handlers[0]->repeatable) {
            if (ctx == NULL) {
                OSSL_STOREerr(OSSL_STORE_F_FILE_LOAD_TRY_DECODE,
                              OSSL_STORE_R_UNSUPPORTED_CONTENT_TYPE);
                OSSL_STORE_INFO_free(result);
                result = NULL;
            } else {
                ctx->last_handler = matching_handlers[0];
                ctx->last_handler_ctx = handler_ctx;
            }
        }

        OPENSSL_free(matching_handlers);
    }

 err:
    OPENSSL_free(new_pem_name);
    BUF_MEM_free(new_mem);

    if (result != NULL
        && (t = OSSL_STORE_INFO_get_type(result)) == OSSL_STORE_INFO_EMBEDDED) {
        pem_name = new_pem_name =
            ossl_store_info_get0_EMBEDDED_pem_name(result);
        new_mem = ossl_store_info_get0_EMBEDDED_buffer(result);
        data = (unsigned char *)new_mem->data;
        len = new_mem->length;
        OPENSSL_free(result);
        result = NULL;
        goto again;
    }

    if (result != NULL)
        ERR_clear_error();

    return result;
}

static OSSL_STORE_INFO *file_load_try_repeat(OSSL_STORE_LOADER_CTX *ctx,
                                             const UI_METHOD *ui_method,
                                             void *ui_data)
{
    OSSL_STORE_INFO *result = NULL;

    if (ctx->last_handler != NULL) {
        result = ctx->last_handler->try_decode(NULL, NULL, NULL, 0,
                                               &ctx->last_handler_ctx,
                                               ui_method, ui_data);

        if (result == NULL) {
            ctx->last_handler->destroy_ctx(&ctx->last_handler_ctx);
            ctx->last_handler_ctx = NULL;
            ctx->last_handler = NULL;
        }
    }
    return result;
}

static int file_read_pem(BIO *bp, char **pem_name, char **pem_header,
                         unsigned char **data, long *len,
                         const UI_METHOD *ui_method,
                         void *ui_data)
{
    int i = PEM_read_bio(bp, pem_name, pem_header, data, len);

    if (i <= 0)
        return 0;

    /*
     * 10 is the number of characters in "Proc-Type:", which
     * PEM_get_EVP_CIPHER_INFO() requires to be present.
     * If the PEM header has less characters than that, it's
     * not worth spending cycles on it.
     */
    if (strlen(*pem_header) > 10) {
        EVP_CIPHER_INFO cipher;
        struct pem_pass_data pass_data;

        if (!PEM_get_EVP_CIPHER_INFO(*pem_header, &cipher)
            || !file_fill_pem_pass_data(&pass_data, "PEM", ui_method, ui_data)
            || !PEM_do_header(&cipher, *data, len, file_get_pem_pass,
                              &pass_data)) {
            return 0;
        }
    }
    return 1;
}

static int file_read_asn1(BIO *bp, unsigned char **data, long *len)
{
    BUF_MEM *mem = NULL;

    if (asn1_d2i_read_bio(bp, &mem) < 0)
        return 0;

    *data = (unsigned char *)mem->data;
    *len = (long)mem->length;
    OPENSSL_free(mem);

    return 1;
}

static int file_eof(OSSL_STORE_LOADER_CTX *ctx);
static int file_error(OSSL_STORE_LOADER_CTX *ctx);
static OSSL_STORE_INFO *file_load(OSSL_STORE_LOADER_CTX *ctx,
                                  const UI_METHOD *ui_method, void *ui_data)
{
    OSSL_STORE_INFO *result = NULL;
    int matchcount = -1;

    result = file_load_try_repeat(ctx, ui_method, ui_data);
    if (result != NULL)
        return result;

    if (file_error(ctx))
        return NULL;

    do {
        char *pem_name = NULL;      /* PEM record name */
        char *pem_header = NULL;    /* PEM record header */
        unsigned char *data = NULL; /* DER encoded data */
        long len = 0;               /* DER encoded data length */

        if (ctx->is_pem) {
            if (!file_read_pem(ctx->file, &pem_name, &pem_header, &data, &len,
                               ui_method, ui_data)) {
                if (!file_eof(ctx))
                    ctx->errcnt++;
                goto err;
            }
        } else {
            if (!file_read_asn1(ctx->file, &data, &len)) {
                if (!file_eof(ctx))
                    ctx->errcnt++;
                goto err;
            }
        }

        matchcount = -1;
        result = file_load_try_decode(ctx, pem_name, pem_header, data, len,
                                      ui_method, ui_data, &matchcount);

     err:
        OPENSSL_free(pem_name);
        OPENSSL_free(pem_header);
        OPENSSL_free(data);
    } while (matchcount == 0 && !file_eof(ctx) && !file_error(ctx));

    /* We bail out on ambiguity */
    if (matchcount > 1)
        return NULL;

    return result;
}

static int file_error(OSSL_STORE_LOADER_CTX *ctx)
{
    return ctx->errcnt > 0;
}

static int file_eof(OSSL_STORE_LOADER_CTX *ctx)
{
    if (ctx->last_handler != NULL
        && !ctx->last_handler->eof(ctx->last_handler_ctx))
        return 0;
    return BIO_eof(ctx->file);
}

static int file_close(OSSL_STORE_LOADER_CTX *ctx)
{
    if (ctx->last_handler != NULL) {
        ctx->last_handler->destroy_ctx(&ctx->last_handler_ctx);
        ctx->last_handler_ctx = NULL;
        ctx->last_handler = NULL;
    }
    BIO_free_all(ctx->file);
    OPENSSL_free(ctx);
    return 1;
}

static OSSL_STORE_LOADER file_loader =
    {
        "file",
        file_open,
        NULL,
        file_load,
        file_eof,
        file_error,
        file_close
    };

static void store_file_loader_deinit(void)
{
    ossl_store_unregister_loader_int(file_loader.scheme);
}

int ossl_store_file_loader_init(void)
{
    int ret = ossl_store_register_loader_int(&file_loader);

    OPENSSL_atexit(store_file_loader_deinit);
    return ret;
}
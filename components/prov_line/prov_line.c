#include "prov_line.h"

#include <string.h>

static bool cn_char_ok(char c)
{
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) ||
           (c == '.') || (c == '_') || (c == '-');
}

static bool host_ok(const char *host, size_t len)
{
    if ((len == 0U) || (len > PROV_LINE_HOST_MAX)) {
        return false;
    }
    bool in_port = false;
    size_t port_digits = 0;
    for (size_t i = 0; i < len; i++) {
        const char c = host[i];
        if (in_port) {
            if ((c < '0') || (c > '9')) {
                return false;
            }
            port_digits++;
        } else if (c == ':') {
            if (i == 0U) {
                return false;
            }
            in_port = true;
        } else if (!(((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
                     ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-'))) {
            return false;
        } else {
            /* caractere válido de nome */
        }
    }
    return !in_port || ((port_digits >= 1U) && (port_digits <= 5U));
}

fw_err_t prov_line_parse(const char *line, prov_line_t *out)
{
    if ((line == NULL) || (out == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }
    memset(out, 0, sizeof(*out));

    const size_t scheme_len = strlen(PROV_LINE_SCHEME);
    if (strncmp(line, PROV_LINE_SCHEME, scheme_len) != 0) {
        return FW_ERR_INVALID_DATA;
    }
    const char *creds = &line[scheme_len];

    /* Mesmo corte do servidor (parse_line): host depois do ÚLTIMO '@',
     * token depois do PRIMEIRO ':'. */
    const char *at = strrchr(creds, '@');
    const char *colon = strchr(creds, ':');
    if ((at == NULL) || (colon == NULL) || (colon > at)) {
        return FW_ERR_INVALID_DATA;
    }

    const size_t cn_len = (size_t)(colon - creds);
    const size_t token_len = (size_t)(at - colon - 1);
    const char *host = at + 1;
    const size_t host_len = strlen(host);

    if ((cn_len < PROV_LINE_CN_MIN) || (cn_len > PROV_LINE_CN_MAX) ||
        (token_len < PROV_LINE_TOKEN_MIN) || (token_len > PROV_LINE_TOKEN_MAX) ||
        !host_ok(host, host_len)) {
        return FW_ERR_INVALID_DATA;
    }
    for (size_t i = 0; i < cn_len; i++) {
        if (!cn_char_ok(creds[i])) {
            return FW_ERR_INVALID_DATA;
        }
    }
    for (size_t i = 0; i < token_len; i++) {
        const char c = colon[1 + i];
        if ((c <= ' ') || (c == 0x7F)) {
            return FW_ERR_INVALID_DATA; /* espaço ou controle: linha colada errada */
        }
    }

    memcpy(out->cn, creds, cn_len);
    memcpy(out->token, colon + 1, token_len);
    memcpy(out->host, host, host_len);
    return FW_OK;
}

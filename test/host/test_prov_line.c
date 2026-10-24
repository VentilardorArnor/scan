#include <string.h>

#include "prov_line.h"
#include "test_support.h"

static void test_parses_platform_line(void)
{
    prov_line_t line;
    TEST_ASSERT_EQUAL(FW_OK,
                      prov_line_parse("agro1://agp-7k2m9x4qpr3t:Zx9_tok-EN.123@api.smartagrobr.com",
                                      &line));
    TEST_ASSERT_EQUAL_STRING("agp-7k2m9x4qpr3t", line.cn);
    TEST_ASSERT_EQUAL_STRING("Zx9_tok-EN.123", line.token);
    TEST_ASSERT_EQUAL_STRING("api.smartagrobr.com", line.host);
}

static void test_split_rules_match_server(void)
{
    prov_line_t line;

    /* Token pode ter ':' (corte no PRIMEIRO ':') e '@' (host depois do ÚLTIMO '@'). */
    TEST_ASSERT_EQUAL(FW_OK, prov_line_parse("agro1://agp-abc:to:k@n@3n@api.x.com", &line));
    TEST_ASSERT_EQUAL_STRING("agp-abc", line.cn);
    TEST_ASSERT_EQUAL_STRING("to:k@n@3n", line.token);
    TEST_ASSERT_EQUAL_STRING("api.x.com", line.host);

    /* Porta no host (ambiente local). */
    TEST_ASSERT_EQUAL(FW_OK, prov_line_parse("agro1://agp-abc:tokentoken@localhost:8443", &line));
    TEST_ASSERT_EQUAL_STRING("localhost:8443", line.host);
}

static void test_rejects_bad_lines(void)
{
    static const char *const bad[] = {
        "",
        "agro2://agp-abc:tokentoken@api.x.com",   /* esquema errado */
        "agro1://agp-abc-tokentoken@api.x.com",   /* sem ':' */
        "agro1://agp-abc:tokentoken",             /* sem '@' */
        "agro1://ab:tokentoken@api.x.com",        /* CN curto */
        "agro1://agp abc:tokentoken@api.x.com",   /* espaço no CN */
        "agro1://agp/abc:tokentoken@api.x.com",   /* '/' no CN */
        "agro1://agp-abc:short@api.x.com",        /* token < 8 */
        "agro1://agp-abc:token token@api.x.com",  /* espaço no token */
        "agro1://agp-abc:tokentoken@",            /* host vazio */
        "agro1://agp-abc:tokentoken@api.x.com/v1",/* caminho no host */
        "agro1://agp-abc:tokentoken@api.x.com:",  /* porta vazia */
        "agro1://agp-abc:tokentoken@:8443",       /* só porta */
    };
    prov_line_t line;
    for (size_t i = 0; i < (sizeof(bad) / sizeof(bad[0])); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FW_ERR_INVALID_DATA, prov_line_parse(bad[i], &line), bad[i]);
    }
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_PARAM, prov_line_parse(NULL, &line));

    /* Token de 65 caracteres: acima do limite do servidor. */
    char longline[160];
    memset(longline, 0, sizeof(longline));
    (void)strcpy(longline, "agro1://agp-abc:");
    memset(&longline[strlen(longline)], 'a', 65);
    (void)strcat(longline, "@api.x.com");
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, prov_line_parse(longline, &line));
}

void run_prov_line_tests(void)
{
    RUN_TEST(test_parses_platform_line);
    RUN_TEST(test_split_rules_match_server);
    RUN_TEST(test_rejects_bad_lines);
}

#include <stdio.h>
#include <string.h>

#include "contract_json.h"
#include "gps_track.h"
#include "nmea.h"
#include "test_support.h"

/* Monta "$<corpo>*HH" com o checksum calculado. */
static void make_sentence(const char *body, char *out, size_t cap)
{
    uint8_t sum = 0;
    for (const char *p = body; *p != '\0'; p++) {
        sum ^= (uint8_t)*p;
    }
    (void)snprintf(out, cap, "$%s*%02X", body, sum);
}

static void test_gga_real_sentence(void)
{
    nmea_sentence_t s;
    TEST_ASSERT_EQUAL(
        FW_OK,
        nmea_parse_sentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
                            &s));
    TEST_ASSERT_EQUAL(NMEA_SENTENCE_GGA, s.type);
    TEST_ASSERT_TRUE(s.fix_valid);
    TEST_ASSERT_TRUE(s.has_position);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 48.1173, s.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 11.516667, s.lon);
    TEST_ASSERT_TRUE(s.has_hdop);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, s.hdop);
    TEST_ASSERT_EQUAL_UINT8(8, s.sats);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 545.4f, s.alt_m);
    TEST_ASSERT_TRUE(s.has_time);
    TEST_ASSERT_EQUAL_UINT8(12, s.hour);
    TEST_ASSERT_EQUAL_UINT8(35, s.minute);
    TEST_ASSERT_EQUAL_UINT8(19, s.second);
}

static void test_rmc_real_sentence(void)
{
    nmea_sentence_t s;
    TEST_ASSERT_EQUAL(
        FW_OK,
        nmea_parse_sentence("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A",
                            &s));
    TEST_ASSERT_EQUAL(NMEA_SENTENCE_RMC, s.type);
    TEST_ASSERT_TRUE(s.fix_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 22.4f * 1.852f, s.speed_kmh);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 84.4f, s.course_deg);
    TEST_ASSERT_TRUE(s.has_date);
    TEST_ASSERT_EQUAL_UINT8(23, s.day);
    TEST_ASSERT_EQUAL_UINT8(3, s.month);
}

static void test_south_west_and_gn_talker(void)
{
    char line[NMEA_LINE_MAX];
    nmea_sentence_t s;

    make_sentence("GNGGA,120000.00,1747.49072,S,05055.15404,W,1,09,0.9,812.4,M,-5.0,M,,", line,
                  sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    TEST_ASSERT_TRUE(s.fix_valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -17.791512, s.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -50.919234, s.lon);
}

static void test_all_accepted_talkers(void)
{
    static const char *const talkers[] = {"GP", "GN", "GL", "GA"};
    char body[NMEA_LINE_MAX];
    char line[NMEA_LINE_MAX];
    nmea_sentence_t s;

    for (size_t i = 0; i < 4U; i++) {
        (void)snprintf(body, sizeof(body), "%sGGA,120000,1747.49072,S,05055.15404,W,1,09,0.9,812.4,M,,M,,",
                       talkers[i]);
        make_sentence(body, line, sizeof(line));
        TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
        TEST_ASSERT_EQUAL(NMEA_SENTENCE_GGA, s.type);
    }

    make_sentence("BDGGA,120000,1747.49072,S,05055.15404,W,1,09,0.9,812.4,M,,M,,", line,
                  sizeof(line));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, nmea_parse_sentence(line, &s));
}

static void test_invalid_checksum_is_rejected(void)
{
    nmea_sentence_t s;
    TEST_ASSERT_EQUAL(
        FW_ERR_INVALID_DATA,
        nmea_parse_sentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48",
                            &s));
    /* Um dígito trocado na latitude, checksum original. */
    TEST_ASSERT_EQUAL(
        FW_ERR_INVALID_DATA,
        nmea_parse_sentence("$GPGGA,123519,4807.138,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
                            &s));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, nmea_parse_sentence("$GPGGA,123519", &s));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, nmea_parse_sentence("", &s));
}

static void test_no_fix_sentences(void)
{
    char line[NMEA_LINE_MAX];
    nmea_sentence_t s;

    make_sentence("GPGGA,120000.00,,,,,0,00,99.99,,,,,,", line, sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    TEST_ASSERT_FALSE(s.fix_valid);
    TEST_ASSERT_FALSE(s.has_position);

    make_sentence("GPRMC,120000.00,V,,,,,,,131026,,,N", line, sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    TEST_ASSERT_FALSE(s.fix_valid);
    TEST_ASSERT_FALSE(s.has_position);

    /* Status 'V' com coordenadas preenchidas continua sem fix. */
    make_sentence("GPRMC,120000.00,V,1747.49072,S,05055.15404,W,0.0,0.0,131026,,,N", line,
                  sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    TEST_ASSERT_FALSE(s.fix_valid);
}

static void test_gsa_hdop(void)
{
    char line[NMEA_LINE_MAX];
    nmea_sentence_t s;

    make_sentence("GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1", line, sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    TEST_ASSERT_EQUAL(NMEA_SENTENCE_GSA, s.type);
    TEST_ASSERT_TRUE(s.fix_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.3f, s.hdop);
}

static void test_gsv_satellites_in_view(void)
{
    char line[NMEA_LINE_MAX];
    nmea_sentence_t s;

    make_sentence("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00", line,
                  sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    TEST_ASSERT_EQUAL(NMEA_SENTENCE_GSV, s.type);
    TEST_ASSERT_EQUAL_STRING("GP", s.talker);
    TEST_ASSERT_TRUE(s.has_sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(11, s.sats_in_view);

    /* Sem satélite nenhum (antena dentro de casa): ainda é NMEA válido. */
    make_sentence("GLGSV,1,1,00", line, sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    TEST_ASSERT_EQUAL_STRING("GL", s.talker);
    TEST_ASSERT_EQUAL_UINT8(0, s.sats_in_view);
}

static void test_framer_handles_garbage_and_crlf(void)
{
    static const char stream[] =
        "lixo\r\n1131.000,E*00\r\n"
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
        "$GPGGA,trunc$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n";
    nmea_framer_t framer;
    nmea_sentence_t s;
    uint32_t valid = 0;
    uint32_t lines = 0;

    nmea_framer_init(&framer);
    for (size_t i = 0; i < (sizeof(stream) - 1U); i++) {
        if (nmea_framer_feed(&framer, (uint8_t)stream[i])) {
            lines++;
            if (nmea_parse_sentence(framer.line, &s) == FW_OK) {
                valid++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(2, lines);
    TEST_ASSERT_EQUAL_UINT32(2, valid);
}

static void feed_gga(gps_track_t *track, double lat_min, const char *hdop, uint64_t now_ms)
{
    char body[NMEA_LINE_MAX];
    char line[NMEA_LINE_MAX];
    nmea_sentence_t s;

    (void)snprintf(body, sizeof(body), "GNGGA,120000.00,17%08.5f,S,05055.15404,W,1,09,%s,812.4,M,,M,,",
                   lat_min, hdop);
    make_sentence(body, line, sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    gps_track_feed(track, &s, now_ms);
}

static void test_track_average_and_filters(void)
{
    gps_track_t track;
    gps_fix_t fix;

    gps_track_init(&track);
    TEST_ASSERT_FALSE(gps_track_get_fix(&track, 0, &fix));

    feed_gga(&track, 30.0, "0.9", 1000);
    feed_gga(&track, 36.0, "1.2", 2000);
    feed_gga(&track, 60.0 - 0.00001, "9.0", 2500); /* HDOP > 5: descartada */

    TEST_ASSERT_TRUE(gps_track_get_fix(&track, 3000, &fix));
    TEST_ASSERT_EQUAL_UINT8(2, fix.sample_count);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -(17.0 + (33.0 / 60.0)), fix.lat);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.2f, fix.hdop);
    TEST_ASSERT_TRUE(fix.has_sats);
    TEST_ASSERT_EQUAL_UINT8(9, fix.sats);

    /* A primeira amostra passa de 10 s e sai da média. */
    TEST_ASSERT_TRUE(gps_track_get_fix(&track, 11500, &fix));
    TEST_ASSERT_EQUAL_UINT8(1, fix.sample_count);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -(17.0 + (36.0 / 60.0)), fix.lat);

    /* Tudo velho: sem fix (o objeto gps deve ser omitido). */
    TEST_ASSERT_FALSE(gps_track_get_fix(&track, 20000, &fix));
}

static void test_track_keeps_last_five(void)
{
    gps_track_t track;
    gps_fix_t fix;

    gps_track_init(&track);
    for (uint32_t i = 0; i < 7U; i++) {
        feed_gga(&track, 10.0 + (double)i, "0.9", 1000U * (i + 1U));
    }
    TEST_ASSERT_TRUE(gps_track_get_fix(&track, 7500, &fix));
    TEST_ASSERT_EQUAL_UINT8(5, fix.sample_count);
    /* Minutos 12..16 -> média 14. */
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -(17.0 + (14.0 / 60.0)), fix.lat);
}

static void test_track_utc_from_rmc(void)
{
    char line[NMEA_LINE_MAX];
    nmea_sentence_t s;
    gps_track_t track;
    int64_t epoch = 0;
    int64_t expected = 0;
    uint64_t at_ms = 0;

    gps_track_init(&track);
    TEST_ASSERT_FALSE(gps_track_get_utc(&track, &epoch, &at_ms));

    make_sentence("GNRMC,120000.00,A,1747.49072,S,05055.15404,W,0.16,271.0,131026,,,A", line,
                  sizeof(line));
    TEST_ASSERT_EQUAL(FW_OK, nmea_parse_sentence(line, &s));
    gps_track_feed(&track, &s, 4242);

    TEST_ASSERT_TRUE(gps_track_get_utc(&track, &epoch, &at_ms));
    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2026-10-13T12:00:00Z", &expected));
    TEST_ASSERT_EQUAL_INT64(expected, epoch);
    TEST_ASSERT_EQUAL_UINT64(4242, at_ms);
}

void run_nmea_tests(void)
{
    RUN_TEST(test_gga_real_sentence);
    RUN_TEST(test_rmc_real_sentence);
    RUN_TEST(test_south_west_and_gn_talker);
    RUN_TEST(test_all_accepted_talkers);
    RUN_TEST(test_invalid_checksum_is_rejected);
    RUN_TEST(test_no_fix_sentences);
    RUN_TEST(test_gsa_hdop);
    RUN_TEST(test_gsv_satellites_in_view);
    RUN_TEST(test_framer_handles_garbage_and_crlf);
    RUN_TEST(test_track_average_and_filters);
    RUN_TEST(test_track_keeps_last_five);
    RUN_TEST(test_track_utc_from_rmc);
}

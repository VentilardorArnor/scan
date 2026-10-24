#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "contract_json.h"
#include "test_support.h"

#define BUF_LEN (1024U)

static const char *str_of(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(item), key);
    return item->valuestring;
}

static double num_of(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNumber(item), key);
    return item->valuedouble;
}

static void test_time_format_and_parse(void)
{
    char buf[CONTRACT_TS_STR_LEN];
    int64_t epoch = -1;

    TEST_ASSERT_EQUAL(FW_OK, contract_time_format(0, buf));
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00Z", buf);
    TEST_ASSERT_EQUAL(FW_OK, contract_time_format(946684800, buf));
    TEST_ASSERT_EQUAL_STRING("2000-01-01T00:00:00Z", buf);
    TEST_ASSERT_EQUAL(FW_OK, contract_time_format(1709210096, buf));
    TEST_ASSERT_EQUAL_STRING("2024-02-29T12:34:56Z", buf); /* dia bissexto */

    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2024-02-29T12:34:56Z", &epoch));
    TEST_ASSERT_EQUAL_INT64(1709210096, epoch);
    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2024-02-29T12:34:56.789Z", &epoch));
    TEST_ASSERT_EQUAL_INT64(1709210096, epoch);

    /* Ida e volta em uma data do contrato. */
    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2026-10-13T12:00:15Z", &epoch));
    TEST_ASSERT_EQUAL(FW_OK, contract_time_format(epoch, buf));
    TEST_ASSERT_EQUAL_STRING("2026-10-13T12:00:15Z", buf);

    /* RFC 3339 com deslocamento (o schema permite): convertido para UTC. É o
     * formato do Pydantic quando a sessão do banco não está em UTC. */
    int64_t utc = 0;
    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2026-10-13T12:00:15Z", &utc));
    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2026-10-13T09:00:15.123456-03:00", &epoch));
    TEST_ASSERT_EQUAL_INT64(utc, epoch);
    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2026-10-13T17:30:15+05:30", &epoch));
    TEST_ASSERT_EQUAL_INT64(utc, epoch);
    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2026-10-13T12:00:15+00:00", &epoch));
    TEST_ASSERT_EQUAL_INT64(utc, epoch);

    /* Sem fuso, fuso malformado ou data inválida: recusado. */
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, contract_time_parse("2026-10-13T12:00:15", &epoch));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, contract_time_parse("2026-10-13T12:00:15-03", &epoch));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA,
                      contract_time_parse("2026-10-13T12:00:15+24:00", &epoch));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA,
                      contract_time_parse("2026-10-13T12:00:15-03:00x", &epoch));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, contract_time_parse("2026-13-13T12:00:15Z", &epoch));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, contract_time_parse("ontem", &epoch));
}

static contract_telemetry_t sample_telemetry(void)
{
    contract_telemetry_t t;
    memset(&t, 0, sizeof(t));
    (void)contract_time_parse("2026-10-13T12:00:00Z", &t.ts);
    t.fw = "0.1.0";
    t.boot_id = "a81f3c92d4e5f601";
    t.seq = 42;
    t.time_source = CONTRACT_TIME_GPS;
    t.has_rssi = true;
    t.rssi = -67;
    t.heap_free = 182340;
    t.journal_pending = 0;
    t.has_gps = true;
    t.gps.lat = -17.791512;
    t.gps.lon = -50.919234;
    t.gps.has_hdop = true;
    t.gps.hdop = 0.9f;
    t.gps.has_sats = true;
    t.gps.sats = 9;
    t.gps.has_alt = true;
    t.gps.alt_m = 812.4f;
    t.gps.has_speed = true;
    t.gps.speed_kmh = 0.3f;
    t.gps.has_course = true;
    t.gps.course_deg = 271.0f;
    return t;
}

static void test_telemetry_envelope_fields(void)
{
    char buf[BUF_LEN];
    const contract_telemetry_t t = sample_telemetry();

    TEST_ASSERT_EQUAL(FW_OK, contract_build_telemetry(&t, buf, sizeof(buf)));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);

    TEST_ASSERT_EQUAL_DOUBLE(1, num_of(root, "v"));
    TEST_ASSERT_EQUAL_STRING("2026-10-13T12:00:00Z", str_of(root, "ts"));
    TEST_ASSERT_EQUAL_STRING("0.1.0", str_of(root, "fw"));
    TEST_ASSERT_EQUAL_STRING("a81f3c92d4e5f601", str_of(root, "boot_id"));
    TEST_ASSERT_EQUAL_DOUBLE(42, num_of(root, "seq"));
    TEST_ASSERT_EQUAL_STRING("gps", str_of(root, "time_source"));

    const cJSON *readings = cJSON_GetObjectItemCaseSensitive(root, "readings");
    TEST_ASSERT_TRUE(cJSON_IsArray(readings));
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(readings));

    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "sys");
    TEST_ASSERT_EQUAL_DOUBLE(-67, num_of(sys, "rssi"));
    TEST_ASSERT_EQUAL_DOUBLE(182340, num_of(sys, "heap_free"));
    TEST_ASSERT_EQUAL_DOUBLE(0, num_of(sys, "journal_pending"));

    const cJSON *gps = cJSON_GetObjectItemCaseSensitive(root, "gps");
    TEST_ASSERT_DOUBLE_WITHIN(1e-7, -17.791512, num_of(gps, "lat"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-7, -50.919234, num_of(gps, "lon"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.9, num_of(gps, "hdop"));
    TEST_ASSERT_EQUAL_DOUBLE(9, num_of(gps, "sats"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 812.4, num_of(gps, "alt_m"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.3, num_of(gps, "speed_kmh"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 271.0, num_of(gps, "course_deg"));

    /* O servidor não confere crc; o contrato manda não enviar. */
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "crc"));
    cJSON_Delete(root);

    /* Os floats saem arredondados, sem ruído binário (0.9, não 0.89999997). */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"hdop\":0.9,"));
}

static void test_telemetry_without_fix_omits_gps(void)
{
    char buf[BUF_LEN];
    contract_telemetry_t t = sample_telemetry();
    t.has_gps = false;
    t.has_rssi = false;
    t.time_source = CONTRACT_TIME_NTP;

    TEST_ASSERT_EQUAL(FW_OK, contract_build_telemetry(&t, buf, sizeof(buf)));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "gps"));
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "readings")));
    TEST_ASSERT_EQUAL_STRING("ntp", str_of(root, "time_source"));
    TEST_ASSERT_NULL(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "sys"), "rssi"));
    cJSON_Delete(root);
}

static void test_build_reports_small_buffer(void)
{
    char buf[32];
    const contract_telemetry_t t = sample_telemetry();
    TEST_ASSERT_EQUAL(FW_ERR_NO_MEM, contract_build_telemetry(&t, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_PARAM, contract_build_telemetry(NULL, buf, sizeof(buf)));
}

static fw_err_t parse_ack(const char *json, contract_telemetry_ack_t *ack)
{
    return contract_parse_telemetry_ack(json, strlen(json), ack);
}

static void test_telemetry_ack_parsing(void)
{
    contract_telemetry_ack_t ack;

    TEST_ASSERT_EQUAL(
        FW_OK,
        parse_ack("{\"v\":1,\"boot_id\":\"a81f3c92d4e5f601\",\"seq\":42,\"status\":\"stored\"}",
                  &ack));
    TEST_ASSERT_EQUAL_STRING("a81f3c92d4e5f601", ack.boot_id);
    TEST_ASSERT_EQUAL_UINT32(42, ack.seq);
    TEST_ASSERT_EQUAL(CONTRACT_ACK_ST_STORED, ack.status);

    TEST_ASSERT_EQUAL(
        FW_OK, parse_ack("{\"v\":1,\"boot_id\":\"b\",\"seq\":1,\"status\":\"duplicate\"}", &ack));
    TEST_ASSERT_EQUAL(CONTRACT_ACK_ST_DUPLICATE, ack.status);

    static const struct {
        const char *reason;
        contract_ack_reason_t expected;
    } cases[] = {
        {"invalid", CONTRACT_ACK_RSN_INVALID},
        {"unclaimed", CONTRACT_ACK_RSN_UNCLAIMED},
        {"revoked", CONTRACT_ACK_RSN_REVOKED},
        {"clock_skew", CONTRACT_ACK_RSN_CLOCK_SKEW},
        {"motivo_novo", CONTRACT_ACK_RSN_UNKNOWN},
    };
    for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        char json[160];
        (void)snprintf(json, sizeof(json),
                       "{\"v\":1,\"boot_id\":\"b\",\"seq\":7,\"status\":\"rejected\",\"reason\":\"%s\"}",
                       cases[i].reason);
        TEST_ASSERT_EQUAL(FW_OK, parse_ack(json, &ack));
        TEST_ASSERT_EQUAL(CONTRACT_ACK_ST_REJECTED, ack.status);
        TEST_ASSERT_EQUAL(cases[i].expected, ack.reason);
    }

    /* Campo novo da nuvem não pode quebrar o firmware. */
    TEST_ASSERT_EQUAL(FW_OK, parse_ack("{\"v\":1,\"boot_id\":\"b\",\"seq\":1,\"status\":\"stored\","
                                       "\"futuro\":{\"x\":[1,2]}}",
                                       &ack));

    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA,
                      parse_ack("{\"v\":1,\"boot_id\":\"b\",\"seq\":1,\"status\":\"talvez\"}", &ack));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, parse_ack("{\"v\":1,\"seq\":1,\"status\":\"stored\"}", &ack));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA,
                      parse_ack("{\"v\":1,\"boot_id\":\"b\",\"seq\":-1,\"status\":\"stored\"}", &ack));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, parse_ack("nao e json", &ack));
}

static void test_status_and_lwt(void)
{
    char buf[BUF_LEN];
    contract_status_t s;
    memset(&s, 0, sizeof(s));
    (void)contract_time_parse("2026-10-13T12:00:00Z", &s.ts);
    s.fw = "0.1.0";
    s.boot_id = "a81f3c92d4e5f601";
    s.uptime_s = 3600;
    s.has_rssi = true;
    s.rssi = -67;
    s.journal_pending = 3;
    s.journal_dropped = 1;

    TEST_ASSERT_EQUAL(FW_OK, contract_build_status(&s, buf, sizeof(buf)));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_DOUBLE(1, num_of(root, "v"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "online")));
    TEST_ASSERT_EQUAL_DOUBLE(3600, num_of(root, "uptime_s"));
    TEST_ASSERT_EQUAL_DOUBLE(3, num_of(root, "journal_pending"));
    TEST_ASSERT_EQUAL_DOUBLE(1, num_of(root, "journal_dropped"));
    cJSON_Delete(root);

    /* O LWT precisa ser exatamente este texto. */
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"online\":false}", CONTRACT_LWT_PAYLOAD);
}

static void test_event(void)
{
    char buf[BUF_LEN];
    int64_t ts = 0;
    (void)contract_time_parse("2026-10-13T12:00:00Z", &ts);

    TEST_ASSERT_EQUAL(FW_OK,
                      contract_build_event(ts, CONTRACT_EV_RELAY_FAILSAFE,
                                           "{\"reason\":\"broker_lost\",\"after_s\":60}", buf,
                                           sizeof(buf)));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING("relay_failsafe", str_of(root, "type"));
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    TEST_ASSERT_EQUAL_STRING("broker_lost", str_of(data, "reason"));
    TEST_ASSERT_EQUAL_DOUBLE(60, num_of(data, "after_s"));
    cJSON_Delete(root);

    TEST_ASSERT_EQUAL(FW_OK, contract_build_event(ts, CONTRACT_EV_BOOT, NULL, buf, sizeof(buf)));
    root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "data"));
    cJSON_Delete(root);

    /* `data` quebrado não pode gerar um evento inválido. */
    TEST_ASSERT_NOT_EQUAL(FW_OK,
                          contract_build_event(ts, CONTRACT_EV_BOOT, "{\"a\":", buf, sizeof(buf)));
}

static void test_state(void)
{
    char buf[BUF_LEN];
    contract_state_t s;
    memset(&s, 0, sizeof(s));
    (void)contract_time_parse("2026-10-13T14:00:01Z", &s.ts);
    s.config_version = 3;
    s.relay_count = 3;
    s.relays_mask = 0x02; /* só o canal 1 fechado */
    s.input_count = 0;

    TEST_ASSERT_EQUAL(FW_OK, contract_build_state(&s, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"ts\":\"2026-10-13T14:00:01Z\",\"config_version\":3,"
                             "\"estop\":false,\"relays\":[false,true,false],\"inputs\":[]}",
                             buf);

    s.estop = true;
    s.input_count = 4;
    s.inputs_mask = 0x01;
    TEST_ASSERT_EQUAL(FW_OK, contract_build_state(&s, buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"estop\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"inputs\":[true,false,false,false]"));

    s.relay_count = 17;
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_PARAM, contract_build_state(&s, buf, sizeof(buf)));
}

static fw_err_t parse_cmd(const char *json, contract_command_t *cmd)
{
    return contract_parse_command(json, strlen(json), cmd);
}

/* docs/contracts/v2/examples/cmd.json e cmd.estop.json */
static const char CMD_EXAMPLE[] =
    "{\"v\":1,\"id\":\"6f1c2b9e-1d2a-4f5e-9b7a-3c4d5e6f7a8b\",\"type\":\"relay.pulse\","
    "\"params\":{\"channel\":0,\"pulse_ms\":500},\"issued_at\":\"2026-10-13T14:00:00Z\","
    "\"expires_at\":\"2026-10-13T14:00:15Z\",\"ttl_ms\":14800}";
static const char CMD_ESTOP_EXAMPLE[] =
    "{\"v\":1,\"id\":\"0a9b8c7d-6e5f-4a3b-2c1d-0e9f8a7b6c5d\",\"type\":\"estop\","
    "\"issued_at\":\"2026-10-13T14:01:00Z\",\"expires_at\":\"2026-10-13T14:01:15Z\","
    "\"ttl_ms\":15000}";

static void test_command_parsing(void)
{
    contract_command_t cmd;
    int64_t expires = 0;

    TEST_ASSERT_EQUAL(FW_OK, parse_cmd(CMD_EXAMPLE, &cmd));
    TEST_ASSERT_EQUAL_STRING("6f1c2b9e-1d2a-4f5e-9b7a-3c4d5e6f7a8b", cmd.id);
    TEST_ASSERT_TRUE(cmd.version_ok);
    TEST_ASSERT_TRUE(cmd.fields_ok);
    TEST_ASSERT_TRUE(cmd.params_ok);
    TEST_ASSERT_EQUAL(CONTRACT_CMD_T_RELAY_PULSE, cmd.type);
    TEST_ASSERT_EQUAL_UINT8(0, cmd.channel);
    TEST_ASSERT_EQUAL_UINT32(500, cmd.pulse_ms);
    TEST_ASSERT_EQUAL_UINT32(14800, cmd.ttl_ms);
    (void)contract_time_parse("2026-10-13T14:00:15Z", &expires);
    TEST_ASSERT_EQUAL_INT64(expires, cmd.expires_at);

    TEST_ASSERT_EQUAL(FW_OK, parse_cmd(CMD_ESTOP_EXAMPLE, &cmd));
    TEST_ASSERT_EQUAL(CONTRACT_CMD_T_ESTOP, cmd.type);
    TEST_ASSERT_TRUE(cmd.params_ok);
    TEST_ASSERT_TRUE(cmd.fields_ok);

    /* `v` é opcional (padrão 1); outra versão não. */
    TEST_ASSERT_EQUAL(FW_OK, parse_cmd("{\"id\":\"a\",\"type\":\"estop\",\"issued_at\":"
                                       "\"2026-10-13T14:01:00Z\",\"expires_at\":"
                                       "\"2026-10-13T14:01:15Z\",\"ttl_ms\":1}",
                                       &cmd));
    TEST_ASSERT_TRUE(cmd.version_ok);
    TEST_ASSERT_EQUAL(FW_OK, parse_cmd("{\"v\":2,\"id\":\"a\",\"type\":\"estop\"}", &cmd));
    TEST_ASSERT_FALSE(cmd.version_ok);
    TEST_ASSERT_FALSE(cmd.fields_ok);

    /* Fora das faixas do schema: canal 16, pulso < 100 ou > 5000, ttl > 60 s. */
    static const char *const bad_params[] = {
        "{\"channel\":16,\"pulse_ms\":500}",
        "{\"channel\":0,\"pulse_ms\":99}",
        "{\"channel\":0,\"pulse_ms\":5001}",
        "{\"channel\":0}",
    };
    for (size_t i = 0; i < 4U; i++) {
        char json[300];
        (void)snprintf(json, sizeof(json),
                       "{\"id\":\"b\",\"type\":\"relay.pulse\",\"params\":%s,\"issued_at\":"
                       "\"2026-10-13T14:00:00Z\",\"expires_at\":\"2026-10-13T14:00:15Z\","
                       "\"ttl_ms\":1000}",
                       bad_params[i]);
        TEST_ASSERT_EQUAL(FW_OK, parse_cmd(json, &cmd));
        TEST_ASSERT_FALSE_MESSAGE(cmd.params_ok, bad_params[i]);
    }
    TEST_ASSERT_EQUAL(FW_OK, parse_cmd("{\"id\":\"c\",\"type\":\"estop\",\"issued_at\":"
                                       "\"2026-10-13T14:01:00Z\",\"expires_at\":"
                                       "\"2026-10-13T14:01:15Z\",\"ttl_ms\":60001}",
                                       &cmd));
    TEST_ASSERT_FALSE(cmd.fields_ok);

    TEST_ASSERT_EQUAL(FW_OK, parse_cmd("{\"id\":\"d\",\"type\":\"relay.set\"}", &cmd));
    TEST_ASSERT_EQUAL(CONTRACT_CMD_T_UNKNOWN, cmd.type);

    /* Sem id não há como responder. */
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, parse_cmd("{\"type\":\"estop\"}", &cmd));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, parse_cmd("{\"id\":\"\"}", &cmd));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_DATA, parse_cmd("{\"id\":", &cmd));
}

static void test_command_ack(void)
{
    char buf[BUF_LEN];
    contract_cmd_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.id = "6f1c2b9e-1d2a-4f5e-9b7a-3c4d5e6f7a8b";
    ack.status = CONTRACT_CMD_STATUS_RECEIVED;

    TEST_ASSERT_EQUAL(FW_OK, contract_build_cmd_ack(&ack, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"v\":1,\"id\":\"6f1c2b9e-1d2a-4f5e-9b7a-3c4d5e6f7a8b\",\"status\":\"received\"}", buf);

    /* docs/contracts/v2/examples/cmd-ack.rejected.json */
    ack.status = CONTRACT_CMD_STATUS_REJECTED;
    ack.reason = "expired";
    TEST_ASSERT_EQUAL(FW_OK, contract_build_cmd_ack(&ack, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"id\":\"6f1c2b9e-1d2a-4f5e-9b7a-3c4d5e6f7a8b\","
                             "\"status\":\"rejected\",\"reason\":\"expired\"}",
                             buf);

    ack.status = CONTRACT_CMD_STATUS_COMPLETED;
    ack.reason = NULL;
    ack.has_ts = true;
    (void)contract_time_parse("2026-10-13T14:00:00Z", &ack.ts);
    TEST_ASSERT_EQUAL(FW_OK, contract_build_cmd_ack(&ack, buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"status\":\"completed\",\"ts\":\"2026-10-13T14:00:00Z\""));
}

void run_contract_tests(void)
{
    RUN_TEST(test_time_format_and_parse);
    RUN_TEST(test_telemetry_envelope_fields);
    RUN_TEST(test_telemetry_without_fix_omits_gps);
    RUN_TEST(test_build_reports_small_buffer);
    RUN_TEST(test_telemetry_ack_parsing);
    RUN_TEST(test_status_and_lwt);
    RUN_TEST(test_event);
    RUN_TEST(test_state);
    RUN_TEST(test_command_parsing);
    RUN_TEST(test_command_ack);
}

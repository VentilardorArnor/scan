#include <stdio.h>
#include <string.h>

#include "control.h"
#include "test_support.h"

#define NOW_EPOCH (1791892800LL)

/* docs/contracts/v2/examples/config.json, com um terceiro canal */
static const char CONFIG_JSON[] =
    "{\"v\":1,\"version\":3,\"estop\":false,\"failsafe_s\":60,\"channels\":["
    "{\"channel\":0,\"kind\":\"start\",\"pulse_ms\":500},"
    "{\"channel\":1,\"kind\":\"stop\",\"pulse_ms\":500},"
    "{\"channel\":5,\"kind\":\"custom\",\"pulse_ms\":1000}]}";

static control_ctx_t s_ctx;
static control_env_t s_env;

static control_config_t parse_ok(const char *json)
{
    control_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    TEST_ASSERT_EQUAL_MESSAGE(FW_OK, control_config_parse(json, strlen(json), &cfg, NULL), json);
    return cfg;
}

static void reset_with_config(void)
{
    control_init(&s_ctx, false, 0);
    const control_config_t cfg = parse_ok(CONFIG_JSON);
    (void)control_apply_config(&s_ctx, &cfg);

    memset(&s_env, 0, sizeof(s_env));
    s_env.time_valid = true;
    s_env.now_epoch = NOW_EPOCH;
    s_env.relay_count = 3;
}

static contract_command_t pulse(const char *id, uint8_t channel, uint32_t pulse_ms)
{
    contract_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    (void)snprintf(cmd.id, sizeof(cmd.id), "%s", id);
    cmd.version_ok = true;
    cmd.fields_ok = true;
    cmd.params_ok = true;
    cmd.type = CONTRACT_CMD_T_RELAY_PULSE;
    cmd.expires_at = NOW_EPOCH + 15;
    cmd.ttl_ms = 15000;
    cmd.channel = channel;
    cmd.pulse_ms = pulse_ms;
    return cmd;
}

static contract_command_t estop(const char *id)
{
    contract_command_t cmd = pulse(id, 0, 0);
    cmd.type = CONTRACT_CMD_T_ESTOP;
    return cmd;
}

static control_verdict_t eval(const contract_command_t *cmd)
{
    control_verdict_t v;
    control_evaluate(&s_ctx, cmd, &s_env, &v);
    return v;
}

static void assert_rejected(const contract_command_t *cmd, const char *reason)
{
    const control_verdict_t v = eval(cmd);
    TEST_ASSERT_EQUAL_MESSAGE(CONTROL_VERDICT_REJECT, v.kind, reason);
    TEST_ASSERT_EQUAL(CONTROL_ACTION_NONE, v.action);
    TEST_ASSERT_NOT_NULL(v.entry);
    TEST_ASSERT_EQUAL(CONTRACT_CMD_STATUS_REJECTED, v.entry->last_status);
    TEST_ASSERT_EQUAL_STRING(reason, v.entry->reason);
}

/* ------------------------------------------------------------------ */

static void test_config_example(void)
{
    const control_config_t cfg = parse_ok(CONFIG_JSON);
    TEST_ASSERT_EQUAL_UINT32(3, cfg.version);
    TEST_ASSERT_FALSE(cfg.estop);
    TEST_ASSERT_EQUAL_UINT16(60, cfg.failsafe_s);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.channel_count);
    TEST_ASSERT_EQUAL_UINT8(5, cfg.channels[2].channel);
    TEST_ASSERT_EQUAL(CONTROL_KIND_CUSTOM, cfg.channels[2].kind);
    TEST_ASSERT_EQUAL_UINT16(1000, cfg.channels[2].pulse_ms);

    /* `v` ausente vale; campo desconhecido é ignorado; lista vazia vale. */
    const control_config_t empty = parse_ok(
        "{\"version\":1,\"estop\":true,\"failsafe_s\":10,\"channels\":[],\"novo\":{\"x\":1}}");
    TEST_ASSERT_TRUE(empty.estop);
    TEST_ASSERT_EQUAL_UINT8(0, empty.channel_count);

    /* Botões de água (pivo-saas 3d3d1fb): sem eles a config inteira seria
     * recusada e nenhum botão funcionaria. */
    const control_config_t water = parse_ok(
        "{\"version\":2,\"estop\":false,\"failsafe_s\":60,\"channels\":["
        "{\"channel\":2,\"kind\":\"water_on\",\"pulse_ms\":500},"
        "{\"channel\":3,\"kind\":\"water_off\",\"pulse_ms\":500}]}");
    TEST_ASSERT_EQUAL(CONTROL_KIND_WATER_ON, water.channels[0].kind);
    TEST_ASSERT_EQUAL(CONTROL_KIND_WATER_OFF, water.channels[1].kind);
}

static void test_config_invalid_keeps_previous(void)
{
    static const char *const bad[] = {
        "nao e json",
        "{\"v\":2,\"version\":1,\"estop\":false,\"failsafe_s\":60,\"channels\":[]}",
        "{\"estop\":false,\"failsafe_s\":60,\"channels\":[]}",
        "{\"version\":0,\"estop\":false,\"failsafe_s\":60,\"channels\":[]}",
        "{\"version\":1,\"failsafe_s\":60,\"channels\":[]}",
        "{\"version\":1,\"estop\":0,\"failsafe_s\":60,\"channels\":[]}",
        "{\"version\":1,\"estop\":false,\"failsafe_s\":9,\"channels\":[]}",
        "{\"version\":1,\"estop\":false,\"failsafe_s\":601,\"channels\":[]}",
        "{\"version\":1,\"estop\":false,\"failsafe_s\":60}",
        "{\"version\":1,\"estop\":false,\"failsafe_s\":60,\"channels\":"
        "[{\"channel\":16,\"kind\":\"start\",\"pulse_ms\":500}]}",
        "{\"version\":1,\"estop\":false,\"failsafe_s\":60,\"channels\":"
        "[{\"channel\":0,\"kind\":\"liga\",\"pulse_ms\":500}]}",
        "{\"version\":1,\"estop\":false,\"failsafe_s\":60,\"channels\":"
        "[{\"channel\":0,\"kind\":\"start\",\"pulse_ms\":99}]}",
        "{\"version\":1,\"estop\":false,\"failsafe_s\":60,\"channels\":"
        "[{\"channel\":0,\"kind\":\"start\",\"pulse_ms\":5001}]}",
        "{\"version\":1,\"estop\":false,\"failsafe_s\":60,\"channels\":"
        "[{\"channel\":0,\"kind\":\"start\",\"pulse_ms\":500},"
        "{\"channel\":0,\"kind\":\"stop\",\"pulse_ms\":500}]}",
    };

    control_config_t cfg = parse_ok(CONFIG_JSON);
    const control_config_t before = cfg;
    for (size_t i = 0; i < (sizeof(bad) / sizeof(bad[0])); i++) {
        const char *detail = NULL;
        TEST_ASSERT_EQUAL_MESSAGE(FW_ERR_INVALID_DATA,
                                  control_config_parse(bad[i], strlen(bad[i]), &cfg, &detail),
                                  bad[i]);
        TEST_ASSERT_NOT_NULL(detail);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &cfg, sizeof(cfg), bad[i]);
    }
}

static void test_pulse_executes_with_limits(void)
{
    reset_with_config();
    contract_command_t cmd = pulse("p1", 0, 400);
    control_verdict_t v = eval(&cmd);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_EXECUTE, v.kind);
    TEST_ASSERT_EQUAL(CONTROL_ACTION_PULSE, v.action);
    TEST_ASSERT_EQUAL_UINT8(0, v.channel);
    TEST_ASSERT_EQUAL_UINT32(400, v.pulse_ms);
    TEST_ASSERT_EQUAL(CONTRACT_CMD_STATUS_RECEIVED, v.entry->last_status);

    /* Nunca passa do pulse_ms do canal (500). */
    cmd = pulse("p2", 1, 5000);
    v = eval(&cmd);
    TEST_ASSERT_EQUAL_UINT32(500, v.pulse_ms);
}

static void test_each_id_runs_at_most_once(void)
{
    reset_with_config();
    const contract_command_t cmd = pulse("dup", 0, 500);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_EXECUTE, eval(&cmd).kind);

    /* Reenvio do servidor enquanto o pulso corre: sem nova ação. */
    control_verdict_t v = eval(&cmd);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_DUPLICATE, v.kind);
    TEST_ASSERT_EQUAL(CONTROL_ACTION_NONE, v.action);
    TEST_ASSERT_EQUAL(CONTRACT_CMD_STATUS_RECEIVED, v.entry->last_status);

    (void)control_record(&s_ctx, "dup", CONTRACT_CMD_STATUS_COMPLETED, NULL);
    v = eval(&cmd);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_DUPLICATE, v.kind);
    TEST_ASSERT_EQUAL(CONTRACT_CMD_STATUS_COMPLETED, v.entry->last_status);

    /* Recusa repetida também não reavalia. */
    const contract_command_t bad = pulse("dup-bad", 9, 500);
    assert_rejected(&bad, CONTRACT_CMD_REASON_UNKNOWN_CHANNEL);
    v = eval(&bad);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_DUPLICATE, v.kind);
    TEST_ASSERT_EQUAL_STRING(CONTRACT_CMD_REASON_UNKNOWN_CHANNEL, v.entry->reason);
}

static void test_cache_keeps_last_32(void)
{
    char id[16];
    reset_with_config();
    for (uint32_t i = 0; i < (CONTRACT_COMMAND_CACHE_SIZE + 1U); i++) {
        (void)snprintf(id, sizeof(id), "c%u", (unsigned)i);
        const contract_command_t cmd = pulse(id, 9, 500);
        TEST_ASSERT_EQUAL(CONTROL_VERDICT_REJECT, eval(&cmd).kind);
    }
    const contract_command_t recent = pulse("c32", 9, 500);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_DUPLICATE, eval(&recent).kind);
    const contract_command_t oldest = pulse("c0", 9, 500);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_REJECT, eval(&oldest).kind);
}

static void test_expiration(void)
{
    reset_with_config();
    contract_command_t cmd = pulse("e1", 0, 500);
    cmd.expires_at = NOW_EPOCH - 1;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_EXPIRED);

    cmd = pulse("e2", 0, 500);
    cmd.expires_at = NOW_EPOCH; /* no limite ainda vale */
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_EXECUTE, eval(&cmd).kind);

    /* E-stop vencido também não executa (regra 2). */
    cmd = estop("e3");
    cmd.expires_at = NOW_EPOCH - 1;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_EXPIRED);
    TEST_ASSERT_FALSE(s_ctx.estop);

    /* Sem relógio vale o ttl_ms desde a chegada. */
    s_env.time_valid = false;
    s_env.now_epoch = 0;
    cmd = pulse("e4", 1, 500);
    cmd.expires_at = NOW_EPOCH - 100; /* ignorado sem relógio */
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_EXECUTE, eval(&cmd).kind);
    cmd = pulse("e5", 1, 500);
    cmd.ttl_ms = 0;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_EXPIRED);
}

static void test_rejections(void)
{
    control_init(&s_ctx, false, 0);
    memset(&s_env, 0, sizeof(s_env));
    s_env.time_valid = true;
    s_env.now_epoch = NOW_EPOCH;
    s_env.relay_count = 3;

    contract_command_t cmd = pulse("r1", 0, 500);
    assert_rejected(&cmd, CONTRACT_CMD_REASON_NO_CONFIG);

    reset_with_config();
    cmd = pulse("r2", 2, 500); /* relé existe, mas canal fora da config */
    assert_rejected(&cmd, CONTRACT_CMD_REASON_UNKNOWN_CHANNEL);
    cmd = pulse("r3", 5, 500); /* na config, mas a placa só tem 3 relés */
    assert_rejected(&cmd, CONTRACT_CMD_REASON_UNKNOWN_CHANNEL);

    s_env.busy_mask = 0x01;
    cmd = pulse("r4", 0, 500);
    assert_rejected(&cmd, CONTRACT_CMD_REASON_BUSY);
    s_env.busy_mask = 0;

    cmd = pulse("r5", 0, 500);
    cmd.params_ok = false;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_INVALID);
    cmd = pulse("r6", 0, 500);
    cmd.fields_ok = false;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_INVALID);
    cmd = pulse("r7", 0, 500);
    cmd.type = CONTRACT_CMD_T_UNKNOWN;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_INVALID);
    cmd = pulse("r8", 0, 500);
    cmd.version_ok = false;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_INVALID);
}

static void test_estop_latch(void)
{
    /* E-stop vale mesmo sem config. */
    control_init(&s_ctx, false, 0);
    memset(&s_env, 0, sizeof(s_env));
    s_env.time_valid = true;
    s_env.now_epoch = NOW_EPOCH;
    s_env.relay_count = 3;
    contract_command_t cmd = estop("s1");
    control_verdict_t v = eval(&cmd);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_EXECUTE, v.kind);
    TEST_ASSERT_EQUAL(CONTROL_ACTION_ESTOP, v.action);
    TEST_ASSERT_TRUE(s_ctx.estop);

    reset_with_config(); /* config v3, estop:false */
    cmd = estop("s2");
    TEST_ASSERT_EQUAL(CONTROL_ACTION_ESTOP, eval(&cmd).action);
    cmd = pulse("s3", 0, 500);
    assert_rejected(&cmd, CONTRACT_CMD_REASON_ESTOP);

    /* A mesma config retida (v3, estop:false) reentregue não destrava. */
    control_config_t cfg = parse_ok(CONFIG_JSON);
    TEST_ASSERT_FALSE(control_apply_config(&s_ctx, &cfg));
    TEST_ASSERT_TRUE(s_ctx.estop);

    /* O servidor sobe a versão ao ligar o E-stop (v4 estop:true) e ao
     * liberar (v5 estop:false). */
    cfg.version = 4;
    cfg.estop = true;
    TEST_ASSERT_FALSE(control_apply_config(&s_ctx, &cfg));
    TEST_ASSERT_TRUE(s_ctx.estop);
    cfg.version = 4;
    cfg.estop = false;
    TEST_ASSERT_FALSE(control_apply_config(&s_ctx, &cfg)); /* mesma versão: não */
    cfg.version = 5;
    TEST_ASSERT_TRUE(control_apply_config(&s_ctx, &cfg));
    TEST_ASSERT_FALSE(s_ctx.estop);

    cmd = pulse("s4", 0, 500);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_EXECUTE, eval(&cmd).kind);
}

static void test_estop_persisted_across_reboot(void)
{
    /* Boot com E-stop travado na v4 lido do NVS; a config retida v4 chega. */
    control_init(&s_ctx, true, 4);
    control_config_t cfg = parse_ok(CONFIG_JSON);
    cfg.version = 4;
    TEST_ASSERT_FALSE(control_apply_config(&s_ctx, &cfg));
    TEST_ASSERT_TRUE(s_ctx.estop);
    cfg.version = 5;
    TEST_ASSERT_TRUE(control_apply_config(&s_ctx, &cfg));
    TEST_ASSERT_FALSE(s_ctx.estop);
}

static void test_control_disabled_rejects_without_acting(void)
{
    /* Placa com o acionamento desligado: a config vale, o comando não age. */
    reset_with_config();
    s_env.control_disabled = true;

    contract_command_t cmd = pulse("d1", 0, 500); /* seria executado */
    assert_rejected(&cmd, CONTRACT_CMD_REASON_CONTROL_DISABLED);

    cmd = estop("d2");
    assert_rejected(&cmd, CONTRACT_CMD_REASON_CONTROL_DISABLED);
    TEST_ASSERT_FALSE(s_ctx.estop); /* recusado = não travou nada */

    /* Reenvio do mesmo id repete a recusa, sem reavaliar. */
    s_env.control_disabled = false;
    cmd = pulse("d1", 0, 500);
    control_verdict_t v = eval(&cmd);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_DUPLICATE, v.kind);
    TEST_ASSERT_EQUAL(CONTRACT_CMD_STATUS_REJECTED, v.entry->last_status);
    TEST_ASSERT_EQUAL_STRING(CONTRACT_CMD_REASON_CONTROL_DISABLED, v.entry->reason);
    s_env.control_disabled = true;

    /* Vencido e inválido continuam com o motivo próprio. */
    cmd = pulse("d3", 0, 500);
    cmd.expires_at = NOW_EPOCH - 1;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_EXPIRED);
    cmd = pulse("d4", 0, 500);
    cmd.fields_ok = false;
    assert_rejected(&cmd, CONTRACT_CMD_REASON_INVALID);

    /* A config com E-stop continua travando e sendo confirmada. */
    control_config_t cfg = parse_ok(CONFIG_JSON);
    cfg.version = 6;
    cfg.estop = true;
    TEST_ASSERT_TRUE(control_apply_config(&s_ctx, &cfg));
    TEST_ASSERT_TRUE(s_ctx.estop);
    TEST_ASSERT_EQUAL_UINT32(6, s_ctx.config.version);
    cmd = pulse("d5", 0, 500);
    assert_rejected(&cmd, CONTRACT_CMD_REASON_CONTROL_DISABLED);
}

static void test_backend_envelope_end_to_end(void)
{
    /* Bytes reais do pivo-saas: CommandEnvelope.model_dump(mode="json",
     * exclude_none=True), com microssegundos e sem `params` no estop. A config
     * é a v12 da bancada (canais 0 e 2, 500 ms). */
    static const char cfg_json[] =
        "{\"v\":1,\"version\":12,\"estop\":false,\"failsafe_s\":60,\"channels\":["
        "{\"channel\":0,\"kind\":\"start\",\"pulse_ms\":500},"
        "{\"channel\":2,\"kind\":\"forward\",\"pulse_ms\":500}]}";
    static const char pulse_json[] =
        "{\"v\":1,\"id\":\"7b0c2f6e-4d1a-4c8e-9a51-2f3d5e6a7b8c\",\"type\":\"relay.pulse\","
        "\"params\":{\"channel\":2,\"pulse_ms\":500},\"issued_at\":\"2026-10-24T03:15:20.123456Z\","
        "\"expires_at\":\"2026-10-24T03:15:35.123456Z\",\"ttl_ms\":14876}";
    static const char estop_json[] =
        "{\"v\":1,\"id\":\"9c1d3e5f-0000-4c8e-9a51-2f3d5e6a7b8c\",\"type\":\"estop\","
        "\"issued_at\":\"2026-10-24T03:15:20.123456Z\","
        "\"expires_at\":\"2026-10-24T03:15:35.123456Z\",\"ttl_ms\":14876}";

    control_init(&s_ctx, false, 0);
    const control_config_t cfg = parse_ok(cfg_json);
    (void)control_apply_config(&s_ctx, &cfg);
    memset(&s_env, 0, sizeof(s_env));
    s_env.time_valid = true;
    TEST_ASSERT_EQUAL(FW_OK, contract_time_parse("2026-10-24T03:15:21Z", &s_env.now_epoch));
    s_env.relay_count = 3;

    contract_command_t cmd;
    TEST_ASSERT_EQUAL(FW_OK, contract_parse_command(pulse_json, strlen(pulse_json), &cmd));
    control_verdict_t v = eval(&cmd);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_EXECUTE, v.kind);
    TEST_ASSERT_EQUAL(CONTROL_ACTION_PULSE, v.action);
    TEST_ASSERT_EQUAL_UINT8(2, v.channel); /* canal 2 = 3º relé (GPIO6) */
    TEST_ASSERT_EQUAL_UINT32(500, v.pulse_ms);

    TEST_ASSERT_EQUAL(FW_OK, contract_parse_command(estop_json, strlen(estop_json), &cmd));
    v = eval(&cmd);
    TEST_ASSERT_EQUAL(CONTROL_VERDICT_EXECUTE, v.kind);
    TEST_ASSERT_EQUAL(CONTROL_ACTION_ESTOP, v.action);
}

void run_control_tests(void)
{
    RUN_TEST(test_config_example);
    RUN_TEST(test_config_invalid_keeps_previous);
    RUN_TEST(test_pulse_executes_with_limits);
    RUN_TEST(test_each_id_runs_at_most_once);
    RUN_TEST(test_cache_keeps_last_32);
    RUN_TEST(test_expiration);
    RUN_TEST(test_rejections);
    RUN_TEST(test_estop_latch);
    RUN_TEST(test_estop_persisted_across_reboot);
    RUN_TEST(test_control_disabled_rejects_without_acting);
    RUN_TEST(test_backend_envelope_end_to_end);
}

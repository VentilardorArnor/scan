#include "control.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define CH_BIT(n) ((uint16_t)(1U << (n)))

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static void set_detail(const char **detail, const char *text)
{
    if (detail != NULL) {
        *detail = text;
    }
}

/* Inteiro em [min, max]. Ausente = erro (todos os campos lidos aqui são obrigatórios). */
static bool read_int(const cJSON *obj, const char *key, uint32_t min, uint32_t max, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || (item->valuedouble != floor(item->valuedouble)) ||
        (item->valuedouble < (double)min) || (item->valuedouble > (double)max)) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

static bool kind_from_str(const char *text, control_kind_t *kind)
{
    static const struct {
        const char *name;
        control_kind_t kind;
    } kinds[] = {
        {CONTRACT_KIND_START, CONTROL_KIND_START},
        {CONTRACT_KIND_STOP, CONTROL_KIND_STOP},
        {CONTRACT_KIND_FORWARD, CONTROL_KIND_FORWARD},
        {CONTRACT_KIND_REVERSE, CONTROL_KIND_REVERSE},
        {CONTRACT_KIND_WATER_ON, CONTROL_KIND_WATER_ON},
        {CONTRACT_KIND_WATER_OFF, CONTROL_KIND_WATER_OFF},
        {CONTRACT_KIND_CUSTOM, CONTROL_KIND_CUSTOM},
    };
    for (size_t i = 0; i < (sizeof(kinds) / sizeof(kinds[0])); i++) {
        if (strcmp(text, kinds[i].name) == 0) {
            *kind = kinds[i].kind;
            return true;
        }
    }
    return false;
}

static bool parse_root(const cJSON *root, control_config_t *cfg, const char **detail)
{
    uint32_t value = 0;

    /* `v` tem padrão 1 no schema: ausente vale. */
    if ((cJSON_GetObjectItemCaseSensitive(root, CONTRACT_K_V) != NULL) &&
        !read_int(root, CONTRACT_K_V, CONTRACT_V_CONFIG, CONTRACT_V_CONFIG, &value)) {
        set_detail(detail, "v");
        return false;
    }
    if (!read_int(root, CONTRACT_K_VERSION, 1U, UINT32_MAX, &cfg->version)) {
        set_detail(detail, "version");
        return false;
    }
    const cJSON *estop = cJSON_GetObjectItemCaseSensitive(root, CONTRACT_K_ESTOP);
    if (!cJSON_IsBool(estop)) {
        set_detail(detail, "estop");
        return false;
    }
    cfg->estop = cJSON_IsTrue(estop);

    if (!read_int(root, CONTRACT_K_FAILSAFE_S, CONTRACT_FAILSAFE_MIN_S, CONTRACT_FAILSAFE_MAX_S,
                  &value)) {
        set_detail(detail, "failsafe_s");
        return false;
    }
    cfg->failsafe_s = (uint16_t)value;

    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, CONTRACT_K_CHANNELS);
    if (!cJSON_IsArray(channels) ||
        ((uint32_t)cJSON_GetArraySize(channels) > CONTRACT_CHANNEL_MAX)) {
        set_detail(detail, "channels");
        return false;
    }

    uint16_t seen = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, channels)
    {
        control_channel_t *ch = &cfg->channels[cfg->channel_count];
        uint32_t channel = 0;
        uint32_t pulse_ms = 0;
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, CONTRACT_K_KIND);

        if (!cJSON_IsObject(item) ||
            !read_int(item, CONTRACT_K_CHANNEL, 0U, CONTRACT_CHANNEL_MAX - 1U, &channel) ||
            ((seen & CH_BIT(channel)) != 0U) ||
            !read_int(item, CONTRACT_K_PULSE_MS, CONTRACT_PULSE_MIN_MS, CONTRACT_PULSE_MAX_MS,
                      &pulse_ms) ||
            !cJSON_IsString(kind) || !kind_from_str(kind->valuestring, &ch->kind)) {
            set_detail(detail, "channels.item");
            return false;
        }
        seen |= CH_BIT(channel);
        ch->channel = (uint8_t)channel;
        ch->pulse_ms = (uint16_t)pulse_ms;
        cfg->channel_count++;
    }
    return true;
}

fw_err_t control_config_parse(const char *json, size_t len, control_config_t *out,
                              const char **detail)
{
    if ((json == NULL) || (out == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }
    set_detail(detail, "");

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_detail(detail, "json");
        return FW_ERR_INVALID_DATA;
    }

    /* Monta numa cópia: `out` só muda se a config inteira for válida. */
    control_config_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    const bool ok = parse_root(root, &candidate, detail);
    cJSON_Delete(root);

    if (!ok) {
        return FW_ERR_INVALID_DATA;
    }
    *out = candidate;
    return FW_OK;
}

/* ------------------------------------------------------------------ */
/* Comandos                                                            */
/* ------------------------------------------------------------------ */

static control_entry_t *cache_find(control_ctx_t *ctx, const char *id)
{
    for (uint32_t i = 0; i < CONTRACT_COMMAND_CACHE_SIZE; i++) {
        if (ctx->cache[i].used && (strcmp(ctx->cache[i].id, id) == 0)) {
            return &ctx->cache[i];
        }
    }
    return NULL;
}

/* Anel: o 33º id sobrescreve o mais antigo. O servidor só reenvia um id 3 s
 * depois, então 32 ids cobrem com folga. */
static control_entry_t *cache_alloc(control_ctx_t *ctx, const char *id)
{
    control_entry_t *entry = &ctx->cache[ctx->cache_next];
    ctx->cache_next = (uint8_t)((ctx->cache_next + 1U) % CONTRACT_COMMAND_CACHE_SIZE);
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    (void)snprintf(entry->id, sizeof(entry->id), "%s", id);
    return entry;
}

static void entry_set(control_entry_t *entry, contract_cmd_status_t status, const char *reason)
{
    entry->last_status = status;
    (void)snprintf(entry->reason, sizeof(entry->reason), "%s", (reason != NULL) ? reason : "");
}

static const control_channel_t *find_channel(const control_ctx_t *ctx, uint8_t channel)
{
    for (uint8_t i = 0; i < ctx->config.channel_count; i++) {
        if (ctx->config.channels[i].channel == channel) {
            return &ctx->config.channels[i];
        }
    }
    return NULL;
}

void control_init(control_ctx_t *ctx, bool estop_latched, uint32_t estop_at_version)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->estop = estop_latched;
    ctx->estop_at_version = estop_at_version;
}

bool control_apply_config(control_ctx_t *ctx, const control_config_t *config)
{
    if ((ctx == NULL) || (config == NULL)) {
        return false;
    }
    const bool before = ctx->estop;

    if (config->estop) {
        if (!ctx->estop || (config->version > ctx->estop_at_version)) {
            ctx->estop_at_version = config->version;
        }
        ctx->estop = true;
    } else if (ctx->estop && (config->version > ctx->estop_at_version)) {
        ctx->estop = false;
    } else {
        /* mesma config (ou mais antiga) reentregue: o E-stop continua */
    }

    ctx->config = *config;
    ctx->has_config = true;
    return ctx->estop != before;
}

static void reject(control_entry_t *entry, control_verdict_t *verdict, const char *reason)
{
    entry_set(entry, CONTRACT_CMD_STATUS_REJECTED, reason);
    verdict->kind = CONTROL_VERDICT_REJECT;
}

void control_evaluate(control_ctx_t *ctx, const contract_command_t *cmd, const control_env_t *env,
                      control_verdict_t *verdict)
{
    if ((ctx == NULL) || (cmd == NULL) || (env == NULL) || (verdict == NULL)) {
        return;
    }
    memset(verdict, 0, sizeof(*verdict));

    /* Regra 1: cada id no máximo uma vez. */
    control_entry_t *entry = cache_find(ctx, cmd->id);
    if (entry != NULL) {
        verdict->kind = CONTROL_VERDICT_DUPLICATE;
        verdict->entry = entry;
        return;
    }
    entry = cache_alloc(ctx, cmd->id);
    verdict->entry = entry;

    if (!cmd->version_ok || !cmd->fields_ok || !cmd->params_ok ||
        (cmd->type == CONTRACT_CMD_T_UNKNOWN)) {
        reject(entry, verdict, CONTRACT_CMD_REASON_INVALID);
        return;
    }

    /* Regra 2: com relógio, `expires_at`; sem relógio, `ttl_ms` desde a
     * chegada (a avaliação acontece na chegada). */
    const bool expired = env->time_valid ? (env->now_epoch > cmd->expires_at) : (cmd->ttl_ms == 0U);
    if (expired) {
        reject(entry, verdict, CONTRACT_CMD_REASON_EXPIRED);
        return;
    }

    /* Sem acionamento nada é executado, nem o estop: responder `completed`
     * seria fingir uma ação que a placa não faz. A config continua valendo. */
    if (env->control_disabled) {
        reject(entry, verdict, CONTRACT_CMD_REASON_CONTROL_DISABLED);
        return;
    }

    /* Regra 4: estop abre tudo na hora, com ou sem config. */
    if (cmd->type == CONTRACT_CMD_T_ESTOP) {
        ctx->estop = true;
        ctx->estop_at_version = ctx->has_config ? ctx->config.version : 0U;
        entry_set(entry, CONTRACT_CMD_STATUS_RECEIVED, NULL);
        verdict->kind = CONTROL_VERDICT_EXECUTE;
        verdict->action = CONTROL_ACTION_ESTOP;
        return;
    }

    /* Regra 3 */
    if (!ctx->has_config) {
        reject(entry, verdict, CONTRACT_CMD_REASON_NO_CONFIG);
        return;
    }
    if (ctx->estop) {
        reject(entry, verdict, CONTRACT_CMD_REASON_ESTOP);
        return;
    }
    const control_channel_t *channel = find_channel(ctx, cmd->channel);
    if ((channel == NULL) || (cmd->channel >= env->relay_count)) {
        reject(entry, verdict, CONTRACT_CMD_REASON_UNKNOWN_CHANNEL);
        return;
    }
    if ((env->busy_mask & CH_BIT(cmd->channel)) != 0U) {
        reject(entry, verdict, CONTRACT_CMD_REASON_BUSY);
        return;
    }

    /* O pulso nunca passa do `pulse_ms` do canal nem do teto do firmware. */
    uint32_t pulse_ms = cmd->pulse_ms;
    if (pulse_ms > channel->pulse_ms) {
        pulse_ms = channel->pulse_ms;
    }
    if (pulse_ms > CONTRACT_PULSE_MAX_MS) {
        pulse_ms = CONTRACT_PULSE_MAX_MS;
    }

    entry_set(entry, CONTRACT_CMD_STATUS_RECEIVED, NULL);
    verdict->kind = CONTROL_VERDICT_EXECUTE;
    verdict->action = CONTROL_ACTION_PULSE;
    verdict->channel = cmd->channel;
    verdict->pulse_ms = pulse_ms;
}

const control_entry_t *control_record(control_ctx_t *ctx, const char *id,
                                      contract_cmd_status_t status, const char *reason)
{
    if ((ctx == NULL) || (id == NULL)) {
        return NULL;
    }
    control_entry_t *entry = cache_find(ctx, id);
    if (entry != NULL) {
        entry_set(entry, status, reason);
    }
    return entry;
}

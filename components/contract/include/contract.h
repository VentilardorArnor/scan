#pragma once

/*
 * contract.h — ÚNICO lugar com as constantes do contrato MQTT da plataforma
 * pivo-saas (tópicos, versões, nomes de campo, nomes de tipo e limites).
 *
 * Fonte normativa: pivo-saas docs/MQTT.md e os JSON Schemas de docs/contracts/v2/.
 * Quando o contrato mudar lá, a mudança no firmware é trocar constantes AQUI.
 * Nenhum outro arquivo pode ter literal de tópico ou de campo.
 */

/* ------------------------------------------------------------------ */
/* Tópicos: agro/v2/{cn}/<sufixo>                                      */
/* ------------------------------------------------------------------ */
#define CONTRACT_TOPIC_PREFIX            "agro/v2/"

#define CONTRACT_TOPIC_TELEMETRY         "/telemetry"
#define CONTRACT_TOPIC_TELEMETRY_ACK     "/telemetry/ack"
#define CONTRACT_TOPIC_STATUS            "/status"
#define CONTRACT_TOPIC_EVENT             "/event"

/* Controle (ADR-007): só em placa com relés */
#define CONTRACT_TOPIC_STATE             "/state"
#define CONTRACT_TOPIC_CMD               "/cmd"
#define CONTRACT_TOPIC_CMD_ACK           "/cmd/ack"
#define CONTRACT_TOPIC_CONFIG            "/config"

#define CONTRACT_QOS                     (1)
#define CONTRACT_MQTT_KEEPALIVE_S        (30)
#define CONTRACT_DEVICE_ID_MAX_LEN       (64U)
#define CONTRACT_TOPIC_MAX_LEN           (96U)

/* ------------------------------------------------------------------ */
/* Versões ("v") de cada payload                                       */
/* ------------------------------------------------------------------ */
#define CONTRACT_V_TELEMETRY             (1)
#define CONTRACT_V_TELEMETRY_ACK         (1)
#define CONTRACT_V_STATUS                (1)
#define CONTRACT_V_EVENT                 (1)
#define CONTRACT_V_STATE                 (1)
#define CONTRACT_V_CMD                   (1)   /* opcional na mensagem; ausente = 1 */
#define CONTRACT_V_CMD_ACK               (1)
#define CONTRACT_V_CONFIG                (1)   /* opcional na mensagem; ausente = 1 */

/* Last Will: exatamente este payload, retido, QoS 1, em /status */
#define CONTRACT_LWT_PAYLOAD             "{\"v\":1,\"online\":false}"

/* ------------------------------------------------------------------ */
/* Campos comuns                                                       */
/* ------------------------------------------------------------------ */
#define CONTRACT_K_V                     "v"
#define CONTRACT_K_TS                    "ts"
#define CONTRACT_K_FW                    "fw"
#define CONTRACT_K_BOOT_ID               "boot_id"
#define CONTRACT_K_SEQ                   "seq"
#define CONTRACT_K_STATUS                "status"
#define CONTRACT_K_REASON                "reason"

/* TelemetryEnvelope (FECHADO) */
#define CONTRACT_K_TIME_SOURCE           "time_source"
#define CONTRACT_K_READINGS              "readings"
#define CONTRACT_K_SYS                   "sys"
#define CONTRACT_K_SYS_RSSI              "rssi"
#define CONTRACT_K_SYS_HEAP_FREE         "heap_free"
#define CONTRACT_K_JOURNAL_PENDING       "journal_pending"
#define CONTRACT_K_GPS                   "gps"
#define CONTRACT_K_GPS_LAT               "lat"
#define CONTRACT_K_GPS_LON               "lon"
#define CONTRACT_K_GPS_HDOP              "hdop"
#define CONTRACT_K_GPS_SATS              "sats"        /* extra, diagnóstico */
#define CONTRACT_K_GPS_ALT_M             "alt_m"       /* extra, diagnóstico */
#define CONTRACT_K_GPS_SPEED_KMH         "speed_kmh"   /* extra, diagnóstico */
#define CONTRACT_K_GPS_COURSE_DEG        "course_deg"  /* extra, diagnóstico */

#define CONTRACT_TIME_SOURCE_NTP         "ntp"
#define CONTRACT_TIME_SOURCE_GPS         "gps"
#define CONTRACT_TIME_SOURCE_RTC         "rtc"
#define CONTRACT_TIME_SOURCE_NONE        "none"

/* TelemetryAck (FECHADO) */
#define CONTRACT_ACK_STORED              "stored"
#define CONTRACT_ACK_DUPLICATE           "duplicate"
#define CONTRACT_ACK_REJECTED            "rejected"
#define CONTRACT_ACK_REASON_INVALID      "invalid"
#define CONTRACT_ACK_REASON_UNCLAIMED    "unclaimed"
#define CONTRACT_ACK_REASON_REVOKED      "revoked"
#define CONTRACT_ACK_REASON_CLOCK_SKEW   "clock_skew"

/* DeviceStatus (FECHADO) */
#define CONTRACT_K_ONLINE                "online"
#define CONTRACT_K_UPTIME_S              "uptime_s"
#define CONTRACT_K_RSSI                  "rssi"
#define CONTRACT_K_JOURNAL_DROPPED       "journal_dropped"

/* DeviceEvent (FECHADO) */
#define CONTRACT_K_TYPE                  "type"
#define CONTRACT_K_DATA                  "data"

#define CONTRACT_EV_BOOT                 "boot"
#define CONTRACT_EV_BROWNOUT             "brownout"
#define CONTRACT_EV_TIME_UNSYNCED        "time_unsynced"
#define CONTRACT_EV_GPS_FIX_LOST         "gps_fix_lost"
#define CONTRACT_EV_GPS_FIX_ACQUIRED     "gps_fix_acquired"
#define CONTRACT_EV_GPS_NO_DATA          "gps_no_data"
#define CONTRACT_EV_JOURNAL_FULL         "journal_full"
#define CONTRACT_EV_JOURNAL_CORRUPT      "journal_corrupt_record"
#define CONTRACT_EV_TELEMETRY_REJECTED   "telemetry_rejected"
#define CONTRACT_EV_RELAY_FAILSAFE       "relay_failsafe"
#define CONTRACT_EV_RELAY_ESTOP          "relay_estop"
#define CONTRACT_EV_CONFIG_INVALID       "config_invalid"
#define CONTRACT_EV_OTA_FAILED           "ota_failed"

/* ------------------------------------------------------------------ */
/* Controle (ADR-007): config, cmd, cmd/ack, state                     */
/* ------------------------------------------------------------------ */
#define CONTRACT_K_ID                    "id"
#define CONTRACT_K_PARAMS                "params"
#define CONTRACT_K_CHANNEL               "channel"
#define CONTRACT_K_PULSE_MS              "pulse_ms"
#define CONTRACT_K_ISSUED_AT             "issued_at"
#define CONTRACT_K_EXPIRES_AT            "expires_at"
#define CONTRACT_K_TTL_MS                "ttl_ms"

#define CONTRACT_K_VERSION               "version"
#define CONTRACT_K_ESTOP                 "estop"
#define CONTRACT_K_FAILSAFE_S            "failsafe_s"
#define CONTRACT_K_CHANNELS              "channels"
#define CONTRACT_K_KIND                  "kind"

#define CONTRACT_K_CONFIG_VERSION        "config_version"
#define CONTRACT_K_RELAYS                "relays"
#define CONTRACT_K_INPUTS                "inputs"

#define CONTRACT_CMD_RELAY_PULSE         "relay.pulse"
#define CONTRACT_CMD_ESTOP               "estop"

#define CONTRACT_CMD_ST_RECEIVED         "received"
#define CONTRACT_CMD_ST_COMPLETED        "completed"
#define CONTRACT_CMD_ST_FAILED           "failed"
#define CONTRACT_CMD_ST_REJECTED         "rejected"

/* Motivos citados em docs/MQTT.md §Comandos */
#define CONTRACT_CMD_REASON_EXPIRED          "expired"
#define CONTRACT_CMD_REASON_UNKNOWN_CHANNEL  "unknown_channel"
#define CONTRACT_CMD_REASON_NO_CONFIG        "no_config"
#define CONTRACT_CMD_REASON_ESTOP            "estop"
/* Motivos do firmware (o contrato pede `reason` curto em minúsculas,
 * padrão ^[a-z0-9_.]+$, sem lista fechada) */
#define CONTRACT_CMD_REASON_INVALID          "invalid"
#define CONTRACT_CMD_REASON_BUSY             "busy"
#define CONTRACT_CMD_REASON_ABORTED          "aborted"
#define CONTRACT_CMD_REASON_FAILSAFE         "failsafe"
#define CONTRACT_CMD_REASON_HARDWARE         "hardware"
/* Placa gravada com o acionamento desligado (PIVO_REMOTE_CONTROL=n). Não é o
 * `control_disabled` do servidor, que é o controle do pivô desligado no painel. */
#define CONTRACT_CMD_REASON_CONTROL_DISABLED "device_control_disabled"

/* `kind` dos canais (config.schema.json) */
#define CONTRACT_KIND_START              "start"
#define CONTRACT_KIND_STOP               "stop"
#define CONTRACT_KIND_FORWARD            "forward"
#define CONTRACT_KIND_REVERSE            "reverse"
#define CONTRACT_KIND_WATER_ON           "water_on"
#define CONTRACT_KIND_WATER_OFF          "water_off"
#define CONTRACT_KIND_CUSTOM             "custom"

/* ------------------------------------------------------------------ */
/* Limites e padrões                                                   */
/* ------------------------------------------------------------------ */
#define CONTRACT_CHANNEL_MAX             (16U)   /* canais 0..15 */
#define CONTRACT_INPUT_MAX               (16U)
#define CONTRACT_PULSE_MIN_MS            (100U)
#define CONTRACT_PULSE_MAX_MS            (5000U) /* também o teto do firmware */
#define CONTRACT_FAILSAFE_MIN_S          (10U)
#define CONTRACT_FAILSAFE_MAX_S          (600U)
#define CONTRACT_TTL_MAX_MS              (60000U)
#define CONTRACT_BOOT_ID_MAX_LEN         (64U)
#define CONTRACT_COMMAND_ID_MAX_LEN      (64U)
#define CONTRACT_REASON_MAX_LEN          (32U)
#define CONTRACT_MSG_MAX_BYTES           (65536U)
#define CONTRACT_READINGS_MAX            (200U)
#define CONTRACT_COMMAND_CACHE_SIZE      (32U)

#define CONTRACT_DEFAULT_STATUS_INTERVAL_S    (60U)
#define CONTRACT_DEFAULT_FAILSAFE_S           (60U)
#define CONTRACT_STATE_INTERVAL_S             (10U)

/* Telemetria confiável (ADR-002) */
#define CONTRACT_TELEMETRY_WINDOW        (8U)
#define CONTRACT_TELEMETRY_ACK_TIMEOUT_MS (30000U)
#define CONTRACT_BACKOFF_MAX_MS          (300000U) /* 5 min: reconexão e unclaimed */

/* Filtro de posição */
#define CONTRACT_GPS_FILTER_SAMPLES      (5U)
#define CONTRACT_GPS_MAX_HDOP            (5.0f)
#define CONTRACT_GPS_MAX_AGE_MS          (10000U)
#define CONTRACT_GPS_NO_DATA_MS          (30000U)

/* Decisões do firmware onde o contrato não fixa o comportamento:
 *
 * - `cmd/ack completed` de `relay.pulse` sai quando o pulso TERMINA (relé já
 *   aberto). O comando vale 15 s e o pulso dura no máximo 5 s.
 * - E-stop: trava com o comando `estop` ou com `config.estop: true` e só
 *   destrava com `config.estop: false` de `version` MAIOR que a que estava em
 *   vigor quando travou (a config é retida e volta igual a cada reconexão). O
 *   servidor sobe a versão ao ligar e ao desligar o E-stop (set_estop).
 * - Comando vencido não executa, nem `estop` (docs/MQTT.md §Comandos, regra 2).
 * - Canal na config mas sem relé instalado nesta placa: `unknown_channel`.
 * - `clock_skew`: o lote é mantido e reenviado, como manda o contrato; um lote
 *   com `ts` realmente errado continua sendo recusado e ocupa a janela.
 */

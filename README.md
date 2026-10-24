# pivo-fw — placa de campo ESP32-S3 (relés + GPS)

Firmware da placa de campo ESP32-S3 N16R8 da plataforma pivo-saas (`smartagrobr.com`). Esta montagem tem **3 relés e 1 GPS**; mais sensores e a expansão para até 8 relés vêm depois. A placa não tem câmera.

O contrato normativo é o do repositório `pivo-saas`: `docs/MQTT.md` e os JSON Schemas de `docs/contracts/v2/`. Onde este README e eles divergirem, valem eles. As constantes do contrato no firmware ficam em [components/contract/include/contract.h](components/contract/include/contract.h).

## Andamento

O trabalho é feito em etapas pequenas; cada uma compila e tem teste.

| # | Etapa | Estado |
|---|---|---|
| 0 | Esqueleto: partições, pinos, versões fixadas, testes de host | feito, testado na placa |
| 1 | Linha de provisionamento (`agro1://`) | feito, testes de host |
| 2 | Enroll (chave P-256 na placa, CSR, certificado no NVS) | feito; **falta testar contra o servidor** |
| 3 | MQTT mTLS: status, LWT, eventos, telemetria com journal | código pronto; depende do enroll |
| 4 | GPS (NEO-M8N) como fonte de `gps` e de hora | código pronto; falta validar com o módulo |
| 5 | Controle: `config`, `cmd`, `cmd/ack`, `state`, fail-safe (contrato v2) | feito, **desligado nesta etapa**; falta testar contra o servidor |
| 6 | Provisionamento BLE (segurança 2), estados, troca de Wi-Fi, reset de fábrica | feito; **falta testar na placa** |
| 7 | Status `ota`, autoteste, revisão de robustez | a fazer |

## Ligar a placa à plataforma

Contrato BLE, estados e o que falta no app: [docs/ble-provisionamento.md](docs/ble-provisionamento.md).

1. **Dados de fábrica:** copie [main/provisioning.h.example](main/provisioning.h.example) para `main/provisioning.h` (fora do git) e preencha:
   - `AGRO_BLE_POP`: prova de posse do BLE, que vai no QR da etiqueta;
   - `AGRO_PROVISIONING`: a linha gerada na tela **Provisionamento** de `smartagrobr.com`.
2. **Gravar o firmware.** Sem Wi-Fi configurado, a placa abre o modo de configuração BLE como `AGRO_XXXXXX` (estado `aguardando_configuracao`).
3. **Configurar o Wi-Fi pelo celular**, com a prova de posse. Enquanto o app não implementa o APP-12, a ferramenta da Espressif faz esse papel:

   ```bash
   python esp_prov.py --transport ble --service_name AGRO_XXXXXX --sec_ver 2 \
       --sec2_username wifiprov --sec2_pwd <prova de posse> --ssid <rede> --passphrase <senha>
   ```

   O script fica em `managed_components/espressif__network_provisioning/tool/esp_prov/` e precisa de `bleak`, `protobuf` e `cryptography`.
4. **A placa segue sozinha:** `conectando_wifi` → `provisionando_identidade` (enroll) → `conectando_mqtt` → `online`, e começa a mandar telemetria.
5. **Claim no app:** com o CN e o código da etiqueta. Antes disso, cada lote volta `unclaimed` e fica guardado na placa.

Depois de reiniciar, a placa reconecta sozinha com o Wi-Fi e o certificado gravados. A espera entre tentativas cresce de 2 s até 60 s no Wi-Fi e até 5 min no MQTT.

**Botão BOOT, com a placa ligada:**
- segurar 5 a 15 s: **troca de Wi-Fi**; preserva a identidade;
- segurar 15 s ou mais: **reset de fábrica**; apaga a identidade, e a placa vai precisar de uma linha nova.

### Controle remoto dos relés

O **acionamento** fica **desligado nesta etapa** (`PIVO_REMOTE_CONTROL = n`): os relés ficam sempre abertos. A placa com relés continua no protocolo de controle mesmo assim. Ela assina `cmd` e `config` a cada conexão e confere os SUBACKs; valida e aplica a config, confirmando a versão em `state` (relés abertos); e responde a todo comando válido com `rejected` / `device_control_disabled`, em vez de deixá-lo expirar. Comando vencido ou inválido continua com o motivo próprio (`expired`, `invalid`), e um id repetido recebe a mesma resposta. Para ligar o acionamento: `idf.py menuconfig` → pivo-fw → *Controle remoto dos relés*. As regras abaixo valem quando ele está ligado.

### Regras de comando que o firmware aplica

- **Duplicata:** cada `id` é executado no máximo uma vez. Um reenvio só repete o último ACK.
- **Vencimento:** comando vencido (`expires_at` com relógio; `ttl_ms` sem relógio) volta `rejected` / `expired`.
- **Recusas de `relay.pulse`:**
  - `no_config`: ainda não chegou config;
  - `estop`: E-stop ativo;
  - `unknown_channel`: canal fora da config ou sem relé nesta placa;
  - `busy`: pulso em andamento no mesmo canal.
- **Duração do pulso:** nunca passa do `pulse_ms` do canal nem de 5 s. O `completed` sai quando o pulso termina.
- **`estop`:** abre todos os relés na hora. Só destrava com uma config `estop: false` de versão maior que a do momento em que travou, e vale também depois de reiniciar.
- **Fail-safe:** sem broker por `failsafe_s` (60 s até chegar a config), todos os relés abrem.
- **`state`:** publicado ao mudar e a cada 10 s, com `relays: [canal 0, canal 1, canal 2]`.

## ⚠️ Segurança elétrica (ADR-007)

- Os relés só fecham **contato seco normalmente aberto** (bornes COM e NA do módulo) nas entradas de comando do painel original do pivô. Nunca acionam contator nem motor.
- O **E-stop físico** e o **intertravamento elétrico** continuam no painel e não dependem deste firmware.
- Ao ligar, reiniciar, perder o broker por `failsafe_s` ou travar, **todos os relés abrem**. Nenhum comando antigo é reaplicado.

## Hardware

| Item | Ligação |
|---|---|
| Placa | ESP32-S3-DevKitC-1 N16R8 (16 MB de flash, 8 MB de PSRAM) |
| Relé 1 (HW-482) | S → GPIO4, + → 5V, − → GND |
| Relé 2 (HW-482) | S → GPIO5, + → 5V, − → GND |
| Relé 3 (HW-482) | S → GPIO6, + → 5V, − → GND |
| GPS NEO-M8N | TX do GPS → GPIO21, RX do GPS ← GPIO47, VCC → 3V3, GND → GND (UART2, 9600 baud) |

O mapa fica em [main/board_config.h](main/board_config.h). O HW-482 é **ativo em HIGH**: S em nível alto fecha o relé e acende o LED vermelho. A expansão para 8 relés usa GPIO 4, 5, 6, 7, 15, 16, 17 e 18.

Pinos evitados: 0, 3, 45, 46 (strapping), 19 e 20 (USB), 43 e 44 (console) e 26 a 37 (flash e PSRAM do N16R8).

Antes de ligar ao painel, confira na bancada, sem carga:

- **Repouso:** com a placa ligada e sem comando, os três relés ficam soltos e o LED apagado.
- **Boot e reset:** durante o boot os GPIOs ficam em alta impedância por um instante. Se o relé der um clique ao energizar, ponha um resistor de 10 kΩ do S ao GND em cada módulo.
- **Alimentação do relé:** o JQC3F-5VDC precisa dos 5 V no `+`, não dos 3,3 V.

## Compilar e gravar

Pela extensão ESP-IDF da Espressif no VS Code (ESP-IDF **v6.1**): *Build*, *Flash* e *Monitor*. Os componentes gerenciados estão fixados em [dependencies.lock](dependencies.lock).

A placa grava pela porta **UART** da DevKitC (conector marcado UART/COM, conversor CH343). Na extensão, o modo de gravação é UART (`idf.flashType`); o modo JTAG só funciona com o cabo na porta USB nativa.

No terminal:

```bash
. ~/.espressif/tools/activate_idf_v6.1.sh
idf.py build
idf.py -p PORTA flash monitor
```

O Wi-Fi não fica no firmware: é configurado pelo celular via BLE e gravado no NVS do driver Wi-Fi. O intervalo de leitura e envio é de 3 s (`PIVO_TELEMETRY_INTERVAL_S`).

A tabela de partições mudou na etapa 0: na primeira gravação, grave a imagem completa (*Flash* grava tudo).

## Partições (16 MB)

| Partição | Tamanho | Uso |
|---|---|---|
| `nvs` | 24 KB | identidade do enroll (chave, certificado, CA raiz, host/porta MQTT), E-stop travado |
| `nvs_keys` | 4 KB | reservada para a criptografia do NVS no build de release |
| `otadata`, `phy_init` | 8 KB, 4 KB | |
| `ota_0`, `ota_1` | 4 MB cada | slots A/B com rollback |
| `journal` | ~7,8 MB | journal de telemetria em LittleFS (6 MB úteis) |
| `coredump` | 64 KB | diagnóstico de pânico |

Com um lote a cada 3 s, os 6 MB do journal guardam por volta de 12 a 15 horas sem conexão; acima disso, os lotes mais antigos são descartados e contados em `journal_dropped`.

O NVS **não é criptografado** nas placas de desenvolvimento. Criptografar exige gravar uma chave em eFuse, o que é irreversível; isso fica para o build de release, depois de testado numa placa descartável. Por isso o firmware nunca apaga o NVS sozinho: apagá-lo perderia a chave do enroll e exigiria uma linha de provisionamento nova.

## Testes de host

Rodam no PC, sem placa e sem ESP-IDF (precisam de CMake, Ninja e um compilador C):

```bash
tools/run_host_tests.sh
```

Cobrem o parser NMEA, a serialização dos payloads, o journal (ACK fora de ordem, CRC corrompido, overflow, reboot no meio da escrita), o envio com ACK de aplicação, a validação de config e de comandos.

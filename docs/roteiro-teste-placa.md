# Roteiro de teste em placa real: BLE → Wi-Fi → identidade → MQTT → telemetria

**Placa:** ESP32-S3 rev 0.2, 16 MB de flash, 8 MB de PSRAM, 3 relés HW-482 (GPIO 4, 5, 6) e GPS NEO-M8N.
**Firmware:** este repositório, com `PIVO_REMOTE_CONTROL = n` (relés sempre abertos).

A coluna **Resultado** diz como cada item foi verificado:

- **placa:** executado na placa real, com o log como evidência;
- **host:** coberto só pelos testes de host (lógica, sem rádio nem servidor);
- **pendente:** ainda não executado, com o motivo.

## Registro das execuções

### Compilação (23/10/2026)

- ESP-IDF v6.1, `network_provisioning` 1.4.0, NimBLE; imagem de 1,3 MB em slot de 4 MB.
- Sem avisos no código do projeto.
- Configuração conferida no `sdkconfig`:
  - `PIVO_REMOTE_CONTROL` desligado;
  - só a segurança 2 do protocomm ligada;
  - log com teto em INFO.

### Testes de host (23/10/2026)

49 testes, 0 falhas (`tools/run_host_tests.sh`).

### Placa (23/10/2026)

| Verificação | Evidência |
|---|---|
| Porta | `/dev/cu.usbmodem5B8F0127961`, conversor CH343 (VID 0x1A86) |
| Chip | ESP32-S3 (QFN56) rev 0.2, flash de 16 MB, PSRAM de 8 MB, MAC `e0:72:a1:f3:f3:00` |
| Tabela de partições lida da flash | idêntica a `partitions.csv` |
| Partição `nvs` antes do teste (só nomes, sem valores) | `nvs.net80211` (Wi-Fi do driver), `phy` (calibração), `misc` e **`csi_cfg`**, gravado por outro firmware (`ssid`, `password`, `node_id`, `target_ip`, `target_port`, `edge_tier`). **Nenhuma identidade deste firmware.** Como o `csi_cfg` não é deste projeto, a NVS **não foi apagada**: o modo BLE é aberto pelo botão (troca de Wi-Fi), que apaga só `nvs.net80211`. |
| Gravação | bootloader, tabela, `otadata` e aplicação; `nvs` intocada; hash verificado |
| Gravação (correções da 2ª rodada) | mesmas regiões; testes de host 49/49 depois das correções |

### Limpeza do journal antes do claim (23/10/2026)

A pedido, só a partição `journal` foi apagada (`parttool.py erase_partition --partition-name journal`, offset 0x820000, 0x7D0000 bytes), para os ~3.350 lotes dos dias de teste não entrarem como histórico da placa. No boot seguinte:
- `journal aberto: 0 pendentes, 0 perdidos` (LittleFS reformatado pelo firmware);
- Wi-Fi conectado sem BLE;
- identidade `agp-9khzrzpxhv93` lida do NVS, com 0 chamadas ao enroll e 0 chaves geradas;
- `online` em 10 s.

NVS, aplicação e bootloader não foram tocados. Não houve reset de fábrica.

### Defeito encontrado depois do claim e corrigido (23/10/2026)

**Sintoma na placa:** depois do primeiro `stored`, os pendentes subiam 20 por minuto (o ritmo de geração) e nada mais saía do journal. Os contadores mostraram:
- os mesmos 8 lotes reenviados a cada 30 s;
- o servidor respondendo `duplicate` a todos;
- o `journal_ack` recusando com `FW_ERR_FULL`;
- a lista de ACKs fora de ordem cheia (32);
- o cursor parado no lote `f4e8363b2058b2e8/1`.

**Causa:**
1. Num boot logo depois do claim, a fila de entrada da tarefa do journal (8 posições) estourou numa rajada de ACKs; o log mostra 1 `mensagem recebida descartada`.
2. O ACK do lote mais antigo se perdeu.
3. A janela de envio seguiu adiante e encheu a lista com ACKs fora de ordem.
4. Com a lista cheia, até o ACK do lote da frente, o único que destravaria o cursor, passou a ser recusado.

**Correção:**
- o ACK do lote da frente do cursor é consumido direto, sem ocupar a lista (`components/journal/journal.c`), com teste de host que reproduz o travamento (`test_head_ack_with_full_out_of_order_list`);
- a fila de entrada passou de 8 para 32 posições;
- os contadores de envio e ACK ficaram no log a cada minuto (`tx: ...`).

Testes de host: 50/50. Na placa, o journal esvaziou e ficou em 0.

### Config e comando com o controle remoto desligado (23/10/2026)

**Sintomas no painel:**
- config desejada na versão 6, sem nenhuma versão aplicada;
- nenhum estado da placa;
- um comando de parada que expirou sem resposta.

**Evidência na placa** (log de assinaturas acrescentado ao `mqtt_link`):

```text
mqtt_link: assinado: agro/v2/agp-9khzrzpxhv93/telemetry/ack
mqtt_link: NÃO assinado (controle remoto desligado): agro/v2/agp-9khzrzpxhv93/cmd
mqtt_link: NÃO assinado (controle remoto desligado): agro/v2/agp-9khzrzpxhv93/config
```

Com `PIVO_REMOTE_CONTROL = n`, a placa não assina `cmd` nem `config`, e as tarefas de relés e de comandos não sobem, então ela também não publica `state` nem `cmd/ack`.

**Causa:**
- **Config:** a versão 6 está retida no broker e só vai ser entregue quando a placa assinar `config`; por isso nenhuma versão aparece como aplicada.
- **Comando:** foi uma mensagem **não recebida**, e não um comando recusado. Sem assinatura, o broker não entrega o `cmd` (QoS 1, não retido, sessão limpa). O servidor republica uma vez em 3 s e marca `EXPIRED` quando vencem os 15 s.
- **Por que a placa não recusou:** uma recusa por "controle desativado" exigiria a placa receber o comando e responder `rejected`; neste modo ela não recebe nada.
- **Não volta depois:** o comando vencido não é reaplicado, porque o `cmd` nunca é retido e a placa usa sessão limpa.

**Tipo do comando:** pelo código do `pivo-saas` (`api/control.py`), o botão comum de parar é recusado com `config_pending` enquanto a placa não confirma a config. Só o E-stop cria comando sem essa checagem. Logo, o comando que expirou deve ter sido o **E-stop**. Isso é inferência pelo código; confirme pelo tipo no histórico de comandos do painel.

### Correção: protocolo de controle com o acionamento desligado (23/10/2026)

A VPS confirmou a mesma coisa do lado do broker: a sessão só assinava `telemetry/ack`, sem negação de ACL (a ACL do `pivo-saas` libera `cmd` e `config` para qualquer placa). Quem desligava a assinatura era o próprio firmware.

**O que mudou:**
- `PIVO_REMOTE_CONTROL` passou a controlar só o acionamento. Em toda placa com relés, a `cmd_task` e a `relay_task` sobem sempre.
- Assinatura de `telemetry/ack`, `cmd` e `config` a cada conexão. Cada SUBACK é conferido (0x80 = recusa). Um SUBACK que não chega em 10 s é reenviado, e uma recusa é tentada de novo depois de 60 s.
- Config validada e aplicada (canais, E-stop, failsafe), com a versão confirmada em `state.config_version` e todos os relés abertos.
- Todo comando válido recebe `rejected` / `device_control_disabled`, inclusive o `estop`: a placa não finge execução. A ordem é:
  1. id repetido: repete a resposta;
  2. `invalid`;
  3. `expired`;
  4. `device_control_disabled`.
- Proteção dupla: mesmo se um pulso chegasse à `relay_task`, ela recusa sem tocar no GPIO. Os pinos só recebem o nível inativo, no boot, no fail-safe ou no E-stop da config.
- Teste de host novo: `test_control_disabled_rejects_without_acting`. Total de 53/53.

**Na placa** (22:05, preservando identidade e Wi-Fi):

```text
app_main: pivo-fw 0.1.0, reset: power_on, relés: 3 (sempre abertos, comandos recusados)
mqtt_link: SUBACK ok (QoS 1): agro/v2/agp-9khzrzpxhv93/telemetry/ack
mqtt_link: SUBACK ok (QoS 1): agro/v2/agp-9khzrzpxhv93/cmd
mqtt_link: SUBACK ok (QoS 1): agro/v2/agp-9khzrzpxhv93/config
relay_task: state passa a confirmar config v6 (estop=0, relés todos abertos)
cmd_task: config v6 aplicada (2 canais, estop=0, failsafe=60 s)
```

- A config v6 tem `estop: false`, o que enfraquece a hipótese do E-stop. Ela também é compatível com um E-stop já liberado, porque a liberação também sobe a versão. O tipo do comando continua para confirmar no histórico.
- A v6 chegou duas vezes em 300 ms: a retida, no SUBACK, e uma republicação do servidor. A segunda foi aplicada sem efeito novo.
- Heap livre em 111 KB (mínimo 99,9 KB), contra 141 KB antes. A diferença são as duas tarefas a mais.

**Ainda falta na placa:**
- um comando de verdade vindo do painel, esperando `rejected` / `device_control_disabled`;
- a reassinatura depois de uma queda de rede (caso 10).

### Reinícios observados durante os testes (23/10/2026)

Foram 28 boots nas capturas de 23/10, com os horários cruzados com cada comando que mexeu na porta serial:
- 27 com `rst:0x1 (POWERON)` e 1 com `rst:0xc (RTC_SW_CPU_RST)`. Esse último foi a troca de Wi-Fi pelo botão BOOT no teste de BLE (log `botao: troca de Wi-Fi ... Reiniciando`).
- Não há `BROWNOUT`, pânico, `task_wdt`, `int_wdt` ou `abort`.
- No ESP32-S3, o pulso no EN feito pelo adaptador USB-serial também aparece como `POWERON`. O motivo sozinho não separa uma queda de alimentação de um reset pelo monitor.
- 26 dos 27 `POWERON` coincidem com uma ação na porta:
  - gravação (reset no fim);
  - `readlog.py` (pulso em EN de propósito);
  - `reset.py` (os segundos boots de `serial_t2`, `serial_t3` e `claim3`, este às 19:24:49, no teste de reinício depois do claim);
  - abertura da porta pelo `waitlog` (no macOS, abrir o CH343 reinicia a placa);
  - o monitor do VS Code aberto junto com a captura (~14:43). O banner da ROM saiu sem as primeiras letras (`SP-ROM`, `uild`), sinal de dois leitores na porta, e o erro "device reports readiness to read but returned no data" foi colado às 14:44.
- 1 boot sem causa registrada, às ~14:54:50 (`serial_tests.log`), durante os testes de BLE com o celular. Desta vez o banner saiu inteiro. É compatível com outro programa abrindo a porta ou com mau contato no cabo, mas não dá para provar só com este log.

**Conclusão:** não há evidência de defeito na alimentação. Para isolar daqui em diante:
1. deixar só um programa com a porta aberta;
2. anotar a hora de cada abertura;
3. comparar com os eventos `boot` (`reset_reason`) que o servidor guarda, porque eles chegam pelo MQTT, sem depender da serial.

Um `brownout` real gera `reset_reason: brownout` e o evento `brownout`.

### Teste físico dos relés em bancada: preparação (23/10/2026)

Relés desconectados de bombas, motores e do painel real.

**Mapeamento** (`main/board_config.h`): o `channel` do contrato é o índice do relé, a partir de 0.

| Canal | GPIO | Relé | Na config v12 |
|---|---|---|---|
| 0 | 4 | 1º HW-482 | sim (`start`, pulso máx 500 ms) |
| 1 | 5 | 2º HW-482 | não, é recusado com `unknown_channel` |
| 2 | 6 | 3º HW-482 | sim (`forward`, pulso máx 500 ms) |
| 3 a 15 | — | não existe | recusado com `unknown_channel`, mesmo se cadastrado |

**Polaridade e boot:**
- ativo em HIGH: GPIO em 1 fecha o relé e acende o LED do módulo;
- no boot, o registrador recebe 0 antes de o pino virar saída;
- nenhum estado de relé é restaurado depois de reiniciar.

**Versão de teste:**
- `CONFIG_PIVO_REMOTE_CONTROL=y` só no `sdkconfig` local, que é ignorado pelo Git; o `sdkconfig.defaults` continua com o controle desligado;
- gravada com `idf.py flash`, que escreve bootloader, tabela, otadata e `ota_0`, sem tocar em `nvs` nem `journal`.

**Boot com a versão de teste:**
- `controle remoto LIGADO`;
- identidade pronta, sem novo enroll;
- journal com 0 pendentes;
- SUBACKs ok em `telemetry/ack`, `cmd` e `config`;
- config v12 aplicada (canais 0 e 2, `estop=0`, failsafe 60 s);
- sem E-stop travado no NVS, porque não apareceu `E-stop travado de um boot anterior`.

**Para voltar ao controle desligado:** trocar `CONFIG_PIVO_REMOTE_CONTROL=y` por `# CONFIG_PIVO_REMOTE_CONTROL is not set` no `sdkconfig`, compilar e gravar.

### Caminho do comando até o relé: revisão (24/10/2026)

- O envelope real do backend foi gerado com o `CommandEnvelope` do pivo-saas e passou pelo parser e pela decisão do firmware: pulso no canal 2 de 500 ms e E-stop sem `params`. É o teste `test_backend_envelope_end_to_end`.
- **Correção no firmware:**
  - **antes:** o parser recusava horários com deslocamento (`-03:00`), embora o schema (`format: date-time`) os permita. Isso só aconteceria se a sessão do banco ou da API saísse de UTC, mas aí **todo** comando seria recusado como `invalid`;
  - **agora:** aceita `Z` ou `±HH:MM` e converte para UTC;
  - continua recusando horário sem fuso;
  - total de 54/54 testes de host.
- **Backend:** nenhuma trava encontrada. Comandos presos em `RECEIVED` expiram em 15 s (`control_due`), então não bloqueiam o pivô.

## Preparação

1. `main/provisioning.h` com `AGRO_BLE_POP` e `AGRO_PROVISIONING` (linha gerada em `smartagrobr.com`).
2. Gravar o firmware (não toca na `nvs`). Para começar sem Wi-Fi, segurar BOOT de 5 a 15 s com a placa ligada: apaga só a rede gravada.
3. Monitor serial aberto. Cada mudança de estado aparece como `estado: <de> -> <para>`.
4. Do lado do celular: o app (quando o APP-12 existir) ou `esp_prov.py` (ver README).

## Casos

| # | Caso | Passos | Esperado | Resultado |
|---|---|---|---|---|
| 1 | Modo de configuração | ligar sem Wi-Fi gravado | `aguardando_configuracao`; BLE anuncia `AGRO_XXXXXX` | **placa**: aprovado. Após o botão BOOT ~7 s, log `modo de configuração BLE aberto como AGRO_F3F300`; a varredura do Mac vê `AGRO_F3F300` (RSSI −44) com o UUID do serviço |
| 2 | Prova de posse errada | conectar com uma prova de posse errada | `sessão BLE recusada: prova de posse incorreta`; nenhuma credencial aceita | **placa**: aprovado. `security2: Failed to authenticate client proof!` → conexão derrubada pela placa |
| 3 | Senha do Wi-Fi errada | enviar a rede certa com senha errada | `erro_senha_wifi`; o celular recebe a falha; o BLE continua aberto | **placa**: aprovado na rede Casa1. 3 tentativas (motivo 15), o celular recebe `Incorrect Password`, `agro-status` = `erro_senha_wifi` / `auth_error`, BLE aberto |
| 4 | Corrigir a senha / conexão correta | reenviar com a senha certa | `conectando_wifi` → IP; BLE fecha; nada de senha no log | **placa**: aprovado. Nova sessão → `rede confirmada`, IP, `credenciais gravadas`, BLE encerrado, NTP em ~2,5 s; estado `sem_dados_de_fabrica` (correto: ainda sem linha) |
| 5 | Rede inexistente | enviar um SSID que não existe | `rede_nao_encontrada` | **placa**: aprovado. 3 tentativas (motivo 201); o celular recebe `Incorrect SSID`; `agro-status` = `rede_nao_encontrada` / `ap_not_found`; nova sessão aceita depois da falha |
| 5a | Lista de redes completa | sessão segura + `prov-scan` | todas as redes ao alcance, para o usuário escolher | **placa**: aprovado depois da correção. Antes vinham só 16 (limite padrão do componente); agora 40 |
| 5b | Queda de energia com credencial não testada | enviar uma rede e reiniciar a placa no meio das tentativas | volta ao modo BLE, sem ficar presa | **placa**: aprovado depois da correção. Log: `rede gravada nunca foi confirmada; reabrindo a configuração BLE` → `aguardando_configuracao` |
| 5c | Estados no boot sem Wi-Fi | ligar sem rede | `iniciando -> aguardando_configuracao` direto | **placa**: aprovado depois da correção (antes aparecia um `conectando_wifi` de 50 ms) |
| 6 | Enroll | com a linha válida, depois do Wi-Fi | `provisionando_identidade` → `certificado gravado; broker mqtt.smartagrobr.com:8883` | **placa**: aprovado em 23/10, CN `agp-9khzrzpxhv93`. Chave EC P-256 gerada na placa (6,8 s); enroll em `api.smartagrobr.com` (7,0 s); certificado gravado e broker `mqtt.smartagrobr.com:8883` (13,1 s). Token, prova de posse, chave privada e CSR: 0 ocorrências no log |
| 7 | MQTT e primeira telemetria | depois do enroll | `conectando_mqtt` → `online`; o servidor responde ao lote (`unclaimed` antes do claim, `stored` depois) | **placa**: aprovado. Antes do claim: mTLS conectado → `online`, `unclaimed`, lotes guardados. Depois do claim: primeiro `telemetria aceita pelo servidor` aos 473 s de uso (fim do intervalo de espera) |
| 8 | Claim | `POST /v1/devices/claim` com o CN e o código | lotes passam a voltar `stored`; os pendentes saem do journal | **placa**: aprovado depois da correção do journal (ver abaixo): 450 pendentes → 0 em ~2 min; em regime, cada lote recebe `stored` (contadores: 504 enviados, 496 `stored`, 12 `duplicate`, 0 reenvios, 0 erros). **Exibição no painel: falta a conferência do usuário** |
| 9 | Reinício | apertar EN/RESET | volta a `online` sozinha, sem BLE e sem novo enroll | **placa**: aprovado antes e depois do claim. Já vinculada: Wi-Fi e identidade preservados, 0 chamadas ao enroll, 0 chaves geradas, 0 `unclaimed`, `online` em ~11 s e telemetria aceita logo em seguida |
| 9a | GPS: comunicação UART | placa ligada, GPS conectado | sentenças NMEA válidas a cada ~1 s | **placa**: 1ª verificação = **falha de comunicação** (0 bytes em 4 janelas). Depois de revisada a ligação do módulo: **aprovado**, ~240 sentenças válidas por 30 s (GGA, RMC e GSV), 0 a 3 inválidas por janela |
| 9b | GPS: fix e posição na plataforma | com comunicação OK, antena com vista do céu | `GPS: FIX OK` → `telemetria passou a incluir o objeto gps` → posição no painel | **placa**: aprovado até o backend. Fix aos ~64 a 88 s do boot; em 3 min, de 3 para 11 satélites em uso (15 em vista) e HDOP de 3,4 para 0,9. Lotes `seq 18` a `37` com `gps` aceitos (`stored` de 17 para 35, 0 erros). Durante o aquecimento a posição sumiu de 1 lote (`seq 22`) e voltou. **Exibição no painel: falta a conferência do usuário** |
| 10 | Queda de rede | desligar o roteador por 2 min e religar | `conectando_wifi`, com esperas de 2, 4, 8… até 60 s; volta a `online`; os lotes do intervalo chegam depois | pendente |
| 11 | Internet fora, Wi-Fi no ar | tirar o cabo WAN do roteador | `sem_internet` (sem hora em 20 s) ou `conectando_mqtt` | pendente |
| 12 | Troca de Wi-Fi | segurar BOOT 5 a 15 s e soltar; configurar outra rede | `aguardando_configuracao`; depois `online` **sem novo enroll** (mesmo CN) | parcial na **placa**: o botão apagou só o Wi-Fi e abriu o BLE; `csi_cfg` e `phy` preservados. Falta a parte de identidade (depende do enroll) |
| 13 | Reset de fábrica | segurar BOOT 15 s ou mais | `aguardando_configuracao`; journal apagado; o enroll com a linha antiga é recusado (`identidade_recusada`, 404) | pendente |
| 14 | Relés seguros | durante todos os casos | os 3 LEDs dos relés apagados o tempo todo, inclusive no boot | pendente |
| 15 | Segredos fora do log | revisar o log completo de 1 a 13 | nenhuma senha, token, prova de posse ou chave privada | **placa** (todos os logs de 23/10): 0 ocorrências do token, da prova de posse, de `PRIVATE KEY` e das senhas de Wi-Fi |

## O que só os testes de host cobrem (sem placa)

- Parser da linha `agro1://` (CN, token, host, porta, recusas).
- Montagem do envelope de telemetria, do status e do evento.
- Journal: ACK fora de ordem, CRC corrompido, overflow, queda no meio da escrita.
- Envio com ACK de aplicação: janela de 8, reenvio em 30 s e cada motivo de `rejected`.
- Config e comandos do controle (que fica desligado nesta etapa).

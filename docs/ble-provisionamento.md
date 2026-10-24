# Provisionamento por BLE: contrato entre a placa e o app

Este documento é para quem implementa a tela **T28 Adicionar dispositivo** no `pivo-app` (APP-12) e para quem prepara as placas na fábrica. O lado da placa está em [main/wifi_sta.c](../main/wifi_sta.c).

O backend não tem nenhum endpoint de BLE: o BLE só existe entre o celular e a placa. A identidade da placa vem do enroll (`POST /v1/devices/enroll`, feito pela própria placa) e o vínculo a uma fazenda vem do claim (`POST /v1/devices/claim`, feito pelo app). As duas coisas estão em `pivo-saas/docs/MQTT.md`.

## Fluxo completo

| # | Quem | O quê |
|---|---|---|
| 1 | Placa | Liga sem Wi-Fi configurado e abre o modo de configuração BLE com o nome `AGRO_XXXXXX` |
| 2 | App | Lê o QR da etiqueta (nome BLE + prova de posse) |
| 3 | App | Conecta por BLE e abre a sessão segura com a prova de posse (segurança 2) |
| 4 | App | Lê `agro-status` para obter o `cn` da placa |
| 5 | App | Pede a lista de redes (`prov-scan`), o usuário escolhe uma e digita a senha |
| 6 | App | Envia SSID e senha (`prov-config`) e acompanha o resultado |
| 7 | Placa | Conecta ao Wi-Fi e informa sucesso, senha errada ou rede não encontrada |
| 8 | Placa | Fecha o BLE; acerta a hora (NTP), faz o enroll e conecta ao MQTT com mTLS |
| 9 | App | Faz o **claim**: `POST /v1/devices/claim` com o `cn` e o **código de claim da etiqueta** (admin da fazenda, com 2FA) |
| 10 | App | Espera a placa aparecer online e a primeira telemetria ser gravada (`GET /v1/devices/{id}`) |

Configurar o Wi-Fi **não** vincula a placa a ninguém. Sem o código de claim (impresso na etiqueta e conhecido só pelo servidor como hash), ninguém associa a placa a uma fazenda. Placa já vinculada a outra fazenda responde 404 no claim.

## Transporte e segurança

- **Componente:** `espressif/network_provisioning` 1.4.0 (sucessor do `wifi_provisioning`), transporte BLE com NimBLE.
- **Segurança 2:** SRP6a + AES-GCM. As versões 0 (texto aberto) e 1 estão **desligadas no firmware**: não existe modo sem proteção.
- **Usuário SRP:** `wifiprov`.
- **Senha SRP:** a prova de posse (*proof of possession*), única por placa, impressa no QR.
- **Nome BLE:** `AGRO_` seguido dos 3 últimos bytes do MAC Wi-Fi, em hexadecimal (ex.: `AGRO_F3F300`).
- **Serviço GATT de provisionamento:** UUID `766f7270-2d6f-7669-702d-72426f72675a` (como aparece no anúncio, conferido na placa), usado para filtrar a varredura.
- **Quando o BLE fica ligado:** só enquanto a placa não tem Wi-Fi configurado. Depois do sucesso, o BLE é desligado e a memória do Bluetooth é liberada.

## Endpoints (características GATT)

| Endpoint | Origem | Uso |
|---|---|---|
| `proto-ver` | padrão | versão e capacidades |
| `prov-session` | padrão | abertura da sessão segura (SRP6a) |
| `prov-scan` | padrão | lista as redes Wi-Fi vistas pela placa |
| `prov-config` | padrão | envia SSID e senha, lê o resultado |
| `prov-ctrl` | padrão | reinicia a máquina de estados depois de uma falha |
| `agro-status` | **SmartagroBr** | estado atual da placa, em JSON |

Os endpoints padrão seguem o protocolo da Espressif. No Flutter, use uma biblioteca que implemente o provisionamento ESP com **segurança 2**; as bibliotecas oficiais da Espressif para Android (`esp-idf-provisioning-android`) e iOS (`esp-idf-provisioning-ios`) implementam esse protocolo.

### `agro-status`

Qualquer escrita, dentro da sessão segura, recebe como resposta:

```json
{"v":1,"cn":"agp-7k2m9x4qpr3t","fw":"0.1.0","state":"conectando_wifi","wifi_error":"auth_error"}
```

| Campo | Significado |
|---|---|
| `cn` | identidade da placa, para o claim (`null` se a placa saiu da fábrica sem linha de provisionamento) |
| `fw` | versão do firmware |
| `state` | um dos estados abaixo |
| `wifi_error` | só quando a última tentativa falhou: `auth_error` (senha errada) ou `ap_not_found` |

Nenhum segredo passa por este endpoint.

| `state` | O que o app mostra |
|---|---|
| `aguardando_configuracao` | Pronto para receber a rede |
| `conectando_wifi` | Conectando ao Wi-Fi |
| `erro_senha_wifi` | Senha incorreta: digite de novo |
| `rede_nao_encontrada` | Rede não encontrada: confira o nome ou aproxime o roteador |
| `sem_internet` | Conectado ao Wi-Fi, mas sem internet |
| `provisionando_identidade` | Registrando a placa na plataforma |
| `identidade_recusada` | Placa recusada pela plataforma: precisa de uma linha nova (fábrica) |
| `sem_dados_de_fabrica` | Placa sem dados de fábrica |
| `conectando_mqtt` | Conectando à plataforma |
| `online` | Pronto |

Na prática, o app só vê os estados até `conectando_wifi` / `erro_senha_wifi` pelo BLE, porque o BLE fecha quando o Wi-Fi conecta. Os estados seguintes chegam pelo servidor (placa online, primeira telemetria).

### Quais redes da lista podem ser escolhidas

A placa devolve até **40 redes** (as que ela enxerga, mais fortes primeiro), e o usuário pode escolher qualquer uma:

| Tipo de rede | Situação |
|---|---|
| WPA/WPA2 pessoal (com senha) | suportado; testado na placa só o caminho de falha (rede inexistente) |
| WPA2/WPA3 pessoal | suportado pelo ESP32-S3; ainda não testado |
| Aberta (sem senha) | suportado: o app envia senha vazia. Redes com portal de login (ex.: hotspots de operadora) conectam, mas ficam em `sem_internet` |
| Empresarial (WPA2-Enterprise, login e senha de usuário) | **não suportado** pelo protocolo de provisionamento |
| 5 GHz | não aparece: o ESP32-S3 só usa 2,4 GHz |
| Oculta (SSID não anunciado) | não aparece na lista; o app pode oferecer "digitar o nome da rede" e mandar o SSID no `prov-config` |

### Senha errada

1. A placa tenta 3 vezes e responde ao `prov-config` com falha (senha errada ou rede não encontrada) e apaga a credencial recusada.
2. O BLE continua aberto, na mesma sessão.
3. O app pede a senha de novo e reenvia o `prov-config`. Não é preciso reconectar.

### Queda de energia no meio do teste

O componente grava SSID e senha **antes** de testar a conexão. A placa marca a credencial como pendente quando a recebe e como confirmada no primeiro IP. Se reiniciar com uma credencial pendente, ela a descarta e reabre o BLE, em vez de ficar tentando para sempre uma senha que nunca funcionou.

## QR da etiqueta

Mesmo formato dos apps da Espressif, para facilitar o uso das bibliotecas oficiais:

```json
{"ver":"v1","name":"AGRO_F3F300","username":"wifiprov","pop":"<prova de posse>","transport":"ble"}
```

O **código de claim** fica na mesma etiqueta, impresso à parte. **Dependência:** hoje o lote de provisionamento do backend gera só o CN, o código de claim e a linha `agro1://`. A prova de posse e o nome BLE (que depende do MAC) são gerados na fábrica e ainda não têm onde ser registrados.

## Ações físicas (botão BOOT, com a placa ligada)

| Ação | Como | O que apaga | O que preserva |
|---|---|---|---|
| **Troca de Wi-Fi** | segurar 5 a 15 s e soltar | só a rede Wi-Fi | identidade, certificado, journal, vínculo à fazenda |
| **Reset de fábrica** | segurar 15 s ou mais e soltar | Wi-Fi, identidade (chave e certificado), E-stop, journal | — |

O log avisa aos 5 s e aos 15 s. Apertar BOOT enquanto a placa liga põe o chip em modo de gravação; isso não é uma ação deste firmware.

Depois do **reset de fábrica**, a placa precisa de uma **linha de provisionamento nova**: o token antigo já foi usado com a chave apagada, e o servidor recusa outra chave com o mesmo token (404).

## Troca de rede × reset de fábrica × transferência de proprietário

| | Quem faz | Placa | Servidor |
|---|---|---|---|
| **Troca de rede** | qualquer pessoa com a placa e a etiqueta | botão 5 a 15 s, depois o app configura de novo | nada muda: mesmo certificado, mesma fazenda |
| **Reset de fábrica** | quem tem a placa | botão 15 s | o certificado antigo continua válido até ser revogado; a placa precisa de linha nova |
| **Transferência de proprietário** | dono atual + conta de provisionamento + novo dono | reset de fábrica + linha nova | 1. o dono atual revoga (`POST /v1/devices/{id}/revoke`); 2. a conta de provisionamento renova a placa com código de claim novo (`POST /v1/provisioning/devices/{cn}/renew`); 3. a placa recebe a linha nova e faz o enroll; 4. o novo dono faz o claim com o código novo |

Na transferência, o firmware não decide nada sobre posse. Ele só apaga o que é local, e o servidor é que impede um terceiro de assumir a placa. **Dependência:** hoje o backend não tem um "liberar sem revogar"; transferir passa por revogação e renovação.

## O que precisa ser implementado no app (APP-12)

1. **Leitura do QR** da etiqueta (nome BLE e prova de posse) e campo para o código de claim.
2. **Varredura BLE** filtrada pelo prefixo `AGRO_` e pelo UUID do serviço.
3. **Sessão segura 2** com usuário `wifiprov` e a prova de posse do QR.
4. **Leitura de `agro-status`** para obter o `cn`.
5. **Lista de redes** (`prov-scan`), seleção, senha, envio (`prov-config`) e tratamento de senha errada sem reconectar.
6. **Claim** (`POST /v1/devices/claim`, com 2FA) com o `cn`, o código da etiqueta, a fazenda e o nome.
7. **Teste:** aguardar a placa online e a primeira telemetria gravada.
8. **Mensagens** para cada estado da tabela acima, sem mostrar a senha em lugar nenhum.

Funcionar numa ferramenta de diagnóstico BLE (`esp_prov.py` ou o app ESP BLE Provisioning) prova que a placa cumpre o protocolo. A experiência só está pronta quando os passos acima existirem no app.

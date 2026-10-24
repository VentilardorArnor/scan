# Diagnóstico do envio de telemetria

A placa registra no log serial, a cada minuto, duas linhas da `journal_task` e uma da `health_task`:

```text
journal_task: tx: env 504 reenv 0 fila_cheia 0 | ack stored 496 dup 12 invalid 0 unclaimed 0 revoked 0 skew 0 outro 0 sem_lote 4 | espera 0 s
journal_task: tx: ack_erro 0 (ultimo 0) | acked 0 | cursor seg 7 off 11322 | 1o pendente -/0
health_task: journal: 0 pendentes, 0 perdidos | heap livre 141328 (mínimo 113508)
```

## O que cada contador conta

Todos são **acumulados desde o boot**: cobrem o mesmo período e zeram quando a placa reinicia. Eles contam **eventos**, não lotes distintos.

| Contador | Conta |
|---|---|
| `env` | primeira publicação de um lote **neste boot**. Inclui os lotes atrasados de boots anteriores que estavam no journal. |
| `reenv` | nova publicação de um lote já publicado neste boot (30 s sem ACK, ou reconexão) |
| `fila_cheia` | publicação adiada porque a fila de saída para o MQTT estava cheia; o lote volta no ciclo seguinte |
| `ack stored` | ACK `stored`: o servidor gravou o lote **pela primeira vez** (só responde depois do commit no banco) |
| `ack dup` | ACK `duplicate`: o servidor **já tinha** aquele `(boot_id, seq)` e não gravou de novo |
| `invalid`, `unclaimed`, `revoked`, `skew` | ACK `rejected` com cada motivo do contrato |
| `outro` | ACK `rejected` sem motivo, ou com motivo desconhecido |
| `sem_lote` | ACK de um lote que **não está mais pendente**, porque já tinha saído do journal: é um ACK repetido, sem efeito |
| `espera` | segundos que faltam numa espera crescente (`unclaimed`, motivo desconhecido) ou de ressincronização (`skew`) |
| `ack_erro (ultimo N)` | ACKs que o journal não aplicou por outro motivo. `6` = lista de ACKs fora de ordem cheia (transitório depois da correção); `5` = erro de E/S |
| `acked` | tamanho atual da lista de ACKs fora de ordem (0 a 32) |
| `cursor seg/off` e `1o pendente` | onde está o lote mais antigo ainda sem ACK (`boot_id/seq`); `-/0` = nenhum |

**Relações úteis:**

- **Total de ACKs recebidos:** `stored + dup + invalid + unclaimed + revoked + skew + outro`.
- **Publicações:** `env + reenv`.
- **Lotes ainda sem resposta:** os pendentes da `health_task` (o journal inteiro).
- **ACKs a mais que publicações:** podem acontecer. O MQTT QoS 1 entrega "pelo menos uma vez", e uma publicação repetida também gera outra resposta. Esses ACKs extras aparecem em `sem_lote`.

## Interpretação da leitura de 23/10 (`env 504`, `stored 496`, `dup 12`, `sem_lote 4`)

Leitura aos 180 s do boot que veio logo depois da correção, com 0 pendentes e nada em voo naquele instante.

| Conta | Resultado | Significado |
|---|---|---|
| ACKs recebidos | 496 + 12 = **508** | |
| Publicações | 504 + 0 = **504**, todas com resposta | |
| Diferença | 508 − 504 = **4** = `sem_lote` | 4 ACKs repetidos de lotes que já tinham saído do journal |
| `dup 12` | lotes que o servidor **já tinha gravado** antes deste boot | os 8 lotes que ficaram no laço de reenvio durante o travamento, mais alguns publicados pouco antes do reinício cujo ACK não chegou a ser aplicado |
| `stored 496` | lotes gravados **pela primeira vez** neste boot | |

Os 504 enviados incluem os 450 que estavam no journal ao ligar e os gerados durante os 180 s (um a cada 3 s).

## O que a correção garante (testes de host)

| Cenário | Comportamento | Teste |
|---|---|---|
| ACK perdido | o lote é reenviado com o mesmo `(boot_id, seq)` depois de 30 s; o servidor responde `duplicate` e o lote sai | `test_puback_alone_does_not_free_and_timeout_resends`, simulação |
| ACK duplicado | o primeiro libera o lote; os seguintes contam em `sem_lote` e não mexem em nada | simulação |
| ACKs fora de ordem | ficam na lista (até 32) e o cursor avança quando o lote da frente é confirmado | `test_ack_out_of_order`, simulação |
| ACK do lote da frente com a lista cheia | **é aplicado mesmo assim** e libera de uma vez os seguintes já confirmados (era o que travava) | `test_head_ack_with_full_out_of_order_list` |
| Rajada maior que a fila ou a lista | o excesso é perdido como um ACK perdido e coberto pelo reenvio | simulação; na placa a fila de entrada passou de 8 para 32 |
| Reboot no meio | o cursor e a lista são gravados de forma atômica; nada se perde e nada é aplicado duas vezes | `test_ack_out_of_order`, `test_reboot_in_the_middle_of_a_write`, simulação |

A **simulação** (`test_lossy_link_never_deadlocks`) roda 150 lotes por até 3 h simuladas, com:

- 10% das publicações perdidas;
- 25% dos ACKs perdidos e 15% repetidos;
- 20% dos ACKs atrasados para o ciclo seguinte, com a ordem embaralhada;
- o ACK do lote mais antigo bloqueado nos primeiros 10 min.

Ela exige que o journal termine vazio, que o servidor grave cada lote **uma única vez** e que a lista tenha chegado ao limite de 32 no caminho. Com o comportamento anterior à correção, a mesma simulação termina travada com 118 lotes pendentes.

## Limite que continua existindo

Se o servidor **nunca** responder a um lote específico (nem `invalid`), o cursor fica parado nele. Os lotes seguintes continuam saindo até a lista de ACKs fora de ordem encher; depois disso, só esse lote e os vizinhos são reenviados. O journal volta a andar quando enche e descarta o segmento mais antigo (evento `journal_full`, contador `journal_dropped`). Com um lote a cada 3 s, isso leva de 12 a 15 horas.

Pelo contrato isso não deveria acontecer: lote malformado recebe `rejected/invalid`, que o firmware descarta. Se acontecer, a linha `1o pendente` do log mostra qual lote está preso.

## Diagnóstico do GPS

A `gps_task` registra uma linha a cada 30 s que separa as situações possíveis:

| Linha no log | Significado | O que conferir |
|---|---|---|
| `GPS: SEM COMUNICAÇÃO - 0 bytes ...` | nenhum sinal chega ao RX (GPIO21) | alimentação do módulo (LED aceso), TX do GPS → GPIO21, GND comum, fios trocados |
| `GPS: N bytes, ... nenhuma sentença NMEA válida` | chegam bytes, mas embaralhados | baud do módulo diferente de 9600, ou ruído e fio longo |
| `GPS: comunicação OK ... SEM FIX: satélites em vista N` | módulo funcionando, ainda sem posição | com 0 em vista, a antena não vê o céu; com vários em vista, é o aquecimento (o primeiro fix pode levar alguns minutos) |
| `GPS: FIX OK ...` | posição válida | a telemetria passa a levar `gps` (log `telemetria passou a incluir o objeto gps`) e o painel mostra Posição |

As coordenadas aparecem no log arredondadas em 2 casas decimais (cerca de 1 km); a telemetria leva o valor completo.

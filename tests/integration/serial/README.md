# Serial Integration Tests — tty0tty Virtual Serial Pair

Testa o `std.serial` com IO real usando o módulo de kernel `tty0tty`,
que cria pares de TTYs virtuais (`/dev/tnt0` ↔ `/dev/tnt1`) registrados
no subsistema tty do kernel — visíveis ao `libserialport` via sysfs/udev.

## Por que Docker?

- `libserialport` enumera portas via `udev`/`sysfs` — só vê TTYs registrados no kernel
- `socat` cria `/dev/pts/N` que não são registrados no sysfs → libserialport não os enxerga
- `tty0tty` cria devices reais (`/dev/tnt*`) que aparecem no `serial.list()` do Fluxa
- `--privileged` permite `insmod` dentro do container

## Pré-requisitos

```bash
# Docker instalado e rodando
docker info

# Kernel headers do host (para compilar tty0tty dentro do container)
uname -r                              # ex: 6.8.0-49-generic
ls /lib/modules/$(uname -r)/build    # deve existir
```

Se os headers não estiverem instalados:
```bash
sudo apt install linux-headers-$(uname -r)
```

## Uso

```bash
# A partir da raiz do projeto:
bash tests/integration/serial/run.sh

# Com output detalhado:
bash tests/integration/serial/run.sh --verbose

# Forçar rebuild da imagem:
bash tests/integration/serial/run.sh --build

# Via docker-compose:
cd tests/integration/serial
docker-compose run --rm fluxa-serial-test
```

## Cenários testados

| Teste | Descrição |
|---|---|
| `list_finds_tnt_devices` | `serial.list()` enumera `/dev/tnt0` e `/dev/tnt1` |
| `open_close_tnt0` | `serial.open/close` em porta virtual |
| `bytes_available_returns_int` | `bytes_available` retorna inteiro |
| `write_tnt0_read_tnt1` | Escreve em tnt0, lê em tnt1 (loopback completo) |
| `write_confirms_sent` | Confirmação de envio sem erro |
| `prst_dyn_port_hot_reload` | Cursor sobrevive hot reload via `prst dyn` |
| `read_timeout_returns_empty_not_error` | Timeout retorna string vazia, não erro |
| `multi_message_sequential` | 3 mensagens sequenciais write/readline |

## Por que o delay não é problema aqui

`tty0tty` é kernel-space — a latência é real (buffer kernel, não memória).
Para simular latência de UART físico, pode-se adicionar `tc qdisc` na
interface de rede, mas para testes de API serial isso não é necessário:
o que validamos é a corretude do protocolo de abertura/fechamento/IO,
não timing de hardware.

## Integração com a suite principal

O `tests/run_tests.sh` não roda estes testes automaticamente (requer Docker).
Para incluir no CI com Docker disponível:

```bash
make test-all                         # unit tests
bash tests/integration/serial/run.sh  # serial IO tests (requer Docker)
```

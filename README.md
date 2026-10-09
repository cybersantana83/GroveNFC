# GroveNFC

![Build](https://github.com/cybersantana83/GroveNFC/actions/workflows/build.yml/badge.svg)
![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)

Firmware de referência pra módulo NFC Grove (M5Stack: AtomS3 / M5StickS3 / M5StickC Plus / CardPuter / M5Paper). Detecta automaticamente qual dos dois chips NFC/RFID da M5 está conectado (`ST25R3916` ou `WS1850S`), lê cartões/tags, e inclui um leitor EMV read-only (PAN + bandeira) validado ponta a ponta sobre ISO14443-4.

Fork de [whywilson/GroveNFC](https://github.com/whywilson/GroveNFC). Parte do laboratório de hardware da **Café com Solda**.

## Status

**Fase 0, 1.5 e leitor EMV concluídos** — leitura real validada em hardware nos dois chips, leitor EMV extraindo PAN + bandeira de cartão real.

- **Detecção automática de chip** — scan de I2C no boot identifica `ST25R3916` (`0x48`, produto "Unit NFC" — FeliCa/ISO15693/emulação completa) ou `WS1850S` (`0x28`, produto "RFID Unit 2" — ISO14443A/MIFARE/NTAG, sem emulação), e roteia pro driver certo sem o usuário precisar configurar nada. Antes disso, conectar o módulo "errado" dava um `NFC Fail` genérico sem nenhuma pista do motivo.
- **Leitor EMV (read-only)** — `RATS` → `SELECT PPSE` → escolhe a aplicação de crédito (fallback: menor prioridade) → `SELECT AID` → `GPO` com PDOL neutro → extrai PAN do Track2. Sem escrita, sem manipulação de tag, sem máscara no número (decisão explícita). Hoje validado em `src/emv_test.cpp` (ambiente isolado); portar pra uma tela no firmware principal é o próximo passo.
- **Leitor FIDO2 NFC (planejado)** — vai reaproveitar a mesma base ISO14443-4 do leitor EMV (`sendIBlock()`, com WTX e chaining já resolvidos). Ferramenta separada da pesquisa **Chaos IAM**, sem compartilhar branding/metadados com ela.
- **Usabilidade da UI** — corrigido: cliques perdidos durante a animação de transição da home (~240ms) e falta de feedback visual durante o hold do botão (agora tem uma barra de progresso). Falta documentar o mapeamento de botões por board.

Detalhes técnicos completos — pegadinhas de protocolo, decisões de escopo, histórico de cada fase — estão no [`ROADMAP.md`](ROADMAP.md).

## Hardware

Dois chips NFC/RFID da M5 são suportados, detectados automaticamente no boot:

| Produto M5 | Chip | Endereço I2C | Protocolos | Emulação |
|---|---|---|---|---|
| Unit NFC | `ST25R3916` | `0x48` | ISO14443A, ISO14443B, ISO15693, FeliCa | Sim (MIFARE 1K, NTAG213/215/216, ISO15693) |
| RFID Unit 2 | `WS1850S` | `0x28` | ISO14443A (MIFARE, NTAG/Ultralight) | Não |

Pinos I2C por board (selecionados automaticamente em `src/main.cpp` pelo target de build):

- AtomS3: SDA `GPIO2`, SCL `GPIO1`
- M5StickS3: SDA `GPIO9`, SCL `GPIO10`
- M5StickC Plus (1.1 / Plus2): SDA `GPIO32`, SCL `GPIO33`
- CardPuter / CardPuter ADV: SDA `GPIO2`, SCL `GPIO1`
- M5Paper (Porta A): SDA `GPIO25`, SCL `GPIO32`

Notas:

- Grove `G1/G2` do AtomS3 corresponde a `G33/G32` do StickC (ou seja, `SCL=33`, `SDA=32`).
- M5StickC Plus (1.1 / Plus2) usa buzzer PWM, sem caminho de speaker completo.
- No StickS3, o firmware liga `EXT_5V` (`M5.Power.setExtOutput(true)`) no boot pra alimentar o módulo Grove externo.
- Se a sua fiação usa pinos diferentes, ajuste `kSdaPin`/`kSclPin` em `src/main.cpp`.

## Estrutura

```
GroveNFC/
├── platformio.ini            ambientes de build (um por board + 3 isolados de validação)
├── patch_m5unit_nfc.py        extra_script: só roda pro ambiente m5paper
├── build_all.sh                builda todos os ambientes de board em sequência
├── ROADMAP.md                   histórico de fases, decisões de escopo, pegadinhas técnicas
└── src/
    ├── main.cpp                  firmware principal (UI, roteamento de chip, todas as páginas)
    ├── GroveNFC.cpp / .h          camada de driver do ST25R3916 (upstream)
    ├── M5Paper_Config.h           config específica do target M5Paper
    ├── ws1850s_test.cpp          sketch isolado de validação do driver WS1850S
    ├── emv_test.cpp              sketch isolado de validação do leitor EMV
    └── test_m5paper.cpp         teste de canvas→sprite→push pro e-ink do M5Paper
```

## Build (PlatformIO)

```bash
pip install --break-system-packages -U platformio   # ou pipx install platformio
```

Ambientes de board disponíveis:

- `m5stack-atoms3`
- `m5stack-sticks3`
- `m5stack-stickcplus`
- `m5stack-cardputer`
- `m5stack-m5paper`

```bash
pio run -e m5stack-sticks3 -t upload -t monitor
# troca m5stack-sticks3 pelo ambiente do seu board
```

Ambientes isolados de validação (não fazem parte do firmware principal — cada um exclui `main.cpp`/`GroveNFC.cpp` do build):

- `ws1850s-test` — valida leitura MIFARE/NTAG no WS1850S puro
- `emv-test` — valida o fluxo EMV completo (RATS → PPSE → GPO → PAN)
- `m5stack-m5paper-test` — valida o pipeline de desenho no e-ink

```bash
pio run -e emv-test -t upload -t monitor
```

## UI e controle de botões

Botão principal: `BtnA` (StickS3/AtomS3/CardPuter), `BtnB` (M5Paper, via touch).

- **Home**: clique troca de card (`Diagnose → Reader → Read NDEF → Emulator`); hold (~380ms StickS3, varia por board) entra na funcionalidade.
- **Reader / Read NDEF**: clique força leitura imediata; hold volta pra Home.
- **Emulator**: clique avança item/ação; hold confirma.
- **CardPuter**: setas (ou `WASD`/`HJKL`) navegam, `Enter` confirma, `ESC`/`Del` volta.
- **M5Paper**: toque direto nos cards da home; botão C volta, botão B confirma, botão A avança.

Feedback visual: uma barra fina no rodapé da tela enche durante o hold, proporcional ao tempo até a ação disparar (não disponível no M5Paper — e-ink é lento demais pra overlay por tick).

## Licença

**GPLv3 ou posterior** (GNU General Public License v3.0-or-later).

- Texto completo em [`LICENSE`](LICENSE) (canônico, SHA-256 `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`).
- Cada arquivo fonte deste fork tem header SPDX `GPL-3.0-or-later`.
- Forks e derivados devem ser redistribuídos sob a mesma licença.

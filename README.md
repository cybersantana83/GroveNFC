# GroveNFC

![Build](https://github.com/cybersantana83/GroveNFC/actions/workflows/build.yml/badge.svg)
![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)

Firmware de referência pra módulo NFC Grove (M5Stack: AtomS3 / M5StickS3 / M5StickC Plus / CardPuter / M5Paper). Detecta automaticamente qual dos dois chips NFC/RFID da M5 está conectado (`ST25R3916` ou `WS1850S`), lê cartões/tags, e inclui um leitor EMV read-only (PAN + bandeira) validado ponta a ponta sobre ISO14443-4.

Fork de [whywilson/GroveNFC](https://github.com/whywilson/GroveNFC). Parte do laboratório de hardware da **Café com Solda**.

## Status

**Fases 0, 1, 1.5 e 2 concluídas** — leitura real validada em hardware nos dois chips, leitor EMV com tela própria no firmware principal extraindo PAN + bandeira de cartão real, dossiê genérico pra qualquer protocolo, leitor FIDO2/CTAP2 via NFC validado em ambiente isolado, e UI com feedback de hold + mapeamento de botões documentado.

- **Detecção automática de chip** — scan de I2C no boot identifica `ST25R3916` (`0x48`, produto "Unit NFC" — FeliCa/ISO15693/emulação completa) ou `WS1850S` (`0x28`, produto "RFID Unit 2" — ISO14443A/MIFARE/NTAG, sem emulação), e roteia pro driver certo sem o usuário precisar configurar nada. Antes disso, conectar o módulo "errado" dava um `NFC Fail` genérico sem nenhuma pista do motivo.
- **Leitor EMV (read-only), com tela própria no firmware** — `RATS` → `SELECT PPSE` → escolhe a aplicação de crédito (fallback: menor prioridade) → `SELECT AID` → `GPO` com PDOL neutro → extrai PAN do Track2 e o AIP (tag `82`). Sem escrita, sem manipulação de tag, sem máscara no número (decisão explícita). O resultado é um **dossiê** (bandeira, PAN, AID, AIP e o perfil de autenticação que o AIP indica — SDA/DDA/CDA, com vetor de ataque público conhecido, ano e risco — ver `src/emv_db.h`/`.cpp`), no mesmo espírito do `cards_db` do [ChaosID](https://github.com/cybersantana83/ChaosID). Página `EMV` no carrossel da home do M5StickS3 (único board com o driver WS1850S ligado hoje) — clique força nova leitura, hold volta pra Home. A lógica em si continua validada em `src/emv_test.cpp` (ambiente isolado) e portada 1:1 pra `src/main.cpp`.
- **Dossiê para qualquer tipo de cartão (não só EMV), direto na página Reader** — `src/cards_db.h`/`.cpp` é o mesmo `cards_db` do ChaosID portado (seção HF, já que os módulos Grove são 13.56MHz — sem LF/125kHz), mapeando o protocolo que o Reader já detecta (`MFC1K`, `MFC4K`, `MFUL`, `NTAG*`, `DESFire`, `FeliCa`, `ISO15693`, `ISO14443B`) pro mesmo tipo de perfil: uso típico, criptografia, vetor de ataque conhecido, ano e risco. Funciona com os dois chips/backends (`ST25R3916`/"Unit NFC" e o driver secundário WS1850S), já que o dossiê só depende da string de protocolo que ambos já produzem — sem precisar de APDU nem de ISO14443-4. Validado primeiro em `src/ws1850s_test.cpp` (ambiente isolado) e depois portado pra tela real do Reader em todo board com tela landscape (AtomS3 fica de fora por falta de espaço na tela quadrada pequena).
- **Leitor FIDO2/CTAP2 via NFC (validação isolada)** — reaproveita a mesma base ISO14443-4 do leitor EMV (`RATS` + I-block com WTX/chaining já resolvidos), mas em sketch próprio (`src/fido2_test.cpp`): `SELECT` no applet FIDO (AID `A0000006472F0001` — **não** o AID de NDEF Tag Application que uma nota antiga deste repo citou por engano) seguido de um `authenticatorGetInfo` (CTAP2). Dump bruto (hex + varredura ASCII), sem parser de CBOR — é validação de alcance/transporte, não um client CTAP2. Ferramenta separada da pesquisa **Chaos IAM**, sem compartilhar branding/metadados com ela.
- **Usabilidade da UI** — corrigido: cliques perdidos durante a animação de transição da home (~240ms) e falta de feedback visual durante o hold do botão (agora tem uma barra de progresso). Mapeamento de botões por board documentado abaixo.

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
├── platformio.ini            ambientes de build (um por board + 4 isolados de validação)
├── patch_m5unit_nfc.py        extra_script: só roda pro ambiente m5paper
├── build_all.sh                builda todos os ambientes de board em sequência
├── ROADMAP.md                   histórico de fases, decisões de escopo, pegadinhas técnicas
└── src/
    ├── main.cpp                  firmware principal (UI, roteamento de chip, todas as páginas,
    │                             incluindo a página EMV no M5StickS3)
    ├── GroveNFC.cpp / .h          camada de driver do ST25R3916 (upstream)
    ├── M5Paper_Config.h           config específica do target M5Paper
    ├── ws1850s_test.cpp          sketch isolado de validação do driver WS1850S
    ├── emv_test.cpp              sketch isolado de validação do leitor EMV
    ├── fido2_test.cpp            sketch isolado de validação do leitor FIDO2/CTAP2 via NFC
    ├── emv_db.h / emv_db.cpp     dossiê EMV: perfil SDA/DDA/CDA a partir do AIP lido
    ├── cards_db.h / cards_db.cpp dossiê genérico (MIFARE/NTAG/DESFire/FeliCa/ISO15693/ISO14443B)
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
- `fido2-test` — valida SELECT no applet FIDO + `authenticatorGetInfo` (CTAP2) via NFC
- `m5stack-m5paper-test` — valida o pipeline de desenho no e-ink

```bash
pio run -e emv-test -t upload -t monitor
```

## UI e controle de botões

A regra geral é a mesma em todo board: **clique** avança/confirma a ação mais comum da tela atual (trocar card na home, forçar leitura, avançar item), e **hold** (segurar o botão por um tempo que varia por board — ver tabela) entra na funcionalidade ou volta pra Home, dependendo da tela.

| Board | Botão "clique" | Botão "hold" | Threshold do hold | Botão secundário |
|---|---|---|---|---|
| AtomS3 | `BtnA` | `BtnA` (mesmo botão) | 700ms | nenhum — só tem 1 botão físico |
| M5StickS3 | `BtnA` | `BtnA` | 380ms | `BtnPWR` **ou** `BtnB` (qualquer um dos dois) — tap = `key_b` (equivale a "anterior"), hold = voltar/cancelar |
| M5StickC Plus (1.1 / Plus2) | `BtnA` | `BtnA` | 700ms | `BtnB` — tap = `key_b`, hold = voltar/cancelar |
| CardPuter / CardPuter ADV | teclado | tecla `b`/`B` | 320ms (hold da tecla `b`) | setas ou `W`/`A`/`H`/`I`/`K`/`;`/`,` (anterior), `S`/`D`/`J`/`L`/`.`/`/` (próximo), `Enter` confirma, `ESC`/`Del` volta |
| M5Paper | `BtnB` (ou toque) | `BtnB` | 500ms | `BtnC` — clique = anterior, hold = voltar/cancelar; `BtnA` = próximo |

Notas:

- No StickS3/StickC Plus, o botão secundário só tem *tap* e *hold* — não existe um "clique" equivalente ao botão principal (ele dispara `nav_prev`/"voltar", não "próximo").
- No AtomS3, por ter só um botão físico, a mesma tecla serve pra avançar (clique) e confirmar/voltar (hold) — o firmware decide qual ação cabe ao hold de acordo com a tela atual.
- No M5Paper, tocar direto num card da home também funciona, além dos botões físicos.

Dentro de cada tela:

- **Home**: clique troca de card (`Diagnose → Reader → Read NDEF → Emulator` → ...); hold entra na funcionalidade.
- **Reader / Read NDEF**: clique força leitura imediata; hold volta pra Home.
- **EMV** (só no M5StickS3, com WS1850S): clique força nova leitura do cartão já no leitor; hold volta pra Home.
- **Emulator**: clique avança item/ação; hold confirma.

Feedback visual: uma barra fina no rodapé da tela enche durante o hold, proporcional ao tempo até a ação disparar (não disponível no M5Paper — e-ink é lento demais pra overlay por tick).

## Licença

**GPLv3 ou posterior** (GNU General Public License v3.0-or-later).

- Texto completo em [`LICENSE`](LICENSE) (canônico, SHA-256 `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`).
- Cada arquivo fonte deste fork tem header SPDX `GPL-3.0-or-later`.
- Forks e derivados devem ser redistribuídos sob a mesma licença.

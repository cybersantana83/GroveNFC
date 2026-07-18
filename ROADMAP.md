# Fork Roadmap — GroveNFC → Console de Operações NFC

Fork de [whywilson/GroveNFC](https://github.com/whywilson/GroveNFC).
Objetivo: sair de "demo de referência" pra base única onde consolidar os
projetos de NFC/EMV que hoje vivem espalhados em `.fap` do Flipper e scripts
Python soltos.

## Por que forkar em vez de contribuir upstream

- Issues estão desabilitadas no repo original (não dá pra abrir PR/discussão
  formal ainda).
- O escopo que eu quero (módulos de EMV, relay, dumps) é maior que o do
  projeto original, que é intencionalmente uma demo de referência.
- Nada impede de mandar patches pontuais pro upstream depois, se o autor
  abrir espaço pra isso.

## Fase 0 — Estabilizar a base (bloqueia tudo o resto)

- [ ] Confirmar causa do `NFC Fail` no StickS3: rodar com o log
      `M5.getBoard()` (já adicionado nesta branch) e comparar contra o board
      ID esperado para M5StickS3.
- [ ] Se for detecção de board: corrigir ou reportar upstream no M5Unified.
- [ ] Se for timing: expor `kNfcBootPowerSettleMs` / `kNfcBootRetryCount`
      como valores fáceis de tunar por board (hoje já são `constexpr` por
      `#if defined(...)`, só falta testar valores maiores no StickS3).
- [ ] Adicionar fallback de pinos SDA/SCL trocados no StickS3, igual ao que
      já existe pro CardPuter (`main.cpp`, dentro de `initNfcAtBoot()`).
- [x] Travar `M5Unified` em versão exata (não `^`) no ambiente StickS3,
      pra não sofrer de novo com "funcionava semana passada".
- [ ] Avaliar pinar `M5UnitNFC` também (hoje sem tag/commit fixo em quase
      todos os ambientes) — cuidado especial com `m5stack-m5paper`, que
      depende do `patch_m5unit_nfc.py` bater contra strings literais do
      código-fonte da lib.

## Fase 1 — Usabilidade da UI (botão único)

- [ ] Dar feedback visual durante o hold (barra de progresso ou highlight
      crescente) em vez de só "nada acontece até completar ~380ms".
- [ ] Não descartar cliques que caem dentro da janela de animação de
      transição de tile (~240ms) — hoje o loop principal faz `return`
      antes de processar qualquer navegação nesse intervalo.
- [ ] Documentar no README o mapeamento real de botões por board
      (ex: StickS3 usa `BtnA` pra próximo/confirmar e `BtnPWR`/`BtnB`
      pra voltar — isso hoje não está escrito em lugar nenhum).

## Fase 1.5 — Suporte a múltiplos chips NFC (achado durante debug do StickS3)

Contexto: o `NFC Fail` no StickS3 não era bug de boot/energia — era o módulo
físico errado. O GroveNFC foi escrito só pro chip `ST25R3916` (produto M5
"Unit NFC", I2C `0x48`). Quem conectar o `RFID Unit 2` (chip `WS1850S`,
I2C `0x28` — outro produto M5, mesmo conector Grove) recebe `NFC Fail` sem
nenhuma pista de que o chip é incompatível, porque o driver simplesmente
não reconhece o endereço.

- [x] **Auto-detecção de chip no boot**: scan de I2C em `setup()`, antes do
      `initNfcAtBoot()`. Confirmado com hardware real: `0x28` = WS1850S,
      nada em `0x48`. Aviso amigável (`boot_notice_line`) no lugar do
      `NFC FAIL` genérico, sem sobrescrever o notice no `runBootDebugFlow()`,
      e sem I2C error spam nas leituras de hw/fw version quando `nfc_ready`
      é `false`. Commits: ver histórico da branch `santana/fork-base`.
- [ ] **Driver WS1850S**: o WS1850S é compatível a nível de comando com a
      família MFRC522 — dá pra reaproveitar uma lib tipo `MFRC522_I2C`
      em vez de escrever do zero. Cobre ISO14443A/MIFARE/NTAG.
      Bloqueado até o módulo "Unit NFC" (ST25R3916) chegar (~06/08) —
      preciso dos dois lado a lado pra não quebrar o caminho ST25R3916
      já funcional enquanto adiciono o WS1850S.
- [ ] **Aviso de limitação na UI e no README**, específico por chip
      detectado:
      - `WS1850S`: ISO14443A, MIFARE, NTAG. **Sem** FeliCa, ISO15693, e
        **sem** emulação completa (o WS1850S não suporta os modos de
        emulação que o firmware demonstra pro ST25R3916).
      - `ST25R3916`: capacidade completa (é o que o firmware original já
        cobre) — FeliCa, ISO15693, emulação MIFARE 1K/NTAG213/215/216/etc.
      Objetivo: quem usar o fork com o módulo "errado" vê um aviso
      informativo em vez de `NFC Fail` genérico.
- [ ] Atualizar README com uma tabela clara dos dois produtos M5
      (nome comercial, chip, endereço I2C, protocolos suportados), já
      que hoje nada no repo avisa que "GroveNFC" mira um chip específico.

## Fase 2 — Arquitetura de módulos (onde entram os projetos pessoais)

Ideia: manter a separação de camadas que o repo original já tem
(hardware GroveNFC vs firmware/UI) e adicionar uma **camada de
capacidades** plugável por cima:

- [ ] **Módulo EMV manipulation** — portar a lógica de manipulação de tags
      TTQ (`9F66`), CTQ (`9F6C`), AIP (`82`) que hoje vive nos scripts do
      rig de Raspberry Pi Zero.
- [ ] **Módulo Relay/MITM** — avaliar usar `ESP-PN532Killer` /
      `ESP-PN532 UART` (mesmo autor, `whywilson`) como transporte
      alternativo ao ST25R3916 nativo do Grove, pra reaproveitar lógica já
      testada em vez de portar do zero.
- [ ] **Módulo Dump manager** — unificar formato de dump entre o que já
      existe no firmware (`/dumps`, Web File Manager) e o que você usa
      hoje nos scripts.

## Fase 3 — Migração gradual

- [ ] Ir descontinuando `.fap`/scripts conforme cada módulo atinge
      paridade de funcionalidade no firmware unificado.
- [ ] Ao final, um único firmware cobrindo AtomS3 / StickS3 / CardPuter
      com todas as capacidades hoje espalhadas.

## Notas técnicas soltas (pra não esquecer)

- Board StickS3 depende de sequência de PMIC (`M5PM1`, registrador `0x06`
  bit 3) pra ligar o 5V do Grove — via `M5.Power.setExtOutput()`. Se
  `M5.getBoard()` não identificar o board corretamente, esse switch-case
  nunca dispara e o Grove nunca recebe 5V real, mesmo que o código "ache"
  que ligou.
- CardPuter tem um retry de pinos SDA/SCL trocados que o StickS3 não tem
  — bom padrão pra copiar.
- `patch_m5unit_nfc.py` só roda no ambiente `m5stack-m5paper`
  (`extra_scripts = pre:patch_m5unit_nfc.py`), nenhum outro board recebe
  esses patches de `M5UnitNFC`.

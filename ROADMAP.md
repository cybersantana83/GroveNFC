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

## Fase 0 — Estabilizar a base (bloqueia tudo o resto) ✅ CONCLUÍDA

**Fechada e validada com hardware real em 18/07.** Causa raiz do `NFC Fail`
não era nada disso que a gente suspeitava de início — era o módulo Grove
errado (`WS1850S` em vez de `Unit NFC`/ST25R3916). Ver Fase 1.5 pra
detalhes. Com o `Unit NFC` correto conectado: `[BOOT] DIAG: PASS`, boot
limpo, e leitura real confirmada (`MFC 1K`, UID `AD2A19D4`).

- [x] Confirmar causa do `NFC Fail` no StickS3: log `M5.getBoard()` = `26`
      (correto). Board detection nunca foi o problema.
- [x] ~~Se for detecção de board~~ — descartado, board sempre detectou certo.
- [x] ~~Se for timing~~ — descartado. Testado com 220ms→1.5s e 5→10 retries,
      resultado idêntico. Timing nunca foi o problema.
- [x] ~~Fallback de pinos SDA/SCL trocados~~ — descartado, pinos GPIO9/10
      sempre estavam corretos (confirmado por scan I2C real).
- [x] Travar `M5Unified` em versão exata (`0.2.18`, StickS3), sem afetar
      `m5paper`.
- [x] Pinar `M5UnitNFC` em `#0.1.0` nos ambientes sem `patch_m5unit_nfc.py`
      (mantido flutuante só no `m5paper`, de propósito).

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
- [x] **Driver WS1850S**: implementado com `arozcan/MFRC522-I2C-Library`
      (o WS1850S é compatível a nível de comando com a família MFRC522).
      Validado com hardware real: MIFARE Classic 1K (UID 4 bytes) e
      NTAG/Ultralight (UID 7 bytes, aparece como `MFUL` — a lib não
      distingue NTAG213/215/216 entre si via SAK, todas caem no mesmo
      `PICC_TYPE_MIFARE_UL`). Cobre Reader apenas — sem FeliCa, ISO15693,
      emulação (ver item de aviso de limitação abaixo).

      **Arquitetura**: `readWs1850sCard()` preenche um `CardInfo` no mesmo
      formato que o resto do firmware já usa; `readCardGeneric()` roteia
      pro backend certo (`nfc_ws1850s_active` ou o `nfc`/ST25R3916 padrão)
      sem a UI precisar saber qual chip está por trás.

      **Pegadinhas encontradas** (documentando pra não cair de novo):
      - A lógica real de polling do Reader não vive em `handleReader()`
        (essa função existe mas nunca é chamada) — está **duplicada e
        embutida** dentro do `nfcWorkerTask` (Core 0), no bloco comentado
        `// --- handleReader (periodic card poll) ---`. Qualquer mudança
        de leitura precisa ir lá, não na função de mesmo nome.
      - `PICC_IsNewCardPresent()` usa REQA internamente, que **não acorda
        cartões em HALT**. Como halitamos o cartão após cada leitura
        (`PICC_HaltA()`), isso fazia o cartão "sumir da tela" após uma
        leitura — resolvido trocando a checagem de presença por
        `PICC_WakeupA()` (WUPA acorda cartões em HALT e em IDLE).
      - `card.protocol` precisa usar os códigos curtos que o resto do
        firmware já espera (`MFC1K`, `MFC4K`, `MFUL`, etc. — ver
        `protocolFull()`), não a string descritiva que a lib MFRC522
        retorna (`PICC_GetTypeName()`) — senão a UI mostra "Unknown".
      - O construtor `MFRC522(addr, resetPin)` exige um GPIO válido pro
        pino de reset mesmo sem conexão física real (o Grove do WS1850S
        não expõe esse pino) — usamos um pino dummy não conectado (`4`).
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

## Fase 2 — Leitores + usabilidade (escopo redefinido em 06/08)

**Mudança de escopo**: EMV manipulation, Relay/MITM e o rig de PN532Killer
saem do plano — decidido não seguir por esse caminho no fork. Foco vira
**leitura/diagnóstico read-only** (EMV, FIDO2) + deixar a UI de verdade
usável, em vez de ferramentas de manipulação/relay.

Arquitetura: manter a separação de camadas do repo original (hardware
GroveNFC/WS1850S vs firmware/UI) e adicionar uma **camada de leitores**
plugável por cima — mesmo padrão que já usamos pro roteamento
ST25R3916/WS1850S (`readCardGeneric()`), só que por protocolo de
aplicação em vez de por chip.

- [x] **Leitor EMV (read-only) — PAN + bandeira**: validado ponta a ponta
      em `src/emv_test.cpp` (ambiente `emv-test`, isolado do firmware
      principal, mesmo padrão do `ws1850s_test.cpp`). Fluxo completo:
      `RATS` → `SELECT PPSE` → lista aplicações (AID/label/prioridade) →
      escolhe crédito por label (fallback: menor prioridade) → `SELECT AID`
      → `GPO` com PDOL neutro (valor zero, sem CVM/online) → extrai PAN do
      Track2 (tag `57`). Testado com cartão real (Visa Crédito + Electron),
      saída automática: `Bandeira: Visa` / `Numero do cartao: ...`.

      **Por que foi bem mais trabalhoso que o Reader normal**: cartões EMV
      usam ISO14443-4 (camada 4, com APDUs), que a `MFRC522-I2C-Library`
      não implementa nativamente (só camada 3, tipo MIFARE). Construímos
      isso na mão em cima do `PCD_TransceiveData()` cru. Pegadinhas
      encontradas, na ordem que apareceram:

      1. **CRC não é anexado automaticamente** — `checkCRC=true` só
         *valida* o CRC da resposta, não *envia* CRC no comando. RATS e
         todo I-block precisam de CRC_A calculado manualmente
         (`PCD_CalculateCRC()`) e anexado antes de transmitir.
      2. **Timeout de hardware fixo em 25ms** (`PCD_Init()` configura
         `TReloadReg` pra isso) — cartões EMV frequentemente declaram
         `FWI` no ATS pedindo 40-80ms+. Precisa reconfigurar
         `TReloadRegH/L` antes de qualquer troca de APDU.
      3. **S(WTX) — "me dá mais tempo"**: além do FWT do ATS, o cartão
         pode pedir extensões de tempo em tempo real via bloco S(WTX)
         (`PCB=0xF2/0xF3`). Preciso ecoar `PCB+WTXM` de volta e esperar
         de novo — GPO chegou a pedir 8+ rounds disso (é processamento
         criptográfico de verdade, bem mais lento que SELECT). Um limite
         de segurança baixo (20 rounds) já causou falso-negativo — subiu
         pra 100.
      4. **N(R) do R(ACK) é o INVERSO do bloco recebido**
         (`i_bn(pcb) ^ 0x01`, confirmado contra a implementação madura
         `M5Unit-NFC/isoDEP.cpp`), não o mesmo valor — usar o mesmo fez o
         cartão reenviar o mesmo bloco pra sempre em vez de avançar.
      5. **CRC não é removido de cada pedaço encadeado** — ao concatenar
         blocos de uma resposta fragmentada (chaining), cada pedaço ainda
         carrega seu próprio CRC de 2 bytes no final; sem descontar isso
         (`backLen - 3`, não `backLen - 1`), o CRC de cada pedaço vaza pro
         meio dos dados remontados (sintoma visível: strings cortadas ao
         meio, ex. "VISA DEBITO" virando "VISA D" + lixo + "EBITO").
      6. **Buffer de 256 bytes com variável `byte` estoura silenciosamente**
         (`byte len = sizeof(buf256)` vira `0`, já que `256 mod 256 = 0`) —
         causou `STATUS_NO_ROOM` mesmo com espaço de sobra. FIFO físico do
         chip é só 64 bytes mesmo, então não tem por que passar disso pra
         os buffers de troca por vez (só o buffer de *acumulação* da
         resposta remontada precisa ser maior, tipado com cuidado).

      **Reaproveitável pro FIDO2**: o `sendIBlock()` (com WTX + chaining +
      todas as correções acima) é genérico — qualquer troca de APDU sobre
      ISO14443-4 usa a mesma base, incluindo FIDO2/CTAP2.
- [ ] **Leitor FIDO2 NFC (read-only)** — ler resposta de authenticators
      FIDO2/CTAP2 via NFC (comandos APDU de autenticação/get-info) e
      mostrar dados estruturados na tela. **Importante**: isso é uma
      ferramenta nova, separada do `fido2-nfc-scanner` (Flipper FAP) e da
      pesquisa Chaos IAM — não misturar branding/metadados entre os dois
      projetos.
- [ ] **Integrar o leitor EMV no firmware principal** — hoje só existe em
      `src/emv_test.cpp` (ambiente isolado `emv-test`). Falta portar pra
      uma tela nova no `main.cpp` (mesmo padrão do `readCardGeneric()`),
      com número completo na tela (decisão explícita: sem máscara).
- [ ] **Usabilidade da UI** (itens que já estavam na Fase 1, consolidados
      aqui por fazerem parte do mesmo esforço de "deixar usável de
      verdade" antes de adicionar mais leitores):
      - Feedback visual durante o hold do botão (barra de progresso).
      - Não descartar cliques na janela de animação de transição (~240ms).
      - Documentar no README o mapeamento real de botões por board.

## Fase 3 — Migração gradual

- [ ] Ao final, um único firmware cobrindo AtomS3 / StickS3 / CardPuter
      com leitura EMV, FIDO2, e os dois chips NFC/RFID suportados.

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

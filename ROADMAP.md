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

- [x] **Feedback visual durante o hold**: barra fina (3px) no rodapé da
      tela, desenhada direto em `M5.Display` a cada tick de `loop()`
      (`drawHoldProgressBar()`), independente do que `drawScreen()` esteja
      fazendo naquele momento — enche proporcionalmente a `kHoldPressMs`
      enquanto o botão principal (`mainButtonIsPressed()`) estiver pressionado
      e some no release ou no latch do hold.
      **Escopo desta primeira passada**: cobre o hold do botão principal
      (`hold_enter`, `hold_connect`, `hold_dumps`, `hold_go_home`, etc. —
      é a maioria dos 8 pontos de hold no `main.cpp`, todos no mesmo botão).
      **Não cobre ainda**: o hold do botão secundário de "voltar/cancelar"
      (`nav_back`/`key_nav.cancel`, que no StickS3/StickC Plus é um botão
      físico diferente — `BtnPWR`/`BtnB` vs `BtnA`). Dá pra estender depois
      com uma segunda barra ou cor diferente se fizer falta.
      Desativado no M5Paper de propósito — refresh e-ink é lento/flicker
      demais pra overlay por tick, e o hold lá já é generoso (500ms).
- [x] **Não descartar cliques na janela de animação de transição (~240ms)**:
      achada a causa raiz — o bloco `if (in_home && home_anim_active)` no
      `loop()` fazia `drawScreen(); delay(10); return;` incondicional
      enquanto a animação do carrossel da home estava rodando. Como
      `wasClicked()`/`wasReleased()` são *one-shot* (a borda já foi
      consumida pelo `M5.update()` daquele mesmo tick, antes desse bloco),
      qualquer clique/hold que caísse ali virava `return` sem nunca chegar
      no bloco de navegação da home (que já sabe re-direcionar uma animação
      em andamento — ver os `home_anim_to != to_page` mais abaixo). Corrigido
      calculando `has_pending_home_input` (clique, prev/next/back do
      teclado, confirm, ou hold em andamento) e só tomando o caminho rápido
      "só anima, nada pra tratar" quando não há nada pendente — caso
      contrário o fluxo cai pro tratamento normal de navegação da home.
- [x] Documentar no README o mapeamento real de botões por board — tabela
      com botão de clique/hold, threshold do hold (320-700ms, varia por
      board) e botão secundário de cada um, lida direto do código
      (`mainButtonClicked()`/`readKeyNavState()`/`kHoldPressMs`), não
      inventada de memória.

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

## Fase 2 — Leitores + usabilidade (escopo redefinido em 06/08) ✅ CONCLUÍDA

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

      **Dossiê (igual ao `cards_db` do ChaosID, adaptado pra EMV)** —
      `src/emv_db.h`/`.cpp`: em vez de só imprimir o PAN, o `tryGPO()`
      também extrai o AIP (Application Interchange Profile, tag `82` — no
      Format 2 via TLV junto do Track2; no Format 1 são os 2 bytes logo
      após tag+length, sem TLV) e usa o bit de autenticação de dados
      offline que o **cartão de verdade declarou** (`CDA` > `DDA` > `SDA`
      > nenhum, prioridade conforme EMV Book 3) pra escolher um perfil de
      referência: método de autenticação, criptografia, vetor de ataque
      público conhecido, ano, nível de risco e notas. Diferente do
      `cards_db` do ChaosID (que mapeia *nome de protocolo* → perfil
      estático), aqui a chave é um dado lido na hora do cartão (AIP), não
      só a bandeira — então o dossiê reflete o que aquele cartão
      específico declarou, não um chute genérico por bandeira. Perfis
      documentados: SDA (replay de assinatura estática, conhecido desde
      os anos 2000), DDA (vulnerável ao MITM de Murdoch et al., "Chip and
      PIN is Broken", IEEE S&P 2010), CDA (fecha essa lacuna, sem ataque
      público conhecido), e "nenhum" (cartão sem autenticação offline —
      não necessariamente frágil, só fora do que esse dossiê classifica).

      **Dossiê não fica só no EMV** — `src/cards_db.h`/`.cpp` porta a
      seção HF do `cards_db` do ChaosID (as entradas LF — EM4100, HID
      Prox, Indala, AWID, T5577 — não se aplicam aqui: os módulos Grove
      são 13.56MHz, sem caminho de 125kHz) pro **Reader comum**, mapeando
      o protocolo que `protocolFull()`/`readWs1850sCard()` já produzem
      (`MFC1K`, `MFC4K`, `MFUL`, `NTAG*`, `DESFire`, `FeliCa`, `ISO15693`,
      `ISO14443B`) pro mesmo formato de dossiê (uso típico, criptografia,
      vetor de ataque, ano, risco). Validado em `src/ws1850s_test.cpp`
      (MIFARE Classic/Ultralight real); os protocolos que só o ST25R3916
      lê (FeliCa, ISO15693, ISO14443B, DESFire) ainda não foram testados
      com hardware real nesse dossiê — só portados da tabela do ChaosID.

      **Atualização**: portado também pra tela real do `main.cpp`
      (página `Reader`, não só o sketch isolado) — card.protocol já é a
      mesma string em qualquer um dos dois backends, então o dossiê
      funciona igual com `ST25R3916`/"Unit NFC" ou com o driver
      secundário WS1850S, sem depender de ISO14443-4/APDU como o EMV
      precisa. Reduz a área do `drawReaderPixelCard()` (que já existia)
      pra metade de cima quando tem cartão, e desenha uso típico /
      criptografia / vetor de ataque + risco embaixo. Fora do AtomS3 (tela
      quadrada pequena, sem espaço sobrando — fica só com UID/tipo, como
      antes).

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
- [x] **Leitor FIDO2 NFC (read-only)** — validado em ambiente isolado
      (`src/fido2_test.cpp`, ambiente `fido2-test`): `RATS` → `SELECT`
      no applet FIDO (`A0000006472F0001`) → `authenticatorGetInfo`
      (CTAP2, comando `0x04`) sobre o mesmo `sendIBlock()` (WTX +
      chaining) já validado no EMV.
      - **Correção importante**: o AID usado para SELECT é
        `A0000006472F0001` (registrado pela FIDO Alliance para
        CTAP/U2F sobre NFC). Uma nota de planejamento anterior deste
        repo tinha citado `D2760000850101` por engano — esse é o AID
        da **NDEF Tag Application** (ISO 7816-4 genérico de tags tipo
        4), não tem nada a ver com FIDO. Corrigido antes de qualquer
        hardware real ser testado contra o AID errado.
      - **Importante (branding)**: ferramenta separada, em código e em
        branding, da pesquisa **Chaos IAM** — nenhum tipo, nome ou
        metadado compartilhado com aquele projeto.
      - **Atualização — reconhecimento/fingerprinting (não só dump
        bruto)**: portado o mesmo esquema do `fido2-nfc-scanner`
        (Flipper FAP, mesmo autor, mesma pesquisa Café com Solda,
        GPLv3) pra este sketch: `detectVendorString()` casa a string de
        fabricante (`"YubiKey"`, `"uTrust"`) em qualquer lugar dos
        bytes crus do ATS (simplificação deliberada — não isola a
        janela exata dos *historical bytes* do ISO7816, só confirma a
        presença da string), e `fido2CborParseGetInfo()` é um parser de
        CBOR mínimo (porte 1:1 de `cbor_parser.c`) que extrai do
        `authenticatorGetInfo` sem PIN/toque: AAGUID, versões CTAP2,
        extensions, options (`rk`/`clientPin`/`largeBlobs`/`alwaysUv`/
        `plat`), `maxMsgSize`, `minPINLength` e firmware version.
        `printFido2Dossier()` imprime isso nas mesmas quatro seções do
        app Flipper (`[DEVICE]`/`[SECURITY]`/`[CAPABILITIES]`/
        `[EXTENSIONS]`) via Serial. Mesma descoberta já divulgada pra
        Yubico/Identiv/FIDO Alliance (CVE pendente MITRE
        CAN-2026-2038918): nada disso exige PIN, toque ou qualquer
        autenticação do token.
      - **Validado em hardware real** contra uma YubiKey 5C NFC (FW
        5.7.4): vendor string `YubiKey` reconhecida no ATS, AAGUID e
        firmware version extraídos, PIN `SET` (min. 4 dígitos),
        resident keys, `largeBlobs`, `maxMsgSize=1536`, as 4 versões
        CTAP2 (`U2F_V2`/`FIDO_2_0`/`FIDO_2_1_PRE`/`FIDO_2_1`) e as 5
        extensions (`credProtect`/`hmac-secret`/`largeBlobKey`/
        `credBlob`/`minPinLength`) — tudo sem PIN e sem toque no token.
      - **Bug encontrado e corrigido no primeiro teste real**: os
        tamanhos usados pra acumular a resposta do `GetInfo`
        (`respLen`/`respCapacity`/`chunkLen`/`fullLen`/`dataLen`) eram
        `byte` (máx. 255) e os buffers (`resp[200]`/`full[400]`)
        pequenos demais — o `GetInfo` de uma YubiKey 5 real passa fácil
        de 255 bytes já remontado via I-block chaining (lista grande de
        versões/extensions/algoritmos), então a leitura abortava com
        "Response exceeds buffer capacity" bem antes de terminar.
        Corrigido: tamanhos passaram pra `uint16_t` em todo o caminho
        (`sendIBlock()`/`tryGetInfo()`/`printHex()`) e os buffers foram
        pra 512/600 bytes (mesma margem do `FIDO2_MAX_RESPONSE` do
        `fido2-nfc-scanner`).
- [x] **Integrar o leitor EMV no firmware principal** — portado de
      `src/emv_test.cpp` pra uma página nova em `main.cpp`
      (`MenuPage::EmvReader`), seguindo o mesmo padrão Core-0/Core-1 do
      Reader/Piano (worker faz o RATS→PPSE→SELECT→GPO, que pode levar
      1-3s, sem travar a UI). Só aparece no carrossel da home do
      M5StickS3 (`APP_TARGET_STICKS3`) — é o único board com o driver
      WS1850S de fato ligado hoje; nem entra no `kHomeOrderNfcUnit` do
      ST25R3916, que não faz APDU ISO14443-4. Número completo na tela
      (decisão explícita: sem máscara).
- [ ] **Usabilidade da UI** (itens que já estavam na Fase 1, consolidados
      aqui por fazerem parte do mesmo esforço de "deixar usável de
      verdade" antes de adicionar mais leitores):
      - [x] Feedback visual durante o hold do botão (barra de progresso) —
        ver detalhes na Fase 1.
      - [x] Não descartar cliques na janela de animação de transição
        (~240ms) — ver detalhes na Fase 1.
      - [x] Documentar no README o mapeamento real de botões por board.

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

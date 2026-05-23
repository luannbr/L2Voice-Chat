# Usando o L2Voice-Chat

Depois de compilar os três componentes ([BUILDING.pt-BR.md](BUILDING.pt-BR.md)),
este guia te leva pela instalação, configuração e uso end-to-end do
sistema.

> 🇺🇸 English version: [USAGE.md](USAGE.md)

---

## Resumo da arquitetura

```
   ┌──────────────────────┐                ┌─────────────────────┐
   │  Cliente L2 + DLL    │ ── áudio UDP ─►│                     │
   │  l2voice (Win32)     │ ◄─── WS ctrl ──┤   voice-server      │
   └──────────────────────┘                │   (binário Go)      │
              ▲                            │                     │
              │ matching de                                      │
              │ porta TCP                  └─────────────────────┘
              ▼                                        ▲
   ┌──────────────────────┐                            │
   │   Game Server L2J    │ ── eventos/RPC via WS ─────┘
   │   + l2voice-bridge   │
   └──────────────────────┘
```

Três caixas pra subir: GS + JAR da bridge, voice-server, e o cliente
L2 com a DLL.

---

## 1. Instalar o voice-server

O voice-server é um binário único **sem dependências de runtime**.
Coloque onde for conveniente — a maioria começa rodando na mesma
máquina do GS, depois move pra uma VPS de baixo ping.

### Rodar

```bash
./voice-server.exe -udp :17666 -ws :17667
```

Pra operação contínua, envelope em `nssm` (serviço Windows) ou unit
systemd (Linux). Exemplo de `start-voice-server.cmd` mínimo:

```bat
@echo off
voice-server.exe -udp :17666 -ws :17667 > server.log 2>&1
```

### Firewall

Abra as portas `17666/udp` e `17667/tcp` no host do voice-server. No
Windows:

```powershell
New-NetFirewallRule -DisplayName "voice-server UDP" -Direction Inbound -Protocol UDP -LocalPort 17666 -Action Allow
New-NetFirewallRule -DisplayName "voice-server WS"  -Direction Inbound -Protocol TCP -LocalPort 17667 -Action Allow
```

---

## 2. Configurar a bridge L2J

Você já buildou e copiou `l2voice-bridge-0.1.0.jar` pro GS em
[BUILDING.pt-BR.md §3.3](BUILDING.pt-BR.md). Agora configure:

`gameserver/config/l2voice.properties`:

```properties
# Lista separada por vírgula com os endpoints dos voice-servers. A
# bridge se conecta em todos e distribui os eventos. Os players
# auto-escolhem o mais próximo via ws_url no voice.ini deles.
l2voice.voice_server.urls = ws://127.0.0.1:17667/bridge

# Master switch. Coloca false pra desativar o sistema de voz inteiro
# sem precisar remover o JAR.
l2voice.enabled = true

# Segredo HMAC opcional pro auth legado baseado em token. Não
# precisa com a resolução padrão de identidade por porta TCP.
l2voice.hmac.secret = REPLACE-WITH-A-LONG-RANDOM-STRING
```

Reinicie o GS. Confirme no log:

```
[VoiceBridge] 1 voice-server link(s) started
[VoiceBridge] voice-link connected to ws://127.0.0.1:17667/bridge
[VoiceBridge] voice-link: rtt=2ms
```

Se aparecer `voice-link disconnected (will retry)`, o voice-server
está inalcançável — verifique firewall e se o voice-server está
rodando.

---

## 3. Instalar a DLL do cliente

A DLL é **side-loaded** no cliente L2 via o método de hijack do IAT
da Engine.dll. Isso evita modificar `L2.exe` (que tem proteção
Themida).

### Passo a passo

1. Copie `l2voice.dll` pro diretório do seu cliente L2 (ao lado de
   `L2.exe` e `Engine.dll`).
2. Garanta que seu injetor de IAT na Engine.dll carrega o
   `l2voice.dll`. Se você já usa um loader custom (ex: pra
   `l2ui.dll` / AutoLogin), adicione `l2voice.dll` à lista. As duas
   DLLs convivem.
3. Crie o `voice.ini` ao lado da DLL (próxima seção).

> ⚠️ **Não modifique `L2.exe`**. O Themida corrompe o D3D9 device
> recreate em L2.exe modificado por CFF. O método do IAT da
> Engine.dll evita isso completamente.

### `voice.ini` — referência completa

```ini
[voice]
# URL WebSocket do voice-server. Use o IP/host público se o
# voice-server estiver em VPS.
ws_url = ws://127.0.0.1:17667/ws

# Master enable/disable. Em 0 mantém a DLL carregada mas mudo.
enabled = 1

# Auto-conectar no attach. Em 0 precisa chamar voice::Init() manual.
auto_connect = 1

# Exige que o L2 esteja em foco antes de transmitir (evita TX
# acidental quando alt-tabado).
require_focus = 1

# Virtual key codes do push-to-talk. Defaults são VK_H/B/N/M para
# proximidade/party/clan/ally — veja a tabela VK_* da Microsoft.
ptt_proximity = 72   ; VK_H
ptt_party     = 66   ; VK_B
ptt_clan      = 78   ; VK_N
ptt_ally      = 77   ; VK_M

# Transmissão always-on em proximidade (sem precisar PTT). Use com
# cautela.
always_on = 0

# Falloff de proximidade (centímetros em unidades do mundo L2).
min_dist_cm = 500
max_dist_cm = 2500

# Volume master 0..200 (= 0.0..2.0). 100 = unity.
master_volume = 100

# Preferências por canal (0=desligado, 1=ligado).
ch_enabled_0 = 1   ; proximidade
ch_enabled_1 = 1   ; party
ch_enabled_2 = 1   ; clan
ch_enabled_3 = 1   ; ally

# Volume por canal 0..200.
ch_volume_0 = 100
ch_volume_1 = 100
ch_volume_2 = 100
ch_volume_3 = 100

# Canal de TX ativo pro PTT principal (e always_on).
# 0=Proximidade, 1=Party, 2=Clan, 3=Ally, 4=CC.
active_tx_channel = 0

# APM (módulo de processamento de áudio). Tudo ligado = melhor
# qualidade.
apm_aec = 1   ; cancelador de eco Speex DSP
apm_hpf = 1   ; high-pass 80 Hz
apm_ns  = 1   ; supressão neural de ruído RNNoise
apm_agc = 1   ; controle automático de ganho
```

---

## 4. Primeira chamada

1. Inicie o voice-server.
2. Inicie o GS (com a bridge configurada).
3. Abra o cliente L2 (com a DLL injetada) e entre no mundo.
4. Abra o overlay — o painel in-game aparece automaticamente quando
   a DLL detecta que a janela do L2 tem foco.
5. Confirme que o overlay mostra `connected` (bolinha verde) e um
   session ID.
6. Segure a **tecla PTT de proximidade** (default `H`) e fale.
   Quem estiver perto de você (até `max_dist_cm`) no mesmo
   instanceId te ouve.
7. Pra voz em grupo: segure a tecla PTT de **party/clan/ally**
   (default `B/N/M`) pra falar nesse canal.

### Controles do overlay

- **Abas de canal** — toggle do canal e ajuste de volume por canal
- **Lista de speakers** — mostra quem está falando agora, com mute e slider de volume por fonte
- **Settings** — volume master, remap de PTT, toggle de always-on

### Indicadores

| Ícone | Significado |
|-------|-------------|
| 🎤 (dourado) | Ocioso — capturando áudio mas não transmitindo |
| 🎤 (vermelho) | Transmitindo (PTT segurado ou always_on) |
| 🔇 | Dispositivo de captura indisponível ou bloqueado |

---

## 5. Deploy multi-VPS

Pra ping mínimo por região, rode um voice-server por região e liste
todos na config da bridge. A bridge distribui os eventos pra todos
os voice-servers; cada cliente conecta no que o `voice.ini` dele
aponta.

Exemplo:

```
gameserver/config/l2voice.properties:
  l2voice.voice_server.urls = ws://br.voice.example.com:17667/bridge,ws://us.voice.example.com:17667/bridge

voice.ini do cliente (player BR):
  ws_url = ws://br.voice.example.com:17667/ws

voice.ini do cliente (player US):
  ws_url = ws://us.voice.example.com:17667/ws
```

Players em voice-servers diferentes **não se ouvem ainda** —
federação entre voice-servers está no roadmap. Hoje, BR↔US só
funciona se ambos estiverem no mesmo voice-server.

---

## 6. Resolução de problemas

### O overlay nunca aparece

- Confirme que `l2voice.dll` está na pasta do cliente.
- Confirme que seu injetor de IAT está carregando ela (veja no
  Process Monitor o evento de DLL load ao iniciar o L2).
- Confira a saída de debug do Visual Studio (DebugView++) procurando
  linhas `[l2voice] ...`.

### Overlay mostra `disconnected`

- O voice-server não está rodando, ou o `ws_url` no `voice.ini` está errado.
- Firewall bloqueando a porta 17667.

### Auth nunca completa (overlay mostra `connected` mas `player_id=0`)

- A bridge não está conversando com o voice-server.
- Confirme que o log do GS mostra `voice-link connected`.
- Se o GS e o cliente L2 estão na mesma máquina, antes isso falhava
  por mismatch de IP loopback — corrigido nas versões atuais (a
  bridge agora aceita loopback host com match só de porta).

### Você se ouve nos outros clientes (multibox)

- O voice-server faz mute por IP igual por default. Pra testar com
  múltiplos clientes no mesmo PC, reinicie com `-multibox-mute=false`.
- Em produção, mantenha o same-IP mute ligado — evita feedback loop
  de multibox.

### Eco na chamada (o outro lado ouve a própria voz de volta)

- O dispositivo de captura está pegando os alto-falantes (sem fone).
- O AEC deveria cobrir isso — verifique `apm_aec = 1` no voice.ini.
- Reverb pesado ou volume > ~70% pode passar do tail de 100ms do
  Speex. Tente baixar o volume de saída ou use fone.

### Estática / dropouts durante raids

- Perda alta de pacote no caminho UDP. Verifique no log do
  `voice-service` warnings de `dropped frames`.
- Causa mais comum é saturação de upstream do usuário. A voz consome
  ~25 kbps por speaker ativo — normalmente irrelevante, a menos que
  o upload do usuário esteja saturado por outra coisa.

---

## Próximos passos

- 📖 [BUILDING.pt-BR.md](BUILDING.pt-BR.md) — compilar do zero
- 📖 [protocol.md](protocol.md) — referência do formato wire
- 📖 [DESIGN.pt-BR.md](DESIGN.pt-BR.md) — brief de design original

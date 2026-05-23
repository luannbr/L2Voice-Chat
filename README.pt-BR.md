# L2Voice-Chat

Voice chat in-game para servidores privados de Lineage II. Três canais
— **Proximidade** (3D posicional), **Party**, **Clan/Ally** —
integrados ao cliente do L2 via uma DLL side-loaded, mais um pequeno
servidor Go de relay e uma bridge Java para game servers L2J.

**🇺🇸 English version:** [README.md](README.md)

---

## ⚠️ Aviso legal

Este projeto **não é afiliado, endossado nem patrocinado pela
NCSoft**. "Lineage II" é marca registrada da NCSoft Corporation.

Este software se destina **exclusivamente a uso em servidores
privados** que você opere ou esteja autorizado a participar. Usar em
servidores oficiais da NCSoft pode violar os Termos de Serviço deles.
Os autores não se responsabilizam por contas, personagens ou ações de
terceiros usando este software.

O repositório **não contém assets do jogo protegidos por copyright** —
as texturas de UI referenciadas pela flag opcional `VOICE_L2_THEME`
não são distribuídas e devem ser fornecidas pelo próprio usuário a
partir de um cliente que ele possua.

Use por sua conta e risco.

---

## Visão geral

Pipeline em resumo:

```
┌────────────────────┐         ┌──────────────────────┐         ┌──────────────┐
│  Cliente L2 + DLL  │◄───────►│  voice-server (Go)   │◄───────►│  L2J bridge  │
│  l2voice.dll       │  UDP    │  SFU + mix espacial  │   WS    │  (Java 17)   │
│  (C++17, Win32)    │  áudio  │  :17666 udp          │ eventos │  Maven mod   │
│                    │  WS     │  :17667 ws           │ + RPC   │              │
│                    │  ctrl   │                      │         │              │
└────────────────────┘         └──────────────────────┘         └──────────────┘
```

- **DLL do cliente** — captura via WASAPI, roda AEC (Speex DSP) +
  NS (RNNoise) + AGC + HPF, codifica Opus, envia por UDP. Overlay
  ImGui pros controles de canal/PTT/volume.
- **voice-server** — binário Go único. Roteamento autoritativo, cálculo
  de proximidade, mixdown por canal. Plano de controle WebSocket;
  plano de áudio UDP.
- **l2j-bridge** — módulo Maven plugado em um GS L2J. Resolve a
  identidade do player via snapshot de portas TCP, distribui eventos
  party/clan/ally pro voice-server e responde queries RPC whoami/name.

Os três componentes conversam por protocolos documentados — veja
[`docs/protocol.md`](docs/protocol.md) pro formato wire.

## Features

- **Três canais de voz.** Proximidade (posicional), Party (grupo
  fechado), Clan/Ally (global). Prioridade do PTT: party > clan >
  ally > proximidade.
- **Cadeia completa de processamento de áudio.** AEC → HPF → NS →
  AGC. Corta eco em setups dual-PC e ruído de teclado/mouse durante
  raids.
- **Sem protocolo de identidade no cliente.** A DLL não carrega
  token. A identidade é resolvida no server-side por matching das
  portas TCP de origem da DLL contra os sockets aceitos pelo GS —
  funciona com qualquer fork L2J sem precisar de hooks do server
  além do módulo bridge.
- **Pronto pra multi-VPS.** A bridge distribui eventos pra N
  voice-servers em paralelo; clientes conectam no mais próximo via
  URL.
- **Zero GPL no runtime.** Tudo ships sob licenças permissivas
  (MIT/BSD), incluindo as deps nativas bundle.

## Stack tecnológica

| Componente | Biblioteca | Licença |
|------------|------------|---------|
| Captura/playback | [miniaudio](https://github.com/mackron/miniaudio) | MIT/Unlicense |
| Codec Opus | [libopus](https://opus-codec.org/) | BSD |
| Cancelamento de eco | [Speex DSP](https://github.com/xiph/speexdsp) | BSD |
| Supressão de ruído | [RNNoise](https://github.com/xiph/rnnoise) (fork cpuimage p/ MSVC) | BSD |
| WebSocket | [IXWebSocket](https://github.com/machinezone/IXWebSocket) | BSD |
| Hooking | [MinHook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause |
| Overlay | [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| voice-server | gorilla/websocket, redis/go-redis, stdlib net | MIT/BSD |
| l2j-bridge | Jedis (cliente Redis) | MIT |

## Quick start

Se você quer só rodar localmente:

```bash
# 1. Buildar o voice-server (precisa Go 1.22+)
cd voice-service && go mod tidy && go build -o voice-server.exe ./cmd/voice-server
./voice-server.exe -udp :17666 -ws :17667

# 2. Buildar a DLL (precisa VS2022 + CMake 3.20+)
cd client
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
# → client/build/Release/l2voice.dll

# 3. Colocar a DLL ao lado do cliente L2 + criar voice.ini
#    (veja docs/USAGE.pt-BR.md pro voice.ini completo)

# 4. Buildar o JAR da bridge L2J (precisa JDK 17 + Maven + JARs do seu server)
cd l2j-bridge && mvn package
```

Guias passo-a-passo completos:

- 🇧🇷 [**docs/BUILDING.pt-BR.md**](docs/BUILDING.pt-BR.md) — compilar os três componentes
- 🇧🇷 [**docs/USAGE.pt-BR.md**](docs/USAGE.pt-BR.md) — instalar, configurar, operar
- 🇺🇸 [**docs/BUILDING.md**](docs/BUILDING.md) — build guide (EN)
- 🇺🇸 [**docs/USAGE.md**](docs/USAGE.md) — usage guide (EN)

## Compatibilidade

| Cliente L2 | Status |
|------------|--------|
| Essence 542 SamuraiCrow (EU) | ✅ verificado |
| Outras builds Essence | ⚠️ provável — offsets do engine podem mudar |
| Interlude | ⚠️ DLL injeta, captura de nome limitada (ver notas) |
| Outros chronicles | ❌ não testado |

| Fork L2J | Status |
|----------|--------|
| l2emuproject Essence 542 | ✅ verificado |
| Outros forks L2J Essence | ⚠️ bridge precisa adaptação mínima de API |
| L2J mainstream / aCis / etc. | ⚠️ bridge precisa port da API L2World |

## Estrutura do projeto

```
.
├── LICENSE                  MIT
├── README.md                este arquivo em inglês
├── README.pt-BR.md          versão em português
├── docs/
│   ├── protocol.md          formato wire (áudio UDP + WS controle + RPC)
│   ├── BUILDING.md          guia de build (EN)
│   ├── BUILDING.pt-BR.md    guia de compilação
│   ├── USAGE.md             guia de uso (EN)
│   ├── USAGE.pt-BR.md       guia de uso
│   └── DESIGN.pt-BR.md      brief de design original (português)
├── client/                  l2voice.dll (C++17, Win32, MSVC)
│   ├── CMakeLists.txt
│   ├── dllmain.cpp
│   └── voice/               capture/playback/codec/net/overlay/apm
├── voice-service/           voice-server (Go 1.22+)
│   ├── cmd/voice-server/
│   └── internal/
└── l2j-bridge/              módulo Maven (JDK 17)
    └── src/main/java/com/luannbr/l2voice/bridge/
```

## Status

| Fase | Descrição | Status |
|------|-----------|--------|
| 1 | Monorepo + doc de protocolo | ✅ |
| 2 | Voz por proximidade na DLL | ✅ |
| 3 | voice-service Go (proximidade + grupos) | ✅ |
| 4 | Bridge L2J (identidade, eventos, RPC) | ✅ |
| 5 | Cadeia de processamento (AEC + NS + AGC) | ✅ |
| 6 | Clan voice com modos operacionais | ✅ MVP |
| — | Casos especiais Olympiad / siege / multibox | ⏳ em progresso |

## Contribuindo

Issues e PRs bem-vindos. Antes de abrir um PR grande, abra uma issue
pra discutir o escopo. O codebase mistura comentários em inglês e
português — inglês é preferido pra código novo.

## Licença

MIT — veja [LICENSE](LICENSE).

Dependências bundle mantêm suas respectivas licenças (todas
permissivas: MIT, BSD, BSD-2-Clause). Sem código GPL no runtime.

# Compilando o L2Voice-Chat

Build passo-a-passo dos três componentes: a DLL do Windows, o
voice-server em Go e a bridge Java pro L2J.

> 🇺🇸 English version: [BUILDING.md](BUILDING.md)

---

## Pré-requisitos

| Componente | Ferramentas | Versões testadas |
|------------|-------------|------------------|
| `client/` (DLL) | Visual Studio 2022 + workload C++ Desktop, CMake ≥ 3.20, Git | VS 17.10, CMake 3.29 |
| `voice-service/` | Go ≥ 1.22 | Go 1.22.x |
| `l2j-bridge/` | JDK 17, Maven 3.9+, JAR do GameServer do seu server | OpenJDK 17.0.x, Maven 3.9.6 |

Onde baixar as ferramentas:

- Visual Studio 2022 Community → <https://visualstudio.microsoft.com/downloads/> (marque **Desenvolvimento para desktop com C++**)
- CMake → <https://cmake.org/download/>
- Go → <https://go.dev/dl/>
- JDK 17 → <https://adoptium.net/> (Temurin) ou qualquer distro OpenJDK 17
- Maven → <https://maven.apache.org/download.cgi> (ou via `choco install maven`)

Acesso à internet é obrigatório no primeiro `cmake configure` —
ele baixa Opus, miniaudio, RNNoise, Speex DSP, MinHook, IXWebSocket
e Dear ImGui via FetchContent (~5 min em uma checkout limpa, fica em
cache depois em `client/build/_deps/`).

---

## 1. Compilar a DLL do cliente (`l2voice.dll`)

O cliente L2 é **32 bits**, então a DLL precisa ser buildada pra
**Win32** (o CMakeLists recusa caso contrário).

```bat
git clone https://github.com/luannbr/L2Voice-Chat.git
cd L2Voice-Chat\client
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Saída: `client\build\Release\l2voice.dll` (~1.5 MB).

### Opcional: tema nativo do L2

O overlay pode usar texturas extraídas de um cliente do Lineage II
para visual nativo. Essas texturas são **propriedade intelectual da
NCSoft** e NÃO são distribuídas com este repo. Para ativar o tema L2:

1. Copie os PNGs do L2UI_CH3 de um cliente que você possua para uma
   pasta `l2ui_assets/` na raiz do repositório.
2. Re-rode o cmake com `-DVOICE_L2_THEME=ON`.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DVOICE_L2_THEME=ON
cmake --build build --config Release
```

Se a flag estiver OFF (default), o overlay cai no visual padrão do
ImGui. Esse é o caminho suportado no projeto open source.

### Erros comuns

| Sintoma | Solução |
|---------|---------|
| `cmake: must be built for Win32 (32-bit)` | Adicione `-A Win32` ao comando configure. |
| `Cannot find Visual Studio 17 2022` | Instale o workload **Desenvolvimento para desktop com C++**. |
| FetchContent travado por muito tempo na 1ª config | Espere ~5 min na primeira; reconfigs ficam em cache. |
| Compilador de RC falha em `l2ui_assets/...` | Você ativou `VOICE_L2_THEME` sem fornecer as texturas. Desligue a flag ou forneça. |

---

## 2. Compilar o voice-server (Go)

```bash
cd voice-service
go mod tidy
go build -o voice-server.exe ./cmd/voice-server   # Windows
# ou
go build -o voice-server ./cmd/voice-server       # Linux/macOS
```

Saída: `voice-service/voice-server[.exe]` (~12 MB, binário estático,
sem dependências de runtime).

### Smoke test (sem L2J)

```bash
./voice-server.exe -udp :17666 -ws :17667
```

Você deve ver:

```
voice-service starting (udp=:17666 ws=:17667)
WS listener ready on :17667
UDP listener ready on :17666
```

O server agora está pronto pra aceitar conexões da DLL e eventos da
bridge.

### Flags úteis

| Flag | Função |
|------|--------|
| `-udp :17666` | Listener UDP para os pacotes Opus de áudio |
| `-ws :17667` | Listener WS (controle + bridge) |
| `-redis 127.0.0.1:6379` | Bus Redis opcional pra eventos legados (não precisa se usar o caminho WS da bridge) |
| `-multibox-mute=false` | Desativa o mute por IP igual (útil pra autoteste com múltiplos clientes na mesma máquina) |

---

## 3. Compilar a bridge L2J (JAR)

A bridge é um módulo Maven que pluga em um GS L2J. Você **precisa do
JAR do GameServer da sua distribuição L2J** — é dependência de build
(a bridge referencia classes do L2J como `L2PcInstance`, `L2World`,
etc).

### 3.1. Instalar o JAR do seu GameServer no Maven local

Se seu fork é `l2emuproject Essence 542` e o JAR buildado está em
`H:\caminho\para\gameserver.jar`:

```bash
mvn install:install-file ^
    -Dfile="H:\caminho\para\gameserver.jar" ^
    -DgroupId=com.l2emuproject ^
    -DartifactId=gameserver ^
    -Dversion=542 ^
    -Dpackaging=jar
```

O `groupId` / `artifactId` / `version` exatos têm que casar com
`l2j-bridge/pom.xml`. Os valores default miram `l2emuproject Essence
542` — ajuste o pom se seu fork for diferente.

### 3.2. Buildar a bridge

```bash
cd l2j-bridge
mvn package
```

Saída: `l2j-bridge/target/l2voice-bridge-0.1.0.jar`.

### 3.3. Deploy no GS

Copie `l2voice-bridge-0.1.0.jar` pro diretório `libs/` (ou
equivalente) do seu GS, daí adicione na config de inicialização do
GS:

```properties
# gameserver/config/l2voice.properties
l2voice.voice_server.urls = ws://127.0.0.1:17667/bridge
l2voice.enabled           = true
```

Para roteamento multi-VPS, use lista separada por vírgula:

```properties
l2voice.voice_server.urls = ws://br.example.com:17667/bridge,ws://us.example.com:17667/bridge
```

Reinicie o GS. Você deve ver no log do GS:

```
[VoiceBridge] 1 voice-server link(s) started
[VoiceBridge] voice-link connected to ws://127.0.0.1:17667/bridge
```

---

## Compilando tudo de uma vez (PowerShell)

```powershell
# Da raiz do repo
$ErrorActionPreference = "Stop"
cmake -S client -B client/build -G "Visual Studio 17 2022" -A Win32
cmake --build client/build --config Release

cd voice-service
go mod tidy
go build -o voice-server.exe ./cmd/voice-server
cd ..

cd l2j-bridge
mvn package
cd ..
```

Tempo de build end-to-end numa máquina moderna: ~6 minutos (boa
parte é o primeiro FetchContent das deps nativas).

---

## Próximos passos

- 📖 [USAGE.pt-BR.md](USAGE.pt-BR.md) — instalar, configurar e operar o sistema
- 📖 [protocol.md](protocol.md) — referência do formato wire (pra quem vai contribuir)
- 📖 [DESIGN.pt-BR.md](DESIGN.pt-BR.md) — brief de design original

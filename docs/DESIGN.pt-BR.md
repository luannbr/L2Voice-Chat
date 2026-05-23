# Projeto: Clan System Voice — Sistema de Voz Hierárquico para Lineage 2

## Contexto

Já existe uma DLL injetada no cliente do Lineage 2 com AutoLogin + Overlay funcional (hook D3D, ImGui, leitura de memória do cliente). Esta tarefa adiciona um **sistema de voz com hierarquia de clan** ao projeto existente. Assume-se que a infraestrutura base (DLL no cliente, serviço de voz em Go, bridge L2J) já foi planejada em fases anteriores. Esta especificação cobre **apenas a camada de regras de canais e permissões**.

## Conceitos-chave

O sistema tem **4 canais** que o jogador pode ouvir/falar simultaneamente:
- `PROXIMITY` — áudio posicional 3D, jogadores próximos no mesmo `instanceId`
- `PARTY` — membros do grupo atual
- `CLAN` — membros do clan
- `ALLY` — membros da aliança (todos os clans aliados)

Cada canal tem uma aba na GUI com:
- Toggle **Ativado / Desativado** (default: ativado)
- Slider de **volume** (0–100%)
- Lista de jogadores no canal com **mute individual** e **volume individual**

## REGRAS DE NEGÓCIO (ler com atenção, são a parte mais importante)

### Regra 1 — Toggle "Desativado" por canal

Quando o jogador desativa um canal:
- Ele **não ouve mais nada** desse canal
- Ele **continua podendo falar** nesse canal (transmissão é independente da escuta)
- Exceção: **lideres/sub-lideres de clan ignoram esse toggle no canal CLAN e ALLY** (ver Regra 3)

### Regra 2 — Hierarquia do Clan

Cada clan tem:
- **1 Líder** (definido pelo L2J, leitura via bridge)
- **N Sub-líderes** (atribuídos pelo Líder dentro da nossa GUI — NÃO é o sistema de ranks nativo do L2, é controle nosso)

O Líder tem um painel "Gerenciar Sub-líderes" onde adiciona/remove membros do clan como sub-líder. Persistência via serviço de voz (banco próprio, não L2J).

Na GUI da aba CLAN, líder e sub-líderes aparecem em um **campo separado no topo** (visualmente destacado), e os membros comuns abaixo.

### Regra 3 — "Override" do líder/sub-líder

Quando um líder ou sub-líder fala no canal CLAN (ou ALLY, se houver aliança):
- O áudio dele **ignora o toggle "Desativado"** dos membros — todos ouvem mesmo que tenham desativado o canal
- O áudio **NÃO ignora**:
  - Mute individual aplicado especificamente naquele líder (cada jogador pode mutar líder X se quiser)
  - Volume individual baixado/zerado especificamente naquele líder
  
Ou seja: o jogador comum não consegue "desligar o canal inteiro" para escapar do líder, mas **consegue silenciar o líder específico** se quiser. Decisão deliberada.

### Regra 4 — Modo "Whisper" do líder

Líder e sub-líderes têm uma opção **"Whisper Mode"** na GUI:
- **Desligado (default):** fala normal — só transmite quando segura a tecla de PTT do canal
- **Ligado em PTT:** ao pressionar uma tecla designada específica do whisper (ex: `H`), a fala vai para TODOS os membros do clan, ignorando "Desativado" deles (Regra 3 se aplica)
- **Ligado em "sempre aberto":** o microfone fica aberto continuamente nesse modo whisper-clan, sem precisar segurar tecla

Whisper é um modo de transmissão, não um canal separado. Funciona no canal CLAN (e ALLY se aplicável).

### Regra 5 — Mute remoto pelo líder/sub-líder

Líder e sub-líderes podem **silenciar/dessilenciar temporariamente o microfone** de qualquer membro:
- Vale apenas nos canais **CLAN e ALLY**
- **NÃO vale para PARTY e PROXIMITY** — nesses, o membro continua transmitindo normalmente mesmo se mutado no clan
- O mute é **temporário** (sessão atual; expira no logout do alvo ou ao ser desfeito)
- O membro mutado vê na GUI um indicador "Você foi silenciado no clan por [nome]"
- Sub-líder só pode mutar membros comuns, **não pode mutar o líder nem outros sub-líderes**
- Líder pode mutar qualquer um, incluindo sub-líderes

### Regra 6 — Modos de operação do clan (PVP / SIEGE / BOSS / FARM)

Apenas líder e sub-líderes têm acesso a um seletor de **"Modo do Clan"** na GUI. Modos disponíveis:
- `NONE` (default)
- `PVP`
- `SIEGE`
- `BOSS`
- `FARM`

Quando um líder/sub-líder ativa qualquer modo diferente de NONE:
- **Todos os membros do clan + ally entram num "canal unificado de modo"** — falam e ouvem entre si livremente, sem PTT separado entre clan e ally (vira um canal só temporariamente)
- O canal de modo **sobrepõe os canais CLAN e ALLY normais** enquanto ativo
- A Regra 3 (override do líder) continua válida
- **Jogadores próximos que NÃO sejam do clan/ally NÃO OUVEM esse áudio** — mesmo estando no raio de proximity. O áudio do canal de modo é totalmente isolado do PROXIMITY.
- Apenas um modo ativo por vez por clan. Trocar modo desativa o anterior.
- Apenas quem ativou (ou um superior na hierarquia) pode desativar.

Cada modo pode ter cores/ícones diferentes na GUI para indicar visualmente o estado, mas a lógica de áudio é a mesma para os 4 modos. Os nomes (PVP/SIEGE/BOSS/FARM) são labels organizacionais.

### Regra 7 — Canal PARTY

- Todos os membros da party falam e ouvem livremente
- Continua ouvindo **CLAN e ALLY** simultaneamente (se membro deles)
- Continua ouvindo **PROXIMITY** simultaneamente
- Party não tem hierarquia, não tem override, não tem modo. É plano.
- Se um modo de clan está ativo (Regra 6), o áudio do modo continua sendo ouvido em paralelo à party. Party não é silenciada por modo de clan.

### Regra 8 — Canal PROXIMITY

- Áudio posicional 3D normal, com atenuação por distância
- Respeita `instanceId` (instâncias separadas não se ouvem mesmo na mesma coordenada)
- **NÃO é afetado** por mute remoto do clan (Regra 5)
- **NÃO ouve** áudio de modo de clan (Regra 6) — terceiros próximos não escapam para o canal isolado

## Matriz de decisão de roteamento (para o serviço de voz)

Para cada pacote de áudio recebido do jogador A no canal C, o serviço decide para quem rotear assim:

```
Se C == PROXIMITY:
  → todos players no mesmo instanceId, raio ≤ max_dist, que tenham PROXIMITY ativado
  → aplicar mute individual e volume individual

Se C == PARTY:
  → todos membros da party de A que tenham PARTY ativado
  → líder/sub-líder NÃO tem override aqui

Se C == CLAN (modo normal):
  → todos membros do clan de A
  → se A é líder ou sub-líder: ignorar toggle "Desativado" dos receptores
  → respeitar mute individual e volume individual de cada receptor sobre A
  → se A está mutado remotamente pelo líder → não rotear

Se C == ALLY (modo normal):
  → idem CLAN, mas escopo é todos os clans da aliança

Se modo de clan ativo (PVP/SIEGE/BOSS/FARM) e A está no clan ou ally:
  → roteia para todos membros do clan + ally, ignorando toggles
  → NÃO roteia para PROXIMITY de não-membros
  → este pacote substitui CLAN/ALLY normal enquanto modo ativo

Se A é líder/sub-líder e Whisper ativo:
  → força roteamento como CLAN com override, mesmo que A não tenha pressionado PTT do canal CLAN
  → se "sempre aberto", roteia continuamente sem PTT
```

## GUI — Estrutura das abas

```
┌─ [PROXIMITY] [PARTY] [CLAN] [ALLY] ──────────────────┐
│                                                       │
│  [✓] Ativado          Volume: [====------] 60%       │
│                                                       │
│  ┌─ Líderes / Sub-líderes ──────────────────────┐    │   (só nas abas CLAN/ALLY)
│  │ 👑 LeaderName    [🔊] [Vol:===]  [Mute]      │    │
│  │ ⭐ SubLeader1    [🔊] [Vol:===]  [Mute]      │    │
│  └──────────────────────────────────────────────┘    │
│                                                       │
│  ┌─ Membros ────────────────────────────────────┐    │
│  │   Player1        [🔊] [Vol:===]  [Mute]      │    │
│  │   Player2        [🔇] [Vol:---]  [Unmute]    │    │
│  │   ...                                         │    │
│  └──────────────────────────────────────────────┘    │
│                                                       │
│  [Painel Líder — só visível se for líder/sub]        │
│  Modo: ( ) None  ( ) PVP  ( ) Siege  (●) Boss  ( ) Farm │
│  [✓] Whisper Mode    Tecla: [H]    [ ] Sempre aberto │
│  [Gerenciar Sub-líderes]  (só líder)                 │
└───────────────────────────────────────────────────────┘
```

## Tarefas de implementação

### Fase A — Modelo de dados e estado
1. Definir estruturas no serviço de voz (Go):
   - `Player { id, clan_id, ally_id, party_id, instance_id, pos, role: MEMBER|SUB_LEADER|LEADER }`
   - `Clan { id, leader_id, sub_leaders: Set<player_id>, mode: NONE|PVP|SIEGE|BOSS|FARM, mode_set_by }`
   - `RemoteMutes { (muter_id, target_id, scope: CLAN|ALLY, expires_at) }`
   - `PlayerPrefs { channel_enabled: map<channel, bool>, channel_volume: map<channel, float>, per_player_mute: Set<player_id>, per_player_volume: map<player_id, float>, whisper_mode: NONE|PTT|ALWAYS_OPEN, whisper_key }`
2. Persistência: sub-líderes e prefs em SQLite (arquivo local do serviço). Mutes remotos só em memória (sessão).
3. Sincronizar `clan_id`, `ally_id`, `party_id`, `role` (LEADER) via Redis pub/sub do L2J bridge. Sub-leader é controle nosso, não vem do L2J.

### Fase B — Roteador
1. Implementar a matriz de decisão da seção anterior como função pura `route(packet, sender, world_state) → []receiver_id`.
2. Cobrir com testes unitários todos os casos:
   - Membro fala em CLAN normal → membros com CLAN ativado recebem
   - Líder fala em CLAN → membros com CLAN desativado também recebem
   - Líder fala em CLAN → membro que mutou o líder NÃO recebe
   - Modo SIEGE ativo + jogador comum perto de não-membro → não-membro não ouve
   - Sub-líder tenta mutar líder → operação rejeitada
   - Membro mutado remotamente fala em PARTY → outros membros da party OUVEM (mute remoto não afeta party)
3. Testes de propriedade (property-based): aplicar sequências aleatórias de eventos e checar invariantes (ex: PROXIMITY nunca recebe áudio de canal de modo).

### Fase C — Protocolo
Estender o WebSocket de controle com mensagens:
- `set_channel_enabled { channel, enabled }`
- `set_channel_volume { channel, volume }`
- `set_player_mute { target_id, muted }`
- `set_player_volume { target_id, volume }`
- `clan_promote_subleader { target_id }` (só líder)
- `clan_demote_subleader { target_id }` (só líder)
- `clan_remote_mute { target_id, muted, scope }` (só líder/sub)
- `clan_set_mode { mode }` (só líder/sub)
- `whisper_set { mode, key }` (só líder/sub)
- Server → client: `notification { type: "remotely_muted_by", from }`, `clan_mode_changed { mode, by }`, `subleader_changed { target, promoted }`

Validar autorização em CADA mensagem no servidor. Cliente não é fonte de verdade.

### Fase D — GUI na DLL
1. Estender o overlay ImGui existente com a estrutura de abas mostrada acima.
2. Mostrar/esconder painéis condicionalmente baseado em `role` do jogador local.
3. Indicador visual de modo ativo (banner no topo da tela com cor: PVP=vermelho, SIEGE=roxo, BOSS=laranja, FARM=verde).
4. Notificação toast quando o jogador é mutado remotamente.
5. Keybind do whisper configurável via GUI.

### Fase E — Bridge L2J
Confirmar que o bridge publica:
- `clan_join`, `clan_leave`, `clan_leader_change`, `ally_join`, `ally_leave`, `party_change` em tempo real

Sub-leader é nosso, não precisa de bridge.

## Restrições

- **Servidor é a fonte de verdade.** Cliente nunca decide se pode mutar ou trocar modo. Sempre valida no Go.
- **Sub-leader é nosso conceito**, separado do rank nativo do L2. Documentar isso claramente.
- **Modo de clan é exclusivo no escopo do clan**: clans diferentes podem ter modos diferentes ativos ao mesmo tempo, são isolados.
- **Não permitir bypass via cliente modificado**: todas as regras de override, mute remoto e modo são checadas no servidor. Cliente só renderiza.
- **Performance**: roteador deve resolver receivers em O(N) no número de membros do clan/ally, não O(N²). Usar índices em memória.
- **Latência adicional do roteamento**: <5ms p99.

## Como começar

1. Ler o código atual da DLL e do serviço de voz para entender:
   - Como o overlay ImGui está estruturado (onde adicionar as abas)
   - Como o WebSocket de controle está implementado
   - Como o estado de clan/party é hoje sincronizado do L2J
2. Confirmar comigo:
   - [ ] Sub-leader é controle nosso (não usa rank do L2)? **Sim, é nosso.**
   - [ ] Limite de sub-leaders por clan? Sugestão: 5. Confirmar.
   - [ ] Modo é "exclusivo por clan" ou pode coexistir com modo da aliança? Sugestão: cada clan tem seu modo independente, e quando há ally, apenas o líder do clan principal define o modo unificado. Confirmar regra.
   - [ ] Whisper "sempre aberto" tem alguma proteção contra esquecer o mic ligado? (ex: timeout, indicador piscando). Sugestão: indicador grande e piscando no HUD do próprio líder.
3. Propor o esquema de dados final e a matriz de roteamento detalhada antes de codar.
4. Trabalhar fase por fase: A → B → C → D → E. Não pular.

## Critérios de "pronto"

- Todos os casos da matriz de decisão cobertos por teste unitário
- GUI renderiza corretamente em todas as combinações de role (membro / sub-líder / líder, com e sem ally)
- Bypass tentado via cliente modificado é bloqueado pelo servidor (teste manual com pacote forjado)
- Líder consegue ativar modo SIEGE e jogador não-membro próximo confirma que não ouve nada
- Membro mutado em CLAN consegue falar normalmente em PARTY e PROXIMITY
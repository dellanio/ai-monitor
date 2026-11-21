# AI Monitor — firmware v0.1

Monitor de sessões de IA para o M5Stack CoreS3 (Stack-chan), equivalente
funcional de código próprio inspirado no conceito do Miblo Desk: mostra as
sessões de IA rodando no computador, avisa quando uma precisa de você e
mostra os limites de uso — com um pet original ("Bolt", um robozinho) que
passeia pela tela quando nada precisa de atenção.

**Nome do projeto: AI Monitor.** Não usa a marca "Miblo" em lugar nenhum
da interface: firmware fechado, arte dos pets e marca do produto original
não foram copiados.

## O que a v0.1 faz

- **Tela 320×240 paisagem**: lista de até 20 sessões com cor por status
  (verde trabalhando, âmbar piscando "precisa de você", azul "terminou").
- **Toque**: tocar numa sessão abre o detalhe (fonte, perfil, o que está
  fazendo agora, tokens/custo).
- **Aprovação**: tela âmbar em tela cheia com comando + descrição e botões
  Aprovar 1x / Na sessão / Negar. **O botão só tem efeito com o Hub
  conectado** — sem Hub, mostra "sem Hub: acao nao enviada".
- **Alertas**: beep no alto-falante quando chega um pedido de permissão;
  aceno com os servos da cabeça (desativado por padrão — ver abaixo).
- **Wi-Fi / configuração**: na primeira inicialização o aparelho abre o
  AP `AIMonitor-Setup` e mostra na tela os 3 passos numerados, o nome
  do AP em destaque, a URL `192.168.4.1` e um **QR code** do portal, com
  uma faixa de status ao vivo ("Aguardando conexao...", "Conectando a
  ...", "Conectado! IP ..." ou o erro exato — "Senha errada", "Rede nao
  encontrada"). O portal lista as **redes encontradas por scan** com
  barras de sinal (toque para escolher), campo de senha com
  mostrar/ocultar, IP do Hub e botão "Salvar e conectar"; depois do envio
  ele **testa a conexão na hora** e mostra o resultado (IP ou erro com
  link para tentar de novo). Depois conecta e anuncia
  `aimonitor.local` via mDNS. Na conexão, a tela mostra o progresso
  (barra + nome da rede) e o erro específico se falhar.
- **Tela CONFIGURACOES**: pressão longa (2 s) no cabeçalho abre a tela
  de configurações — Wi-Fi atual com força do sinal, Hub (IP:porta e
  estado), IP do aparelho e os botões "Trocar rede" (abre o portal),
  "Esquecer Wi-Fi" e "Voltar". A pressão longa **não apaga mais o Wi-Fi
  direto**; o "esquecer" mora dentro de Configurações.
- **Rodapé**: Wi-Fi, estado do Hub e `CPU 34% MEM 61%` do PC (ou
  `CPU --% MEM --%` sem dados do Hub). O IP do aparelho saiu do rodapé
  e agora aparece na tela COMPUTADOR.
- **Tela COMPUTADOR**: toque no rodapé da lista abre a tela de
  estatísticas do PC, em tela cheia: cena de mesa estilizada (parede
  roxo-azulada com estrelas, quadro, janela com sol e nuvens, luminária
  acesa, laptop com código, aparelhinho amarelo e planta) com o título e o
  selo de estado (TRANQUILO / PESADO / BAIXA) dentro da cena, e embaixo um
  cartão marrom com CPU% e RAM% grandes com barras, rede (download/upload),
  GPU%, processo que mais consome CPU e bateria (quando o PC tem uma). O IP
  do aparelho fica na última linha do cartão. Qualquer toque volta à lista.
- **Modo mock**: sem Hub configurado, simula 3 sessões falsas que ciclam
  entre estados (alerta a cada ~45 s) para demonstrar a UI sem nada no PC,
  e simula as estatísticas do computador (incluindo estados PESADO e
  BAIXA de vez em quando).
- **Pet**: após 60 s sem toque, "Bolt" (robozinho original em pixel art
  32x32, desenhado em `src/pet.cpp`) passeia pela tela com uma plaquinha
  mostrando hora + limites + última tarefa terminada. Qualquer alerta tira
  o pet da tela na hora.
- **Relógio**: via NTP (America/Sao_Paulo).

## Hardware

- M5Stack CoreS3 (ESP32-S3, display IPS touch 320×240 ILI9342C,
  alto-falante, microfone, 2 servos, USB-C).
- **Servos**: os pinos estão em `src/config.h` como `SERVO_YAW_PIN` /
  `SERVO_PITCH_PIN` com o comentário `CONFIRMAR pinagem do Stack-chan
  K151` e vêm **desativados** (`-1`, `SERVO_ENABLED 0`). Não foi afirmada
  nenhuma pinagem sem conferir a documentação oficial do M5Stack. Para
  ativar o aceno, coloque os GPIOs reais e ligue `SERVO_ENABLED`.

## Como compilar e gravar

Pré-requisitos: [PlatformIO](https://platformio.org/) (`pip install
platformio`) e o cabo USB-C do Stack-chan.

```bash
cd firmware
pio run                 # compila
pio run -t upload       # grava no CoreS3
pio device monitor -b 115200   # log serial
```

Na primeira gravação, o aparelho abre o AP `AIMonitor-Setup` e mostra
na tela os passos + QR code: conecte o celular/PC nele, escaneie o QR
ou abra `http://192.168.4.1`, toque na sua rede na lista, digite a senha
e o IP do Hub (deixe o Hub vazio para o modo de demonstração) e salve —
ele testa a conexão na hora, mostra o resultado e reinicia conectado.

> **Migração de nome:** o projeto se chamava ChanMonitor; o namespace de
> Preferences mudou para `dellmonitor`, então aparelhos que já tinham
> Wi-Fi salvo na versão ChanMonitor precisam configurar o Wi-Fi de novo
> uma vez (o portal abre sozinho).

## Como apontar o Hub

O Hub (programa no computador, em `../hub/`) escuta WebSocket na porta
8790 por padrão. No portal, informe o IP do PC e a porta. O protocolo
JSON completo está em `PROTOCOL.md`, alinhado com o mapa da API do
Hermes em `../hermes-api-map.md` (status `waiting` → `needs-you`,
aprovação como request servidor→cliente, uso via `session.usage` /
`usage.bars`). O Hub também envia as estatísticas do PC (frame `host`):
CPU/RAM sempre; GPU (só NVIDIA, via `nvidia-smi`), processo top, rede
e bateria são best-effort e chegam `null` quando o PC não tem o recurso.

## Voltar ao Nous-Girl (firmware anterior)

Este firmware substitui o Nous-Girl/XiaoZhi. Para restaurar, siga
`%LOCALAPPDATA%/StackChan-Recovery/RESTAURAR.md`: grava o backup
completo da flash (16 MB, SHA-256 conferido) com `esptool`, depois de
conferir o MAC do aparelho. M5Burner/UIFlow2 **não** devolve o Nous-Girl.

## Arquivos

```
platformio.ini        PlatformIO: board m5stack-cores3, Arduino, libs
PROTOCOL.md           protocolo JSON Hub <-> dispositivo
README.md             este arquivo
src/
  main.cpp            boot, máquina de estados, loop, beep/aceno
  gfx.h/.cpp          quadro em memória (PSRAM), sem cintilação
  face.h/.cpp         rosto da Nous-Girl (GIFs embutidos) e avatar tingido
  nous_avatar.h       avatar 36x36 gerado por tools/gen_avatar.py
  config.h            constantes (nome, mDNS, servos, timeouts, cores)
  model.h/.cpp        sessões, status, aprovação, limites, estatísticas do PC
  wifi_setup.h/.cpp   portal captivo com scan de redes, QR code,
                      status ao vivo, conexão, mDNS
  hub_client.h/.cpp   WebSocket do Hub + modo mock (sessões e estatísticas)
  ui.h/.cpp           telas (lista, detalhe, aprovação, pet, COMPUTADOR,
                      CONFIGURACOES, rodapé)
  pet.h/.cpp          sprite original "Bolt" + passeio + plaquinha
  servo_head.h/.cpp   aceno da cabeça (desativado por padrão)
```

## Limites honestos da v0.1

- **Sem Hub, só o modo mock funciona.** Sem ele, as sessões e as
  estatísticas do PC são simuladas (ver `PROTOCOL.md`).
- **GPU só NVIDIA**: a tela COMPUTADOR mostra `GPU --` em PCs sem GPU
  NVIDIA ou sem `nvidia-smi`; bateria só aparece em notebooks.
- **Aprovar por toque só tem efeito com o Hub conectado.** O botão já está
  desenhado, mas sem Hub a ação é só local.
- **Sem OTA**: atualização é por cabo USB (`pio run -t upload`). OTA por
  Wi-Fi fica para uma versão futura.
- **Sem tela de cron**: os jobs chegam no protocolo, mas ainda não há tela
  própria (a lista mostra só sessões).
- **Só as 3 primeiras sessões aparecem na lista**; havendo mais, o rodapé
  avisa quantas faltam (rolagem fica para depois).
- **Servos desativados** até confirmar a pinagem do K151 (ver acima).
- **Nada foi testado no hardware real**: o build é verificado por
  compilação; o comportamento na tela, o toque e o Wi-Fi precisam de teste
  com o Stack-chan em mãos.
- **Segurança**: o aparelho nunca recebe senhas de serviços — só o estado
  das sessões via Hub na rede local. Não exponha o Hub fora da sua LAN.

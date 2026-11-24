# Monitor IoT OBD-II · Diagnóstico Preventivo e Eco-Condução

Sistema embarcado + web que lê dados do carro pela porta OBD-II com um **ESP32**, guarda tudo em **PostgreSQL** e mostra em um painel em tempo real, com alertas, gráficos e um **simulador de testes de componentes**.

Projeto de extensão universitária (PEX IV) sobre mobilidade urbana sustentável: ajudar condutores a gastar menos combustível e manutenção e a poluir menos, com diagnóstico antecipado de falhas e orientação de condução.

## Funcionalidades

- **Painel do Condutor:** faixa verde/amarela/vermelha, mensagens simples ("Temperatura acima do recomendado. Verifique o sistema de arrefecimento."), dicas e alertas recentes.
- **Análise Técnica:** valores em tempo real, 4 gráficos, histórico, consulta por período, alertas, status da comunicação e limites de referência.
- **Simulador de Testes:** carro em SVG com 8 componentes clicáveis; cada teste é gravado no banco e testes com problema geram alerta.
- **Tempo real** por WebSocket, **alertas automáticos** com controle de repetição e **modo de simulação** para usar sem o carro.
- **Transparência dos dados:** toda leitura ou teste carrega a origem `real` ou `simulado`, sempre exibida na interface. Componentes sem sensor (catalisador, injeção) são testados de forma simulada e identificada.

## Arquitetura

```
ESP32 + ELM327 (Bluetooth) --HTTP POST/Wi-Fi--> API Node.js/Express --SQL--> PostgreSQL
                                                  |  WebSocket + REST
                                                  v
                                       Painel web (3 abas)
```

## Tecnologias

ESP32 (C++/Arduino, ELMduino, Adafruit SSD1306) · Node.js, Express, `pg`, `ws` · PostgreSQL · HTML/CSS/JS puro (sem framework) · scripts `.bat` para Windows.

## Estrutura

```
server.js            API, WebSocket, regras de alerta e migração do banco
public/index.html    Painel (Condutor, Análise Técnica, Simulador)
database/schema.sql  Esquema de referência
firmware/OBD22/      Sketch do ESP32 + secrets.example.h
iniciar-obd2.bat     Inicia tudo (Windows)      parar-obd2.bat   Encerra
.env.example         Modelo de configuração
```

## Como executar

**Requisitos:** Node.js 20.12+ e PostgreSQL. O ESP32 e o computador precisam estar na mesma rede Wi-Fi.

1. Copie `.env.example` para `.env` e preencha `PG_SENHA` (senha do usuário `postgres`) e `API_KEY` (chave compartilhada com o ESP32).
2. **Windows:** dê dois cliques em `iniciar-obd2.bat`. Ele confere o Node.js, instala as dependências, inicia o PostgreSQL, sobe a API e abre `http://localhost:3000`.
   **Outros sistemas:** `npm install` e `npm start`.
3. O banco `obd2` e as tabelas são criados automaticamente.
4. Sem o carro: em **Análise técnica**, clique em **Ligar simulação**.

## Firmware (ESP32)

1. Arduino IDE com a placa **ESP32 Dev Module**. Instale as bibliotecas **ELMduino**, **Adafruit SSD1306** e **Adafruit GFX**.
2. Em `Ferramentas > Partition Scheme`, escolha **Huge APP**, pois Bluetooth e Wi-Fi juntos ocupam muito espaço.
3. Copie `firmware/OBD22/secrets.example.h` para `secrets.h` e preencha Wi-Fi, IP do computador, `API_KEY` e o MAC do ELM327.
4. Grave. Com o carro respondendo, o Monitor Serial (115200) mostra `[API] POST -> 201` a cada 3 s.

Contrato de envio (`POST /api/amostras`, cabeçalho `x-api-key`):
`{"dev":"esp32-01","rpm":2100,"vel":54,"temp":88,"tps":18,"score":82,"tensao":13.9,"dtc":"P0300"}`

## API

| Rota | Função |
|---|---|
| `POST /api/amostras` | Recebe leitura do ESP32 (exige `x-api-key`) |
| `GET /api/ultimo` | Última leitura + avaliação por sensor |
| `GET /api/amostras?limit&de&ate&origem` | Histórico, com filtro por período e origem |
| `GET /api/alertas` | Alertas recentes |
| `POST /api/testes` · `GET /api/testes` | Executa e lista testes de componentes |
| `POST /api/simulacao` | Liga/desliga o modo de simulação |
| `GET /api/status` | Estado da comunicação |
| `WS /ws` | Eventos em tempo real (amostra, alerta, teste) |

## Hardware

ESP32 (ESP-WROOM-32), adaptador OBD-II ELM327 Bluetooth, display OLED 0,96" I2C, buzzer, LEDs e regulador step-down 12V→5V. Custo estimado dos componentes: cerca de R$ 160.

## Validação e limitações

- Backend, banco, simulação, alertas e WebSocket foram validados com testes automatizados de API e de interface contra um PostgreSQL real; incluem migração de um banco no formato anterior sem perda de dados.
- O firmware foi testado no carro para leitura via ELM327; o envio por Wi-Fi com os dados do painel novo ainda precisa de validação em campo.
- A economia de combustível de 10% a 20% citada na proposta é uma **estimativa**, a ser comprovada em testes de campo.
- Não há sensor de umidade no protótipo. A leitura de DTCs pelo firmware e o login de usuários ainda não foram implementados.
- Os limites de alerta estão fixos no código. Use chaves e senhas próprias e não exponha a API à internet sem autenticação.

## Roadmap

Leitura de DTCs no firmware · limites configuráveis · autenticação · PWA e app mobile · hospedagem em nuvem · testes de campo com condutores.

## Licença e autoria

MIT (veja `LICENSE`). Desenvolvido por **Eduardo** · [LinkedIn](https://www.linkedin.com/in/breno-guara/) · [GitHub](https://github.com/azarothy)

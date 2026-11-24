const express = require("express"), cors = require("cors"), http = require("http");
const { Pool } = require("pg");
const { WebSocketServer } = require("ws");
const path = require("path");
try { process.loadEnvFile(path.join(__dirname, ".env")); } catch {}   // carrega o arquivo .env (Node >= 20.12)
if (!process.env.API_KEY || !(process.env.PG_SENHA || process.env.DATABASE_URL)) {
  console.error("Configuracao ausente: copie .env.example para .env e preencha PG_SENHA e API_KEY.");
  process.exit(1);
}

const SENHA = process.env.PG_SENHA || "", BASE = `postgres://postgres:${encodeURIComponent(SENHA)}@localhost:5432/`;
const pool = new Pool({ connectionString: process.env.DATABASE_URL || BASE + "obd2" });
async function criarBanco() {   // cria o banco obd2 se ainda nao existir
  const c = new Pool({ connectionString: BASE + "postgres" });
  const r = await c.query("SELECT 1 FROM pg_database WHERE datname='obd2'");
  if (!r.rowCount) { await c.query("CREATE DATABASE obd2"); console.log("Banco obd2 criado."); }
  await c.end();
}
const KEY = process.env.API_KEY, DEV = "esp32-01";
const app = express();
app.use(cors()); app.use(express.json()); app.use(express.static(path.join(__dirname, "public")));
const srv = http.createServer(app);
const wss = new WebSocketServer({ server: srv, path: "/ws" });
const bc = m => { const s = JSON.stringify(m); wss.clients.forEach(c => c.readyState === 1 && c.send(s)); };
const w = f => (q, r) => f(q, r).catch(e => { console.error(e.message); r.status(500).json({ erro: e.message }); });

// Componentes (tem_sensor = existe medição real no protótipo)
const COMP = [["motor","Motor",1],["arrefecimento","Arrefecimento",1],["acelerador","Borboleta / acelerador",1],
  ["bateria","Bateria e carga",1],["transmissao","Transmissão e marchas",1],["modulo","Módulo ESP32 / OBD-II",1],
  ["catalisador","Catalisador e escape",0],["injecao","Injeção eletrônica",0]];

async function migrar() {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS dispositivos(id TEXT PRIMARY KEY, veiculo TEXT, criado_em TIMESTAMPTZ DEFAULT now());
    CREATE TABLE IF NOT EXISTS amostras(id BIGSERIAL PRIMARY KEY, dispositivo TEXT NOT NULL REFERENCES dispositivos(id),
      t TIMESTAMPTZ NOT NULL DEFAULT now(), rpm INT, vel INT, temp INT, tps INT, score INT);
    CREATE INDEX IF NOT EXISTS ix_amostras ON amostras(dispositivo, t DESC);
    CREATE TABLE IF NOT EXISTS dtcs(id BIGSERIAL PRIMARY KEY, dispositivo TEXT NOT NULL REFERENCES dispositivos(id),
      t TIMESTAMPTZ DEFAULT now(), codigo TEXT NOT NULL);
    ALTER TABLE amostras ADD COLUMN IF NOT EXISTS tensao REAL;
    ALTER TABLE amostras ADD COLUMN IF NOT EXISTS origem TEXT NOT NULL DEFAULT 'real';
    CREATE TABLE IF NOT EXISTS componentes(codigo TEXT PRIMARY KEY, nome TEXT NOT NULL, tem_sensor BOOLEAN NOT NULL);
    CREATE TABLE IF NOT EXISTS alertas(id BIGSERIAL PRIMARY KEY, dispositivo TEXT NOT NULL REFERENCES dispositivos(id),
      t TIMESTAMPTZ NOT NULL DEFAULT now(), sensor TEXT NOT NULL, nivel TEXT NOT NULL, mensagem TEXT NOT NULL, origem TEXT NOT NULL DEFAULT 'real');
    CREATE TABLE IF NOT EXISTS testes(id BIGSERIAL PRIMARY KEY, dispositivo TEXT NOT NULL REFERENCES dispositivos(id),
      componente TEXT NOT NULL REFERENCES componentes(codigo), t TIMESTAMPTZ NOT NULL DEFAULT now(),
      status TEXT NOT NULL, detalhes JSONB, origem TEXT NOT NULL);
    CREATE INDEX IF NOT EXISTS ix_alertas_t ON alertas(t DESC);
    CREATE INDEX IF NOT EXISTS ix_testes_t ON testes(t DESC);`);
  for (const [c, n, s] of COMP) await pool.query("INSERT INTO componentes VALUES($1,$2,$3) ON CONFLICT DO NOTHING", [c, n, !!s]);
  await pool.query("INSERT INTO dispositivos(id,veiculo) VALUES($1,'Carro de teste') ON CONFLICT DO NOTHING", [DEV]);
}

// Regras de avaliação (valores de referência)
const nv = (v, am, ve, alto = true) => v == null ? null : alto ? (v >= ve ? "vermelho" : v >= am ? "amarelo" : "verde") : (v <= ve ? "vermelho" : v <= am ? "amarelo" : "verde");
function avaliar(a) {
  if (!a) return [];
  const o = [];
  const add = (sensor, nome, valor, nivel, msgs, dica) => nivel && o.push({ sensor, nome, valor, nivel, mensagem: msgs[nivel], dica: nivel === "verde" ? "" : dica });
  add("temp", "Temperatura do motor", a.temp + " °C", nv(a.temp, 98, 105),
    { verde: "Temperatura normal.", amarelo: "Temperatura acima do recomendado. Verifique o sistema de arrefecimento.", vermelho: "Superaquecimento! Pare em local seguro e desligue o motor." },
    "Confira o nível do líquido de arrefecimento com o motor frio.");
  add("rpm", "Rotação do motor", a.rpm + " rpm", nv(a.rpm, 4000, 5500),
    { verde: "Rotação adequada.", amarelo: "Rotação alta. Troque de marcha para economizar.", vermelho: "Rotação muito alta. Alivie o acelerador." },
    "Trocar de marcha mais cedo reduz consumo e poluição.");
  add("tps", "Acelerador", a.tps + " %", nv(a.tps, 80, 95),
    { verde: "Aceleração suave.", amarelo: "Aceleração forte. Pise com mais suavidade.", vermelho: "Aceleração muito brusca." },
    "Acelerações bruscas aumentam o consumo.");
  if (a.tensao != null && a.rpm > 500)
    add("tensao", "Bateria", Number(a.tensao).toFixed(1) + " V", a.tensao > 15 ? "vermelho" : nv(a.tensao, 13.0, 12.0, false),
      { verde: "Carga da bateria normal.", amarelo: "Carga baixa. Observe a bateria e o alternador.", vermelho: "Problema na carga da bateria. Procure uma oficina." },
      "Se o aviso continuar, leve o carro para revisão elétrica.");
  return o;
}

const cool = {};
async function salvar(b, origem) {
  const dev = b.dev || DEV;
  const r = await pool.query("INSERT INTO amostras(dispositivo,rpm,vel,temp,tps,score,tensao,origem) VALUES($1,$2,$3,$4,$5,$6,$7,$8) RETURNING *, extract(epoch from t)::int AS ts",
    [dev, b.rpm, b.vel, b.temp, b.tps, b.score, b.tensao ?? null, origem]);
  const a = r.rows[0], av = avaliar(a);
  for (const x of av) if (x.nivel !== "verde" && Date.now() - (cool[x.sensor + x.nivel] || 0) > 60000) {
    cool[x.sensor + x.nivel] = Date.now();
    const al = await pool.query("INSERT INTO alertas(dispositivo,sensor,nivel,mensagem,origem) VALUES($1,$2,$3,$4,$5) RETURNING *, extract(epoch from t)::int AS ts", [dev, x.sensor, x.nivel, x.mensagem, origem]);
    bc({ tipo: "alerta", dados: al.rows[0] });
  }
  if (b.dtc) for (const c of String(b.dtc).split(",")) if (c.trim()) await pool.query("INSERT INTO dtcs(dispositivo,codigo) VALUES($1,$2)", [dev, c.trim()]);
  bc({ tipo: "amostra", dados: { ...a, avaliacao: av } });
  return a;
}

app.post("/api/amostras", w(async (q, r) => {
  if (q.get("x-api-key") !== KEY) return r.sendStatus(401);
  await salvar(q.body, "real"); r.sendStatus(201);
}));

const filtro = q => {
  const p = [q.query.dev || DEV]; let s = "dispositivo=$1";
  if (q.query.de) { p.push(q.query.de); s += ` AND t >= $${p.length}`; }
  if (q.query.ate) { p.push(q.query.ate); s += ` AND t <= $${p.length}`; }
  if (q.query.origem) { p.push(q.query.origem); s += ` AND origem = $${p.length}`; }
  return [s, p];
};
app.get("/api/ultimo", w(async (q, r) => {
  const [s, p] = filtro(q);
  const a = (await pool.query(`SELECT *, extract(epoch from t)::int AS ts FROM amostras WHERE ${s} ORDER BY t DESC LIMIT 1`, p)).rows[0];
  const d = await pool.query("SELECT string_agg(DISTINCT codigo, ', ') AS dtc FROM dtcs WHERE dispositivo=$1 AND t > now() - interval '1 hour'", [q.query.dev || DEV]);
  r.json({ ...(a || {}), avaliacao: avaliar(a), dtc: d.rows[0].dtc });
}));
app.get("/api/amostras", w(async (q, r) => {
  const [s, p] = filtro(q); p.push(Math.min(parseInt(q.query.limit) || 60, 2000));
  r.json((await pool.query(`SELECT *, extract(epoch from t)::int AS ts FROM (SELECT * FROM amostras WHERE ${s} ORDER BY t DESC LIMIT $${p.length}) x ORDER BY t`, p)).rows);
}));
app.get("/api/alertas", w(async (q, r) =>
  r.json((await pool.query("SELECT *, extract(epoch from t)::int AS ts FROM alertas ORDER BY t DESC LIMIT $1", [Math.min(parseInt(q.query.limit) || 20, 200)])).rows)));

// Testes de componentes: real quando há sensor, simulado (identificado) quando não há
async function testar(cod, a) {
  const idade = a ? Date.now() / 1000 - a.ts : 1e9, fresco = idade < 15;
  const lv = n => n === "verde" ? "normal" : n === "amarelo" ? "atencao" : "anomalia";
  let st = "sem_dados", det = {}, origem = a && a.origem || "real";
  if (cod === "modulo") { st = idade < 10 ? "normal" : idade < 60 ? "atencao" : "anomalia"; det = { segundos_desde_ultima_leitura: Math.round(idade) }; }
  else if (cod === "catalisador" || cod === "injecao") {
    origem = "simulado"; const ok = Math.random() > 0.25;
    st = ok ? "normal" : "atencao";
    det = cod === "catalisador" ? { eficiencia_simulada_pct: ok ? 92 : 71 } : { desvio_mistura_simulado_pct: ok ? 1.2 : 6.5 };
  } else if (fresco) {
    if (cod === "motor") { st = lv(nv(a.rpm, 4000, 5500)); det = { rpm: a.rpm }; }
    if (cod === "arrefecimento") { st = lv(nv(a.temp, 98, 105)); det = { temp_c: a.temp }; }
    if (cod === "acelerador") { st = lv(nv(a.tps, 80, 95)); det = { tps_pct: a.tps }; }
    if (cod === "bateria" && a.tensao != null) { st = lv(nv(a.tensao, 13.0, 12.0, false)); det = { tensao_v: a.tensao }; }
    if (cod === "transmissao") {
      if (a.vel > 20) { const rel = Math.round(a.rpm / a.vel); st = rel > 70 ? "atencao" : "normal"; det = { rpm: a.rpm, vel: a.vel, rpm_por_kmh: rel }; }
      else { st = "sem_dados"; det = { obs: "Veículo parado ou lento: teste exige velocidade > 20 km/h" }; }
    }
  }
  return { status: st, detalhes: det, origem };
}
app.post("/api/testes", w(async (q, r) => {
  const cod = q.body.componente;
  if (!COMP.some(c => c[0] === cod)) return r.status(400).json({ erro: "componente inválido" });
  const a = (await pool.query("SELECT *, extract(epoch from t)::int AS ts FROM amostras WHERE dispositivo=$1 ORDER BY t DESC LIMIT 1", [DEV])).rows[0];
  const t = await testar(cod, a);
  const x = (await pool.query("INSERT INTO testes(dispositivo,componente,status,detalhes,origem) VALUES($1,$2,$3,$4,$5) RETURNING *, extract(epoch from t)::int AS ts", [DEV, cod, t.status, t.detalhes, t.origem])).rows[0];
  if (t.status === "atencao" || t.status === "anomalia") {
    const nome = COMP.find(c => c[0] === cod)[1];
    const al = await pool.query("INSERT INTO alertas(dispositivo,sensor,nivel,mensagem,origem) VALUES($1,$2,$3,$4,$5) RETURNING *, extract(epoch from t)::int AS ts",
      [DEV, cod, t.status === "anomalia" ? "vermelho" : "amarelo", `Teste de ${nome}: ${t.status === "anomalia" ? "anomalia detectada" : "requer atenção"}.`, t.origem]);
    bc({ tipo: "alerta", dados: al.rows[0] });
  }
  if (x.status === "atencao" || x.status === "anomalia") {   // teste com problema vira alerta
    const nome = COMP.find(c => c[0] === cod)[1], nivel = x.status === "anomalia" ? "vermelho" : "amarelo";
    const msg = "Teste de " + nome + ": " + (x.status === "anomalia" ? "anomalia detectada." : "requer atenção.") + (x.origem === "simulado" ? " (simulado)" : "");
    const al = await pool.query("INSERT INTO alertas(dispositivo,sensor,nivel,mensagem,origem) VALUES($1,$2,$3,$4,$5) RETURNING *, extract(epoch from t)::int AS ts", [DEV, cod, nivel, msg, x.origem]);
    bc({ tipo: "alerta", dados: al.rows[0] });
  }
  bc({ tipo: "teste", dados: x }); r.json(x);
}));
app.get("/api/testes", w(async (q, r) =>
  r.json((await pool.query("SELECT t.*, extract(epoch from t.t)::int AS ts, c.nome FROM testes t JOIN componentes c ON c.codigo=t.componente ORDER BY t.t DESC LIMIT $1", [Math.min(parseInt(q.query.limit) || 30, 200)])).rows)));
app.get("/api/componentes", w(async (q, r) => r.json((await pool.query("SELECT * FROM componentes ORDER BY nome")).rows)));

// Modo de simulação (dados gravados com origem = 'simulado')
let simT = null, sim = { rpm: 900, vel: 0, temp: 80 };
app.post("/api/simulacao", w(async (q, r) => {
  const on = !!q.body.ativo;
  if (on && !simT) simT = setInterval(() => {
    sim.rpm = Math.max(800, Math.min(6000, sim.rpm + Math.round(Math.random() * 1000 - 450)));
    sim.vel = Math.max(0, Math.min(120, sim.vel + Math.round(Math.random() * 14 - 6)));
    sim.temp = Math.max(75, Math.min(112, sim.temp + Math.round(Math.random() * 4 - 1.6)));
    const tps = Math.min(100, Math.round(sim.rpm / 60));
    salvar({ rpm: sim.rpm, vel: sim.vel, temp: sim.temp, tps, score: Math.max(0, 100 - Math.max(0, Math.round((sim.rpm - 3000) / 50))), tensao: +(13.2 + Math.random() * 1.2).toFixed(1) }, "simulado").catch(e => console.error(e.message));
  }, 2000);
  if (!on && simT) { clearInterval(simT); simT = null; }
  r.json({ ativo: !!simT });
}));
app.get("/api/status", w(async (q, r) => {
  const a = (await pool.query("SELECT origem, extract(epoch from t)::int AS ts FROM amostras WHERE dispositivo=$1 ORDER BY t DESC LIMIT 1", [DEV])).rows[0];
  const n = (await pool.query("SELECT count(*)::int AS n FROM amostras WHERE dispositivo=$1 AND origem='real' AND t > now() - interval '1 minute'", [DEV])).rows[0].n;
  r.json({ ultima: a || null, reais_ultimo_minuto: n, simulacao: !!simT, clientes_ws: wss.clients.size, agora: Math.round(Date.now() / 1000) });
}));

wss.on("error", () => {});   // o erro de porta ocupada é tratado abaixo
srv.on("error", e => { console.error(e.code === "EADDRINUSE" ? "Porta 3000 ocupada. Execute parar-obd2.bat e tente de novo." : e.message); process.exit(1); });
criarBanco().then(migrar).then(() => srv.listen(3000, "0.0.0.0", () => console.log("API v2 rodando na porta 3000")))
  .catch(e => { console.error("Falha na migração:", e.message); process.exit(1); });

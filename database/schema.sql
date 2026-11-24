-- Esquema de referencia. O servidor aplica isto automaticamente ao iniciar (server.js),
-- entao rodar este arquivo manualmente e opcional.
CREATE TABLE IF NOT EXISTS dispositivos (id TEXT PRIMARY KEY, veiculo TEXT, criado_em TIMESTAMPTZ DEFAULT now());
CREATE TABLE IF NOT EXISTS amostras (
  id BIGSERIAL PRIMARY KEY, dispositivo TEXT NOT NULL REFERENCES dispositivos(id),
  t TIMESTAMPTZ NOT NULL DEFAULT now(), rpm INT, vel INT, temp INT, tps INT, score INT,
  tensao REAL, origem TEXT NOT NULL DEFAULT 'real');            -- origem: 'real' | 'simulado'
CREATE INDEX IF NOT EXISTS ix_amostras ON amostras(dispositivo, t DESC);
CREATE TABLE IF NOT EXISTS dtcs (id BIGSERIAL PRIMARY KEY, dispositivo TEXT NOT NULL REFERENCES dispositivos(id),
  t TIMESTAMPTZ DEFAULT now(), codigo TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS componentes (codigo TEXT PRIMARY KEY, nome TEXT NOT NULL, tem_sensor BOOLEAN NOT NULL);
CREATE TABLE IF NOT EXISTS alertas (
  id BIGSERIAL PRIMARY KEY, dispositivo TEXT NOT NULL REFERENCES dispositivos(id),
  t TIMESTAMPTZ NOT NULL DEFAULT now(), sensor TEXT NOT NULL, nivel TEXT NOT NULL,   -- verde | amarelo | vermelho
  mensagem TEXT NOT NULL, origem TEXT NOT NULL DEFAULT 'real');
CREATE INDEX IF NOT EXISTS ix_alertas_t ON alertas(t DESC);
CREATE TABLE IF NOT EXISTS testes (
  id BIGSERIAL PRIMARY KEY, dispositivo TEXT NOT NULL REFERENCES dispositivos(id),
  componente TEXT NOT NULL REFERENCES componentes(codigo), t TIMESTAMPTZ NOT NULL DEFAULT now(),
  status TEXT NOT NULL,                                          -- normal | atencao | anomalia | sem_dados
  detalhes JSONB, origem TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS ix_testes_t ON testes(t DESC);

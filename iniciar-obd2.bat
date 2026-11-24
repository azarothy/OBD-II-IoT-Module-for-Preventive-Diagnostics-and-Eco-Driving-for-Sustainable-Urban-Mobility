@echo off
setlocal
cd /d "%~dp0"
title OBD-II - Inicializacao
if exist ".env" goto env_ok
if exist ".env.example" copy ".env.example" ".env" >nul
echo [ATENCAO] Arquivo .env criado a partir do exemplo.
echo           Defina PG_SENHA (senha do usuario postgres) e API_KEY, salve e execute este arquivo de novo.
notepad ".env"
pause
exit /b 0
:env_ok
echo ============================================
echo   Monitor OBD-II - iniciando o ambiente
echo ============================================
echo [1/5] Verificando Node.js...
where node >nul 2>&1
if errorlevel 1 goto sem_node
for /f %%v in ('node -v') do echo       Node.js %%v encontrado.
echo [2/5] Verificando dependencias...
if exist "node_modules\ws" if exist "node_modules\pg" goto deps_ok
echo       Instalando dependencias. Aguarde...
call npm install --no-audit --no-fund
if errorlevel 1 goto erro_npm
:deps_ok
echo       Dependencias OK.
echo [3/5] Verificando PostgreSQL...
powershell -NoProfile -Command "$s=Get-Service 'postgresql*' -ErrorAction SilentlyContinue; if(-not $s){exit 2}; if($s | Where-Object Status -eq 'Running'){exit 0}else{exit 1}"
if errorlevel 2 goto sem_pg
if errorlevel 1 goto iniciar_pg
echo       PostgreSQL em execucao.
goto api
:iniciar_pg
echo       PostgreSQL parado. Tentando iniciar...
powershell -NoProfile -Command "Get-Service 'postgresql*' | Start-Service" >nul 2>&1
if errorlevel 1 goto erro_pg
echo       PostgreSQL iniciado.
:api
echo [4/5] Iniciando a API e o painel...
netstat -ano | findstr ":3000 " | findstr LISTENING >nul
if not errorlevel 1 (
  echo       A porta 3000 ja esta em uso. Usando o servidor que ja esta rodando.
  goto navegador
)
start "OBD2-API (nao feche esta janela)" cmd /k node server.js
set /a n=0
:espera
set /a n+=1
powershell -NoProfile -Command "try{[void](Invoke-WebRequest http://localhost:3000/api/status -UseBasicParsing -TimeoutSec 2)}catch{exit 1}"
if not errorlevel 1 goto navegador
if %n% GEQ 20 goto erro_api
timeout /t 1 >nul
goto espera
:navegador
echo       API respondendo em http://localhost:3000
echo [5/5] Abrindo o navegador...
start "" http://localhost:3000
echo.
echo Tudo pronto. Para encerrar, execute parar-obd2.bat
timeout /t 5 >nul
exit /b 0
:sem_node
echo [ERRO] Node.js nao encontrado. Instale a versao LTS em https://nodejs.org e execute de novo.
pause
exit /b 1
:erro_npm
echo [ERRO] Falha ao instalar dependencias. Verifique a conexao com a internet.
pause
exit /b 1
:sem_pg
echo [ERRO] PostgreSQL nao encontrado. Instale em postgresql.org/download e informe a senha do usuario postgres em PG_SENHA no arquivo .env.
pause
exit /b 1
:erro_pg
echo [ERRO] Nao foi possivel iniciar o PostgreSQL. Execute este arquivo como administrador ou inicie o servico em services.msc.
pause
exit /b 1
:erro_api
echo [ERRO] A API nao respondeu em 20 segundos. Veja a janela OBD2-API:
echo   - password authentication failed: confira PG_SENHA no arquivo .env.
echo   - Porta ocupada: execute parar-obd2.bat.
pause
exit /b 1

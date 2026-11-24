@echo off
setlocal
cd /d "%~dp0"
echo Encerrando o servidor OBD-II...
set achou=0
for /f "tokens=5" %%p in ('netstat -ano ^| findstr ":3000 " ^| findstr LISTENING') do (
  taskkill /PID %%p /F /T >nul 2>&1
  set achou=1
)
if "%achou%"=="1" (echo Servidor encerrado.) else (echo Nenhum servidor estava rodando na porta 3000.)
taskkill /FI "WINDOWTITLE eq OBD2-API*" /F >nul 2>&1
set /p r=Parar tambem o PostgreSQL? (s/N): 
if /i "%r%"=="s" powershell -NoProfile -Command "Get-Service 'postgresql*' | Stop-Service" && echo PostgreSQL parado.
pause

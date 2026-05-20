@echo off
setlocal

set "ROOT_DIR=%~dp0"
set "WEB_DIR=%ROOT_DIR%web"

where node >nul 2>nul
if errorlevel 1 (
  echo Node.js was not found. Please install Node.js first.
  pause
  exit /b 1
)

where npm >nul 2>nul
if errorlevel 1 (
  echo npm was not found. Please install Node.js with npm first.
  pause
  exit /b 1
)

if not exist "%WEB_DIR%\package.json" (
  echo Web project was not found at: %WEB_DIR%
  pause
  exit /b 1
)

cd /d "%WEB_DIR%"

if not exist "node_modules" (
  echo Installing web dependencies...
  call npm install
  if errorlevel 1 (
    echo npm install failed.
    pause
    exit /b 1
  )
)

for /f %%P in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "$p=5173; while($p -lt 5200){ $c=New-Object Net.Sockets.TcpClient; try{ $c.Connect('127.0.0.1',$p); $c.Close(); $p++ } catch { $c.Close(); Write-Output $p; break } }"') do set "WEB_PORT=%%P"

if "%WEB_PORT%"=="" (
  echo No free local port was found from 5173 to 5199.
  pause
  exit /b 1
)

echo Starting Fridge Spirit web console at http://127.0.0.1:%WEB_PORT%
start "Fridge Spirit Web Console" cmd /k "cd /d ""%WEB_DIR%"" && npm run dev -- --host 127.0.0.1 --port %WEB_PORT%"
timeout /t 3 /nobreak >nul
start "" "http://127.0.0.1:%WEB_PORT%"

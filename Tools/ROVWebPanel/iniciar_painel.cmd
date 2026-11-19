@echo off
rem Inicia a ponte do painel web do ROVsim e abre o navegador.
rem Deixe esta janela aberta enquanto usar o painel; feche-a (ou Ctrl+C) para parar.
title Painel ROVsim - feche esta janela para parar
cd /d "%~dp0"
start "" http://localhost:8080
dotnet run
pause

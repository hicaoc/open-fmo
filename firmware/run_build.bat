@echo off
cmd /c "set MSYSTEM=&& C:\esp\esp-idf\export.bat && cd /d D:\work\open-fmo\firmware && idf.py build"

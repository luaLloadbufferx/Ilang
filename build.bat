@echo off
setlocal
cd /d "%~dp0"

set FLAGS=-O3 -march=native -flto
if /i "%~1"=="goto" set FLAGS=%FLAGS% -DILANG_VM_COMPUTED_GOTO

echo [1/6] Building Ilangc...
g++ -O2 -march=native -flto ./Ilangc/main.cpp -o ilangc.exe
if errorlevel 1 goto :fail

echo [2/6] Building Ilangdis...
gcc -O2 ./Ilangdis/*.c -o ilangdis.exe
if errorlevel 1 goto :fail

echo [3/6] Compiling PGO training sources...
if not exist IlangVM\pgo mkdir IlangVM\pgo
for %%F in (IlangVM\pgo_src\*.i) do (
    echo     %%F -> IlangVM\pgo\%%~nF.ic
    ilangc.exe "%%F" -o "IlangVM\pgo\%%~nF.ic"
    if errorlevel 1 goto :fail
)

echo [4/6] Building the instrumented IlangVM...
cd IlangVM
del /q *.gcda 2>nul
gcc %FLAGS% -fprofile-generate -fprofile-update=single ./main.c ./VM/*.c -o ilangvm.exe
if errorlevel 1 goto :fail

echo [5/6] Running the training programs...
for %%F in (pgo\*.ic) do (
    echo     %%F
    ilangvm.exe "%%F" >nul 2>nul
)

echo [6/6] Building the optimized IlangVM...
gcc %FLAGS% -fprofile-use -fprofile-correction -Wno-missing-profile ./main.c ./VM/*.c -o ilangvm.exe
if errorlevel 1 goto :fail

del /q *.gcda 2>nul
move ilangvm.exe ..\ilangvm.exe
cd ..
echo Done: ilangc.exe, ilangdis.exe and ilangvm.exe built successfully.
exit /b 0

:fail
cd /d "%~dp0"
echo Build failed.
exit /b 1
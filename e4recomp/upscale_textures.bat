@echo off
rem AI-upscales dumped textures 4x with Real-ESRGAN into ..\textures\replace\esrgan\.
rem The game picks them up on the next launch (file names keep the texture hash).
rem Usage: upscale_textures.bat            - the menu/title art listed below
rem        upscale_textures.bat <name.png> - one specific dumped file
rem Model: realesrgan-x4plus-anime suits clean graphics/logos; realesrgan-x4plus is the
rem general-purpose alternative if a result looks too flat.
setlocal
set ESRGAN=%~dp0..\tools\realesrgan
set DUMP=%~dp0..\textures\dump
set OUT=%~dp0..\textures\replace\esrgan
set MODEL=realesrgan-x4plus-anime
if not exist "%OUT%" mkdir "%OUT%"

if not "%~1"=="" (
  call :up "%~1"
  goto :done
)
rem Title face, XBLA logo, Q Entertainment logo, title text outline.
rem (The 1024x171 k_4_4_4_4 font sheet packs glyphs per colour channel - don't upscale it.)
call :up 640x512_k_8_8_8_8_e3ebf6f9b7b050ce.png
call :up 960x326_k_8_8_8_8_2904ecdad441ad57.png
call :up 448x448_k_8_8_8_8_0819e85e1445394c.png
call :up 1050x70_k_8_8_8_8_80ef03a33ef33db2.png
goto :done

:up
echo Upscaling %~1 ...
"%ESRGAN%\realesrgan-ncnn-vulkan.exe" -i "%DUMP%\%~1" -o "%OUT%\%~1" -n %MODEL% -s 4 -m "%ESRGAN%\models" -f png
exit /b

:done
echo Done. Replacements are in %OUT%

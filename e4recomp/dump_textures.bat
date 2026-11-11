@echo off
rem Runs the game with texture dumping on. Every texture the game loads is saved once as
rem PNG to ..\textures\dump\ (named <W>x<H>_<format>_<hash>.png). Visit the screens whose
rem images you want, then quit. Files that already exist are not rewritten.
call "%~dp0run_e4.bat" relwithdebinfo --texture_dump=true

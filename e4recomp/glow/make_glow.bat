@echo off
rem Bake neon glow into enemy / explosion / title sprites -> ..\..\textures\replace\@glow
rem Re-run after playing new stages with texture dumping on (dump_textures.bat), so newly
rem dumped sprites get their glow too. Needs Python 3 with Pillow and numpy.
python "%~dp0make_glow.py" "%~dp0..\..\textures"
pause

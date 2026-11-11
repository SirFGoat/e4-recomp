@echo off
rem Launch the E4 recomp. Usage: run_e4.bat [debug|release|relwithdebinfo] [extra args...]
setlocal
set ROOT=%~dp0
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=relwithdebinfo
set BUILD=%ROOT%out\build\win-amd64-%CONFIG%
if not exist "%BUILD%\e4.exe" (
  echo e4.exe not found in %BUILD% - build it first: cmake --build --preset win-amd64-%CONFIG%
  exit /b 1
)

rem --- Graphics -------------------------------------------------------------
rem Internal render scale over the 360's native 1280x720.
rem   6 = 7680x4320: on a 4K screen this is 2x2 supersampling (anti-aliased, sharp)
rem   3 = 3840x2160: native 4K, no anti-aliasing (lighter on the GPU)
rem   2 = 2560x1440
rem Pick a scale that is a whole multiple of your screen size; in-between values downscale unevenly.
set RES_SCALE=3
rem Anisotropic filtering: 0 off, 1=1x, 2=2x, 3=4x, 4=8x, 5=16x.
set ANISO=5
rem Post-process anti-aliasing: none, fxaa, fxaa_extreme (smooths edges but softens thin lines;
rem mostly useful with RES_SCALE=3 or lower).
set POST_AA=none

rem --- Texture replacement ----------------------------------------------------
rem Folder with "dump" (written by dump_textures.bat) and "replace" (your high-res PNGs).
rem A PNG in replace\ whose name ends in _<hash>.png replaces the texture with that hash,
rem at any resolution. Subfolders are fine.
set TEX_DIR=%ROOT%..\textures
set TEX_REPLACE=true
rem Save every new texture the game shows to TEX_DIR\dump (each one once). Turn off when done
rem collecting - it can cause brief hitches when new images first appear.
set TEX_DUMP=true

rem --- Explosion particles (E4 recomp addition) -------------------------------
rem Extra sparks that fly out of explosions, plus a shockwave echo on rings.
set PARTICLES=true
rem Sparks per explosion, 0-32.
set PARTICLE_COUNT=12
rem Neon glow baked into enemy/explosion/title sprites (textures\replace\@glow,
rem built by glow\make_glow.bat). Set to empty to turn it off.
set TEX_VARIANTS=glow
rem Full-screen bloom (glows everything, HUD included - off by default).
rem Strengths are 0-2ish; THRESHOLD is the brightness (0-1) where glow starts.
set BLOOM=false
set BLOOM_INTENSITY=2.6
set BLOOM_HAZE=1.6
set BLOOM_THRESHOLD=0.1
rem Diagnostic: log which game code draws sprites (for finding the explosions).
set SPRITE_STATS=false

rem --- Audio / sync ----------------------------------------------------------------
rem Game audio frames queued ahead of the speakers (5.3 ms each, 2-64). Fewer = sound
rem sooner after the beat; too few can crackle (try 3 or 4 if it does). The engine
rem default of 8 delayed the sound by ~43 ms; 2 is ~11 ms.
set AUDIO_QUEUE=2
rem Beat timing. The game's beat clock (beat bar, lamps, vibration, beat-timed sounds) sits
rem on the music's downbeat, but the beat is felt on the off-beat, half a beat later
rem (measured from tap-along recordings). BEAT_BEATS shifts the clock by that many beats
rem (0.5 = half a beat at every tempo; 0 = original timing), and BEAT_OFFSET adds
rem fine-tuning in ms for a particular TV/speaker setup.
set BEAT_BEATS=0.5
set BEAT_OFFSET=0
rem Music decoded only this many ms ahead of the game's mixer (0 = engine default, which
rem keeps ~130 ms decoded ahead and makes the music late).
set XMA_AHEAD=10
rem Log audio / frame / controller latency every 2 seconds.
set SYNC_MONITOR=true
rem Record music loudness and button taps with timestamps (sync_audio.csv, sync_taps.csv
rem next to e4.exe) for lining up taps with the beat.
set SYNC_RECORD=true

rem --- Leaderboards ------------------------------------------------------------
rem Your name, player ID and secret key come from the launcher (e4_launcher.ini next to
rem e4.exe): run e4_launcher.exe once and enter a name. Without them scores aren't sent.
set PLAYER_NAME=Player
set PLAYER_ID=
set PLAYER_KEY=
if exist "%BUILD%\e4_launcher.ini" for /f "usebackq tokens=1,* delims==" %%a in ("%BUILD%\e4_launcher.ini") do (
  if "%%a"=="player_name" if not "%%b"=="" set "PLAYER_NAME=%%b"
  if "%%a"=="player_id" set "PLAYER_ID=%%b"
  if "%%a"=="player_key" set "PLAYER_KEY=%%b"
)
rem Online leaderboards on/off.
set LEADERBOARDS=true
rem Report the profile as signed in to Xbox LIVE, so the game opens its online menus.
set SIGNIN_LIVE=true
rem Log every Xbox LIVE / stats call the game makes (diagnostics).
set XLIVE_TRACE=false

rem license_mask=1: report the XBLA title as purchased (otherwise it runs as the trial).
pushd "%BUILD%"
"%BUILD%\e4.exe" --game_data_root="%ROOT%..\game" --gpu_plugin=xenos --license_mask=1 ^
  --resolution_scale=%RES_SCALE% --anisotropic_override=%ANISO% --swap_post_effect=%POST_AA% ^
  --texture_dir="%TEX_DIR%" --texture_replace=%TEX_REPLACE% --texture_dump=%TEX_DUMP% ^
  --texture_variants=%TEX_VARIANTS% ^
  --e4_extra_particles=%PARTICLES% --e4_particle_count=%PARTICLE_COUNT% ^
  --bloom=%BLOOM% --bloom_intensity=%BLOOM_INTENSITY% --bloom_haze=%BLOOM_HAZE% ^
  --bloom_threshold=%BLOOM_THRESHOLD% --e4_sprite_stats=%SPRITE_STATS% ^
  --user_name="%PLAYER_NAME%" --user_xuid=%PLAYER_ID% --e4_player_token=%PLAYER_KEY% ^
  --e4_leaderboards=%LEADERBOARDS% --user_signin_live=%SIGNIN_LIVE% --xlive_trace=%XLIVE_TRACE% ^
  --audio_maxqframes=%AUDIO_QUEUE% --sync_monitor=%SYNC_MONITOR% --sync_record=%SYNC_RECORD% ^
  --e4_beat_offset_beats=%BEAT_BEATS% --e4_beat_offset_ms=%BEAT_OFFSET% --xma_decode_ahead_ms=%XMA_AHEAD% ^
  --log_file="%ROOT%e4_last_run.log" ^
  %2 %3 %4 %5 %6 %7 %8 %9
popd

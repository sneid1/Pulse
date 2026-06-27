@echo off
rem Open the PULSE FPS viewmodel workbench (bumstrum arms + Quaternius animated gun) in the
rem vendored Blender, ready to fine-tune the grip/pose. Double-click to run.
rem Rebuild the scene from the source assets:  run setup_fps_workbench.py (or re-run this after).
set "HERE=%~dp0"
"%HERE%blender-5.1.2-windows-x64\blender.exe" "%HERE%fps_workbench_pistol.blend"

# Copy this file to local.ps1 (ignored by git) and edit the paths for your PC.
# build.ps1, build-multi.ps1 and package.ps1 load local.ps1 if it exists.
$env:VCPKG_ROOT = 'C:/path/to/vcpkg'                     # a clone of https://github.com/microsoft/vcpkg
# $env:MINISKIRT_DEPLOY_DIR = 'C:/MO2/mods/MiniSkirt'    # optional: build.ps1 copies the VR build here

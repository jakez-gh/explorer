# Copies the engine's Starter Content (photographed textures, scanned rock and bush) into the project.
# It's ~600 MB and ships with every engine install, so it isn't committed.
param([string]$Engine = "C:\Users\jake\UnrealEngine\UE_5.4")

$src = Join-Path $Engine "Samples\StarterContent\Content\StarterContent"
$dst = Join-Path $PSScriptRoot "..\Content\StarterContent"
if (-not (Test-Path $src)) { throw "Starter Content not found at $src (install it with the engine's 'Starter Content' option)." }
Copy-Item $src $dst -Recurse -Force
Write-Output "Starter Content copied to $dst"

# Builds the SD card folder for the native 3DS app.
# Cover/episode images are resized to 512x256 JPEGs (power-of-two, 16:9 content squashed
# to 2:1 on purpose; the app stretches it back when drawing).
#
# Run from the project folder (the one that contains assets\):
#   powershell -ExecutionPolicy Bypass -File .\prepare_sd.ps1
#   powershell -ExecutionPolicy Bypass -File .\prepare_sd.ps1 -Videos   # also copy .ogv files
#
# Then copy the whole  sd_card\GlitchCollection  folder to the ROOT of the 3DS SD card.

param(
    [string]$Images = "assets\images",
    [string]$VideosDir = "assets\videos",
    [string]$Out = "sd_card\GlitchCollection",
    [switch]$Videos
)

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Host "ffmpeg not found on PATH." -ForegroundColor Red
    exit 1
}

$count = 0
foreach ($dir in Get-ChildItem -Path $Images -Directory) {
    $dest = Join-Path $Out $dir.Name
    New-Item -ItemType Directory -Force -Path $dest | Out-Null

    foreach ($f in Get-ChildItem -Path $dir.FullName -File |
             Where-Object { $_.Extension -match '^\.(jpg|jpeg|png)$' }) {
        $target = Join-Path $dest ($f.BaseName + ".jpg")
        & ffmpeg -hide_banner -loglevel error -y -i $f.FullName `
            -vf "scale=512:256" -pix_fmt yuvj420p -q:v 3 $target
        if ($LASTEXITCODE -eq 0) { $count++ } else { Write-Host "FAILED: $($f.FullName)" -ForegroundColor Red }
    }

    if ($Videos) {
        $vdir = Join-Path $VideosDir $dir.Name
        if (Test-Path $vdir) {
            Get-ChildItem -Path $vdir -Filter *.ogv | Where-Object { $_.Name -notlike "*.part.ogv" } |
                ForEach-Object {
                    Copy-Item $_.FullName (Join-Path $dest $_.Name) -Force
                    # also pull out the Vorbis audio as <n>.ogg (used by the audio test)
                    $ogg = Join-Path $dest ($_.BaseName + ".ogg")
                    & ffmpeg -hide_banner -loglevel error -y -i $_.FullName -vn -c:a copy $ogg
                    if ($LASTEXITCODE -ne 0) { Write-Host "audio extract FAILED: $($_.Name)" -ForegroundColor Red }
                }
        }
    }
}

Write-Host "Wrote $count images to $Out" -ForegroundColor Green
Write-Host "Copy the folder '$Out' to the root of the 3DS SD card."
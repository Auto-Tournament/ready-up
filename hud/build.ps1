# Builds the Auto Tournament HUD addon (at_hud) with the CS2 Workshop Tools resource compiler.
#
#   powershell -ExecutionPolicy Bypass -File hud\build.ps1
#   powershell -ExecutionPolicy Bypass -File hud\build.ps1 -Cs2 "D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive"
#
# Copies hud\layout\*.xml and hud\styles\*.css into content\csgo_addons\at_hud\panorama\...,
# compiles each one, and checks the .vxml_c / .vcss_c files landed in game\csgo_addons\at_hud.
# It also deletes the tool caches in game\csgo_addons\at_hud, which otherwise get packed
# into the Workshop upload. Publishing is a separate, manual step (see hud\README.md).

param(
    [string]$Cs2 = "",
    [string]$Addon = "at_hud"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-Cs2 {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { $steam = "C:\Program Files (x86)\Steam" }
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"(.+?)"' -AllMatches).Matches) {
            $libs += $m.Groups[1].Value.Replace('\\', '\')
        }
    }
    foreach ($lib in $libs) {
        $dir = Join-Path $lib "steamapps\common\Counter-Strike Global Offensive"
        if (Test-Path (Join-Path $dir "game\bin\win64\cs2.exe")) { return $dir }
    }
    return ""
}

if (-not $Cs2) { $Cs2 = Find-Cs2 }
if (-not $Cs2) { throw "Counter-Strike 2 not found. Pass -Cs2 <install folder>." }

$compiler = Join-Path $Cs2 "game\bin\win64\resourcecompiler.exe"
if (-not (Test-Path $compiler) -or -not (Test-Path (Join-Path $Cs2 "content"))) {
    throw "CS2 Workshop Tools are not installed (no resourcecompiler.exe / content folder). Steam > CS2 > Properties > DLC > Counter-Strike 2 Workshop Tools."
}

$content = Join-Path $Cs2 "content\csgo_addons\$Addon"
$game = Join-Path $Cs2 "game\csgo_addons\$Addon"
$jobs = @(
    @{ Src = "layout"; Dst = "panorama\layout\custom_game"; Ext = "xml"; Out = "vxml_c" },
    @{ Src = "styles"; Dst = "panorama\styles\custom_game"; Ext = "css"; Out = "vcss_c" }
)

Write-Output "CS2:   $Cs2"
Write-Output "Addon: $Addon"

$failed = 0
foreach ($job in $jobs) {
    $dstDir = Join-Path $content $job.Dst
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    foreach ($file in Get-ChildItem (Join-Path $here $job.Src) -Filter "*.$($job.Ext)") {
        $target = Join-Path $dstDir $file.Name
        Copy-Item $file.FullName $target -Force
        # The compiler works out the addon from the content\csgo_addons\<addon> path.
        & $compiler -i $target | Out-Host
        $out = Join-Path $game (Join-Path $job.Dst ($file.BaseName + "." + $job.Out))
        if (Test-Path $out) {
            Write-Output ("OK      {0} ({1} bytes)" -f $out, (Get-Item $out).Length)
        } else {
            Write-Output "FAILED  $out"
            $failed++
        }
    }
}

# Images: the .png and its .vtex recipe (BGRA8888: DXT5 renders pink in Panorama) go into
# panorama\images\custom_game; only the .vtex is compiled, to <name>.vtex_c.
$imgDst = Join-Path $content "panorama\images\custom_game"
New-Item -ItemType Directory -Force -Path $imgDst | Out-Null
Get-ChildItem (Join-Path $here "images") -File | Copy-Item -Destination $imgDst -Force
foreach ($file in Get-ChildItem (Join-Path $here "images") -Filter "*.vtex") {
    & $compiler -i (Join-Path $imgDst $file.Name) | Out-Host
    $out = Join-Path $game ("panorama\images\custom_game\" + $file.BaseName + ".vtex_c")
    if (Test-Path $out) {
        Write-Output ("OK      {0} ({1} bytes)" -f $out, (Get-Item $out).Length)
    } else {
        Write-Output "FAILED  $out"
        $failed++
    }
}

# Icons over CS2's own: images\icons\<name>.svg goes to panorama\images\icons\ui\<name>.svg and
# compiles to .vsvg_c at the same path as the game's file, which the mounted addon wins over
# (ct_logo_1c / t_logo_1c: the emblem between health and ammo).
$iconDst = Join-Path $content "panorama\images\icons\ui"
New-Item -ItemType Directory -Force -Path $iconDst | Out-Null
foreach ($file in Get-ChildItem (Join-Path $here "images\icons") -Filter "*.svg") {
    $target = Join-Path $iconDst $file.Name
    Copy-Item $file.FullName $target -Force
    & $compiler -i $target | Out-Host
    $out = Join-Path $game ("panorama\images\icons\ui\" + $file.BaseName + ".vsvg_c")
    if (Test-Path $out) {
        Write-Output ("OK      {0} ({1} bytes)" -f $out, (Get-Item $out).Length)
    } else {
        Write-Output "FAILED  $out"
        $failed++
    }
    # The team emblems also exist as panorama\images\icons\ct_logo / t_logo (no _1c); as CT the
    # circle still showed CS2's emblem with only the _1c file replaced (2026-11-24). Same picture.
    if ($file.BaseName -match '^(ct|t)_logo_1c$') {
        $side = $Matches[1]
        $plain = Join-Path (Join-Path $content "panorama\images\icons") ($side + "_logo.svg")
        Copy-Item $file.FullName $plain -Force
        & $compiler -i $plain | Out-Host
        $out2 = Join-Path $game ("panorama\images\icons\" + $side + "_logo.vsvg_c")
        if (Test-Path $out2) { Write-Output ("OK      {0} ({1} bytes)" -f $out2, (Get-Item $out2).Length) } else { Write-Output "FAILED  $out2"; $failed++ }
    }
    # The same ram as a team logo (mp_teamlogo_1/2 atram, set by hud.so; CS2 cuts a logo name at 7 characters): CS2 draws the team
    # logo in that circle and loads it by name when needed, so unlike the icons above (cached at
    # startup) it shows every time.
    if ($file.BaseName -eq "ct_logo_1c") {
        $teamDir = Join-Path $content "panorama\images\tournaments\teams"
        New-Item -ItemType Directory -Force -Path $teamDir | Out-Null
        $team = Join-Path $teamDir "atram.svg"
        Copy-Item $file.FullName $team -Force
        & $compiler -i $team | Out-Host
        $out3 = Join-Path $game "panorama\images\tournaments\teams\atram.vsvg_c"
        if (Test-Path $out3) { Write-Output ("OK      {0} ({1} bytes)" -f $out3, (Get-Item $out3).Length) } else { Write-Output "FAILED  $out3"; $failed++ }
    }
}

# Tool caches must not end up in the Workshop upload.
foreach ($junk in @("_bakeresourcecache", "tools_thumbnail_cache.sqlite3", "tools_thumbnail_cache.sqlite3-shm",
                    "tools_thumbnail_cache.sqlite3-wal", "tools_asset_info.bin", "_iconcache")) {
    $p = Join-Path $game $junk
    if (Test-Path $p) {
        try { Remove-Item $p -Recurse -Force -ErrorAction Stop; Write-Output "Removed $p" }
        catch { Write-Output "Could not remove $p (Workshop Tools open?). Close the tools and run again before publishing." }
    }
}

if ($failed) { throw "$failed file(s) failed to compile." }
Write-Output "Done. Publish from Workshop Tools (see hud\README.md)."

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

# Tool caches must not end up in the Workshop upload.
foreach ($junk in @("_bakeresourcecache", "tools_thumbnail_cache.sqlite3", "tools_asset_info.bin", "_iconcache")) {
    $p = Join-Path $game $junk
    if (Test-Path $p) { Remove-Item $p -Recurse -Force; Write-Output "Removed $p" }
}

if ($failed) { throw "$failed file(s) failed to compile." }
Write-Output "Done. Publish from Workshop Tools (see hud\README.md)."

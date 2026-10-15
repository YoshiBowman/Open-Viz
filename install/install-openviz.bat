@echo off
setlocal
rem Openviz installer for Windows: downloads the newest Openviz from GitHub and sets it up.
rem Double-click it. It asks before it installs anything or puts Openviz anywhere.
rem The rest of this file, after the marker line further down, is PowerShell, run by the next lines.
set "OPENVIZ_INSTALLER=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:OPENVIZ_INSTALLER); $i = $s.LastIndexOf('##OPENVIZ-' + 'POWERSHELL##'); if ($i -lt 0) { Write-Host 'This installer is incomplete: download it again.'; exit 1 }; Invoke-Expression $s.Substring($i)"
echo.
pause
exit /b
##OPENVIZ-POWERSHELL##
# The installer itself, in PowerShell; the lines above run it.
# It asks where Openviz is to go (C:\Openviz unless told otherwise), checks what Openviz needs and
# offers to install what winget can install (Python, Visual Studio, the Epic Games Launcher; each
# shows its own terms and asks for an administrator's yes itself), downloads the newest release
# from GitHub, checks it against GitHub's checksum, unpacks it, and runs Openviz's own setup
# (launcher\windows_setup.py), which builds the core and the visualizer and makes the shortcuts.
# Run it again to update: the shows, fixtures and settings in the folder's library are kept.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'    # Windows PowerShell downloads far more slowly while it draws progress
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$Repository = 'YoshiBowman/Open-Viz'
$Agent = 'Openviz-installer'

function Say([string]$Text) { Write-Host $Text }
function Step([string]$Text) { Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan }
function Ask([string]$Question, [string]$Default) {
    $answer = Read-Host $Question
    if ([string]::IsNullOrWhiteSpace($answer)) { return $Default }
    return $answer.Trim().Trim('"')
}
function YesNo([string]$Question, [bool]$Default) {
    $answer = Read-Host ($Question + $(if ($Default) { ' [Y/n]' } else { ' [y/N]' }))
    if ([string]::IsNullOrWhiteSpace($answer)) { return $Default }
    return $answer.Trim() -match '^[yY]'
}
function Stop-Install([string]$Why) { throw "Not installed: $Why" }

# Programs installed a moment ago are on the PATH Windows keeps, not yet on this window's.
function Update-Path {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}

function Find-Python {
    $candidates = @(
        @{ Exe = 'py'; Args = @('-3') },
        @{ Exe = 'python'; Args = @() },
        @{ Exe = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Launcher\py.exe'); Args = @('-3') }
    )
    foreach ($candidate in $candidates) {
        try {
            $arguments = $candidate.Args + @('-c', 'import sys; print(sys.version_info >= (3, 9))')
            $out = & $candidate.Exe @arguments 2>$null
            if ($LASTEXITCODE -eq 0 -and "$out".Trim() -eq 'True') { return $candidate }
        } catch { }
    }
    return $null
}

function Find-VisualStudio {
    $where = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $where)) { return @{ Any = $false; Cpp = $null } }
    try {
        $any = & $where -latest -products * -property installationPath 2>$null
        $cpp = & $where -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        return @{ Any = [bool]$any; Cpp = $cpp }
    } catch { return @{ Any = $false; Cpp = $null } }
}

function Find-Unreal {
    if ($env:UE_ROOT -and (Test-Path -LiteralPath (Join-Path $env:UE_ROOT 'Engine\Binaries\Win64\UnrealEditor.exe'))) { return $env:UE_ROOT }
    try {
        $folder = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\EpicGames\Unreal Engine\5.8' -Name InstalledDirectory -ErrorAction Stop).InstalledDirectory
        if ($folder -and (Test-Path -LiteralPath (Join-Path $folder 'Engine\Binaries\Win64\UnrealEditor.exe'))) { return $folder }
    } catch { }
    try {
        $list = Get-Content -Raw -LiteralPath (Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat') | ConvertFrom-Json
        foreach ($entry in $list.InstallationList) {
            if ($entry.AppName -eq 'UE_5.8' -and (Test-Path -LiteralPath (Join-Path $entry.InstallLocation 'Engine\Binaries\Win64\UnrealEditor.exe'))) { return $entry.InstallLocation }
        }
    } catch { }
    $usual = Join-Path $env:ProgramFiles 'Epic Games\UE_5.8'
    if (Test-Path -LiteralPath (Join-Path $usual 'Engine\Binaries\Win64\UnrealEditor.exe')) { return $usual }
    return $null
}

function Find-EpicLauncher {
    foreach ($root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ($root -and (Test-Path -LiteralPath (Join-Path $root 'Epic Games\Launcher\Portal\Binaries\Win64\EpicGamesLauncher.exe'))) { return $true }
    }
    return $false
}

# winget shows each package's terms itself and asks to agree; nothing is agreed to here.
function Install-WithWinget([string]$Id, [string[]]$More) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Say 'winget (Windows'' own installer of programs) is not on this computer: install it from the Microsoft Store ("App Installer"), or install this one yourself.'
        return $false
    }
    $arguments = @('install', '--id', $Id, '--exact', '--source', 'winget') + $More
    & winget @arguments | Out-Host
    Update-Path
    return ($LASTEXITCODE -eq 0)
}

try {
    Say 'Openviz installer'
    Say "This puts Openviz, a show-lighting visualizer, on this PC, from github.com/$Repository."

    Step '1. What Openviz needs'
    $python = Find-Python
    if (-not $python) {
        Say 'NOT FOUND  Python 3.9 or newer: Openviz and its setup are written in it.'
        if (YesNo 'Install Python 3.12 now (with winget)?' $true) {
            [void](Install-WithWinget 'Python.Python.3.12' @())
            $python = Find-Python
        }
        if (-not $python) { Stop-Install 'Python is needed. Install it from https://www.python.org/downloads (tick "Add python.exe to PATH"), then run this installer again.' }
    }
    Say ('ok         Python (' + $python.Exe + ')')

    $studio = Find-VisualStudio
    if ($studio.Cpp) {
        Say "ok         Visual Studio with C++ ($($studio.Cpp))"
    } elseif ($studio.Any) {
        Say 'NOT READY  Visual Studio is here, but without its C++ tools: open the Visual Studio Installer, Modify, tick "Game development with C++" and "Desktop development with C++", and Modify.'
    } else {
        Say 'NOT FOUND  Visual Studio with C++: the visualizer and Openviz''s core are built with it (several GB).'
        Say '           Use the version Epic lists for Unreal 5.8; this offers Visual Studio 2022 Community.'
        if (YesNo 'Install Visual Studio 2022 Community with the C++ game development tools now (with winget; it takes a while)?' $true) {
            $override = '--wait --passive --add Microsoft.VisualStudio.Workload.NativeGame --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended'
            [void](Install-WithWinget 'Microsoft.VisualStudio.2022.Community' @('--override', $override))
            $studio = Find-VisualStudio
            if ($studio.Cpp) { Say "ok         Visual Studio with C++ ($($studio.Cpp))" }
        }
    }

    $unreal = Find-Unreal
    if ($unreal) {
        Say "ok         Unreal Engine 5.8 ($unreal)"
    } else {
        Say 'later      Unreal Engine 5.8 is not installed. Openviz installs without it, but needs it to show a rig.'
        if (-not (Find-EpicLauncher) -and (YesNo 'Install the Epic Games Launcher now (with winget)? Unreal Engine is installed from it.' $true)) {
            [void](Install-WithWinget 'EpicGames.EpicGamesLauncher' @())
        }
        Say '           In the Epic Games Launcher: sign in, Unreal Engine, Library, the + beside Engine Versions, 5.8, Install.'
        Say '           Then run this installer again: it builds the visualizer once Unreal is there.'
    }

    # Run from inside an Openviz folder (as the .exe installer does at its end), this sets that one up.
    $self = $env:OPENVIZ_INSTALLER
    $here = $(if ($self) { Split-Path -Parent (Split-Path -Parent $self) } else { '' })
    $inPlace = $here -and (Test-Path -LiteralPath (Join-Path $here 'launcher\launcher.py'))
    if ($inPlace) {
        $dest = $here; $updating = $false
        $tag = 'in ' + $here
        $versionFile = Join-Path $here 'openviz-version.txt'
        if (Test-Path -LiteralPath $versionFile) { $tag = (Get-Content -Raw -LiteralPath $versionFile).Trim() }
    } else {
    Step '2. Where Openviz goes'
    $dest = Ask 'Install Openviz in C:\Openviz? Press Enter, or type another folder' 'C:\Openviz'
    $dest = [IO.Path]::GetFullPath($dest)
    if (Test-Path -LiteralPath (Join-Path $dest '.git')) { Stop-Install "$dest is a copy of Openviz made with Git: update it there with  git pull  and run launcher\setup_windows.bat." }
    $updating = Test-Path -LiteralPath (Join-Path $dest 'launcher\launcher.py')
    if ($updating) {
        Say "Openviz is already in ${dest}: it is updated, and its library (shows, fixtures, settings) is kept."
    } elseif ((Test-Path -LiteralPath $dest) -and (Get-ChildItem -LiteralPath $dest -Force | Select-Object -First 1)) {
        Stop-Install "$dest has other things in it. Run the installer again and name an empty or new folder."
    } else {
        Say "Openviz goes in $dest"
    }
    if ($dest -match '\s') { Say 'note       the folder''s path has a space in it: allowed for, but the first thing to suspect if a show will not open.' }

    Step '3. Downloading'
    $work = Join-Path ([IO.Path]::GetTempPath()) ('openviz-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        try {
            $releases = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases" -UserAgent $Agent -UseBasicParsing
        } catch { Stop-Install 'GitHub could not be reached. Is this PC on the internet?' }
        $asset = $null; $tag = $null
        foreach ($release in @($releases)) {
            if ($release.draft) { continue }
            $asset = @($release.assets) | Where-Object { $_.name -match '^Openviz-.*\.zip$' } | Select-Object -First 1
            if ($asset) { $tag = $release.tag_name; break }
        }
        if (-not $asset) { Stop-Install 'no release of Openviz with a download was found on GitHub.' }
        Say "Openviz ${tag}: $($asset.name) ($([math]::Round($asset.size / 1MB, 1)) MB)"
        $zip = Join-Path $work $asset.name
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -UserAgent $Agent -UseBasicParsing
        if ("$($asset.digest)" -like 'sha256:*') {
            $got = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($got -ne $asset.digest.Substring(7).ToLowerInvariant()) { Stop-Install 'the download is not what GitHub says it should be (its checksum differs). Try again.' }
            Say 'ok         checked against GitHub''s checksum'
        }
        $unpacked = Join-Path $work 'unpacked'
        Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
        $source = Join-Path $unpacked 'Openviz'
        if (-not (Test-Path -LiteralPath (Join-Path $source 'launcher\launcher.py'))) { Stop-Install 'the download does not hold Openviz as expected.' }

        Step '4. Putting Openviz in place'
        # What is built on this PC and what was made with Openviz stay; everything else is the release's.
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        $keep = @((Join-Path $dest 'library'), (Join-Path $dest 'core\build'), 'Binaries', 'Intermediate', 'Saved', 'DerivedDataCache')
        & robocopy $source $dest /MIR /XD @keep /XF windows-check.txt /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Stop-Install "the files could not be copied into $dest (robocopy said $LASTEXITCODE)." }
        Say "ok         $dest"
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    }

    Step $(if ($inPlace) { '2. Openviz''s own setup' } else { '5. Openviz''s own setup' })
    Say 'It builds the core and the visualizer (ten minutes to half an hour the first time), puts Openviz in the Start menu and on the desktop, and checks this PC.'
    $arguments = $python.Args + @('-X', 'utf8', (Join-Path $dest 'launcher\windows_setup.py'))
    if (-not $unreal) { $arguments += '--skip-unreal' }
    Push-Location -LiteralPath $dest
    try { & $python.Exe @arguments | Out-Host } finally { Pop-Location }
    $setupCode = $LASTEXITCODE

    Step 'Done'
    if ($inPlace) { Say "Openviz $tag is set up in $dest." } elseif ($updating) { Say "Openviz is updated to $tag, in $dest." } else { Say "Openviz $tag is installed in $dest." }
    if (-not $unreal) { Say 'Install Unreal Engine 5.8 in the Epic Games Launcher, then run this installer again to build the visualizer.' }
    elseif ($setupCode -ne 0) { Say 'The setup did not finish everything: what it listed under "What came of it" says what to put right; then run this installer again.' }
    $shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Openviz.lnk'
    if ((Test-Path -LiteralPath $shortcut) -and (YesNo 'Open Openviz now?' $true)) { Start-Process -FilePath $shortcut }
} catch {
    Write-Host ''
    Write-Host $_.Exception.Message -ForegroundColor Yellow
}

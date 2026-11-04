param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon',
    [switch]$ResourcesOnly,
    [switch]$NativeRender,
    [switch]$Diagnose
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$package = $PSScriptRoot
$stateFile = Join-Path $package 'windows_patch_status.json'
$sharedLog = Join-Path $package 'windows_launch.log'
$expectedGame = '4d78fbfaa65b7d002d2208d02da809200f4a5723119521180e30100d6f1a75f2'
$state = [ordered]@{ phase='preparing'; runtime_verified=$false; disk_binary_modified=$false }
function Write-State {
    $state['updated_at'] = [DateTime]::UtcNow.ToString('o')
    [System.IO.File]::WriteAllText($stateFile, ($state | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
}
function Copy-PatchLog {
    if (-not (Test-Path -LiteralPath $localLog)) { return }
    $stream = [System.IO.File]::Open($localLog, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8)
    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    [System.IO.File]::WriteAllText($sharedLog, $text, (New-Object System.Text.UTF8Encoding($false)))
}
function Copy-GameLog {
    $path = Join-Path $GameRoot 'app.log'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return }
    $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8)
    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    [System.IO.File]::WriteAllText((Join-Path $package 'windows_game_app.log'), $text,
        (New-Object System.Text.UTF8Encoding($false)))
}
try {
    if (Get-Process -Name Darkest -ErrorAction SilentlyContinue) {
        throw 'Save and exit Darkest Dungeon normally, then run this command again.'
    }
    if (-not (Get-Process -Name steam -ErrorAction SilentlyContinue)) {
        throw 'Start the Steam client first.'
    }
    $game = Join-Path $GameRoot '_windows\win64\Darkest.exe'
    $before = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($before -ne $expectedGame) { throw 'Unsupported Windows game EXE; no game was launched or patched.' }
    $launcherSource = Join-Path $package 'dd_windows_launcher.exe'
    $librarySource = Join-Path $package 'dd_skin_rig_patch.dll'
    $version = ((Get-FileHash -LiteralPath $launcherSource -Algorithm SHA256).Hash.Substring(0,16) +
                (Get-FileHash -LiteralPath $librarySource -Algorithm SHA256).Hash.Substring(0,16)).ToLowerInvariant()
    $target = Join-Path $env:LOCALAPPDATA ('DarkestDungeonPatches\windows_x64\' + $version)
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($name in @('dd_windows_launcher.exe', 'dd_skin_rig_patch.dll')) {
        $source = Join-Path $package $name
        $destination = Join-Path $target $name
        Copy-Item -LiteralPath $source -Destination $destination -Force
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash) {
            throw ('Patch copy hash mismatch: ' + $name)
        }
    }
    $launcher = Join-Path $target 'dd_windows_launcher.exe'
    $localLog = Join-Path $target 'windows_launch.log'
    $arguments = @('--game', ('"{0}"' -f $game))
    if ($ResourcesOnly) { $arguments += '--resources-only' }
    if ($NativeRender) { $arguments += '--native-render' }
    $check = Start-Process -FilePath $launcher -ArgumentList ($arguments + '--check') -NoNewWindow -PassThru
    $check.WaitForExit()
    if ($check.ExitCode -ne 0) { throw ('Native EXE validation failed: ' + $check.ExitCode) }
    $state['phase'] = 'checked'
    $state['game_sha256'] = $before
    $state['patch_directory'] = $target
    $state['runtime_rig'] = -not $ResourcesOnly
    $state['native_render'] = [bool]$NativeRender
    $state['diagnose'] = [bool]$Diagnose
    Write-State
    if ($Diagnose) { $arguments += '--diagnose' }
    $launch = Start-Process -FilePath $launcher -ArgumentList $arguments -NoNewWindow -PassThru
    $launch.WaitForExit()
    Copy-PatchLog
    Copy-GameLog
    if ($launch.ExitCode -ne 0) { throw ('Native patch launch failed: ' + $launch.ExitCode + '; see windows_launch.log') }
    $after = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($after -ne $before) { throw 'The disk EXE changed during launch; inspect the game installation.' }
    $gameProcess = Get-Process -Name Darkest -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $game } | Select-Object -First 1
    $state['phase'] = 'launched'
    $state['game_exe_sha256_unchanged'] = $true
    $state['pid'] = if ($gameProcess) { $gameProcess.Id } else { $null }
    Write-State
    Write-Host 'Patch launcher succeeded. Enable DeepSeek Highwaywoman Animation Skin and test original A-D, then E.'
    Write-Host 'Leave this window open while testing; it returns patch logs to the shared folder for up to 10 minutes.'
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while ($gameProcess -and -not $gameProcess.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Copy-PatchLog
        Start-Sleep -Seconds 1
        $gameProcess.Refresh()
    }
    Copy-PatchLog
    Copy-GameLog
    $state['phase'] = if ($gameProcess -and -not $gameProcess.HasExited) { 'monitor_finished' } else { 'game_exited' }
    Write-State
    exit 0
} catch {
    $state['phase'] = 'failed'
    $state['error'] = $_.Exception.Message
    try { if ($localLog) { Copy-PatchLog }; Copy-GameLog } catch { }
    try { Write-State } catch { }
    Write-Host ('Windows patch: ' + $state['error'])
    exit 1
}

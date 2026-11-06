param([Parameter(Mandatory=$true)][string]$BundleRoot)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('dd-setup-ci-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
function Assert([bool]$Condition,[string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Invoke-Native([string]$Executable,[string]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $started = $false
    try {
        $started = $process.Start()
        Assert $started 'Could not start fixture child.'
        Assert ($process.WaitForExit(30000)) 'Fixture child timed out.'
        return [int]$process.ExitCode
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
}
try {
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $BundleRoot 'SHA256SUMS'))) {
        $parts = $line -split '  ',2
        Assert ($parts.Count -eq 2) 'Invalid checksum manifest.'
        $actual = (Get-FileHash -LiteralPath (Join-Path $BundleRoot $parts[1]) -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert ($actual -ceq $parts[0]) ('Checksum mismatch: '+$parts[1])
    }
    $steam = Join-Path $root 'Steam with spaces'
    $config = Join-Path $steam 'userdata\123\config\localconfig.vdf'
    New-Item -ItemType Directory -Path (Split-Path -Parent $config) -Force | Out-Null
    $original = '// preserve this fixture' + "`r`n" + '"UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "apps" { "262060" { "LaunchOptions" "-fixture" } } } } } }'
    [IO.File]::WriteAllText($config,$original)
    $install = Join-Path $root 'Installed patch'
    $setup = Join-Path $BundleRoot 'dd_patch_setup.exe'
    $options = '--quiet --steam-root "'+$steam+'" --steam-user 123 --install-root "'+$install+'"'
    $status = Invoke-Native $setup ('--status '+$options)
    Assert ($status -eq 0) 'Self-contained EXE status failed.'
    $log = Join-Path $env:LOCALAPPDATA 'DarkestDungeonPatches\setup.log'
    $state = [IO.File]::ReadAllText($log)|ConvertFrom-Json
    Assert (-not $state.enabled) 'Fixture unexpectedly installed.'
    Assert ($state.launch_options -eq '-fixture') 'Original fixture options lost.'
    Assert ([IO.File]::ReadAllText($config) -ceq $original) 'Status changed Steam configuration.'
    Assert (-not (Test-Path -LiteralPath $install)) 'Status installed a payload.'
    $game = Join-Path $root 'Fixture game'
    $binary = Join-Path $game '_windows\win64\Darkest.exe'
    New-Item -ItemType Directory -Path (Split-Path -Parent $binary) -Force | Out-Null
    [IO.File]::WriteAllBytes($binary,[byte[]](77,90,0,0))
    $rejected = Invoke-Native $setup ('--preview '+$options+' --game-root "'+$game+'"')
    Assert ($rejected -ne 0) 'Unsupported game was accepted.'
    Assert ([IO.File]::ReadAllText($log).Contains('Invalid Windows x64 game EXE format')) 'Unexpected EXE payload failure.'
    Assert ([IO.File]::ReadAllText($config) -ceq $original) 'Rejection changed Steam configuration.'
    Assert (-not (Test-Path -LiteralPath $install)) 'Rejection installed a payload.'
    $native = Invoke-Native (Join-Path $BundleRoot 'dd_windows_launcher.exe') ('--game "'+$binary+'" --check')
    Assert ($native -eq 125) 'Native injector accepted an invalid executable.'
    # A generated valid PE format with no matching instructions is accepted;
    # no game build number or full-file hash is needed for installation.
    $header = New-Object byte[] 512
    $header[0]=77; $header[1]=90
    [BitConverter]::GetBytes([uint32]128).CopyTo($header,60)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($header,128)
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($header,132)
    [BitConverter]::GetBytes([uint16]1).CopyTo($header,134)
    [BitConverter]::GetBytes([uint16]240).CopyTo($header,148)
    [BitConverter]::GetBytes([uint16]0x20b).CopyTo($header,152)
    [IO.File]::WriteAllBytes($binary,$header)
    $preview = Invoke-Native $setup ('--preview '+$options+' --game-root "'+$game+'"')
    Assert ($preview -eq 0) 'Preview blocked a valid unknown-layout executable.'
    $native = Invoke-Native (Join-Path $BundleRoot 'dd_windows_launcher.exe') ('--game "'+$binary+'" --check')
    Assert ($native -eq 0) 'Native check blocked a valid unknown-layout executable.'
    # Registry and libraryfolders paths commonly differ in case and slashes.
    $installedGame = Join-Path $steam 'steamapps\common\Fixture game'
    $installedBinary = Join-Path $installedGame '_windows\win64\Darkest.exe'
    New-Item -ItemType Directory -Path (Split-Path -Parent $installedBinary) -Force | Out-Null
    [IO.File]::WriteAllBytes($installedBinary,$header)
    [IO.File]::WriteAllText((Join-Path $steam 'steamapps\appmanifest_262060.acf'),'"AppState" { "installdir" "Fixture game" }')
    $alias = $steam.Replace('\','/').ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $steam 'steamapps\libraryfolders.vdf'),'"libraryfolders" { "0" { "path" "'+$alias+'" } }')
    $automatic = Invoke-Native $setup ('--preview '+$options)
    Assert ($automatic -eq 0) 'Automatic discovery treated equivalent Steam paths as different installations.'
    $discovered = [IO.File]::ReadAllText($log)|ConvertFrom-Json
    Assert ($discovered.game -ieq $installedBinary) 'Automatic discovery selected a different game.'
    Write-Host 'Windows EXE payload, PowerShell 5 setup, checksums and compatibility checks passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}

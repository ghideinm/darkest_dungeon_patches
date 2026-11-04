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
    Assert ([IO.File]::ReadAllText($log).Contains('Unsupported Windows x64 game EXE')) 'Unexpected EXE payload failure.'
    Assert ([IO.File]::ReadAllText($config) -ceq $original) 'Rejection changed Steam configuration.'
    Assert (-not (Test-Path -LiteralPath $install)) 'Rejection installed a payload.'
    $native = Invoke-Native (Join-Path $BundleRoot 'dd_windows_launcher.exe') ('--game "'+$binary+'" --check')
    Assert ($native -eq 125) 'Native injector did not refuse an unknown game.'
    Write-Host 'Windows EXE payload, PowerShell 5 setup, checksums and refusal checks passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}

param(
    [ValidateSet('Install','Preview','Status','Uninstall')][string]$Action = 'Install',
    [string]$SteamRoot,
    [string]$SteamUser,
    [string]$GameRoot,
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'DarkestDungeonPatches\steam')
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

function ConvertFrom-VdfToken([string]$Raw) {
    if (-not $Raw.StartsWith('"')) { return $Raw }
    return [regex]::Replace($Raw.Substring(1,$Raw.Length-2), '\\([\\"nrt])',
        [Text.RegularExpressions.MatchEvaluator]{param($m)
            switch ($m.Groups[1].Value) {
                'n' { "`n" }; 'r' { "`r" }; 't' { "`t" }; default { $m.Groups[1].Value }
            }
        })
}
function ConvertTo-VdfToken([AllowNull()][string]$Value) {
    return '"' + $Value.Replace('\','\\').Replace('"','\"').Replace("`n",'\n').Replace("`r",'\r').Replace("`t",'\t') + '"'
}
function Read-VdfEntries($Tokens,[ref]$Position,[bool]$Nested) {
    $nodes = [Collections.Generic.List[object]]::new()
    while ($Position.Value -lt $Tokens.Count) {
        $key = $Tokens[$Position.Value]
        if ($key.Value -eq '}') {
            if (-not $Nested) { throw 'Unexpected closing VDF brace.' }
            return ,$nodes.ToArray()
        }
        if ($key.Value -eq '{' -or $Position.Value+1 -ge $Tokens.Count) { throw 'Missing VDF value.' }
        $value = $Tokens[$Position.Value+1]
        $Position.Value += 2
        $node = [pscustomobject]@{Key=(ConvertFrom-VdfToken $key.Value);Value=$null;Start=$value.Index;End=$value.Index+$value.Length;Close=0;Children=@()}
        if ($value.Value -eq '{') {
            $node.Children = Read-VdfEntries $Tokens $Position $true
            if ($Position.Value -ge $Tokens.Count -or $Tokens[$Position.Value].Value -ne '}') { throw 'Unclosed VDF block.' }
            $closing = $Tokens[$Position.Value]
            $node.Close = $closing.Index
            $node.End = $closing.Index+$closing.Length
            $Position.Value++
        } elseif ($value.Value -eq '}') { throw 'Missing VDF value.' }
        else { $node.Value = ConvertFrom-VdfToken $value.Value }
        $nodes.Add($node)
    }
    if ($Nested) { throw 'Unclosed VDF block.' }
    return ,$nodes.ToArray()
}
function Read-Vdf([string]$Text) {
    $tokens = [Collections.Generic.List[object]]::new()
    $position = 0
    foreach ($match in [regex]::Matches($Text,'\s+|//[^\r\n]*|"(?:\\.|[^"\\])*"|[{}]|[^\s{}"]+')) {
        if ($match.Index -ne $position) { throw 'Malformed VDF.' }
        $position = $match.Index+$match.Length
        if ($match.Value -notmatch '^\s' -and -not $match.Value.StartsWith('//')) { $tokens.Add($match) }
    }
    if ($position -ne $Text.Length) { throw 'Malformed VDF.' }
    $index = 0
    return [pscustomobject]@{Children=(Read-VdfEntries $tokens ([ref]$index) $false)}
}
function Get-VdfChild($Node,[string]$Key) {
    $matches = @($Node.Children | Where-Object { $_.Key -ieq $Key })
    if ($matches.Count -gt 1) { throw ('Duplicate VDF key: '+$Key) }
    if ($matches.Count) { return $matches[0] }
    return $null
}
function Get-SteamApps([string]$Text) {
    $node = Read-Vdf $Text
    foreach ($key in @('UserLocalConfigStore','Software','Valve','Steam','apps')) {
        $node = Get-VdfChild $node $key
        if ($null -eq $node -or $null -ne $node.Value) { throw ('Missing Steam apps block: '+$key) }
    }
    return $node
}
function Get-SteamLaunchOptions([string]$Text) {
    $app = Get-VdfChild (Get-SteamApps $Text) '262060'
    if ($null -eq $app) { return $null }
    if ($null -ne $app.Value) { throw 'Steam app must be a block.' }
    $node = Get-VdfChild $app 'LaunchOptions'
    if ($null -eq $node) { return $null }
    if ($null -eq $node.Value) { throw 'Steam LaunchOptions must be a string.' }
    return $node.Value
}
function Set-SteamLaunchOptions([string]$Text,[AllowNull()][string]$Value) {
    $apps = Get-SteamApps $Text
    $app = Get-VdfChild $apps '262060'
    if ($app -and $null -ne $app.Value) { throw 'Steam app must be a block.' }
    $node = if ($app) { Get-VdfChild $app 'LaunchOptions' } else { $null }
    if ($node) {
        if ($null -eq $node.Value) { throw 'Steam LaunchOptions must be a string.' }
        return $Text.Substring(0,$node.Start) + (ConvertTo-VdfToken $Value) + $Text.Substring($node.End)
    }
    if ($null -eq $Value) { return $Text }
    if ($app) {
        $insert = "`n`t`t`t`t`t`t" + '"LaunchOptions"' + "`t`t" + (ConvertTo-VdfToken $Value) + "`n`t`t`t`t`t"
        return $Text.Substring(0,$app.Close)+$insert+$Text.Substring($app.Close)
    }
    $insert = "`n`t`t`t`t`t" + '"262060"' + "`n`t`t`t`t`t{`n`t`t`t`t`t`t" + '"LaunchOptions"' + "`t`t" + (ConvertTo-VdfToken $Value) + "`n`t`t`t`t`t}`n`t`t`t`t"
    return $Text.Substring(0,$apps.Close)+$insert+$Text.Substring($apps.Close)
}
function Get-WrappedLaunchOptions([string]$Launcher,[AllowNull()][string]$Previous) {
    if ($Launcher -match '["\r\n%]') { throw 'Unsupported launcher path.' }
    $prefix = '"'+$Launcher+'" --steam '
    $count = [regex]::Matches($Previous,'%command%').Count
    if ($count -gt 1) { throw 'Multiple Steam command placeholders.' }
    if ($count) { return $Previous.Replace('%command%',$prefix+'%command%') }
    return $prefix+'%command%'+$(if ($Previous) { ' '+$Previous } else { '' })
}
function Assert-RegularPath([string]$Path) {
    if ((Test-Path -LiteralPath $Path) -and ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw ('Refusing reparse point: '+$Path)
    }
}
function Write-AtomicBytes([string]$Path,[byte[]]$Bytes) {
    Assert-RegularPath $Path
    $temporary = Join-Path (Split-Path -Parent $Path) ('.dd-steam-'+[guid]::NewGuid().ToString('N')+'.tmp')
    try {
        [IO.File]::WriteAllBytes($temporary,$Bytes)
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary,$Path,[NullString]::Value) }
        else { [IO.File]::Move($temporary,$Path) }
    } finally { [IO.File]::Delete($temporary) }
}
function Read-SteamConfig([string]$Path) {
    Assert-RegularPath $Path
    $bytes = [IO.File]::ReadAllBytes($Path)
    $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191
    $utf8 = [Text.UTF8Encoding]::new($bom,$true)
    $text = $utf8.GetString($bytes)
    if ($bom) { $text = $text.Substring(1) }
    return [pscustomobject]@{Bytes=$bytes;Text=$text;Encoding=$utf8}
}
function Write-SteamConfig([string]$Path,$Before,[AllowNull()][string]$Options) {
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($Path)) -cne [Convert]::ToBase64String($Before.Bytes)) {
        throw 'Steam configuration changed during installation; nothing overwritten.'
    }
    $text = Set-SteamLaunchOptions $Before.Text $Options
    [byte[]]$bytes = $Before.Encoding.GetPreamble()+$Before.Encoding.GetBytes($text)
    Write-AtomicBytes $Path $bytes
}
function Find-SteamRoot {
    $path = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $path) { $path = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction SilentlyContinue).InstallPath }
    if (-not $path) { throw 'Steam installation missing; specify -SteamRoot.' }
    return $path
}
function Find-SteamUser([string]$Root,[string]$User) {
    if (-not $User) {
        $users = Get-VdfChild (Read-Vdf ([IO.File]::ReadAllText((Join-Path $Root 'config\loginusers.vdf')))) 'users'
        $candidates = @($users.Children | Where-Object { $_.Key -match '^\d+$' } | ForEach-Object {
            $recent = Get-VdfChild $_ 'MostRecent'; $timestamp = Get-VdfChild $_ 'Timestamp'
            [pscustomobject]@{Id=([uint64]$_.Key-[uint64]76561197960265728).ToString();Recent=[int]($recent -and $recent.Value -eq '1');Timestamp=$(if($timestamp){[long]$timestamp.Value}else{0})}
        } | Sort-Object -Property @{Expression='Recent';Descending=$true},@{Expression='Timestamp';Descending=$true})
        if (-not $candidates.Count) { throw 'Steam login missing; specify -SteamUser.' }
        if ($candidates.Count -gt 1 -and $candidates[0].Recent -eq $candidates[1].Recent -and $candidates[0].Timestamp -eq $candidates[1].Timestamp) { throw 'Steam account is ambiguous; specify -SteamUser.' }
        $User = $candidates[0].Id
    }
    if ($User -notmatch '^\d+$') { throw '-SteamUser must be a numeric userdata ID.' }
    return $User
}
function Find-SteamGame([string]$Root) {
    $roots = [Collections.Generic.List[string]]::new(); $roots.Add($Root)
    $libraries = Join-Path $Root 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $libraries) {
        $node = Get-VdfChild (Read-Vdf ([IO.File]::ReadAllText($libraries))) 'libraryfolders'
        foreach ($library in $node.Children) {
            if ($library.Key -match '^\d+$') { $path = Get-VdfChild $library 'path'; if ($path) { $roots.Add($path.Value) } }
        }
    }
    $games = @($roots.ToArray() | Select-Object -Unique | ForEach-Object {
        $manifest = Join-Path $_ 'steamapps\appmanifest_262060.acf'
        if (Test-Path -LiteralPath $manifest) {
            $app = Get-VdfChild (Read-Vdf ([IO.File]::ReadAllText($manifest))) 'AppState'
            $dir = Get-VdfChild $app 'installdir'
            if ($dir -and $dir.Value -notmatch '[\\/:]' -and $dir.Value -notin @('.','..')) {
                $candidate = Join-Path $_ ('steamapps\common\'+$dir.Value)
                if (Test-Path -LiteralPath (Join-Path $candidate '_windows\win64\Darkest.exe')) { $candidate }
            }
        }
    } | Select-Object -Unique)
    if ($games.Count -ne 1) { throw 'Game installation missing or ambiguous; specify -GameRoot.' }
    return $games[0]
}
function Assert-SteamStopped {
    if (Get-Process -Name steam,Darkest -ErrorAction SilentlyContinue) { throw 'Save and exit Darkest Dungeon and Steam normally before installing or uninstalling.' }
}
function Assert-GameExecutable([string]$Path) {
    # Inspect only the executable format; game versions and full-file hashes
    # are deliberately not an installation requirement.
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Invalid Windows x64 game EXE format.' }
        $stream.Position = 0x3c
        $pe = $reader.ReadUInt32()
        if ($pe -lt 64 -or $pe + 264 -gt $stream.Length) { throw 'Invalid Windows x64 game EXE format.' }
        $stream.Position = $pe
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Invalid Windows x64 game EXE format.' }
        $stream.Position = $pe + 24
        if ($reader.ReadUInt16() -ne 0x20b) { throw 'Invalid Windows x64 game EXE format.' }
    } finally { $reader.Dispose() }
}
function Invoke-SteamSetup {
    if (-not $SteamRoot) { $SteamRoot = Find-SteamRoot }
    $SteamRoot = [IO.Path]::GetFullPath($SteamRoot)
    $InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
    Assert-RegularPath $InstallRoot
    $user = Find-SteamUser $SteamRoot $SteamUser
    $config = Join-Path $SteamRoot ('userdata\'+$user+'\config\localconfig.vdf')
    $before = Read-SteamConfig $config
    $current = Get-SteamLaunchOptions $before.Text
    $statePath = Join-Path $InstallRoot 'steam_install.json'
    Assert-RegularPath $statePath
    $stateBytes = if (Test-Path -LiteralPath $statePath) { [IO.File]::ReadAllBytes($statePath) } else { $null }
    $state = if ($stateBytes) { [Text.Encoding]::UTF8.GetString($stateBytes)|ConvertFrom-Json } else { $null }
    if ($state -and ($state.version -ne 1 -or $state.app_id -ne '262060' -or $state.localconfig -ine $config)) { throw 'Installation state belongs to another account or format.' }
    if ($Action -eq 'Status') {
        return [ordered]@{enabled=([bool]$state -and $current -ceq $state.launch_options);localconfig=$config;launch_options=$current;install_root=$InstallRoot}
    }
    if ($state -and $current -cne $state.launch_options) { throw 'Steam launch options were edited after installation; refusing to overwrite.' }
    if ($Action -eq 'Uninstall') {
        Assert-SteamStopped
        if ($state) { Write-SteamConfig $config $before $state.previous_launch_options; Remove-Item -LiteralPath $statePath }
        return [ordered]@{enabled=$false;changed=[bool]$state;retained_binaries=$InstallRoot}
    }
    if (-not $GameRoot) { $GameRoot = Find-SteamGame $SteamRoot }
    $game = Join-Path ([IO.Path]::GetFullPath($GameRoot)) '_windows\win64\Darkest.exe'
    Assert-GameExecutable $game
    $launcher = Join-Path $InstallRoot 'dd_windows_launcher.exe'
    $previous = if ($state) { $state.previous_launch_options } else { $current }
    $options = Get-WrappedLaunchOptions $launcher $previous
    if ($Action -eq 'Preview') {
        return [ordered]@{changed=$false;localconfig=$config;previous_launch_options=$previous;launch_options=$options;game=$game;install_root=$InstallRoot}
    }
    Assert-SteamStopped
    New-Item -ItemType Directory -Path $InstallRoot -Force | Out-Null
    $names = @('dd_windows_launcher.exe','dd_skin_rig_patch.dll')
    $saved = @{}
    foreach ($name in $names) {
        $destination = Join-Path $InstallRoot $name
        Assert-RegularPath $destination
        $saved[$name] = if(Test-Path -LiteralPath $destination){[IO.File]::ReadAllBytes($destination)}else{$null}
    }
    try {
        foreach ($name in $names) {
            $source = Join-Path $PSScriptRoot $name; $destination = Join-Path $InstallRoot $name
            if ([IO.Path]::GetFullPath($source) -ine $destination) { Copy-Item -LiteralPath $source -Destination $destination -Force }
            if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash) { throw ('Patch copy SHA256 mismatch: '+$name) }
        }
        $info = [Diagnostics.ProcessStartInfo]::new(); $info.FileName=$launcher
        $info.Arguments='--game "'+$game+'" --check'; $info.UseShellExecute=$false; $info.WorkingDirectory=$InstallRoot
        $process = [Diagnostics.Process]::new(); $process.StartInfo=$info
        try { if(-not $process.Start()){throw 'Native check failed to start.'};$process.WaitForExit();if($process.ExitCode -ne 0){throw 'Native executable format check failed.'} } finally {$process.Dispose()}
        $newState = [ordered]@{version=1;app_id='262060';localconfig=$config;previous_launch_options=$previous;launch_options=$options;game=$game;runtime_python_required=$false}
        Write-AtomicBytes $statePath ([Text.UTF8Encoding]::new($false).GetBytes(($newState|ConvertTo-Json -Depth 3)))
        Assert-SteamStopped
        Write-SteamConfig $config $before $options
    } catch {
        foreach ($name in $names) {
            $destination=Join-Path $InstallRoot $name
            if ($null -ne $saved[$name]) { [IO.File]::WriteAllBytes($destination,$saved[$name]) }
            elseif (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination }
        }
        if ($stateBytes) { Write-AtomicBytes $statePath $stateBytes }
        elseif (Test-Path -LiteralPath $statePath) { Remove-Item -LiteralPath $statePath }
        throw
    }
    return [ordered]@{enabled=$true;localconfig=$config;launch_options=$options;install_root=$InstallRoot;runtime_python_required=$false;log=(Join-Path $InstallRoot 'windows_launch.log')}
}
if ($MyInvocation.InvocationName -ne '.') {
    try { Invoke-SteamSetup | ConvertTo-Json -Depth 4 }
    catch { Write-Host ('Steam patch setup: '+$_.Exception.Message); exit 1 }
}

param(
    [ValidateSet('Install','Preview','Status','Uninstall')][string]$Action = 'Install',
    [string]$SteamRoot,
    [string]$GameRoot
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
        $node = [pscustomobject]@{Key=(ConvertFrom-VdfToken $key.Value);KeyStart=$key.Index;Value=$null;Start=$value.Index;End=$value.Index+$value.Length;Close=0;Children=@()}
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
function Set-SteamLaunchOptions([string]$Text,[AllowNull()]$Value) {
    $apps = Get-SteamApps $Text
    $app = Get-VdfChild $apps '262060'
    if ($app -and $null -ne $app.Value) { throw 'Steam app must be a block.' }
    $node = if ($app) { Get-VdfChild $app 'LaunchOptions' } else { $null }
    if ($node) {
        if ($null -eq $node.Value) { throw 'Steam LaunchOptions must be a string.' }
        if ($null -eq $Value) { return $Text.Substring(0,$node.KeyStart)+$Text.Substring($node.End) }
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
function Write-SteamConfig([string]$Path,$Before,[AllowNull()]$Options) {
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
function Find-SteamGame([string]$Root) {
    $roots = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [void]$roots.Add([IO.Path]::GetFullPath($Root))
    $libraries = Join-Path $Root 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $libraries) {
        $node = Get-VdfChild (Read-Vdf ([IO.File]::ReadAllText($libraries))) 'libraryfolders'
        foreach ($library in $node.Children) {
            if ($library.Key -match '^\d+$') { $path = Get-VdfChild $library 'path'; if ($path) { [void]$roots.Add([IO.Path]::GetFullPath($path.Value)) } }
        }
    }
    $games = @($roots | ForEach-Object {
        $manifest = Join-Path $_ 'steamapps\appmanifest_262060.acf'
        if (Test-Path -LiteralPath $manifest) {
            $app = Get-VdfChild (Read-Vdf ([IO.File]::ReadAllText($manifest))) 'AppState'
            $dir = Get-VdfChild $app 'installdir'
            if ($dir -and $dir.Value -notmatch '[\\/:]' -and $dir.Value -notin @('.','..')) {
                $candidate = Join-Path $_ ('steamapps\common\'+$dir.Value)
                if ((Test-Path -LiteralPath (Join-Path $candidate '_windows\win64\Darkest.exe')) -or (Test-Path -LiteralPath (Join-Path $candidate '_windowsnosteam\win64\Darkest.exe'))) { $candidate }
            }
        }
    } | Select-Object -Unique)
    if ($games.Count -ne 1) { throw 'Game installation missing or ambiguous; specify -GameRoot.' }
    return $games[0]
}
function Assert-GameStopped {
    if (Get-Process -Name Darkest,dd_windows_launcher -ErrorAction SilentlyContinue) { throw 'Save and exit Darkest Dungeon before installing or uninstalling.' }
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
function Test-OurPlugin([string]$Path) {
    return (Test-Path -LiteralPath $Path) -and [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path)).Contains('DarkestDungeonPatches.Win64.ProxyPlugin.v1')
}
function Get-LegacyInstalls([string]$Root) {
    foreach ($name in @('steam','codex_vm_test','windows_x64')) {
        $folder=Join-Path $env:LOCALAPPDATA ('DarkestDungeonPatches\'+$name)
        $path=Join-Path $folder 'steam_install.json'
        if (-not (Test-Path -LiteralPath $path)) { continue }
        Assert-RegularPath $folder; Assert-RegularPath $path
        $state=[IO.File]::ReadAllText($path)|ConvertFrom-Json
        if ($state.version -ne 1 -or $state.app_id -ne '262060') { throw 'Unknown legacy installation state.' }
        $expected=Join-Path $Root '_windows\win64\Darkest.exe'
        if ([IO.Path]::GetFullPath($state.game) -ine [IO.Path]::GetFullPath($expected)) { continue }
        $client=if($SteamRoot){$SteamRoot}else{Find-SteamRoot}
        $prefix=[IO.Path]::GetFullPath((Join-Path $client 'userdata'))+[IO.Path]::DirectorySeparatorChar
        $config=[IO.Path]::GetFullPath($state.localconfig)
        if (-not $config.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or $config.Substring($prefix.Length) -notmatch '^\d+[\\/]config[\\/]localconfig\.vdf$') { throw 'Invalid legacy Steam configuration path.' }
        $before=Read-SteamConfig $config
        if ((Get-SteamLaunchOptions $before.Text) -cne $state.launch_options) { throw 'Steam launch options changed after the old installation; restore them before migrating.' }
        [pscustomobject]@{Folder=$folder;State=$path;Config=$config;Before=$before;Previous=$state.previous_launch_options}
    }
}
function Invoke-Setup {
    $root=if($GameRoot){$GameRoot}else{if(-not $SteamRoot){$script:SteamRoot=Find-SteamRoot};Find-SteamGame $SteamRoot}
    $root=[IO.Path]::GetFullPath($root);Assert-RegularPath $root
    $targets=@(foreach($sub in @('_windows\win64','_windowsnosteam\win64')) {
        $dir=Join-Path $root $sub;$exe=Join-Path $dir 'Darkest.exe'
        if(Test-Path -LiteralPath $exe){Assert-RegularPath $dir;Assert-RegularPath $exe;Assert-GameExecutable $exe;$dir}
    })
    if(-not $targets.Count){throw 'No _windows/win64 or _windowsnosteam/win64 game found.'}
    $names=@('dd_engine_patch.asi','winmm.ini','dd_engine_patch.ini','winmm.dll')
    $proxyHash=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'winmm.dll')).Hash
    $legacy=@(Get-LegacyInstalls $root)
    $details=@(foreach($dir in $targets){
        $plugin=Join-Path $dir 'dd_engine_patch.asi';$proxy=Join-Path $dir 'winmm.dll'
        Assert-RegularPath $plugin;Assert-RegularPath $proxy
        $ours=Test-OurPlugin $plugin
        if((Test-Path -LiteralPath $plugin) -and -not $ours){throw ('Unrecognized plugin, refusing to overwrite: '+$plugin)}
        $configuration=Join-Path $dir 'winmm.ini'
        Assert-RegularPath $configuration
        $patchConfiguration=Join-Path $dir 'dd_engine_patch.ini'
        Assert-RegularPath $patchConfiguration
        if((Test-Path -LiteralPath $configuration) -and -not $ours){throw ('Existing WinMM configuration belongs to another installation: '+$configuration)}
        $hasProxy=Test-Path -LiteralPath $proxy
        if($hasProxy -and (-not $ours -or (Get-FileHash -LiteralPath $proxy).Hash -cne $proxyHash)){throw ('Existing WinMM proxy belongs to another installation: '+$proxy)}
        [ordered]@{directory=$dir;installed=($ours -and $hasProxy -and (Test-Path -LiteralPath $configuration) -and (Test-Path -LiteralPath $patchConfiguration))}
    })
    if($Action -in @('Status','Preview')){return [ordered]@{targets=$details;legacy_installations=$legacy.Count;files=$names}}
    Assert-GameStopped
    if($legacy.Count -and (Get-Process -Name steam -ErrorAction SilentlyContinue)){throw 'Exit Steam normally once to migrate the old launch options.'}
    if($Action -eq 'Install' -and -not(Test-OurPlugin (Join-Path $PSScriptRoot $names[0]))){throw 'Invalid patch payload.'}
    $saved=@{};$touched=New-Object 'Collections.Generic.List[string]'
    function Save-Before([string]$Path){
        Assert-RegularPath $Path
        if(-not $saved.ContainsKey($Path)){$saved[$Path]=if(Test-Path -LiteralPath $Path){[IO.File]::ReadAllBytes($Path)}else{$null};$touched.Add($Path)}
    }
    try {
        foreach($dir in $targets){
            foreach($name in $(if($Action -eq 'Uninstall'){@('winmm.dll','dd_engine_patch.asi','winmm.ini','dd_engine_patch.ini')}else{$names})){
                $path=Join-Path $dir $name
                if($Action -eq 'Install' -and $name -eq 'dd_engine_patch.ini' -and (Test-Path -LiteralPath $path)){continue}
                Save-Before $path
                if($Action -eq 'Uninstall'){if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}}
                else {
                    $source=Join-Path $PSScriptRoot $name
                    Write-AtomicBytes $path ([IO.File]::ReadAllBytes($source))
                    if((Get-FileHash -LiteralPath $path).Hash -cne (Get-FileHash -LiteralPath $source).Hash){throw 'Patch copy checksum mismatch.'}
                }
            }
        }
        foreach($old in $legacy){
            if(Get-Process -Name steam -ErrorAction SilentlyContinue){throw 'Steam restarted during migration.'}
            Save-Before $old.Config
            Write-SteamConfig $old.Config $old.Before $old.Previous
            foreach($name in @('dd_windows_launcher.exe','dd_skin_rig_patch.dll','steam_install.json','windows_launch.log')){
                $path=Join-Path $old.Folder $name
                if(Test-Path -LiteralPath $path){Save-Before $path;Remove-Item -LiteralPath $path}
            }
            $log=Join-Path (Split-Path -Parent $old.Folder) 'setup.log'
            if(Test-Path -LiteralPath $log){Save-Before $log;Remove-Item -LiteralPath $log}
        }
    } catch {
        for($i=$touched.Count-1;$i -ge 0;$i--){
            $path=$touched[$i]
            if($null -eq $saved[$path]){if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}}
            else{Write-AtomicBytes $path $saved[$path]}
        }
        throw
    }
    return [ordered]@{installed=($Action -eq 'Install');targets=$targets;files=$names;migrated=$legacy.Count}
}
if ($MyInvocation.InvocationName -ne '.') {
    try { Invoke-Setup | ConvertTo-Json -Depth 5 }
    catch { Write-Output ('Patch setup: '+$_.Exception.Message); exit 1 }
}

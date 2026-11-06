param([Parameter(Mandatory=$true)][string]$BundleRoot)
$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('dd-setup-ci-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
function Assert([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Invoke-Native([string]$Executable,[string]$Arguments){
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$Executable;$info.Arguments=$Arguments
    $info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $p=[Diagnostics.Process]::new();$p.StartInfo=$info
    try{
        Assert ($p.Start()) 'Could not start fixture child.'
        $text=$p.StandardOutput.ReadToEnd();$errorText=$p.StandardError.ReadToEnd()
        Assert ($p.WaitForExit(30000)) 'Fixture child timed out.'
        [pscustomobject]@{Code=$p.ExitCode;Text=$text;ErrorText=$errorText}
    }finally{if($p.Id -and -not $p.HasExited){$p.Kill()};$p.Dispose()}
}
try{
    foreach($line in [IO.File]::ReadAllLines((Join-Path $BundleRoot 'SHA256SUMS'))){
        $parts=$line -split '  ',2
        Assert ($parts.Count -eq 2) 'Invalid checksum manifest.'
        $actual=(Get-FileHash -LiteralPath (Join-Path $BundleRoot $parts[1])).Hash.ToLowerInvariant()
        Assert ($actual -ceq $parts[0]) ('Checksum mismatch: '+$parts[1])
    }
    $oldLocal=$env:LOCALAPPDATA;$oldTemp=$env:TEMP;$oldTmp=$env:TMP
    $env:LOCALAPPDATA=Join-Path $root 'local';$env:TEMP=Join-Path $root 'temp';$env:TMP=$env:TEMP
    New-Item -ItemType Directory -Path $env:LOCALAPPDATA,$env:TEMP -Force | Out-Null
    $game=Join-Path $root 'Fixture game with spaces'
    $header=New-Object byte[] 512;$header[0]=77;$header[1]=90
    [BitConverter]::GetBytes([uint32]128).CopyTo($header,60)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($header,128)
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($header,132)
    [BitConverter]::GetBytes([uint16]0x20b).CopyTo($header,152)
    $folders=@('_windows\win64','_windowsnosteam\win64')
    foreach($sub in $folders){
        $dir=Join-Path $game $sub;New-Item -ItemType Directory -Path $dir -Force|Out-Null
        [IO.File]::WriteAllBytes((Join-Path $dir 'Darkest.exe'),$header)
    }
    $setup=Join-Path $BundleRoot 'dd_patch_setup.exe';$options='--quiet --game-root "'+$game+'"'
    $preview=Invoke-Native $setup ('--preview '+$options)
    Assert ($preview.Code -eq 0) ('Preview failed: '+$preview.Text+$preview.ErrorText)
    $details=$preview.Text|ConvertFrom-Json
    Assert ($details.targets.Count -eq 2) 'Both win64 entries were not found.'
    $first=Invoke-Native $setup $options
    Assert ($first.Code -eq 0) ('Installation failed: '+$first.Text+$first.ErrorText)
    foreach($sub in $folders){
        $dir=Join-Path $game $sub
        Assert (@(Get-ChildItem -LiteralPath $dir -File).Count -eq 4) 'Installed files exceed EXE plus two DLLs and proxy configuration.'
        foreach($name in @('winmm.dll','dd_engine_patch.asi','winmm.ini')){
            Assert ((Get-FileHash (Join-Path $dir $name)).Hash -eq (Get-FileHash (Join-Path $BundleRoot $name)).Hash) 'Installed payload mismatch.'
        }
    }
    $again=Invoke-Native $setup $options
    Assert ($again.Code -eq 0) 'Idempotent update failed.'
    $status=Invoke-Native $setup ('--status '+$options)
    Assert ($status.Code -eq 0) 'Status failed.'
    Assert (@(($status.Text|ConvertFrom-Json).targets|Where-Object {-not $_.installed}).Count -eq 0) 'Status lost an installed entry.'
    Assert (-not(Test-Path (Join-Path $env:LOCALAPPDATA 'DarkestDungeonPatches'))) 'Setup left AppData state or logs.'
    Assert (@(Get-ChildItem -LiteralPath $env:TEMP -Filter 'ddp*').Count -eq 0) 'Temporary setup payload was retained.'
    $removed=Invoke-Native $setup ('--uninstall '+$options)
    Assert ($removed.Code -eq 0) 'Uninstall failed.'
    foreach($sub in $folders){Assert (@(Get-ChildItem (Join-Path $game $sub) -File).Count -eq 1) 'Uninstall retained patch files.'}
    # A pre-existing proxy must never be replaced or claimed by the installer.
    $foreign=Join-Path (Join-Path $game $folders[1]) 'winmm.dll'
    [IO.File]::WriteAllText($foreign,'fixture belonging to another mod')
    $blocked=Invoke-Native $setup $options
    Assert ($blocked.Code -ne 0) 'Foreign proxy was overwritten.'
    Assert ([IO.File]::ReadAllText($foreign) -ceq 'fixture belonging to another mod') 'Foreign proxy changed.'
    Assert (-not(Test-Path (Join-Path (Join-Path $game $folders[0]) 'winmm.dll'))) 'Conflict installed a partial first entry.'
    Write-Host 'Native EXE installs both win64 entries, updates, removes, rejects conflicts and retains no logs or state.'
}finally{
    $env:LOCALAPPDATA=$oldLocal;$env:TEMP=$oldTemp;$env:TMP=$oldTmp
    Remove-Item -LiteralPath $root -Recurse -Force
}

param([string]$PackageDirectory=(Join-Path (Split-Path $PSScriptRoot -Parent) 'dist/Apex回放'))
$ErrorActionPreference='Stop'
$taskTest=Join-Path (Split-Path $PSScriptRoot -Parent) ('artifacts/settings-memory-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskTest -Force | Out-Null
$taskSettings=Join-Path $taskTest 'settings.json'
foreach($taskMode in 'write','read','read-missing'){
    $taskReport=Join-Path $taskTest ($taskMode+'.json')
    $taskProcess=Start-Process -FilePath (Join-Path $PackageDirectory 'Apex回放.exe') -ArgumentList @('--settings-smoke',('"'+$taskSettings+'"'),$taskMode,('"'+$taskReport+'"')) -WindowStyle Hidden -PassThru
    if(!$taskProcess.WaitForExit(15000)){throw 'Settings memory test timed out'}
    if($taskProcess.ExitCode -ne 0){throw 'Settings memory test process failed'}
    $taskResult=Get-Content -LiteralPath $taskReport -Raw | ConvertFrom-Json
    if(!$taskResult.passed){throw "Settings $taskMode failed"}
}
Write-Output 'PASS directory / microphone / denoise / developer mode / balance / cache / timings / video / theme / Windows startup / four damage criteria survived immediate exit and new processes'
Write-Output $taskTest

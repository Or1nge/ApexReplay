param([string]$PackageDirectory=(Join-Path (Split-Path $PSScriptRoot -Parent) 'dist/Apex回放'))
$ErrorActionPreference='Stop'
$taskTest=Join-Path (Split-Path $PSScriptRoot -Parent) ('artifacts/startup-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskTest -Force | Out-Null
$taskSettings=Join-Path $taskTest 'settings.json'
@{outputDirectory=(Join-Path $taskTest 'clips')} | ConvertTo-Json | Set-Content -LiteralPath $taskSettings -Encoding utf8NoBOM
$taskReport=Join-Path $taskTest 'report.json'
$taskProcess=Start-Process -FilePath (Join-Path $PackageDirectory 'Apex回放.exe') -ArgumentList @('--startup-smoke',('"'+$taskSettings+'"'),'test',('"'+$taskReport+'"')) -WindowStyle Hidden -PassThru
if(!$taskProcess.WaitForExit(25000)){throw 'Automatic capture startup timed out'}
if($taskProcess.ExitCode -ne 0){throw 'Automatic capture process failed'}
$taskResult=Get-Content -LiteralPath $taskReport -Raw | ConvertFrom-Json
if(!$taskResult.passed){throw ('Automatic capture startup failed: '+$taskResult.error)}
Write-Output 'PASS app automatically arms capture without clicking Start'
Write-Output $taskTest

param([switch]$SkipBuild,[string]$Version='0.4.0-preview')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
if(!$SkipBuild){& (Join-Path $PSScriptRoot 'build.ps1')}
$taskPackage=Join-Path $taskRoot 'dist\Apex回放'
$taskDocs=Join-Path $taskPackage 'docs'
New-Item -ItemType Directory -Path $taskDocs -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRoot 'README.md') -Destination $taskPackage -Force
foreach($taskDocument in 'BUILD.md','THIRD_PARTY.md','VALIDATION.md'){
    Copy-Item -LiteralPath (Join-Path $taskRoot ('docs/'+$taskDocument)) -Destination $taskDocs -Force
}
$taskOldInventory=Join-Path $taskDocs 'APP_INVENTORY.md'
if(Test-Path -LiteralPath $taskOldInventory){Remove-Item -LiteralPath $taskOldInventory -Force}
foreach($taskLicense in 'LICENSE.txt','ThirdPartyNotices.txt'){
    $taskLicenseSource=Join-Path $taskRoot ('.tools\dotnet\'+$taskLicense)
    if(Test-Path -LiteralPath $taskLicenseSource){Copy-Item -LiteralPath $taskLicenseSource -Destination (Join-Path $taskPackage ('licenses\dotnet-'+$taskLicense)) -Force}
}
$taskFiles=Get-ChildItem -LiteralPath $taskPackage -Recurse -File | Where-Object Name -ne 'PACKAGE-MANIFEST.json' | ForEach-Object {
    @{path=[IO.Path]::GetRelativePath($taskPackage,$_.FullName);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
@{name='Apex回放';version=$Version;builtUtc=[DateTime]::UtcNow.ToString('O');architecture='win-x64';files=@($taskFiles)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskPackage 'PACKAGE-MANIFEST.json') -Encoding utf8
$taskZip=Join-Path $taskRoot 'dist\Apex回放-win-x64.zip'
Compress-Archive -LiteralPath $taskPackage -DestinationPath $taskZip -Force -CompressionLevel Optimal
Get-Item -LiteralPath $taskZip | Select-Object FullName,Length

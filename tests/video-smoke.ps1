param([string]$PackageDirectory=(Join-Path (Split-Path $PSScriptRoot -Parent) 'dist/Apex回放'))
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$taskTest=Join-Path $taskRoot ('artifacts/video-options-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskTest -Force | Out-Null
$taskCases=@(
    @{name='native-hevc';resolution='source';codec='hevc';preset='p6';bitrate=60},
    @{name='1080p-h264';resolution='1080p';codec='h264';preset='p4';bitrate=35},
    @{name='1440p-hevc';resolution='1440p';codec='hevc';preset='p6';bitrate=80},
    @{name='4k-hevc';resolution='2160p';codec='hevc';preset='p6';bitrate=120}
)
$taskResults=@()
foreach($taskCase in $taskCases){
    $taskDirectory=Join-Path $taskTest $taskCase.name
    $taskSettings=Join-Path $taskTest ($taskCase.name+'.json')
    @{videoResolution=$taskCase.resolution;videoCodec=$taskCase.codec;videoPreset=$taskCase.preset;videoBitrateMbps=$taskCase.bitrate} | ConvertTo-Json | Set-Content -LiteralPath $taskSettings -Encoding utf8NoBOM
    & (Join-Path $PackageDirectory 'ApexPerif.Worker.exe') --session-selftest $taskDirectory $taskSettings > (Join-Path $taskTest ($taskCase.name+'-output.json'))
    if($LASTEXITCODE -ne 0){throw "Capture failed: $($taskCase.name)"}
    $taskReport=Get-Content -LiteralPath (Join-Path $taskDirectory 'session-report.json') -Raw | ConvertFrom-Json
    $taskStatus=@($taskReport.messages | Where-Object type -eq 'status')
    if(!($taskStatus | Where-Object {$_.micNoiseSuppression -eq $false}) -or !($taskStatus | Where-Object {$_.micNoiseSuppression -eq $true})){throw 'Live microphone noise suppression toggle failed'}
    if(($taskStatus[-1].audioHealthy -contains $false) -or !$taskStatus[-1].captureBorderHidden){throw 'Audio or borderless capture was not available'}
    if($taskStatus[-1].videoTargetBitrateMbps -ne $taskCase.bitrate -or $taskStatus[-1].videoPreset -ne $taskCase.preset){throw 'Configured encoder bitrate or preset mismatch'}
    $taskVideo=Get-ChildItem -LiteralPath $taskDirectory -Filter '*.mp4' | Select-Object -First 1
    $taskStreams=& (Join-Path $PackageDirectory 'ffprobe.exe') -v error -show_entries stream=codec_name,width,height,r_frame_rate -of json $taskVideo.FullName | ConvertFrom-Json
    if($taskStreams.streams.Count -ne 4 -or $taskStreams.streams[0].codec_name -ne $taskCase.codec){throw 'Exported codecs or track count mismatch'}
    if($taskStreams.streams[0].width -ne $taskStatus[-1].videoWidth -or $taskStreams.streams[0].height -ne $taskStatus[-1].videoHeight){throw 'Exported resolution mismatch'}
    if($taskStreams.streams[0].r_frame_rate -ne '60/1'){throw 'Exported frame rate mismatch'}
    $taskResults+=[pscustomobject]@{case=$taskCase.name;codec=$taskStreams.streams[0].codec_name;width=$taskStreams.streams[0].width;height=$taskStreams.streams[0].height;frames=$taskReport.frames;borderHidden=$true;audioTracks=3;liveDenoiseToggle=$true}
    Write-Output ('PASS '+$taskCase.name+' '+$taskStreams.streams[0].width+'x'+$taskStreams.streams[0].height+' / 60 fps / 3 audio tracks / hidden border')
}
$taskResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskTest 'summary.json') -Encoding utf8NoBOM
Write-Output $taskTest

param([switch]$NativeOnly,[switch]$SkipTests,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$taskTools=Join-Path $taskRoot '.tools'
$taskBuild=Join-Path $taskRoot 'build'
$taskDist=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $taskRoot 'dist\Apex回放'}
New-Item -ItemType Directory -Path $taskBuild,$taskDist -Force | Out-Null
$taskSetup=Join-Path $taskTools 'msvc\setup_x64.bat'
if (!(Test-Path -LiteralPath $taskSetup)) { throw 'Portable MSVC is missing; see docs/BUILD.md.' }
$taskEnvironment=& $env:ComSpec /d /s /c ('""{0}" && set"' -f $taskSetup)
foreach($taskLine in $taskEnvironment) {
    if($taskLine -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$taskFf=(Get-ChildItem -LiteralPath (Join-Path $taskTools 'ffmpeg-extract') -Directory | Select-Object -First 1).FullName
$taskCommon=@('/nologo','/std:c++20','/EHsc','/O2','/utf-8','/MT','/permissive-','/D_WIN32_WINNT=0x0A00','/DWINVER=0x0A00','/I'+(Join-Path $taskFf 'include'))
Push-Location $taskBuild
try {
    & cl.exe @taskCommon (Join-Path $taskRoot 'src\native\main.cpp') ('/Fe:'+(Join-Path $taskDist 'ApexPerif.Worker.exe')) /link ('/LIBPATH:'+(Join-Path $taskFf 'lib')) avcodec.lib avformat.lib avutil.lib avfilter.lib swresample.lib swscale.lib d3d11.lib dxgi.lib d3dcompiler.lib windowsapp.lib ole32.lib uuid.lib propsys.lib xinput.lib user32.lib gdi32.lib
    if($LASTEXITCODE -ne 0){throw 'Native worker compilation failed.'}
    if(!$SkipTests) {
        & cl.exe /nologo /std:c++20 /EHsc /O2 /utf-8 /MD (Join-Path $taskRoot 'tests\rules_tests.cpp') ('/Fe:'+(Join-Path $taskBuild 'rules_tests.exe'))
        if($LASTEXITCODE -ne 0){throw 'Rule tests compilation failed.'}
        & (Join-Path $taskBuild 'rules_tests.exe')
        if($LASTEXITCODE -ne 0){throw 'Rule scenario tests failed.'}
        & cl.exe @taskCommon (Join-Path $taskRoot 'tests\native_tests.cpp') ('/Fe:'+(Join-Path $taskBuild 'native_tests.exe')) /link ('/LIBPATH:'+(Join-Path $taskFf 'lib')) avcodec.lib avformat.lib avutil.lib avfilter.lib swresample.lib swscale.lib windowsapp.lib ole32.lib uuid.lib
        if($LASTEXITCODE -ne 0){throw 'Native scenario tests compilation failed.'}
        $env:PATH=(Join-Path $taskFf 'bin')+';'+$env:PATH
        & (Join-Path $taskBuild 'native_tests.exe')
        if($LASTEXITCODE -ne 0){throw 'Native scenario tests failed.'}
    }
}finally{Pop-Location}
Get-ChildItem -LiteralPath (Join-Path $taskFf 'bin') -Filter '*.dll' | Copy-Item -Destination $taskDist -Force
Copy-Item -LiteralPath (Join-Path $taskFf 'bin\ffprobe.exe') -Destination $taskDist -Force
New-Item -ItemType Directory -Path (Join-Path $taskDist 'config'),(Join-Path $taskDist 'licenses') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRoot 'config\rules.v1.json') -Destination (Join-Path $taskDist 'config') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'config\hud.zh.v1.json') -Destination (Join-Path $taskDist 'config') -Force
Copy-Item -LiteralPath (Join-Path $taskFf 'LICENSE.txt') -Destination (Join-Path $taskDist 'licenses\FFmpeg.txt') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'docs\nlohmann-LICENSE.txt') -Destination (Join-Path $taskDist 'licenses') -Force
if(!$NativeOnly) {
    $env:DOTNET_ROOT=Join-Path $taskTools 'dotnet'
    $env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
    & (Join-Path $env:DOTNET_ROOT 'dotnet.exe') publish (Join-Path $taskRoot 'src\ApexPerif.App\ApexPerif.App.csproj') -c Release -r win-x64 --self-contained true -o $taskDist
    if($LASTEXITCODE -ne 0){throw 'WPF publish failed.'}
}
Write-Output $taskDist

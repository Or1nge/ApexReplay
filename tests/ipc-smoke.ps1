param([string]$PackageDirectory=(Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\Apex回放'))
$ErrorActionPreference='Stop'
$taskPipeName='ApexPerif-test-'+[Guid]::NewGuid().ToString('N')
$taskPipe=[IO.Pipes.NamedPipeServerStream]::new($taskPipeName,[IO.Pipes.PipeDirection]::InOut,1,[IO.Pipes.PipeTransmissionMode]::Byte,[IO.Pipes.PipeOptions]::Asynchronous -bor [IO.Pipes.PipeOptions]::CurrentUserOnly)
$taskStart=[Diagnostics.ProcessStartInfo]::new((Join-Path $PackageDirectory 'ApexPerif.Worker.exe'))
$taskStart.UseShellExecute=$false;$taskStart.CreateNoWindow=$true;$taskStart.RedirectStandardError=$true
$taskStart.ArgumentList.Add('--pipe');$taskStart.ArgumentList.Add($taskPipeName)
$taskWorker=[Diagnostics.Process]::Start($taskStart)
$taskReader=$null;$taskWriter=$null
try {
    $taskConnection=$taskPipe.WaitForConnectionAsync()
    if(!$taskConnection.Wait(10000)){throw 'Worker did not connect'}
    $taskReader=[IO.StreamReader]::new($taskPipe,[Text.UTF8Encoding]::new($false),$false,4096,$true)
    $taskWriter=[IO.StreamWriter]::new($taskPipe,[Text.UTF8Encoding]::new($false),4096,$true);$taskWriter.AutoFlush=$true
    function Read-State([string]$State) {
        for($taskI=0;$taskI -lt 20;$taskI++){
            $taskRead=$taskReader.ReadLineAsync();if(!$taskRead.Wait(5000)){throw "Timeout waiting for $State"}
            $taskMessage=$taskRead.Result | ConvertFrom-Json
            if($taskMessage.type -eq 'fatal'){throw $taskMessage.message}
            if($State -eq 'armed' -and $taskMessage.state -in @('waiting','starting','buffering','pending','paused')){return $taskMessage}
            if($taskMessage.type -eq $State -or $taskMessage.state -eq $State){return $taskMessage}
        };throw "State $State missing"
    }
    Read-State 'ready' | Out-Null
    $taskWriter.WriteLine(' {"command":"stop"}');Read-State 'stopped' | Out-Null
    $taskSettings=@{outputDirectory=(Join-Path (Split-Path $PSScriptRoot -Parent) 'artifacts\ipc-output');replayMinutes=30;memoryPercent=60}
    $taskWriter.WriteLine((@{command='start';settings=$taskSettings} | ConvertTo-Json -Compress));Read-State 'armed' | Out-Null
    $taskSettings.replayMinutes=7;$taskSettings.memoryPercent=45
    $taskWriter.WriteLine((@{command='configure';settings=$taskSettings} | ConvertTo-Json -Compress))
    $taskWriter.WriteLine('{"command":"stop"}');Read-State 'stopped' | Out-Null
    $taskWriter.WriteLine((@{command='start';settings=$taskSettings} | ConvertTo-Json -Compress));Read-State 'armed' | Out-Null
    $taskWriter.WriteLine('{"command":"quit"}')
    Read-State 'exiting' | Out-Null
    if(!$taskWorker.WaitForExit(10000)){throw 'Worker failed to exit'}
    if($taskWorker.ExitCode -ne 0){throw $taskWorker.StandardError.ReadToEnd()}
    Write-Output 'PASS named pipe start / configure / stop / restart / quit'
} finally {
    if(!$taskWorker.HasExited){$taskWorker.Kill()}
    if($taskReader){$taskReader.Dispose()};if($taskWriter){$taskWriter.Dispose()};$taskPipe.Dispose();$taskWorker.Dispose()
}

using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
namespace ApexPerif;
public sealed class WorkerClient : IAsyncDisposable
{
    private NamedPipeServerStream? pipe;
    private StreamWriter? writer;
    private Process? worker;
    private readonly SemaphoreSlim sending=new(1,1);
    private readonly CancellationTokenSource cancellation=new();
    private Task? receiving;
    private bool exiting;
    public event Action<JsonElement>? Message;
    public event Action<string>? Failed;
    public string LogPath {get;}
    private readonly object logLock=new();
    public WorkerClient()
    {
        var directory=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"ApexPerif","logs");
        Directory.CreateDirectory(directory);
        LogPath=Path.Combine(directory,$"session-{DateTime.Now:yyyyMMdd-HHmmss}-{Environment.ProcessId}.jsonl");
    }
    private void Log(string text)
    {lock(logLock){try{File.AppendAllText(LogPath,text+Environment.NewLine,Encoding.UTF8);}catch(IOException){}}}
    public async Task StartAsync()
    {
        var name="ApexPerif-"+Guid.NewGuid().ToString("N");
        pipe=new(name,PipeDirection.InOut,1,PipeTransmissionMode.Byte,PipeOptions.Asynchronous|PipeOptions.CurrentUserOnly,65536,65536);
        var path=Path.Combine(AppContext.BaseDirectory,"ApexPerif.Worker.exe");
        if(!File.Exists(path))throw new FileNotFoundException("找不到采集器，请使用完整运行包。",path);
        var start=new ProcessStartInfo(path){UseShellExecute=false,CreateNoWindow=true,RedirectStandardError=true,WorkingDirectory=AppContext.BaseDirectory};
        start.ArgumentList.Add("--pipe");start.ArgumentList.Add(name);
        worker=Process.Start(start)??throw new InvalidOperationException("采集器未能启动");
        _=Task.Run(async()=>{while(await worker.StandardError.ReadLineAsync() is {} line)Log(JsonSerializer.Serialize(new{type="stderr",message=line}));});
        _=Task.Run(async()=>{await worker.WaitForExitAsync();if(!exiting)Failed?.Invoke($"采集器已退出（{worker.ExitCode}）。日志：{LogPath}");});
        using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(15));
        await pipe.WaitForConnectionAsync(timeout.Token);
        writer=new StreamWriter(pipe,new UTF8Encoding(false),4096,true){AutoFlush=true};
        receiving=Task.Run(async()=>
        {
            using var reader=new StreamReader(pipe,Encoding.UTF8,false,4096,true);
            try
            {
                while(await reader.ReadLineAsync(cancellation.Token) is {} line)
                {
                    using var doc=JsonDocument.Parse(line);
                    if(doc.RootElement.GetProperty("type").GetString()!="audio_levels")Log(line);
                    Message?.Invoke(doc.RootElement.Clone());
                }
                if(!exiting)Failed?.Invoke("与采集器的连接中断，请重新启动应用。");
            }
            catch(OperationCanceledException){}
            catch(Exception e){if(!exiting)Failed?.Invoke(e.Message);}
        });
    }
    public async Task SendAsync(string command,object? settings=null)
    {
        if(writer is null)throw new InvalidOperationException("采集器尚未连接。");
        await sending.WaitAsync();
        try{await writer.WriteLineAsync(JsonSerializer.Serialize(new{command,settings}));}finally{sending.Release();}
    }
    public async ValueTask DisposeAsync()
    {
        if(exiting)return;
        exiting=true;
        try
        {
            if(writer is not null)await SendAsync("quit");
            if(worker is {HasExited:false})
            {
                using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(20));
                try{await worker.WaitForExitAsync(timeout.Token);}catch(OperationCanceledException){worker.Kill(true);}
            }
        }
        catch(Exception e){Log(JsonSerializer.Serialize(new{type="shutdown_error",message=e.Message}));}
        try{writer?.Dispose();}catch(Exception e)when(e is IOException or ObjectDisposedException){Log(JsonSerializer.Serialize(new{type="pipe_closed",message=e.Message}));}
        cancellation.Cancel();pipe?.Dispose();if(receiving is not null){try{await receiving;}catch{}}
        worker?.Dispose();cancellation.Dispose();sending.Dispose();
    }
}

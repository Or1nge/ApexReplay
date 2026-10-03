using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Forms=System.Windows.Forms;
namespace ApexPerif;
public partial class MainWindow : Window
{
    private readonly ViewModel model=new();
    private WorkerClient? client;
    private Forms.NotifyIcon? tray;
    private System.Drawing.Icon? trayIcon;
    private bool exiting,settingsLoaded;
    private string? restoredMicrophoneId;
    private int saved;
    private readonly Dictionary<int,string> audioErrors=new();
    private readonly DispatcherTimer changes=new(){Interval=TimeSpan.FromMilliseconds(350)};
    private readonly string settingsPath=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"ApexPerif","settings.json");
    [DllImport("dwmapi.dll")]private static extern int DwmSetWindowAttribute(IntPtr window,int attribute,ref int value,int size);
    public MainWindow()
    {
        var args=Environment.GetCommandLineArgs();
        if(args.Length==5&&args[1]=="--settings-smoke")settingsPath=Path.GetFullPath(args[2]);
        InitializeComponent();DataContext=model;
        SourceInitialized+=(_,_)=>{var enabled=1;DwmSetWindowAttribute(new WindowInteropHelper(this).Handle,20,ref enabled,sizeof(int));};
        Loaded+=LoadedAsync;Closing+=OnClosing;
        model.SettingsChanged+=()=>{if(settingsLoaded){SaveSettings();changes.Stop();changes.Start();}};
        changes.Tick+=async(_,_)=>{changes.Stop();if(model.Running&&client is not null)try{await client.SendAsync("configure",model.Settings());}catch(Exception e){Error(e.Message);}};
    }
    private async void LoadedAsync(object sender,RoutedEventArgs e)
    {
        var args=Environment.GetCommandLineArgs();
        bool integration=args.Length>=3&&args[1]=="--integration-smoke";
        if(args.Length>=3&&args[1]=="--ui-smoke")
        {
            model.Ready=true;model.Running=true;model.OutputDirectory=@"G:\ApexHighlights";
            model.StatusTitle="回放缓存运行中";model.StatusDetail="中文 HUD 已匹配 · 等待精彩交战";
            model.BufferDetail="缓存 143 秒 · 684 MiB · 当前内存上限 47.6 GiB";
            model.LastSaved="已保存 3 段素材 · 最近一次：连续多杀";
            model.ApplyAudioLevels([.12,.04,0],[true,true,true]);
            model.StatusBrush=new SolidColorBrush(System.Windows.Media.Color.FromRgb(68,217,179));
            foreach(var expander in Descendants<System.Windows.Controls.Expander>(this))expander.IsExpanded=true;
            await Task.Delay(500);UpdateLayout();
            var bitmap=new RenderTargetBitmap((int)ActualWidth,(int)ActualHeight,96,96,PixelFormats.Pbgra32);bitmap.Render(this);
            var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(bitmap));using(var file=File.Create(args[2]))png.Save(file);
            Descendants<System.Windows.Controls.ScrollViewer>(this).First().ScrollToBottom();await Task.Delay(150);UpdateLayout();
            bitmap=new RenderTargetBitmap((int)ActualWidth,(int)ActualHeight,96,96,PixelFormats.Pbgra32);bitmap.Render(this);png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(bitmap));
            using(var file=File.Create(Path.ChangeExtension(args[2],"settings.png")))png.Save(file);
            exiting=true;System.Windows.Application.Current.Shutdown();return;
        }
        try
        {
            if(!integration&&File.Exists(settingsPath)){using var settings=JsonDocument.Parse(File.ReadAllText(settingsPath));model.Load(settings.RootElement);restoredMicrophoneId=settings.RootElement.TryGetProperty("microphoneId",out var mic)?mic.GetString():null;}
        }
        catch(Exception){model.StatusDetail="上次设置无法读取，已恢复默认值。";}
        settingsLoaded=!integration;
        if(args.Length==5&&args[1]=="--settings-smoke")
        {
            var expected=Path.Combine(Path.GetDirectoryName(settingsPath)!,"素材 保存目录");
            if(args[3]=="write")
            {
                model.OutputDirectory=expected;model.MicrophoneId="test-microphone";model.MicNoiseSuppression=false;
                model.ShortPre=7;model.ShortPost=4;model.LongPre=13;model.LongPost=8;model.Balance=-3;model.ReplayMinutes=42;model.MemoryPercent=45;
                model.VideoResolution="1080p";model.VideoCodec="h264";model.VideoBitrateMbps=75;model.VideoPreset="p4";
            }
            else
            {
                using var ready=JsonDocument.Parse(args[3]=="read-missing"?"{\"type\":\"ready\",\"microphones\":[]}":"{\"type\":\"ready\",\"microphones\":[{\"id\":\"test-microphone\",\"name\":\"测试麦克风\"}]}");
                Receive(ready.RootElement);await Task.Delay(100);
            }
            bool restored=model.OutputDirectory==expected&&model.MicrophoneId=="test-microphone"&&!model.MicNoiseSuppression&&
                model.ShortPre==7&&model.ShortPost==4&&model.LongPre==13&&model.LongPost==8&&model.Balance==-3&&model.ReplayMinutes==42&&model.MemoryPercent==45&&
                model.VideoResolution=="1080p"&&model.VideoCodec=="h264"&&model.VideoBitrateMbps==75&&model.VideoPreset=="p4";
            File.WriteAllText(args[4],JsonSerializer.Serialize(new{passed=restored,mode=args[3],settings=model.Settings()}));
            // Exit immediately, before the debounced worker configuration runs.
            exiting=true;changes.Stop();System.Windows.Application.Current.Shutdown();return;
        }
        CreateTray();
        client=new WorkerClient();
        client.Message+=message=>Dispatcher.BeginInvoke(()=>Receive(message));
        client.Failed+=message=>Dispatcher.BeginInvoke(()=>{model.Ready=false;model.Running=false;model.ResetAudio("连接中断");Error(message);});
        try{
            await client.StartAsync();if(integration)await IntegrationSmokeAsync(args[2]);
            else if(args.Contains("--resume-capture")&&!string.IsNullOrWhiteSpace(model.OutputDirectory)){
                Directory.CreateDirectory(model.OutputDirectory);SaveSettings();await client.SendAsync("start",model.Settings());model.Running=true;
            }
        }catch(Exception error){Error(error.Message);if(integration){File.WriteAllText(args[2],JsonSerializer.Serialize(new{passed=false,error=error.Message}));await ExitForTestAsync();}}
    }
    private async Task IntegrationSmokeAsync(string output)
    {
        async Task Until(Func<bool> condition){var until=DateTime.UtcNow.AddSeconds(8);while(!condition()){if(DateTime.UtcNow>until)throw new TimeoutException("界面集成状态超时："+model.StatusTitle);await Task.Delay(50);}}
        await Until(()=>model.Ready);model.OutputDirectory=Path.Combine(Path.GetDirectoryName(output)!,"gui-test-output");
        model.ReplayMinutes=30;model.MemoryPercent=60;
        await client!.SendAsync("start",model.Settings());await Until(()=>model.StatusTitle is "等待 Apex" or "回放缓存运行中" or "窗口最小化，暂停画面采集");
        string startedState=model.StatusTitle;
        using(var waiting=JsonDocument.Parse("{\"type\":\"status\",\"state\":\"waiting\"}"))Receive(waiting.RootElement);
        bool waitingMeter=model.GameLevel==0&&model.GameAudioStatus=="等待 Apex";
        using(var sample=JsonDocument.Parse("{\"type\":\"audio_levels\",\"levels\":[0.1,0,0.5],\"audioHealthy\":[true,true,false]}"))Receive(sample.RootElement);
        bool audioDisplay=Math.Abs(model.GameLevel-2.0/3)<.001&&model.GameAudioStatus=="-20.0 dBFS"&&model.MicLevel==0&&model.MicAudioStatus=="静音 / 极低"&&model.DesktopLevel==0&&model.DesktopAudioStatus=="设备不可用";
        Close();bool hidden=!IsVisible&&tray?.Visible==true;Show();
        await client.SendAsync("stop");await Until(()=>!model.Running);
        bool clearedMeter=model.GameLevel==0&&model.GameAudioStatus=="未采集";
        File.WriteAllText(output,JsonSerializer.Serialize(new{passed=hidden&&waitingMeter&&audioDisplay&&clearedMeter,appName=Title,workerReady=true,startedState,trayHide=hidden,stopped=true,waitingMeter,audioDisplay,clearedMeter,replayMinutes=model.ReplayMinutes,memoryPercent=model.MemoryPercent}));
        await ExitForTestAsync();
    }
    private async Task ExitForTestAsync(){exiting=true;changes.Stop();tray?.Dispose();trayIcon?.Dispose();if(client is not null)await client.DisposeAsync();System.Windows.Application.Current.Shutdown();}
    private static IEnumerable<T> Descendants<T>(DependencyObject root)where T:DependencyObject
    {
        for(int i=0;i<VisualTreeHelper.GetChildrenCount(root);i++){var child=VisualTreeHelper.GetChild(root,i);if(child is T item)yield return item;foreach(var nested in Descendants<T>(child))yield return nested;}
    }
    private void CreateTray()
    {
        var menu=new Forms.ContextMenuStrip();
        menu.Items.Add("打开 Apex回放",null,(_,_)=>Dispatcher.Invoke(()=>{Show();WindowState=WindowState.Normal;Activate();}));
        menu.Items.Add("停止采集",null,async(_,_)=>{if(client is not null)try{await client.SendAsync("stop");}catch(Exception error){Error(error.Message);}});
        menu.Items.Add("退出",null,(_,_)=>Dispatcher.Invoke(ExitAsync));
        using(var resource=System.Windows.Application.GetResourceStream(new Uri("pack://application:,,,/Assets/apex-replay.ico")).Stream)
        using(var icon=new System.Drawing.Icon(resource))trayIcon=(System.Drawing.Icon)icon.Clone();
        tray=new Forms.NotifyIcon{Text="Apex回放 · 自动精彩采集",Icon=trayIcon,Visible=true,ContextMenuStrip=menu};
        tray.DoubleClick+=(_,_)=>Dispatcher.Invoke(()=>{Show();WindowState=WindowState.Normal;Activate();});
    }
    private void OnClosing(object? sender,CancelEventArgs e){if(!exiting){SaveSettings();e.Cancel=true;Hide();}}
    private async void ExitAsync()
    {
        if(exiting)return;SaveSettings();exiting=true;changes.Stop();model.Ready=false;model.StatusTitle="正在退出";model.StatusDetail="结束采集并完成待保存素材…";
        tray?.Dispose();trayIcon?.Dispose();if(client is not null)await client.DisposeAsync();System.Windows.Application.Current.Shutdown();
    }
    private void SaveSettings()
    {
        if(!settingsLoaded)return;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(settingsPath)!);
            var temporary=settingsPath+".tmp";File.WriteAllText(temporary,JsonSerializer.Serialize(model.Settings(),new JsonSerializerOptions{WriteIndented=true}));
            File.Move(temporary,settingsPath,true);
        }
        catch(Exception e){Error("设置保存失败："+e.Message);}
    }
    private void Browse_Click(object sender,RoutedEventArgs e)
    {
        var dialog=new Microsoft.Win32.OpenFolderDialog{Title="选择精彩素材的保存目录"};
        if(Directory.Exists(model.OutputDirectory))dialog.InitialDirectory=model.OutputDirectory;
        if(dialog.ShowDialog(this)==true)model.OutputDirectory=dialog.FolderName;
    }
    private async void Start_Click(object sender,RoutedEventArgs e)
    {
        if(client is null)return;
        try
        {
            if(model.Running){await client.SendAsync("stop");return;}
            if(string.IsNullOrWhiteSpace(model.OutputDirectory)){Browse_Click(sender,e);if(string.IsNullOrWhiteSpace(model.OutputDirectory))return;}
            Directory.CreateDirectory(model.OutputDirectory);
            string probe=Path.Combine(model.OutputDirectory,".apexperif-write-"+Guid.NewGuid().ToString("N"));
            using(var f=new FileStream(probe,FileMode.CreateNew,FileAccess.Write,FileShare.None,1,FileOptions.DeleteOnClose))f.WriteByte(0);
            SaveSettings();await client.SendAsync("start",model.Settings());model.Running=true;
            model.StatusTitle="等待 Apex";model.StatusDetail="游戏窗口出现后自动开始缓存。";
        }
        catch(Exception error){Error(error.Message);}
    }
    private void OpenFolder_Click(object sender,RoutedEventArgs e)
    {
        if(Directory.Exists(model.OutputDirectory))Process.Start(new ProcessStartInfo("explorer.exe"){UseShellExecute=true,ArgumentList={model.OutputDirectory}});
    }
    private async void Retry_Click(object sender,RoutedEventArgs e){if(client is not null)try{await client.SendAsync("retry");}catch(Exception error){Error(error.Message);}}
    private void Error(string message)
    {
        model.StatusTitle="需要处理";model.StatusDetail=message;
        model.StatusBrush=new SolidColorBrush(System.Windows.Media.Color.FromRgb(242,162,99));
    }
    private void Receive(JsonElement message)
    {
        string type=message.GetProperty("type").GetString()??"";
        if(type=="ready")
        {
            model.Ready=true;model.StatusTitle="准备就绪";model.StatusDetail="选择目录，然后启动自动采集。";
            var selected=restoredMicrophoneId??model.MicrophoneId;restoredMicrophoneId=null;
            foreach(var mic in message.GetProperty("microphones").EnumerateArray())
                model.Microphones.Add(new(mic.GetProperty("id").GetString()??"",mic.GetProperty("name").GetString()??"麦克风"));
            if(!string.IsNullOrEmpty(selected)&&!model.Microphones.Any(m=>m.Id==selected))model.Microphones.Add(new(selected,"已记住的麦克风（当前未连接）"));
            model.MicrophoneId=selected;
        }
        else if(type=="status")
        {
            string state=message.GetProperty("state").GetString()??"";
            model.Running=state!="stopped";
            if(state is "waiting" or "starting" or "stopping" or "stopped")model.ResetAudio(state switch {"waiting"=>"等待 Apex","starting"=>"正在连接","stopping"=>"正在停止",_=>"未采集"});
            model.StatusBrush=new SolidColorBrush(System.Windows.Media.Color.FromRgb(68,217,179));
            (model.StatusTitle,model.StatusDetail)=state switch
            {
                "waiting"=>("等待 Apex","游戏窗口出现后自动开始缓存。"),
                "starting"=>("正在启动缓存","准备画面采集、硬件编码与三个音轨…"),
                "paused"=>("窗口最小化，暂停画面采集","恢复 Apex 窗口后继续缓存，当前片段已截断。"),
                "pending"=>("精彩片段待确认","正在等待相关后续击倒，合并后保存。"),
                "buffering"=>("回放缓存运行中",message.TryGetProperty("detection",out var d)?d.GetString()??"":""),
                "stopping"=>("正在停止","完成当前候选片段与待保存素材。"),
                _=>("已停止","启动后继续自动匹配 Apex。")
            };
            if(message.TryGetProperty("bufferSeconds",out var seconds))
            {
                double mib=message.GetProperty("bufferBytes").GetDouble()/(1024*1024);
                double budget=message.TryGetProperty("memoryBudgetBytes",out var memory)?memory.GetDouble()/Math.Pow(1024,3):0;
                model.BufferDetail=$"缓存 {seconds.GetDouble():0} 秒 · {mib:0} MiB · 当前内存上限 {budget:0.0} GiB";
                if(message.GetProperty("exporting").GetBoolean())model.StatusTitle="正在保存素材";
                model.Retry=message.GetProperty("failedExports").GetInt32()>0;
                if(model.Retry){model.StatusTitle="素材保存失败，可重试";model.StatusBrush=new SolidColorBrush(System.Windows.Media.Color.FromRgb(242,162,99));}
                if(message.GetProperty("analysisMs").GetDouble()>150)model.StatusDetail+=" · 识别负载较高";
                if(message.TryGetProperty("videoWidth",out var videoWidth)&&message.TryGetProperty("videoHeight",out var videoHeight))model.StatusDetail+=$" · {videoWidth.GetInt32()} × {videoHeight.GetInt32()} / 60 fps";
                if(message.TryGetProperty("captureBorderHidden",out var border)&&!border.GetBoolean())model.StatusDetail+=" · Windows 尚未允许隐藏采集边框";
                var health=message.GetProperty("audioHealthy").EnumerateArray().Select(x=>x.GetBoolean()).ToArray();
                string[] names=["Apex 音轨","麦克风","其他应用音轨"];
                for(int i=0;i<Math.Min(3,health.Length);i++)if(!health[i])model.StatusDetail+=$" · {names[i]}暂不可用";
            }
        }
        else if(type=="audio_levels")
        {
            model.ApplyAudioLevels(message.GetProperty("levels").EnumerateArray().Select(x=>x.GetDouble()).ToArray(),
                message.GetProperty("audioHealthy").EnumerateArray().Select(x=>x.GetBoolean()).ToArray());
        }
        else if(type=="saved")
        {
            saved++;model.LastSaved=$"已保存 {saved} 段素材 · {Path.GetFileName(message.GetProperty("path").GetString())}";
            if(!model.Running)model.StatusTitle="已停止";
        }
        else if(type=="audio_status")
        {
            int track=message.GetProperty("track").GetInt32();
            if(message.GetProperty("healthy").GetBoolean())audioErrors.Remove(track);
            else if(message.TryGetProperty("message",out var reason))audioErrors[track]=reason.GetString()??"音频设备不可用";
        }
        else if(type is "fatal" or "error" or "export_failed")
        {
            Error(message.GetProperty("message").GetString()??"采集失败");
            if(type=="fatal"){model.Running=false;model.ResetAudio("采集已中断");}if(type=="export_failed")model.Retry=true;
        }
    }
}

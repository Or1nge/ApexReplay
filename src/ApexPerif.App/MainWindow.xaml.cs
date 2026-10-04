using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Input;
using System.Windows.Controls;
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
    private bool exiting,settingsLoaded,uiPreview;
    private bool restoringStartup;
    private readonly TaskCompletionSource workerReady=new(TaskCreationOptions.RunContinuationsAsynchronously);
    private string? restoredMicrophoneId;
    private readonly Dictionary<int,string> audioErrors=new();
    private readonly DispatcherTimer changes=new(){Interval=TimeSpan.FromMilliseconds(350)};
    private readonly DispatcherTimer themeClock=new(){Interval=TimeSpan.FromMinutes(1)};
    private readonly string settingsPath=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"ApexPerif","settings.json");
    private static readonly SolidColorBrush GoodBrush=Frozen(61,214,140),BusyBrush=Frozen(91,155,255),IdleBrush=Frozen(107,119,137),WarnBrush=Frozen(245,165,36);
    private static SolidColorBrush Frozen(byte r,byte g,byte b){var brush=new SolidColorBrush(System.Windows.Media.Color.FromRgb(r,g,b));brush.Freeze();return brush;}
    [DllImport("dwmapi.dll")]private static extern int DwmSetWindowAttribute(IntPtr window,int attribute,ref int value,int size);
    public MainWindow()
    {
        var args=Environment.GetCommandLineArgs();
        if(args.Length==5&&args[1] is "--settings-smoke" or "--startup-smoke")settingsPath=Path.GetFullPath(args[2]);
        InitializeComponent();DataContext=model;
        // Settings are read before the window appears so the saved appearance shows without a flash.
        bool integration=args.Length>=3&&args[1]=="--integration-smoke",uiSmoke=args.Length>=3&&args[1]=="--ui-smoke";
        try
        {
            if(!integration&&!uiSmoke&&File.Exists(settingsPath)){using var settings=JsonDocument.Parse(File.ReadAllText(settingsPath));model.Load(settings.RootElement);restoredMicrophoneId=settings.RootElement.TryGetProperty("microphoneId",out var mic)?mic.GetString():null;}
        }
        catch(Exception){model.StatusDetail="设置读取失败，已恢复默认";}
        ApplyTheme();
        model.PropertyChanged+=(_,e)=>{if(e.PropertyName==nameof(ViewModel.Theme))ApplyTheme();};
        themeClock.Tick+=(_,_)=>ApplyTheme();themeClock.Start();
        SourceInitialized+=(_,_)=>{UpdateTitleBar();HwndSource.FromHwnd(new WindowInteropHelper(this).Handle)?.AddHook(WindowMessage);};
        model.StatusBrush=IdleBrush;
        Loaded+=LoadedAsync;Closing+=OnClosing;
        // The folder can change outside the app (clips deleted or moved), so re-read it whenever the window comes back.
        Activated+=(_,_)=>RefreshClips();
        model.PropertyChanged+=(_,e)=>{if(e.PropertyName==nameof(ViewModel.OutputDirectory))RefreshClips();};
        model.SettingsChanged+=()=>{if(settingsLoaded){SaveSettings();changes.Stop();changes.Start();}};
        model.PropertyChanged+=(_,e)=>{
            if(e.PropertyName==nameof(ViewModel.StartWithWindows)&&settingsLoaded&&!restoringStartup&&!args.Any(arg=>arg.EndsWith("-smoke",StringComparison.Ordinal)))
                try{StartupRegistration.SetEnabled(model.StartWithWindows);}catch(Exception error){restoringStartup=true;model.StartWithWindows=StartupRegistration.IsEnabled;restoringStartup=false;Error("开机自启设置失败："+error.Message);}
        };
        changes.Tick+=async(_,_)=>{changes.Stop();if(model.Running&&client is not null)try{await client.SendAsync("configure",model.Settings());}catch(Exception e){Error(e.Message);}};
    }
    private IntPtr WindowMessage(IntPtr hwnd,int message,IntPtr wParam,IntPtr lParam,ref bool handled)
    {
        if((uint)message==App.ShowMessage){ShowInTaskbar=true;Show();WindowState=WindowState.Normal;Activate();handled=true;}
        else if((uint)message==App.ExitMessage){ExitAsync();handled=true;}
        return IntPtr.Zero;
    }
    private async void LoadedAsync(object sender,RoutedEventArgs e)
    {
        var args=Environment.GetCommandLineArgs();
        bool integration=args.Length>=3&&args[1]=="--integration-smoke";
        if(args.Length>=3&&args[1]=="--ui-smoke")
        {
            uiPreview=true;model.Ready=true;model.Running=true;model.OutputDirectory=@"G:\ApexHighlights";
            model.StatusTitle="正在缓存";model.StatusDetail="HUD 已识别";
            model.ApplyBuffer(143,684d*1024*1024,47.6*Math.Pow(1024,3));model.CaptureSize="2560×1440";
            model.SavedCount=3;
            var now=DateTime.Now;
            model.Clips.Add(new(@"G:\ApexHighlights\a.mp4",now.AddMinutes(-4),48L<<20,3,42,true));
            model.Clips.Add(new(@"G:\ApexHighlights\b.mp4",now.AddMinutes(-21),36L<<20,2,31));
            model.Clips.Add(new(@"G:\ApexHighlights\c.mp4",now.AddMinutes(-47),77L<<20,4,65));
            model.Clips.Add(new(@"G:\ApexHighlights\d.mp4",now.AddDays(-1),33L<<20,2));
            model.ApplyAudioLevels([.12,.04,0],[true,true,true]);
            model.DeveloperMode=true;model.CanSaveReplay=true;model.HudLine="伤害 1194 · 击杀 5 · 助攻 1";
            model.DevLogPath=@"G:\ApexHighlights\开发者记录\Apex_dev_20261003_213000_000_4242_0.json";
            model.StatusBrush=GoodBrush;
            themeClock.Stop();
            void Snapshot(string path)
            {
                var bitmap=new RenderTargetBitmap((int)ActualWidth,(int)ActualHeight,96,96,PixelFormats.Pbgra32);bitmap.Render(this);
                var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(bitmap));using var file=File.Create(path);png.Save(file);
            }
            // Overview and settings pages: ui.png / ui.settings.png in dark, ui.light.png / ui.light.settings.png in light.
            foreach(var light in new[]{false,true})
            {
                Themes.Apply(light);UpdateTitleBar();NavHome.IsChecked=true;SettingsScroll.ScrollToTop();await Task.Delay(500);UpdateLayout();
                var path=light?Path.ChangeExtension(args[2],"light.png"):args[2];Snapshot(path);
                NavSettings.IsChecked=true;await Task.Delay(150);UpdateLayout();Snapshot(Path.ChangeExtension(path,"settings.png"));
                SettingsScroll.ScrollToVerticalOffset(SettingsScroll.VerticalOffset+CriteriaSection.TranslatePoint(new System.Windows.Point(),SettingsScroll).Y-20);
                await Task.Delay(150);UpdateLayout();Snapshot(Path.ChangeExtension(path,"criteria.png"));
                SettingsScroll.ScrollToBottom();await Task.Delay(150);UpdateLayout();Snapshot(Path.ChangeExtension(path,"general.png"));
            }
            exiting=true;System.Windows.Application.Current.Shutdown();return;
        }
        settingsLoaded=!integration;
        if(args.Length==5&&args[1]=="--settings-smoke")
        {
            var expected=Path.Combine(Path.GetDirectoryName(settingsPath)!,"素材 保存目录");
            if(args[3]=="write")
            {
                model.OutputDirectory=expected;model.MicrophoneId="test-microphone";model.MicNoiseSuppression=false;model.DeveloperMode=true;
                model.ShortPre=7;model.ShortPost=4;model.LongPre=13;model.LongPost=8;model.Balance=-3;model.ReplayMinutes=42;model.MemoryPercent=45;
                model.VideoResolution="1080p";model.VideoCodec="h264";model.VideoBitrateMbps=75;model.VideoPreset="p4";model.Theme="light";
                model.BurstSeconds=6;model.BurstDamage=300;model.FastBurstSeconds=1.5;model.FastBurstDamage=160;model.StartWithWindows=true;
            }
            else
            {
                using var ready=JsonDocument.Parse(args[3]=="read-missing"?"{\"type\":\"ready\",\"microphones\":[]}":"{\"type\":\"ready\",\"microphones\":[{\"id\":\"test-microphone\",\"name\":\"测试麦克风\"}]}");
                Receive(ready.RootElement);await Task.Delay(100);
            }
            bool restored=model.OutputDirectory==expected&&model.MicrophoneId=="test-microphone"&&!model.MicNoiseSuppression&&model.DeveloperMode&&
                model.ShortPre==7&&model.ShortPost==4&&model.LongPre==13&&model.LongPost==8&&model.Balance==-3&&model.ReplayMinutes==42&&model.MemoryPercent==45&&
                model.VideoResolution=="1080p"&&model.VideoCodec=="h264"&&model.VideoBitrateMbps==75&&model.VideoPreset=="p4"&&model.Theme=="light"&&
                model.BurstSeconds==6&&model.BurstDamage==300&&model.FastBurstSeconds==1.5&&model.FastBurstDamage==160&&model.StartWithWindows;
            File.WriteAllText(args[4],JsonSerializer.Serialize(new{passed=restored,mode=args[3],settings=model.Settings()}));
            // Exit immediately, before the debounced worker configuration runs.
            exiting=true;changes.Stop();System.Windows.Application.Current.Shutdown();return;
        }
        CreateTray();
        if(args.Contains("--startup")){ShowInTaskbar=false;Hide();}
        if(!integration&&!args.Contains("--startup-smoke"))
        {
            restoringStartup=true;
            try{
                if(args.Contains("--enable-startup"))StartupRegistration.SetEnabled(true);
                else if(args.Contains("--disable-startup"))StartupRegistration.SetEnabled(false);
                model.StartWithWindows=StartupRegistration.IsEnabled;
            }catch(Exception error){Error(error.Message);}
            finally{restoringStartup=false;}
        }
        client=new WorkerClient();
        client.Message+=message=>Dispatcher.BeginInvoke(()=>Receive(message));
        client.Failed+=message=>Dispatcher.BeginInvoke(()=>{model.Ready=false;model.Running=false;model.CanSaveReplay=false;model.ResetAudio("连接中断");Error(message);});
        try{
            await client.StartAsync();if(integration)await IntegrationSmokeAsync(args[2]);
            else {
                await workerReady.Task.WaitAsync(TimeSpan.FromSeconds(10));await StartCaptureAsync();
                if(args.Length==5&&args[1]=="--startup-smoke"){
                    var until=DateTime.UtcNow.AddSeconds(8);while(model.StatusTitle is not ("等待 Apex" or "正在缓存" or "已暂停")){if(DateTime.UtcNow>until)throw new TimeoutException("自动采集未开始");await Task.Delay(50);}
                    File.WriteAllText(args[4],JsonSerializer.Serialize(new{passed=model.Running,automaticCapture=model.Running,state=model.StatusTitle,output=model.OutputDirectory}));await ExitForTestAsync();
                }
            }
        }catch(Exception error){Error(error.Message);if(integration||args.Contains("--startup-smoke")){File.WriteAllText(integration?args[2]:args[4],JsonSerializer.Serialize(new{passed=false,error=error.Message}));await ExitForTestAsync();}}
    }
    private async Task IntegrationSmokeAsync(string output)
    {
        async Task Until(Func<bool> condition){var until=DateTime.UtcNow.AddSeconds(8);while(!condition()){if(DateTime.UtcNow>until)throw new TimeoutException("界面集成状态超时："+model.StatusTitle);await Task.Delay(50);}}
        await Until(()=>model.Ready);model.OutputDirectory=Path.Combine(Path.GetDirectoryName(output)!,"gui-test-output");
        model.ReplayMinutes=30;model.MemoryPercent=60;
        await client!.SendAsync("start",model.Settings());await Until(()=>model.StatusTitle is "等待 Apex" or "正在缓存" or "已暂停");
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
    private void ApplyTheme(){if(Themes.Apply(Themes.IsLight(model.Theme,DateTime.Now)))UpdateTitleBar();}
    private void UpdateTitleBar()
    {
        var handle=new WindowInteropHelper(this).Handle;if(handle==IntPtr.Zero)return;
        bool light=Themes.LightActive;
        var dark=light?0:1;DwmSetWindowAttribute(handle,20,ref dark,sizeof(int));
        // Windows 11: title bar matches the window background (COLORREF 0x00BBGGRR).
        var caption=light?0xF2EFEE:0x120F0E;DwmSetWindowAttribute(handle,35,ref caption,sizeof(int));
    }
    private async Task ExitForTestAsync(){exiting=true;changes.Stop();tray?.Dispose();trayIcon?.Dispose();if(client is not null)await client.DisposeAsync();System.Windows.Application.Current.Shutdown();}
    private void CreateTray()
    {
        var menu=new Forms.ContextMenuStrip();
        menu.Items.Add("打开 Apex回放",null,(_,_)=>Dispatcher.Invoke(()=>{ShowInTaskbar=true;Show();WindowState=WindowState.Normal;Activate();}));
        var save=menu.Items.Add("保存全部缓存",null,(_,_)=>Dispatcher.Invoke(()=>SaveReplay_Click(this,new RoutedEventArgs())));save.Enabled=model.CanSaveReplay;
        model.PropertyChanged+=(_,e)=>{if(e.PropertyName==nameof(ViewModel.CanSaveReplay))save.Enabled=model.CanSaveReplay;};
        menu.Items.Add("停止采集",null,async(_,_)=>{if(client is not null)try{await client.SendAsync("stop");}catch(Exception error){Error(error.Message);}});
        menu.Items.Add("退出",null,(_,_)=>Dispatcher.Invoke(ExitAsync));
        using(var resource=System.Windows.Application.GetResourceStream(new Uri("pack://application:,,,/Assets/apex-replay.ico")).Stream)
        using(var icon=new System.Drawing.Icon(resource))trayIcon=(System.Drawing.Icon)icon.Clone();
        tray=new Forms.NotifyIcon{Text="Apex回放",Icon=trayIcon,Visible=true,ContextMenuStrip=menu};
        tray.DoubleClick+=(_,_)=>Dispatcher.Invoke(()=>{ShowInTaskbar=true;Show();WindowState=WindowState.Normal;Activate();});
    }
    private void OnClosing(object? sender,CancelEventArgs e){if(!exiting){SaveSettings();e.Cancel=true;Hide();}}
    private async void ExitAsync()
    {
        if(exiting)return;SaveSettings();exiting=true;changes.Stop();model.Ready=false;model.StatusTitle="正在退出";model.StatusDetail="正在完成待保存的片段";
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
        if(dialog.ShowDialog(this)!=true)return;
        model.OutputDirectory=dialog.FolderName;
        if(model.StatusTitle=="准备就绪")model.StatusDetail="";
    }
    private async Task StartCaptureAsync()
    {
        if(client is null)return;
        if(string.IsNullOrWhiteSpace(model.OutputDirectory))model.OutputDirectory=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos),"Apex回放");
        Directory.CreateDirectory(model.OutputDirectory);
        string probe=Path.Combine(model.OutputDirectory,".apexperif-write-"+Guid.NewGuid().ToString("N"));
        using(var f=new FileStream(probe,FileMode.CreateNew,FileAccess.Write,FileShare.None,1,FileOptions.DeleteOnClose))f.WriteByte(0);
        SaveSettings();await client.SendAsync("start",model.Settings());model.Running=true;
        model.StatusTitle="等待 Apex";model.StatusDetail="游戏窗口出现后自动开始缓存";model.StatusBrush=BusyBrush;
    }
    private async void Start_Click(object sender,RoutedEventArgs e)
    {
        if(client is null)return;
        try
        {
            if(model.Running){await client.SendAsync("stop");return;}
            await StartCaptureAsync();
        }
        catch(Exception error){Error(error.Message);}
    }
    private async void SaveReplay_Click(object sender,RoutedEventArgs e)
    {
        if(client is null||!model.CanSaveReplay)return;
        model.CanSaveReplay=false;
        try{await client.SendAsync("save_replay");}catch(Exception error){Error(error.Message);}
    }
    private void OpenDevFolder_Click(object sender,RoutedEventArgs e)
    {
        try{if(string.IsNullOrWhiteSpace(model.OutputDirectory))return;var directory=Path.Combine(model.OutputDirectory,"开发者记录");
            Directory.CreateDirectory(directory);Process.Start(new ProcessStartInfo("explorer.exe"){UseShellExecute=true,ArgumentList={directory}});}
        catch(Exception error){Error(error.Message);}
    }
    private void NumberInput_KeyDown(object sender,System.Windows.Input.KeyEventArgs e)
    {
        if(e.Key!=Key.Enter||sender is not System.Windows.Controls.TextBox box)return;
        var binding=box.GetBindingExpression(System.Windows.Controls.TextBox.TextProperty);binding?.UpdateSource();binding?.UpdateTarget();Keyboard.ClearFocus();e.Handled=true;
    }
    private void NumberInput_LostFocus(object sender,RoutedEventArgs e)
    {
        if(sender is System.Windows.Controls.TextBox box){var binding=box.GetBindingExpression(System.Windows.Controls.TextBox.TextProperty);binding?.UpdateSource();binding?.UpdateTarget();}
    }
    private const int ClipLimit=30;
    private void RefreshClips()
    {
        if(uiPreview)return;
        var files=SavedClip.Scan(model.OutputDirectory,ClipLimit);
        if(files.Select(f=>f.FullName).SequenceEqual(model.Clips.Select(c=>c.Path),StringComparer.OrdinalIgnoreCase))return;
        var known=model.Clips.ToDictionary(c=>c.Path,StringComparer.OrdinalIgnoreCase);
        model.Clips.Clear();
        // Keep live entries (they know duration and squad wipe) and their thumbnails.
        foreach(var file in files){var clip=known.TryGetValue(file.FullName,out var existing)?existing:SavedClip.FromFile(file);model.Clips.Add(clip);Thumbnails.Request(clip);}
    }
    private void AddClip(SavedClip clip)
    {
        var old=model.Clips.FirstOrDefault(c=>string.Equals(c.Path,clip.Path,StringComparison.OrdinalIgnoreCase));if(old is not null)model.Clips.Remove(old);
        model.Clips.Insert(0,clip);while(model.Clips.Count>ClipLimit)model.Clips.RemoveAt(model.Clips.Count-1);
        Thumbnails.Request(clip);
    }
    private void Clip_Click(object sender,RoutedEventArgs e)
    {
        if((sender as FrameworkElement)?.DataContext is not SavedClip clip)return;
        try{Process.Start(new ProcessStartInfo(clip.Path){UseShellExecute=true});}catch(Exception error){Error(error.Message);}
    }
    private void ShowClip_Click(object sender,RoutedEventArgs e)
    {
        e.Handled=true; // the row behind would otherwise open the video too
        if((sender as FrameworkElement)?.DataContext is not SavedClip clip)return;
        try{Process.Start(new ProcessStartInfo("explorer.exe",$"/select,\"{clip.Path}\""){UseShellExecute=true});}catch(Exception error){Error(error.Message);}
    }
    private void OpenFolder_Click(object sender,RoutedEventArgs e)
    {
        if(Directory.Exists(model.OutputDirectory))Process.Start(new ProcessStartInfo("explorer.exe"){UseShellExecute=true,ArgumentList={model.OutputDirectory}});
    }
    private async void Retry_Click(object sender,RoutedEventArgs e){if(client is not null)try{await client.SendAsync("retry");}catch(Exception error){Error(error.Message);}}
    private void Error(string message)
    {
        model.StatusTitle="需要处理";model.StatusDetail=message;
        model.StatusBrush=WarnBrush;
    }
    private void Receive(JsonElement message)
    {
        string type=message.GetProperty("type").GetString()??"";
        if(type=="ready")
        {
            model.Ready=true;model.StatusTitle="准备就绪";model.StatusDetail=string.IsNullOrWhiteSpace(model.OutputDirectory)?"选择保存位置后即可开始":"";
            var selected=restoredMicrophoneId??model.MicrophoneId;restoredMicrophoneId=null;
            foreach(var mic in message.GetProperty("microphones").EnumerateArray())
                model.Microphones.Add(new(mic.GetProperty("id").GetString()??"",mic.GetProperty("name").GetString()??"麦克风"));
            if(!string.IsNullOrEmpty(selected)&&!model.Microphones.Any(m=>m.Id==selected))model.Microphones.Add(new(selected,"上次使用的麦克风（未连接）"));
            model.MicrophoneId=selected;
            workerReady.TrySetResult();
        }
        else if(type=="status")
        {
            string state=message.GetProperty("state").GetString()??"";
            model.CanSaveReplay=message.TryGetProperty("canSaveReplay",out var canSave)&&canSave.GetBoolean();
            model.DevLogPath=message.TryGetProperty("devLogPath",out var logPath)?logPath.GetString()??"":"";
            if(message.TryGetProperty("hud",out var hud)){
                string Number(string key)=>hud.TryGetProperty(key,out var n)&&n.ValueKind==JsonValueKind.Number?n.GetInt32().ToString():"—";
                model.HudLine=$"伤害 {Number("damage")} · 击杀 {Number("kills")} · 助攻 {Number("assists")}";
            }
            model.Running=state!="stopped";
            if(state is "waiting" or "starting" or "stopping" or "stopped")model.ResetAudio(state switch {"waiting"=>"等待 Apex","starting"=>"正在连接","stopping"=>"正在停止",_=>"未采集"});
            if(state=="stopped")model.ResetBuffer();
            model.StatusBrush=state switch {"buffering" or "pending"=>GoodBrush,"stopped"=>IdleBrush,_=>BusyBrush};
            (model.StatusTitle,model.StatusDetail)=state switch
            {
                "waiting"=>("等待 Apex","游戏窗口出现后自动开始缓存"),
                "starting"=>("正在启动",""),
                "paused"=>("已暂停","Apex 窗口已最小化"),
                "pending"=>("片段待确认","等待可能的后续击倒"),
                "buffering"=>("正在缓存",message.TryGetProperty("detection",out var d)?d.GetString()??"":""),
                "stopping"=>("正在停止","正在完成待保存的片段"),
                _=>("已停止","")
            };
            if(message.TryGetProperty("bufferSeconds",out var seconds))
            {
                model.ApplyBuffer(seconds.GetDouble(),message.GetProperty("bufferBytes").GetDouble(),
                    message.TryGetProperty("memoryBudgetBytes",out var memory)?memory.GetDouble():0);
                if(message.TryGetProperty("videoWidth",out var videoWidth)&&message.TryGetProperty("videoHeight",out var videoHeight))model.CaptureSize=$"{videoWidth.GetInt32()}×{videoHeight.GetInt32()}";
                var notes=new List<string>();
                if(!string.IsNullOrEmpty(model.StatusDetail))notes.Add(model.StatusDetail);
                if(message.GetProperty("exporting").GetBoolean())model.StatusTitle="正在保存片段";
                model.Retry=message.GetProperty("failedExports").GetInt32()>0;
                if(model.Retry){model.StatusTitle="保存失败";model.StatusBrush=WarnBrush;}
                if(message.GetProperty("analysisMs").GetDouble()>150)notes.Add("识别负载较高");
                if(message.TryGetProperty("captureBorderHidden",out var border)&&!border.GetBoolean())notes.Add("无法隐藏采集边框");
                var health=message.GetProperty("audioHealthy").EnumerateArray().Select(x=>x.GetBoolean()).ToArray();
                string[] names=["Apex 音轨","麦克风","其他应用音轨"];
                for(int i=0;i<Math.Min(3,health.Length);i++)if(!health[i])notes.Add(names[i]+"不可用");
                model.StatusDetail=string.Join(" · ",notes);
            }
        }
        else if(type=="audio_levels")
        {
            model.ApplyAudioLevels(message.GetProperty("levels").EnumerateArray().Select(x=>x.GetDouble()).ToArray(),
                message.GetProperty("audioHealthy").EnumerateArray().Select(x=>x.GetBoolean()).ToArray());
            if(message.TryGetProperty("speech",out var speech)&&speech.ValueKind==JsonValueKind.Array)
                model.ApplySpeech(speech.EnumerateArray().Select(x=>new ViewModel.SpeechState(x.GetProperty("active").GetBoolean(),x.GetProperty("learned").GetBoolean(),
                    x.GetProperty("level").GetDouble(),x.GetProperty("gain").GetDouble())).ToArray(),DateTime.UtcNow);
        }
        else if(type=="saved")
        {
            model.SavedCount++;
            var path=message.GetProperty("path").GetString()??"";
            if(path.Length==0)return;
            double seconds=0;bool wipe=false;
            if(message.TryGetProperty("clip",out var clip))
            {
                seconds=clip.GetProperty("end").GetDouble()-clip.GetProperty("start").GetDouble();
                wipe=clip.TryGetProperty("squadWipe",out var w)&&w.GetBoolean();
            }
            AddClip(SavedClip.FromFile(new FileInfo(path),seconds,wipe));
            if(!model.Running)model.StatusTitle="已停止";
        }
        else if(type=="manual_save")
        {
            var state=message.GetProperty("state").GetString();
            if(state is "queued" or "busy"){model.CanSaveReplay=false;model.StatusDetail="正在保存全部缓存";}
            else if(state=="saved")model.StatusDetail="全部缓存已保存";
            else if(state=="unavailable"){model.CanSaveReplay=false;model.StatusDetail="当前没有可保存的缓存";}
            else if(state=="failed")Error(message.TryGetProperty("message",out var error)?error.GetString()??"手动保存失败":"手动保存失败");
        }
        else if(type=="devlog_error"){model.DevLogError=message.GetProperty("message").GetString()??"开发者记录已停用";Error(model.DevLogError);}
        else if(type=="audio_status")
        {
            int track=message.GetProperty("track").GetInt32();
            if(message.GetProperty("healthy").GetBoolean())audioErrors.Remove(track);
            else if(message.TryGetProperty("message",out var reason))audioErrors[track]=reason.GetString()??"音频设备不可用";
        }
        else if(type is "fatal" or "error" or "export_failed")
        {
            Error(message.GetProperty("message").GetString()??"采集失败");
            if(type=="fatal"){model.Running=false;model.CanSaveReplay=false;model.ResetAudio("采集已中断");}if(type=="export_failed"&&(!message.TryGetProperty("retryable",out var retryable)||retryable.GetBoolean()))model.Retry=true;
        }
    }
}

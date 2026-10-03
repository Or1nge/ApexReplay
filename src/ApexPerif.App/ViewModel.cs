using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Windows.Media;
namespace ApexPerif;
public record Microphone(string Id, string Name);
public record VideoOption(string Id,string Name);
public sealed class ViewModel : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    public event Action? SettingsChanged;
    private void Changed([CallerMemberName] string? name=null) => PropertyChanged?.Invoke(this,new(name));
    private bool Set<T>(ref T field,T value,[CallerMemberName]string? name=null)
    { if(EqualityComparer<T>.Default.Equals(field,value))return false;field=value;Changed(name);return true; }
    private string output="",mic="",title="正在准备",detail="",bufferTime="0:00",memoryLine="";
    private double bufferSeconds;
    private string? captureSize;
    private int savedCount;
    private double sp=5,so=3,lp=10,lo=5,balance=0,gameLevel,micLevel,desktopLevel;
    private double replayMinutes=30,memoryPercent=60;
    private double videoBitrateMbps=60;
    private double burstSeconds=5,burstDamage=250,fastBurstSeconds=2,fastBurstDamage=150;
    private string videoResolution="source",videoCodec="hevc",videoPreset="p6",theme="auto";
    private bool ready,running,retry,startWithWindows,micNoiseSuppression=true;
    private string gameAudioStatus="未采集",micAudioStatus="未采集",desktopAudioStatus="未采集",micVoice="",desktopVoice="";
    private readonly DateTime[] lastSpeech=new DateTime[3];
    private System.Windows.Media.Brush statusBrush=new SolidColorBrush(System.Windows.Media.Color.FromRgb(115,131,153));
    public string OutputDirectory { get=>output;set{if(Set(ref output,value)){Changed(nameof(OutputDisplay));SettingsChanged?.Invoke();}} }
    public string OutputDisplay => string.IsNullOrWhiteSpace(output)?"未选择":output;
    public string MicrophoneId { get=>mic;set{if(Set(ref mic,value??""))SettingsChanged?.Invoke();} }
    public bool MicNoiseSuppression {get=>micNoiseSuppression;set{if(Set(ref micNoiseSuppression,value))SettingsChanged?.Invoke();}}
    public bool StartWithWindows {get=>startWithWindows;set{if(Set(ref startWithWindows,value))SettingsChanged?.Invoke();}}
    public double BurstSeconds {get=>burstSeconds;set{if(double.IsFinite(value)&&Set(ref burstSeconds,Math.Clamp(Math.Round(value,1),1,15))){FastBurstSeconds=fastBurstSeconds;SettingsChanged?.Invoke();}}}
    public double BurstDamage {get=>burstDamage;set{if(double.IsFinite(value)&&Set(ref burstDamage,Math.Clamp(Math.Round(value),50,2000)))SettingsChanged?.Invoke();}}
    public double FastBurstSeconds {get=>fastBurstSeconds;set{if(double.IsFinite(value)&&Set(ref fastBurstSeconds,Math.Clamp(Math.Round(value,1),.5,BurstSeconds)))SettingsChanged?.Invoke();}}
    public double FastBurstDamage {get=>fastBurstDamage;set{if(double.IsFinite(value)&&Set(ref fastBurstDamage,Math.Clamp(Math.Round(value),0,1000)))SettingsChanged?.Invoke();}}
    public double VideoBitrateMbps {get=>videoBitrateMbps;set{if(Set(ref videoBitrateMbps,Math.Clamp(Math.Round(value),10,200)))SettingsChanged?.Invoke();}}
    public string VideoResolution {get=>videoResolution;set{if(Set(ref videoResolution,value is "source" or "1080p" or "1440p" or "2160p"?value:"source")){Changed(nameof(VideoSummary));SettingsChanged?.Invoke();}}}
    public string VideoCodec {get=>videoCodec;set{if(Set(ref videoCodec,value=="h264"?"h264":"hevc")){Changed(nameof(VideoSummary));SettingsChanged?.Invoke();}}}
    public string VideoPreset {get=>videoPreset;set{if(Set(ref videoPreset,value is "p4" or "p5" or "p6"?value:"p6"))SettingsChanged?.Invoke();}}
    public string VideoSummary=>$"{(VideoResolution=="source"&&captureSize is not null?captureSize:VideoResolution switch {"1080p"=>"1080p","1440p"=>"1440p","2160p"=>"4K",_=>"游戏分辨率"})} · 60 fps · {(VideoCodec=="h264"?"H.264":"HEVC")}";
    /// <summary>Actual capture size reported by the worker; shown instead of "游戏分辨率" while capturing.</summary>
    public string? CaptureSize {get=>captureSize;set{if(Set(ref captureSize,value))Changed(nameof(VideoSummary));}}
    public bool VideoSettingsEnabled=>!Running;
    /// <summary>"auto" (by local time), "light" or "dark".</summary>
    public string Theme {get=>theme;set{if(Set(ref theme,value is "light" or "dark"?value:"auto"))SettingsChanged?.Invoke();}}
    public IReadOnlyList<VideoOption> VideoResolutions {get;}=[new("source","跟随游戏"),new("1080p","1080p · 1920 × 1080"),new("1440p","1440p · 2560 × 1440"),new("2160p","4K · 3840 × 2160")];
    public IReadOnlyList<VideoOption> VideoCodecs {get;}=[new("hevc","HEVC / H.265"),new("h264","H.264")];
    public IReadOnlyList<VideoOption> VideoPresets {get;}=[new("p4","性能优先"),new("p5","均衡"),new("p6","画质优先")];
    public double ShortPre {get=>sp;set{if(Set(ref sp,Math.Clamp(value,0,20)))SettingsChanged?.Invoke();}}
    public double ShortPost {get=>so;set{if(Set(ref so,Math.Clamp(value,0,15)))SettingsChanged?.Invoke();}}
    public double LongPre {get=>lp;set{if(Set(ref lp,Math.Clamp(value,0,30)))SettingsChanged?.Invoke();}}
    public double LongPost {get=>lo;set{if(Set(ref lo,Math.Clamp(value,0,20)))SettingsChanged?.Invoke();}}
    public double Balance {get=>balance;set{if(Set(ref balance,Math.Clamp(value,-12,12)))SettingsChanged?.Invoke();}}
    public double ReplayMinutes {get=>replayMinutes;set{if(Set(ref replayMinutes,Math.Clamp(Math.Round(value),1,120))){Changed(nameof(BufferProgress));SettingsChanged?.Invoke();}}}
    public double MemoryPercent {get=>memoryPercent;set{if(Set(ref memoryPercent,Math.Clamp(Math.Round(value),10,90)))SettingsChanged?.Invoke();}}
    public bool Ready {get=>ready;set=>Set(ref ready,value);}
    public bool Running {get=>running;set{if(Set(ref running,value)){Changed(nameof(StartButtonText));Changed(nameof(VideoSettingsEnabled));}}}
    public string StartButtonText=>running?"停止采集":"开始采集";
    public string StatusTitle {get=>title;set=>Set(ref title,value);}
    public string StatusDetail {get=>detail;set=>Set(ref detail,value);}
    public string BufferTime {get=>bufferTime;set=>Set(ref bufferTime,value);}
    /// <summary>Buffered history as a fraction of the configured maximum.</summary>
    public double BufferProgress=>Math.Clamp(bufferSeconds/(ReplayMinutes*60),0,1);
    public string MemoryLine {get=>memoryLine;set=>Set(ref memoryLine,value);}
    /// <summary>Clips saved since the app started.</summary>
    public int SavedCount {get=>savedCount;set=>Set(ref savedCount,value);}
    public ObservableCollection<SavedClip> Clips {get;}=[];
    public System.Windows.Media.Brush StatusBrush {get=>statusBrush;set=>Set(ref statusBrush,value);}
    public double GameLevel {get=>gameLevel;set=>Set(ref gameLevel,value);}
    public double MicLevel {get=>micLevel;set=>Set(ref micLevel,value);}
    public double DesktopLevel {get=>desktopLevel;set=>Set(ref desktopLevel,value);}
    public string GameAudioStatus {get=>gameAudioStatus;private set=>Set(ref gameAudioStatus,value);}
    public string MicAudioStatus {get=>micAudioStatus;private set=>Set(ref micAudioStatus,value);}
    public string DesktopAudioStatus {get=>desktopAudioStatus;private set=>Set(ref desktopAudioStatus,value);}
    // Speech balancing of the microphone and other-apps tracks: measured speaking level and applied gain.
    public string MicVoice {get=>micVoice;private set=>Set(ref micVoice,value);}
    public string DesktopVoice {get=>desktopVoice;private set=>Set(ref desktopVoice,value);}
    public void ResetAudio(string status)
    {
        GameLevel=MicLevel=DesktopLevel=0;
        GameAudioStatus=MicAudioStatus=DesktopAudioStatus=status;
        MicVoice=DesktopVoice="";Array.Clear(lastSpeech);
    }
    public record SpeechState(bool Active,bool Learned,double Level,double Gain);
    public void ApplySpeech(SpeechState[] speech,DateTime now)
    {
        if(speech.Length!=3)return;
        string Display(int i)
        {
            var s=speech[i];if(s.Active)lastSpeech[i]=now;
            bool talking=now-lastSpeech[i]<TimeSpan.FromSeconds(.6);
            if(!s.Learned||!double.IsFinite(s.Level)||!double.IsFinite(s.Gain))return talking?"检测到人声 · 正在测量说话音量":"等待人声";
            return $"{(talking?"说话中":"人声")} {s.Level:0} dB · 平衡 {s.Gain:+0.0;-0.0;0.0} dB";
        }
        MicVoice=Display(1);DesktopVoice=Display(2);
    }
    public void ApplyAudioLevels(double[] peaks,bool[] healthy)
    {
        if(peaks.Length!=3||healthy.Length!=3)return;
        (double level,string status) Display(int i)
        {
            if(!healthy[i])return (0,"设备不可用");
            double peak=double.IsFinite(peaks[i])?Math.Clamp(peaks[i],0,1):0;
            if(peak<=.001)return (0,"静音 / 极低");
            double db=20*Math.Log10(peak);
            return (Math.Clamp((db+60)/60,0,1),$"{db:0.0} dBFS");
        }
        (GameLevel,GameAudioStatus)=Display(0);
        (MicLevel,MicAudioStatus)=Display(1);
        (DesktopLevel,DesktopAudioStatus)=Display(2);
    }
    public void ApplyBuffer(double seconds,double bytes,double budgetBytes)
    {
        bufferSeconds=Math.Max(0,seconds);Changed(nameof(BufferProgress));
        var time=TimeSpan.FromSeconds(bufferSeconds);
        BufferTime=$"{(int)time.TotalMinutes}:{time.Seconds:00}";
        string size=bytes>=1024d*1024*1024?$"{bytes/Math.Pow(1024,3):0.0} GiB":$"{bytes/(1024*1024):0} MiB";
        MemoryLine=budgetBytes>0?$"{size} · 内存上限 {budgetBytes/Math.Pow(1024,3):0.0} GiB":size;
    }
    public void ResetBuffer(){bufferSeconds=0;Changed(nameof(BufferProgress));BufferTime="0:00";MemoryLine="";CaptureSize=null;}
    public bool Retry {get=>retry;set{if(Set(ref retry,value))Changed(nameof(RetryVisibility));}}
    public System.Windows.Visibility RetryVisibility=>retry?System.Windows.Visibility.Visible:System.Windows.Visibility.Collapsed;
    public ObservableCollection<Microphone> Microphones {get;}=[new("","默认通信设备")];
    public object Settings()=>new {outputDirectory=OutputDirectory,microphoneId=MicrophoneId,micNoiseSuppression=MicNoiseSuppression,videoResolution=VideoResolution,videoCodec=VideoCodec,videoBitrateMbps=VideoBitrateMbps,videoPreset=VideoPreset,shortPre=ShortPre,shortPost=ShortPost,longPre=LongPre,longPost=LongPost,balance=Balance,controllerFire="LB",replayMinutes=ReplayMinutes,memoryPercent=MemoryPercent,theme=Theme,startWithWindows=StartWithWindows,burstSeconds=BurstSeconds,burstDamage=BurstDamage,fastBurstSeconds=FastBurstSeconds,fastBurstDamage=FastBurstDamage};
    public void Load(JsonElement s)
    {
        if(s.TryGetProperty("outputDirectory",out var o))OutputDirectory=o.GetString()??"";
        if(s.TryGetProperty("microphoneId",out var m))MicrophoneId=m.GetString()??"";
        if(s.TryGetProperty("micNoiseSuppression",out var noise))MicNoiseSuppression=noise.GetBoolean();
        if(s.TryGetProperty("videoResolution",out var resolution))VideoResolution=resolution.GetString()??"source";
        if(s.TryGetProperty("videoCodec",out var codec))VideoCodec=codec.GetString()??"hevc";
        if(s.TryGetProperty("videoBitrateMbps",out var bitrate))VideoBitrateMbps=bitrate.GetDouble();
        if(s.TryGetProperty("videoPreset",out var preset))VideoPreset=preset.GetString()??"p6";
        if(s.TryGetProperty("shortPre",out var a))ShortPre=a.GetDouble();
        if(s.TryGetProperty("shortPost",out var b))ShortPost=b.GetDouble();
        if(s.TryGetProperty("longPre",out var c))LongPre=c.GetDouble();
        if(s.TryGetProperty("longPost",out var d))LongPost=d.GetDouble();
        if(s.TryGetProperty("balance",out var e))Balance=e.GetDouble();
        if(s.TryGetProperty("replayMinutes",out var f))ReplayMinutes=f.GetDouble();
        if(s.TryGetProperty("memoryPercent",out var g))MemoryPercent=g.GetDouble();
        if(s.TryGetProperty("theme",out var t))Theme=t.GetString()??"auto";
        if(s.TryGetProperty("startWithWindows",out var startup))StartWithWindows=startup.GetBoolean();
        if(s.TryGetProperty("burstSeconds",out var window))BurstSeconds=window.GetDouble();
        if(s.TryGetProperty("burstDamage",out var damage))BurstDamage=damage.GetDouble();
        if(s.TryGetProperty("fastBurstSeconds",out var fastWindow))FastBurstSeconds=fastWindow.GetDouble();
        if(s.TryGetProperty("fastBurstDamage",out var fastDamage))FastBurstDamage=fastDamage.GetDouble();
    }
}

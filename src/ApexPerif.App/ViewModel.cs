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
    private string output="",mic="",title="正在准备",detail="连接本地采集器…",buffer="最长 30 分钟 · 闲置内存的 60%",last="";
    private double sp=5,so=3,lp=10,lo=5,balance=0,gameLevel,micLevel,desktopLevel;
    private double replayMinutes=30,memoryPercent=60;
    private double videoBitrateMbps=60;
    private string videoResolution="source",videoCodec="hevc",videoPreset="p6";
    private bool ready,running,retry,micNoiseSuppression=true;
    private string gameAudioStatus="未采集",micAudioStatus="未采集",desktopAudioStatus="未采集";
    private string audioMeterHint="启动采集并找到 Apex 窗口后，显示三个音轨的实时电平。";
    private System.Windows.Media.Brush statusBrush=new SolidColorBrush(System.Windows.Media.Color.FromRgb(115,131,153));
    public string OutputDirectory { get=>output;set{if(Set(ref output,value)){Changed(nameof(OutputDisplay));SettingsChanged?.Invoke();}} }
    public string OutputDisplay => string.IsNullOrWhiteSpace(output)?"选择一个素材目录":output;
    public string MicrophoneId { get=>mic;set{if(Set(ref mic,value??""))SettingsChanged?.Invoke();} }
    public bool MicNoiseSuppression {get=>micNoiseSuppression;set{if(Set(ref micNoiseSuppression,value))SettingsChanged?.Invoke();}}
    public double VideoBitrateMbps {get=>videoBitrateMbps;set{if(Set(ref videoBitrateMbps,Math.Clamp(Math.Round(value),10,200)))SettingsChanged?.Invoke();}}
    public string VideoResolution {get=>videoResolution;set{if(Set(ref videoResolution,value is "source" or "1080p" or "1440p" or "2160p"?value:"source")){Changed(nameof(VideoSummary));SettingsChanged?.Invoke();}}}
    public string VideoCodec {get=>videoCodec;set{if(Set(ref videoCodec,value=="h264"?"h264":"hevc")){Changed(nameof(VideoSummary));SettingsChanged?.Invoke();}}}
    public string VideoPreset {get=>videoPreset;set{if(Set(ref videoPreset,value is "p4" or "p5" or "p6"?value:"p6"))SettingsChanged?.Invoke();}}
    public string VideoSummary=>$"{VideoResolution switch {"1080p"=>"1080p","1440p"=>"1440p","2160p"=>"4K",_=>"原始分辨率"}} · 60 fps · {(VideoCodec=="h264"?"H.264":"HEVC")}";
    public bool VideoSettingsEnabled=>!Running;
    public IReadOnlyList<VideoOption> VideoResolutions {get;}=[new("source","跟随游戏分辨率（默认）"),new("1080p","1080p · 1920 × 1080"),new("1440p","1440p · 2560 × 1440"),new("2160p","4K · 3840 × 2160")];
    public IReadOnlyList<VideoOption> VideoCodecs {get;}=[new("hevc","HEVC / H.265"),new("h264","H.264（兼容性更广）")];
    public IReadOnlyList<VideoOption> VideoPresets {get;}=[new("p4","性能优先"),new("p5","均衡"),new("p6","画质优先")];
    public double ShortPre {get=>sp;set{if(Set(ref sp,Math.Clamp(value,0,20)))SettingsChanged?.Invoke();}}
    public double ShortPost {get=>so;set{if(Set(ref so,Math.Clamp(value,0,15)))SettingsChanged?.Invoke();}}
    public double LongPre {get=>lp;set{if(Set(ref lp,Math.Clamp(value,0,30)))SettingsChanged?.Invoke();}}
    public double LongPost {get=>lo;set{if(Set(ref lo,Math.Clamp(value,0,20)))SettingsChanged?.Invoke();}}
    public double Balance {get=>balance;set{if(Set(ref balance,Math.Clamp(value,-12,12)))SettingsChanged?.Invoke();}}
    public double ReplayMinutes {get=>replayMinutes;set{if(Set(ref replayMinutes,Math.Clamp(Math.Round(value),1,120)))SettingsChanged?.Invoke();}}
    public double MemoryPercent {get=>memoryPercent;set{if(Set(ref memoryPercent,Math.Clamp(Math.Round(value),10,90)))SettingsChanged?.Invoke();}}
    public bool Ready {get=>ready;set=>Set(ref ready,value);}
    public bool Running {get=>running;set{if(Set(ref running,value)){Changed(nameof(StartButtonText));Changed(nameof(VideoSettingsEnabled));}}}
    public string StartButtonText=>running?"停止采集":"启动自动采集";
    public string StatusTitle {get=>title;set=>Set(ref title,value);}
    public string StatusDetail {get=>detail;set=>Set(ref detail,value);}
    public string BufferDetail {get=>buffer;set=>Set(ref buffer,value);}
    public string LastSaved {get=>last;set=>Set(ref last,value);}
    public System.Windows.Media.Brush StatusBrush {get=>statusBrush;set=>Set(ref statusBrush,value);}
    public double GameLevel {get=>gameLevel;set=>Set(ref gameLevel,value);}
    public double MicLevel {get=>micLevel;set=>Set(ref micLevel,value);}
    public double DesktopLevel {get=>desktopLevel;set=>Set(ref desktopLevel,value);}
    public string GameAudioStatus {get=>gameAudioStatus;private set=>Set(ref gameAudioStatus,value);}
    public string MicAudioStatus {get=>micAudioStatus;private set=>Set(ref micAudioStatus,value);}
    public string DesktopAudioStatus {get=>desktopAudioStatus;private set=>Set(ref desktopAudioStatus,value);}
    public string AudioMeterHint {get=>audioMeterHint;private set=>Set(ref audioMeterHint,value);}
    public void ResetAudio(string status)
    {
        GameLevel=MicLevel=DesktopLevel=0;
        GameAudioStatus=MicAudioStatus=DesktopAudioStatus=status;
        AudioMeterHint="启动采集并找到 Apex 窗口后，显示三个音轨的实时电平。";
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
        AudioMeterHint="显示录入音轨的峰值电平，每秒刷新约 10 次；条越长，声音越大。";
    }
    public bool Retry {get=>retry;set{if(Set(ref retry,value))Changed(nameof(RetryVisibility));}}
    public System.Windows.Visibility RetryVisibility=>retry?System.Windows.Visibility.Visible:System.Windows.Visibility.Collapsed;
    public ObservableCollection<Microphone> Microphones {get;}=[new("","默认通信麦克风")];
    public object Settings()=>new {outputDirectory=OutputDirectory,microphoneId=MicrophoneId,micNoiseSuppression=MicNoiseSuppression,videoResolution=VideoResolution,videoCodec=VideoCodec,videoBitrateMbps=VideoBitrateMbps,videoPreset=VideoPreset,shortPre=ShortPre,shortPost=ShortPost,longPre=LongPre,longPost=LongPost,balance=Balance,controllerFire="LB",replayMinutes=ReplayMinutes,memoryPercent=MemoryPercent};
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
    }
}

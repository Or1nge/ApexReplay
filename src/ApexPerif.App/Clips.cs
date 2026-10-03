using System.Collections.Concurrent;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
namespace ApexPerif;
/// <summary>One exported MP4 shown in the recent clips list.</summary>
public sealed class SavedClip : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    private ImageSource? thumbnail;
    public string Path {get;}
    public string Title {get;}
    public string Detail {get;}
    public string Duration {get;}
    public bool SquadWipe {get;}
    public ImageSource? Thumbnail {get=>thumbnail;set{thumbnail=value;PropertyChanged?.Invoke(this,new(nameof(Thumbnail)));}}
    public SavedClip(string path,DateTime time,long bytes,int kills,double seconds=0,bool squadWipe=false,bool highDamage=false)
    {
        Path=path;SquadWipe=squadWipe;
        Title=highDamage?(kills>0?$"高伤害 · {kills} 名敌人":"高伤害 · 助攻"):kills>0?$"{kills} 名敌人":"精彩片段";
        var day=(DateTime.Today-time.Date).Days;
        string when=day switch {0=>$"今天 {time:HH:mm}",1=>$"昨天 {time:HH:mm}",_=>time.Year==DateTime.Today.Year?$"{time.Month}月{time.Day}日 {time:HH:mm}":$"{time:yyyy/M/d}"};
        Detail=bytes>0?$"{when} · {Size(bytes)}":when;
        Duration=seconds>0?$"{(int)(seconds/60)}:{(int)seconds%60:00}":"";
    }
    private static string Size(long bytes)=>bytes>=1L<<30?$"{bytes/(double)(1L<<30):0.0} GB":$"{Math.Max(1,bytes>>20)} MB";
    /// <summary>Files are named Apex_日期_时间_毫秒_类型_人数_进程_序号.mp4 by the worker.</summary>
    public static SavedClip FromFile(FileInfo file,double seconds=0,bool squadWipe=false)
    {
        var parts=System.IO.Path.GetFileNameWithoutExtension(file.Name).Split('_');
        int kills=parts.Length>=6&&int.TryParse(parts[5],out var k)?k:0;
        return new(file.FullName,file.LastWriteTime,file.Exists?file.Length:0,kills,seconds,squadWipe,parts.Length>=5&&parts[4]=="高伤害");
    }
    /// <summary>Newest clips in the output folder; partially written files are skipped.</summary>
    public static List<FileInfo> Scan(string directory,int limit)
    {
        try
        {
            if(string.IsNullOrWhiteSpace(directory)||!Directory.Exists(directory))return [];
            return new DirectoryInfo(directory).EnumerateFiles("Apex_*.mp4").Where(f=>!f.Name.EndsWith(".partial.mp4",StringComparison.OrdinalIgnoreCase))
                .OrderByDescending(f=>f.LastWriteTimeUtc).Take(limit).ToList();
        }
        catch(Exception e)when(e is IOException or UnauthorizedAccessException){return [];}
    }
}
/// <summary>Video thumbnails from the Windows shell (the same ones Explorer shows), loaded on a background STA thread.</summary>
public static class Thumbnails
{
    private static readonly BlockingCollection<(SavedClip clip,Dispatcher dispatcher)> queue=new();
    static Thumbnails()
    {
        var thread=new Thread(()=>{foreach(var (clip,dispatcher) in queue.GetConsumingEnumerable()){var image=Load(clip.Path,320,180);if(image is not null)dispatcher.BeginInvoke(()=>clip.Thumbnail=image);}})
            {IsBackground=true,Name="Thumbnails"};
        thread.SetApartmentState(ApartmentState.STA);thread.Start();
    }
    public static void Request(SavedClip clip){if(clip.Thumbnail is null)queue.Add((clip,Dispatcher.CurrentDispatcher));}
    private static BitmapSource? Load(string path,int width,int height)
    {
        IShellItemImageFactory? factory=null;
        try
        {
            SHCreateItemFromParsingName(path,IntPtr.Zero,typeof(IShellItemImageFactory).GUID,out factory);
            // Thumbnail only: no generic file icon when Windows cannot decode the video (e.g. HEVC without the codec extension).
            if(factory.GetImage(new NativeSize{Width=width,Height=height},BiggerSizeOk|ThumbnailOnly,out var bitmap)!=0)return null;
            try{var source=Imaging.CreateBitmapSourceFromHBitmap(bitmap,IntPtr.Zero,Int32Rect.Empty,BitmapSizeOptions.FromEmptyOptions());source.Freeze();return source;}
            finally{DeleteObject(bitmap);}
        }
        catch(Exception){return null;}
        finally{if(factory is not null)Marshal.ReleaseComObject(factory);}
    }
    private const int BiggerSizeOk=0x1,ThumbnailOnly=0x8;
    [StructLayout(LayoutKind.Sequential)]private struct NativeSize{public int Width,Height;}
    [ComImport,Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItemImageFactory{[PreserveSig]int GetImage(NativeSize size,int flags,out IntPtr bitmap);}
    [DllImport("shell32.dll",CharSet=CharSet.Unicode,PreserveSig=false)]
    private static extern void SHCreateItemFromParsingName(string path,IntPtr bindContext,[MarshalAs(UnmanagedType.LPStruct)]Guid riid,out IShellItemImageFactory factory);
    [DllImport("gdi32.dll")]private static extern bool DeleteObject(IntPtr handle);
}

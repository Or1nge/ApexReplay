using Microsoft.Win32;
using System.IO;
namespace ApexPerif;
public static class StartupRegistration
{
    private const string RunKey=@"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName="ApexReplay";
    public static bool IsEnabled
    {
        get{using var key=Registry.CurrentUser.OpenSubKey(RunKey);return key?.GetValue(ValueName) is string command&&!string.IsNullOrWhiteSpace(command);}
    }
    public static void SetEnabled(bool enabled)
    {
        using var key=Registry.CurrentUser.CreateSubKey(RunKey,true);
        if(enabled)
        {
            string executable=Path.Combine(AppContext.BaseDirectory,"Apex回放.exe");
            string command=$"\"{executable}\" --startup";
            if(command.Length>260)throw new IOException("程序路径过长，请将运行包移到较短的路径后启用开机自启。");
            key.SetValue(ValueName,command,RegistryValueKind.String);
        }
        else key.DeleteValue(ValueName,false);
    }
}

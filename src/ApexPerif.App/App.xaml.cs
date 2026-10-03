namespace ApexPerif;
public partial class App : System.Windows.Application
{
    private Mutex? instance;
    private bool ownsInstance;
    [System.Runtime.InteropServices.DllImport("user32.dll",CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
    private static extern uint RegisterWindowMessage(string name);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr window,uint message,IntPtr wParam,IntPtr lParam);
    public static readonly uint ShowMessage=RegisterWindowMessage("ApexReplay.ShowWindow");
    public static readonly uint ExitMessage=RegisterWindowMessage("ApexReplay.Exit");
    protected override void OnStartup(System.Windows.StartupEventArgs e)
    {
        if(e.Args.Contains("--quit")){PostMessage(new IntPtr(0xffff),ExitMessage,IntPtr.Zero,IntPtr.Zero);Shutdown();return;}
        bool smoke=e.Args.Any(arg=>arg.EndsWith("-smoke",StringComparison.Ordinal));
        if(!smoke)
        {
            var user=System.Security.Principal.WindowsIdentity.GetCurrent().User?.Value??Environment.UserName;
            instance=new Mutex(true,@"Local\ApexReplay-"+user,out ownsInstance);
            if(!ownsInstance){if(!e.Args.Contains("--startup"))PostMessage(new IntPtr(0xffff),ShowMessage,IntPtr.Zero,IntPtr.Zero);Shutdown();return;}
        }
        base.OnStartup(e);
    }
    protected override void OnExit(System.Windows.ExitEventArgs e)
    {
        if(ownsInstance)instance?.ReleaseMutex();instance?.Dispose();base.OnExit(e);
    }
}

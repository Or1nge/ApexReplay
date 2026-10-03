using System.Globalization;
using System.Windows;
using System.Windows.Data;
namespace ApexPerif;
public static class Themes
{
    private static bool? current;
    public static bool LightActive=>current==true;
    /// <summary>"auto" follows the local clock: light from 07:00 to 19:00, dark otherwise.</summary>
    public static bool IsLight(string mode,DateTime now)=>mode switch {"light"=>true,"dark"=>false,_=>now.Hour is >=7 and <19};
    /// <summary>Swaps the palette dictionary; returns false when that palette is already active.</summary>
    public static bool Apply(bool light)
    {
        if(current==light)return false;
        current=light;
        System.Windows.Application.Current.Resources.MergedDictionaries[0]=new ResourceDictionary{Source=new Uri($"pack://application:,,,/Themes/{(light?"Light":"Dark")}.xaml")};
        return true;
    }
}
/// <summary>Binds a radio button to one value of a string property (ConverterParameter).</summary>
public sealed class EqualsConverter : IValueConverter
{
    public object Convert(object? value,Type targetType,object? parameter,CultureInfo culture)=>Equals(value?.ToString(),parameter?.ToString());
    public object ConvertBack(object? value,Type targetType,object? parameter,CultureInfo culture)=>value is true?parameter??System.Windows.Data.Binding.DoNothing:System.Windows.Data.Binding.DoNothing;
}

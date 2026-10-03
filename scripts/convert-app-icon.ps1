param(
    [string]$Source=(Join-Path (Split-Path $PSScriptRoot -Parent) 'src/ApexPerif.App/Assets/apex-replay.png'),
    [string]$Destination=(Join-Path (Split-Path $PSScriptRoot -Parent) 'src/ApexPerif.App/Assets/apex-replay.ico')
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$taskSource=[Drawing.Bitmap]::new($Source)
$taskSizes=@(16,20,24,32,40,48,64,128,256)
$taskFrames=[Collections.Generic.List[byte[]]]::new()
try {
    foreach($taskSize in $taskSizes){
        $taskBitmap=[Drawing.Bitmap]::new($taskSize,$taskSize,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $taskGraphics=[Drawing.Graphics]::FromImage($taskBitmap)
        $taskStream=[IO.MemoryStream]::new()
        try {
            $taskGraphics.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
            $taskGraphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
            $taskGraphics.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::Half
            $taskGraphics.DrawImage($taskSource,[Drawing.Rectangle]::new(0,0,$taskSize,$taskSize),0,0,$taskSource.Width,$taskSource.Height,[Drawing.GraphicsUnit]::Pixel)
            $taskBitmap.Save($taskStream,[Drawing.Imaging.ImageFormat]::Png)
            $taskFrames.Add($taskStream.ToArray())
        } finally {$taskStream.Dispose();$taskGraphics.Dispose();$taskBitmap.Dispose()}
    }
    $taskFile=[IO.File]::Create($Destination)
    $taskWriter=[IO.BinaryWriter]::new($taskFile)
    try {
        $taskWriter.Write([uint16]0);$taskWriter.Write([uint16]1);$taskWriter.Write([uint16]$taskSizes.Count)
        $taskOffset=6+16*$taskSizes.Count
        for($taskIndex=0;$taskIndex -lt $taskSizes.Count;$taskIndex++){
            $taskDimension=if($taskSizes[$taskIndex] -eq 256){0}else{$taskSizes[$taskIndex]}
            $taskWriter.Write([byte]$taskDimension);$taskWriter.Write([byte]$taskDimension)
            $taskWriter.Write([byte]0);$taskWriter.Write([byte]0)
            $taskWriter.Write([uint16]1);$taskWriter.Write([uint16]32)
            $taskWriter.Write([uint32]$taskFrames[$taskIndex].Length);$taskWriter.Write([uint32]$taskOffset)
            $taskOffset+=$taskFrames[$taskIndex].Length
        }
        foreach($taskFrame in $taskFrames){$taskWriter.Write($taskFrame)}
    } finally {$taskWriter.Dispose();$taskFile.Dispose()}
} finally {$taskSource.Dispose()}
Get-Item -LiteralPath $Destination | Select-Object FullName,Length

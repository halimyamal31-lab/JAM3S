$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$dir = Join-Path $PSScriptRoot 'branding'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
function New-Art([int]$w,[int]$h,[string]$file,[bool]$small) {
  $bmp = New-Object System.Drawing.Bitmap($w,$h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::FromArgb(8,9,13))
  $rect = New-Object System.Drawing.Rectangle(0,0,$w,$h)
  $metal = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect,[System.Drawing.Color]::FromArgb(210,217,228),[System.Drawing.Color]::FromArgb(29,34,43),90)
  $g.FillRectangle($metal,0,0,$w,$h)
  $g.FillRectangle([System.Drawing.Brushes]::Black,5,5,$w-10,$h-10)
  for($i=1;$i -le 7;$i++){
    $pen=New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(18,255,15,53),[single]($i*2))
    $g.DrawRectangle($pen,9,9,$w-19,$h-19);$pen.Dispose()
  }
  $red=New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255,24,56),2)
  $g.DrawRectangle($red,9,9,$w-19,$h-19)
  if(!$small){
    for($j=0;$j -lt 4;$j++){
      $yy=120+$j*36
      $g.FillRectangle($metal,15,$yy,$w-30,30)
      $g.FillRectangle([System.Drawing.Brushes]::Black,17,$yy+2,$w-34,26)
      $g.DrawLine($red,20,$yy+2,$w-20,$yy+2)
    }
  }
  $cx=$w/2; $cy=$h*.58; $r=[Math]::Min($w*.34,$h*.27)
  if($small){$cy=$h/2;$r=$h*.30}
  $ring=New-Object System.Drawing.RectangleF([single]($cx-$r),[single]($cy-$r),[single](2*$r),[single](2*$r))
  if($small){$g.FillEllipse($metal,$ring)
  $inside=New-Object System.Drawing.RectangleF([single]($cx-$r*.76),[single]($cy-$r*.76),[single](1.52*$r),[single](1.52*$r))
  $g.FillEllipse([System.Drawing.Brushes]::Black,$inside)
  $g.DrawEllipse($red,$ring)
  $g.DrawLine($red,[single]$cx,[single]($cy-$r*.7),[single]$cx,[single]($cy-$r*.28))}
  $font=New-Object System.Drawing.Font('Arial',[single]$(if($small){12}else{25}),[System.Drawing.FontStyle]::Bold)
  $fmt=New-Object System.Drawing.StringFormat
  $fmt.Alignment=[System.Drawing.StringAlignment]::Center
  if(!$small){
    $g.DrawString('JAM3S',$font,$metal,(New-Object System.Drawing.RectangleF(0,35,$w,45)),$fmt)
    $sub=New-Object System.Drawing.Font('Arial',10,[System.Drawing.FontStyle]::Bold)
    $brush=New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,40,65))
    $g.DrawString('CHROME INFERNO',$sub,$brush,(New-Object System.Drawing.RectangleF(0,86,$w,30)),$fmt)
    $g.DrawString('VOCAL ENGINE  /  2.4',$sub,[System.Drawing.Brushes]::Silver,(New-Object System.Drawing.RectangleF(0,($h-57),$w,30)),$fmt)
    $names=@('TONE','DYNAMICS','CHARACTER','SPACE')
    for($j=0;$j -lt 4;$j++){
      $g.DrawString($names[$j],$sub,[System.Drawing.Brushes]::Silver,(New-Object System.Drawing.RectangleF(0,(125+$j*36),$w,24)),$fmt)
    }
    $sub.Dispose();$brush.Dispose()
  }
  $bmp.Save($file,[System.Drawing.Imaging.ImageFormat]::Bmp)
  $fmt.Dispose();$font.Dispose();$red.Dispose();$metal.Dispose();$g.Dispose();$bmp.Dispose()
}
New-Art 164 314 (Join-Path $dir 'wizard.bmp') $false
New-Art 55 55 (Join-Path $dir 'header.bmp') $true
$b=New-Object System.Drawing.Bitmap(32,32)
$g=[System.Drawing.Graphics]::FromImage($b)
$g.Clear([System.Drawing.Color]::FromArgb(12,12,17))
$p=New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255,24,56),3)
$g.DrawEllipse($p,4,4,24,24)
$g.DrawLine($p,16,6,16,16)
$ic=[System.Drawing.Icon]::FromHandle($b.GetHicon())
$fs=[System.IO.File]::Create((Join-Path $dir 'jam3s.ico'))
$ic.Save($fs);$fs.Dispose();$ic.Dispose();$p.Dispose();$g.Dispose();$b.Dispose()

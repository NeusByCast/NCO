$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class SetupUi {
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
 [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect r);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
function Find-Control([IntPtr]$Root, [string]$Pattern) {
 $script:control = [IntPtr]::Zero
 [SetupUi]::EnumChildWindows($Root, [SetupUi+EnumProc]{param($h,$p)
  $text = New-Object System.Text.StringBuilder 512
  [void][SetupUi]::GetWindowText($h,$text,512)
  if ($text.ToString().Replace('&','').Trim() -like $Pattern) { $script:control=$h; return $false }
  return $true
 },[IntPtr]::Zero) | Out-Null
 return $script:control
}
function Click-Control([IntPtr]$Root, [string]$Pattern) {
 $h=Find-Control $Root $Pattern
 if ($h -eq [IntPtr]::Zero) { throw "Installer control absent: $Pattern" }
 [void][SetupUi]::PostMessage($h,0x00F5,[IntPtr]::Zero,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 450
}
$wizardProcess=$null
$p=Start-Process 'output/NCO-Setup.exe' -ArgumentList '/SP- /NORESTART /LANG=english' -PassThru
try {
 $deadline=(Get-Date).AddSeconds(20)
 $script:wizard=[IntPtr]::Zero
 do {
  [void][SetupUi]::EnumWindows([SetupUi+EnumProc]{param($h,$l)
   $cls=New-Object System.Text.StringBuilder 256
   $caption=New-Object System.Text.StringBuilder 512
   [void][SetupUi]::GetClassName($h,$cls,256)
   [void][SetupUi]::GetWindowText($h,$caption,512)
   if ($cls.ToString() -eq 'TWizardForm' -and $caption.ToString() -like '*NCO*' -and [SetupUi]::IsWindowVisible($h)) {
    $script:wizard=$h;return $false
   }
   return $true
  },[IntPtr]::Zero)
  if (!$script:wizard) { Start-Sleep -Milliseconds 200 }
 } while (!$script:wizard -and (Get-Date) -lt $deadline)
 $root=$script:wizard
 if (!$root) { throw 'Installer did not create its wizard' }
 [uint32]$wizardProcessId=0
 [void][SetupUi]::GetWindowThreadProcessId($root,[ref]$wizardProcessId)
 $wizardProcess=Get-Process -Id $wizardProcessId
 [void][SetupUi]::SetForegroundWindow($root)
 New-Item -ItemType Directory -Force nco-self-test | Out-Null
 for ($page=0; $page -lt 5; $page++) {
  Start-Sleep -Milliseconds 400
  $r=New-Object SetupUi+Rect
  [void][SetupUi]::GetWindowRect($root,[ref]$r)
  $image=New-Object System.Drawing.Bitmap ($r.Right-$r.Left),($r.Bottom-$r.Top)
  $g=[System.Drawing.Graphics]::FromImage($image)
  $g.CopyFromScreen($r.Left,$r.Top,0,0,$image.Size)
  $image.Save((Join-Path $PWD "nco-self-test/installer-$page.png"))
  $g.Dispose();$image.Dispose()
  if ($page -eq 1) { Click-Control $root 'I accept the agreement' }
  if ($page -lt 4) { Click-Control $root 'Next*' }
 }
 'PASS interactive installer welcome/license/destination/tasks/ready pages opened; screenshots captured for contrast review' | Set-Content nco-self-test/installer-ui-results.txt
 Click-Control $root 'Cancel'
 $script:dialogs=New-Object System.Collections.Generic.List[System.IntPtr]
 [SetupUi]::EnumWindows([SetupUi+EnumProc]{param($h,$l)
  [uint32]$owner=0
  [void][SetupUi]::GetWindowThreadProcessId($h,[ref]$owner)
  if ($owner -eq $wizardProcessId) { $script:dialogs.Add($h) }
  return $true
 },[IntPtr]::Zero) | Out-Null
 foreach ($dialog in $script:dialogs) {
  $yes=Find-Control $dialog 'Yes'
  if ($yes -ne [IntPtr]::Zero) { [void][SetupUi]::SendMessage($yes,0x00F5,[IntPtr]::Zero,[IntPtr]::Zero) }
 }
 if (!$p.WaitForExit(5000)) { throw 'Installer preview did not close after cancellation' }
} finally {
 if ($wizardProcess -and !$wizardProcess.HasExited) { $wizardProcess.Kill();$wizardProcess.WaitForExit() }
 if (!$p.HasExited) { $p.Kill();$p.WaitForExit() }
}

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public class SetupUi {
 delegate bool EnumProc(IntPtr h, IntPtr p);
 [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll",SetLastError=true)] static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll",SetLastError=true)] static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 public static int[] Bounds(IntPtr h) {
  Rect r;
  if (!GetWindowRect(h,out r)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
  return new int[]{r.Left,r.Top,r.Right-r.Left,r.Bottom-r.Top};
 }
 public static uint ProcessId(IntPtr h) { uint p;GetWindowThreadProcessId(h,out p);return p; }
 public static IntPtr FindWizard() {
  IntPtr result=IntPtr.Zero;
  EnumWindows((h,p)=> {
   var cls=new StringBuilder(256);var title=new StringBuilder(512);
   GetClassName(h,cls,256);GetWindowText(h,title,512);
   if (cls.ToString()=="TWizardForm" && title.ToString().Contains("NCO") && IsWindowVisible(h)) {
    var r=Bounds(h);
    if (r[2]>400 && r[3]>250) {result=h;return false;}
   }
   return true;
  },IntPtr.Zero);
  return result;
 }
 public static bool Click(IntPtr root,string caption) {
  IntPtr result=IntPtr.Zero;
  EnumChildWindows(root,(h,p)=> {
   var text=new StringBuilder(512);GetWindowText(h,text,512);
   if (IsWindowVisible(h) && text.ToString().Replace("&","").Trim().StartsWith(caption,StringComparison.OrdinalIgnoreCase)) {
    result=h;return false;
   }
   return true;
  },IntPtr.Zero);
  return result!=IntPtr.Zero && PostMessage(result,0x00F5,IntPtr.Zero,IntPtr.Zero);
 }
 public static void ConfirmExit(uint process) {
  EnumWindows((h,p)=> {if (ProcessId(h)==process) Click(h,"Yes");return true;},IntPtr.Zero);
 }
}
'@
function Click-Control([IntPtr]$Root, [string]$Caption) {
 if (![SetupUi]::Click($Root,$Caption)) { throw "Installer control absent or inaccessible: $Caption" }
 Start-Sleep -Milliseconds 450
}
$wizardProcess=$null
$p=Start-Process 'output/NCO-Setup.exe' -ArgumentList '/SP- /NORESTART /LANG=english /CURRENTUSER' -PassThru
try {
 $deadline=(Get-Date).AddSeconds(20)
 do {
  $root=[SetupUi]::FindWizard()
  if ($root -eq [IntPtr]::Zero) {Start-Sleep -Milliseconds 200}
 } while ($root -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
 if ($root -eq [IntPtr]::Zero) {throw 'Installer did not create its visible wizard'}
 $wizardProcessId=[SetupUi]::ProcessId($root)
 if ($wizardProcessId -lt 4) {throw 'Invalid installer process id'}
 $wizardProcess=Get-Process -Id $wizardProcessId
 [void][SetupUi]::SetForegroundWindow($root)
 New-Item -ItemType Directory -Force nco-self-test | Out-Null
 for ($page=0; $page -lt 5; $page++) {
  Start-Sleep -Milliseconds 400
  $r=[SetupUi]::Bounds($root)
  $image=[System.Drawing.Bitmap]::new([int]$r[2],[int]$r[3])
  $g=[System.Drawing.Graphics]::FromImage($image)
  $g.CopyFromScreen([int]$r[0],[int]$r[1],0,0,$image.Size)
  $image.Save((Join-Path $PWD "nco-self-test/installer-$page.png"))
  $g.Dispose();$image.Dispose()
  if ($page -eq 1) {Click-Control $root 'I accept the agreement'}
  if ($page -lt 4) {Click-Control $root 'Next'}
 }
 'PASS interactive installer welcome/license/destination/tasks/ready pages opened; screenshots captured for contrast review' | Set-Content nco-self-test/installer-ui-results.txt
 Click-Control $root 'Cancel'
 [SetupUi]::ConfirmExit($wizardProcessId)
 if (!$p.WaitForExit(5000)) {throw 'Installer preview did not close after cancellation'}
} finally {
 try {if ($wizardProcess -and !$wizardProcess.HasExited) {$wizardProcess.Kill();$wizardProcess.WaitForExit()}} catch {Write-Warning "Wizard cleanup: $_"}
 try {if (!$p.HasExited) {$p.Kill();$p.WaitForExit()}} catch {Write-Warning "Launcher cleanup: $_"}
}

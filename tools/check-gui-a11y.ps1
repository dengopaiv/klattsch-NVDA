# What a screen reader sees in the sample generator, checked automatically.
#
#   powershell -File tools/check-gui-a11y.ps1 [-Exe build-msvc\klattsch_gui.exe]
#
# Windows PowerShell 5.1, for the Accessibility interop assembly. (A first
# version asked .NET's UI Automation client instead; without Windows'
# client-side proxies loaded it reports every control as a Pane named by its
# own text, which is not what a screen reader reads.)
#
# Launches the generator and asks three questions, without sending a single
# keystroke to the desktop -- so it is safe to run while other windows are
# in front, and it never types into them:
#
#   1. Tab order. Walked with GetNextDlgTabItem, the function IsDialogMessage
#      itself calls when Tab is pressed, from the text box round to the text
#      box again. Every stop must be reached and the walk must come back.
#   2. Names. Each stop is asked for its name and role through MSAA
#      (oleacc's AccessibleObjectFromWindow), which is what NVDA reads
#      standard Win32 controls through, and must have a name. A Win32 edit or combo
#      has none of its own; the accessibility layer takes it from the static
#      label created just before it, so a control made in the wrong order
#      reads as nameless, or as its neighbour. The names are printed so they
#      can be read against the labels.
#   3. Keyboard traps and Escape. Each multi-line edit is asked, with the
#      real WM_GETDLGCODE and a Tab keydown, whether it wants the key -- it
#      must not, or Tab would be typed into it and focus could not leave.
#      The same for Escape, which a multi-line edit answers by closing its
#      parent window.
#      Escape is posted to the generator's own text box, through its own
#      message loop, and the window must still be open afterwards: Escape
#      stops speech here and must not close the program.
#
# It does not replace listening with NVDA running, which is the last step in
# docs/20-generator.md and is done by a person.
#
# The first version of this script pressed Tab with SendKeys after trying to
# bring the window to the front. The window did not come to the front, and
# the keystrokes went to whatever did have it. Never again: nothing here
# depends on focus.

param([string]$Exe = "build-msvc\klattsch_gui.exe")

$ErrorActionPreference = "Stop"
Add-Type -Namespace KlA11y -Name Win -ReferencedAssemblies Accessibility -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetNextDlgTabItem(IntPtr dlg, IntPtr ctl, bool prev);
[DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dlg, int id);
[DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, ref MSG lp);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
[DllImport("oleacc.dll")] public static extern int AccessibleObjectFromWindow(IntPtr h, uint id, ref Guid iid,
    [MarshalAs(UnmanagedType.IUnknown)] out object acc);
[DllImport("oleacc.dll", CharSet = CharSet.Unicode)] public static extern uint GetRoleText(uint role, System.Text.StringBuilder s, uint n);
static Accessibility.IAccessible Acc(IntPtr h) {
  Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");   // IID_IAccessible
  object o; AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref iid, out o);      // OBJID_CLIENT
  return (Accessibility.IAccessible)o;
}
public static string AccName(IntPtr h) { return Acc(h).get_accName(0); }
public static string AccRole(IntPtr h) {
  var s = new System.Text.StringBuilder(64);
  GetRoleText(Convert.ToUInt32(Acc(h).get_accRole(0)), s, 64);
  return s.ToString();
}
[StructLayout(LayoutKind.Sequential)] public struct MSG {
  public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam;
  public uint time; public int x; public int y;
}
'@

$repo = Split-Path $PSScriptRoot -Parent
$path = if ([IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $repo $Exe }
$proc = Start-Process -FilePath $path -PassThru
$failures = 0
function Fail($msg) { $script:failures++; Write-Host "FAIL $msg" }
$W = [KlA11y.Win]

# The generator's control IDs (the enum in klattsch_gui.cpp).
$IDC_TEXT = 101
$IDC_MESSAGES = 113

try {
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 50 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 100
    $proc.Refresh()
    $hwnd = $proc.MainWindowHandle
  }
  if ($hwnd -eq [IntPtr]::Zero) { throw "the generator's window did not appear" }
  Write-Host "window: $($proc.MainWindowTitle)"

  # 1 and 2: the tab order, and the name of every stop ------------------------
  $text = $W::GetDlgItem($hwnd, $IDC_TEXT)
  $stops = @()
  $h = $text
  for ($i = 0; $i -lt 60; $i++) {
    $cls = New-Object System.Text.StringBuilder 64
    [void]$W::GetClassName($h, $cls, 64)
    $stops += [pscustomobject]@{
      Id = $W::GetDlgCtrlID($h); Class = $cls.ToString()
      Type = $W::AccRole($h); Name = $W::AccName($h)
    }
    $h = $W::GetNextDlgTabItem($hwnd, $h, $false)
    if ($h -eq $text) { break }
  }
  Write-Host "`nTab order, and the role and name MSAA gives each stop:"
  $n = 0
  foreach ($s in $stops) {
    $n++
    Write-Host ("  {0,2}. {1,-14} {2}" -f $n, $s.Type, $s.Name)
    if ([string]::IsNullOrWhiteSpace($s.Name)) { Fail "stop $n ($($s.Class) id $($s.Id)) has no name" }
  }
  if ($h -ne $text) { Fail "the tab order does not come back round to the text box" }
  $expected = 1 + 1 + 2 + 11 + 5 + 1   # text, checkbox, 2 combos, 11 settings, 5 buttons, messages
  if ($stops.Count -ne $expected) { Fail "$($stops.Count) tab stops, expected $expected" }
  $dupes = $stops | Group-Object Name | Where-Object Count -gt 1
  foreach ($d in $dupes) { Fail "two stops share the name '$($d.Name)'" }

  # 3: no keyboard trap, and Escape keeps the window ---------------------------
  $WM_GETDLGCODE = 0x0087; $WM_KEYDOWN = 0x0100; $VK_TAB = 0x09; $VK_ESCAPE = 0x1B
  $DLGC_WANTALLKEYS = 0x0004; $DLGC_WANTTAB = 0x0002
  Write-Host ""
  foreach ($id in @($IDC_TEXT, $IDC_MESSAGES)) {
    foreach ($key in @(@('Tab', $VK_TAB), @('Escape', $VK_ESCAPE))) {
      $edit = $W::GetDlgItem($hwnd, $id)
      $m = New-Object KlA11y.Win+MSG
      $m.hwnd = $edit; $m.message = $WM_KEYDOWN; $m.wParam = [IntPtr]$key[1]
      $code = [int64]$W::SendMessage($edit, $WM_GETDLGCODE, [IntPtr]$key[1], [ref]$m)
      if ($code -band ($DLGC_WANTALLKEYS -bor $DLGC_WANTTAB)) {
        Fail ("control {0} keeps {1} for itself (DLGC 0x{2:x})" -f $id, $key[0], $code)
      } else {
        Write-Host ("control {0}: gives up {1} (DLGC 0x{2:x})" -f $id, $key[0], $code)
      }
    }
  }
  [void]$W::PostMessage($text, $WM_KEYDOWN, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 500
  $proc.Refresh()
  if ($proc.HasExited -or -not $W::IsWindow($hwnd)) { Fail "Escape closed the window" }
  else { Write-Host "Escape: the window is still open" }
} finally {
  if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}

if ($failures) { Write-Host "`n$failures failure(s)"; exit 1 }
Write-Host "`naccessibility: $($stops.Count) stops, all named, Tab goes round, no trap, Escape keeps the window"

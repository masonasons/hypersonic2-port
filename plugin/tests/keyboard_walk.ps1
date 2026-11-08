# Keyboard and screen-reader check of the standalone app through UI Automation (what NVDA / JAWS / Narrator read).
# Starts "HS2 Port.exe", checks that focus lands in the search box, walks the Tab order, then tries search, Enter,
# arrows, type-ahead, the Ctrl shortcuts, the part selector and a combi load with real scan codes. Keys are only sent while the app's window is in
# the foreground (otherwise it stops). Opens a window for about half a minute.
# Usage: powershell -ExecutionPolicy Bypass -File plugin\tests\keyboard_walk.ps1
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
 [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
 public static void Key(byte vk, bool down, bool ext) { keybd_event(vk, (byte)MapVirtualKey(vk, 0), (down ? 0u : 2u) | (ext ? 1u : 0u), UIntPtr.Zero); }
 public static void Combo(byte mod, byte vk, bool ext) { Key(mod, true, false); Key(vk, true, ext); Key(vk, false, ext); Key(mod, false, false); }
 public static void Combo2(byte m1, byte m2, byte vk, bool ext) { Key(m1, true, false); Key(m2, true, false); Key(vk, true, ext); Key(vk, false, ext); Key(m2, false, false); Key(m1, false, false); } }
"@
$exe = 'C:\stuff\Hypersonic 2\build_plugin\plugin\HS2Port_artefacts\Release\Standalone\HS2 Port.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 7
$p.Refresh()
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500
function Send([string]$k) { if ([W]::GetForegroundWindow() -ne $p.MainWindowHandle) { "ABORT: plugin window is not in the foreground"; Stop-Process -Id $p.Id; exit 1 }; [System.Windows.Forms.SendKeys]::SendWait($k) }
function Ctrl([byte]$vk, [bool]$ext = $false) { if ([W]::GetForegroundWindow() -ne $p.MainWindowHandle) { "ABORT: plugin window is not in the foreground"; Stop-Process -Id $p.Id; exit 1 }; [W]::Combo(0x11, $vk, $ext) }
function Selected { foreach ($e in [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Descendants, (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)))) { if ($e.Current.ControlType.ProgrammaticName -eq 'ControlType.List') { $names = @(); foreach ($r in $e.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)) { try { if ($r.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) { $names += $r.Current.Name } } catch {} }; return ('list "' + $e.Current.Name + '", selected row: ' + ($names -join '; ')) } } }
function Focused { $f = [System.Windows.Automation.AutomationElement]::FocusedElement; if ($f) { $c = $f.Current; $v=''; try { $v = ' = ' + $f.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value } catch {}; $c.ControlType.ProgrammaticName.Replace('ControlType.','') + ' "' + $c.Name + '"' + $v } else { '(none)' } }
"start: " + (Focused)
Start-Sleep -Milliseconds 600
"after the first timer tick: " + (Focused)
for ($i = 1; $i -le 34; $i++) { Send('{TAB}'); Start-Sleep -Milliseconds 150; "tab $i : " + (Focused) }
# Search and list
Ctrl 0x46; Start-Sleep -Milliseconds 200; "ctrl+f: " + (Focused)
Send('rubber bass'); Start-Sleep -Milliseconds 1200
[W]::Key(0x0D,$true,$false); [W]::Key(0x0D,$false,$false); Start-Sleep -Milliseconds 300; "enter: " + (Focused) + ' / ' + (Selected)
[W]::Key(0x28,$true,$true); [W]::Key(0x28,$false,$true); Start-Sleep -Milliseconds 2500; "down: " + (Focused) + ' / ' + (Selected)
[W]::Key(0x26,$true,$true); [W]::Key(0x26,$false,$true); Start-Sleep -Milliseconds 300; "up: " + (Focused)
Ctrl 0x49; Start-Sleep -Milliseconds 300; "ctrl+i: " + (Focused)
$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
$all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
foreach ($e in $all) {
  $n = $e.Current.Name
  if ($n -eq 'Patch information' -or $n -eq 'Status') {
    try { $tp = $e.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern); "$n text: " + $tp.DocumentRange.GetText(400).Replace("`n"," | ") } catch { "$n : no text pattern" }
  }
  if ($e.Current.ControlType.ProgrammaticName -eq 'ControlType.List') { "list items visible: " + $e.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition).Count }
}
Ctrl 0x22 $true; Start-Sleep -Milliseconds 2500
foreach ($e in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) { if ($e.Current.Name -eq 'Status') { try { "after ctrl+pgdn status: " + $e.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange.GetText(300) } catch {} } }
Ctrl 0x4C; Start-Sleep -Milliseconds 300; "ctrl+l: " + (Focused)
Ctrl 0x46; Start-Sleep -Milliseconds 200; Send('{BACKSPACE}'); Start-Sleep -Milliseconds 1200
Ctrl 0x4C; Send('m'); Start-Sleep -Milliseconds 300; "ctrl+l then m: " + (Focused) + ' / ' + (Selected)
Send('e'); Start-Sleep -Milliseconds 300; "then e: " + (Focused) + ' / ' + (Selected)
# Parts
function Info { foreach ($e in [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Descendants, (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)))) { if ($e.Current.Name -eq 'Patch information') { try { return $e.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange.GetText(300).Replace("`n"," | ") } catch {} } } }
Ctrl 0x50; Start-Sleep -Milliseconds 300; "ctrl+p: " + (Focused)
[W]::Key(0x28,$true,$true); [W]::Key(0x28,$false,$true); Start-Sleep -Milliseconds 400; "down: " + (Focused)
[W]::Combo2(0x11, 0x10, 0x22, $true); Start-Sleep -Milliseconds 400; "ctrl+shift+pgdn: " + (Focused)
"info: " + (Info)
Ctrl 0x46; Start-Sleep -Milliseconds 200; Send('rubber bass'); Start-Sleep -Milliseconds 1200
[W]::Key(0x0D,$true,$false); [W]::Key(0x0D,$false,$false); Start-Sleep -Milliseconds 300
[W]::Key(0x0D,$true,$false); [W]::Key(0x0D,$false,$false); Start-Sleep -Milliseconds 2500; "enter in list: " + (Focused)
Ctrl 0x50; Start-Sleep -Milliseconds 300; "ctrl+p: " + (Focused)
"info: " + (Info)
for ($i = 1; $i -le 6; $i++) { Send('+{TAB}'); Start-Sleep -Milliseconds 150 }
"shift+tab x6 from the part box: " + (Focused)
# A combi
Ctrl 0x46; Start-Sleep -Milliseconds 200; Send('arctic bubbles'); Start-Sleep -Milliseconds 1200
[W]::Key(0x0D,$true,$false); [W]::Key(0x0D,$false,$false); Start-Sleep -Milliseconds 300; "enter: " + (Focused) + ' / ' + (Selected)
[W]::Key(0x0D,$true,$false); [W]::Key(0x0D,$false,$false); Start-Sleep -Milliseconds 4000
foreach ($e in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) { if ($e.Current.Name -eq 'Status') { try { "combi status: " + $e.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange.GetText(300) } catch {} } }
Ctrl 0x50; Start-Sleep -Milliseconds 300; "ctrl+p: " + (Focused)
[W]::Key(0x28,$true,$true); [W]::Key(0x28,$false,$true); Start-Sleep -Milliseconds 400; "down: " + (Focused)
"info: " + (Info)
Stop-Process -Id $p.Id

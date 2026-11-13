param([Parameter(Mandatory)][int]$AppProcessId)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$element = [System.Windows.Automation.AutomationElement]
$scope = [System.Windows.Automation.TreeScope]::Descendants
$process = Get-Process -Id $AppProcessId
$process.Refresh()
if ($process.MainWindowHandle -eq [IntPtr]::Zero) { throw 'The app window is not open.' }
$root = $element::FromHandle($process.MainWindowHandle)

function Assert-Shortcut($Name, $Accelerator, $AccessKey, $ControlType) {
    $condition = [System.Windows.Automation.PropertyCondition]::new($element::NameProperty, $Name)
    if ($null -ne $ControlType) {
        $role = [System.Windows.Automation.PropertyCondition]::new($element::ControlTypeProperty, $ControlType)
        $condition = [System.Windows.Automation.AndCondition]::new($condition, $role)
    }
    $control = $null
    for ($attempt = 0; $attempt -lt 20 -and $null -eq $control; ++$attempt) {
        $control = $root.FindFirst($scope, $condition)
        if ($null -eq $control) { Start-Sleep -Milliseconds 100 }
    }
    if ($null -eq $control) { throw "Missing control '$Name' under '$($root.Current.Name)'." }
    $actualAccelerator = $control.Current.AcceleratorKey
    $actualAccessKey = $control.Current.AccessKey
    if ($actualAccelerator -cne $Accelerator -or $actualAccessKey -cne $AccessKey) {
        throw "${Name}: expected '$Accelerator' / '$AccessKey'; got '$actualAccelerator' / '$actualAccessKey'."
    }
}

Assert-Shortcut 'Keyboard help' 'F1' ''
Assert-Shortcut 'Previous patch' 'Ctrl+Page Up' ''
Assert-Shortcut 'Next patch' 'Ctrl+Page Down' ''
Assert-Shortcut 'Search patches' 'Ctrl+F' ''
Assert-Shortcut 'Patch information' 'Ctrl+I' ''
Assert-Shortcut 'Preferences' 'Ctrl+,' ''
Assert-Shortcut 'Clear part' '' 'Alt+C'
Assert-Shortcut 'Load selected patch' '' 'Alt+U'
Assert-Shortcut 'Musical Keyboard' 'Ctrl+K' ''
Assert-Shortcut 'Hide to tray' 'Ctrl+H' ''
Assert-Shortcut 'Status' '' 'Alt+S' ([System.Windows.Automation.ControlType]::Edit)
foreach ($control in $root.FindAll($scope, [System.Windows.Automation.Condition]::TrueCondition)) {
    if ($control.Current.AcceleratorKey -and $control.Current.AccessKey) {
        throw "Two shortcuts advertised for '$($control.Current.Name)'."
    }
}
Write-Output 'Windows UI Automation shortcut metadata passed.'

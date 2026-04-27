<#
.SYNOPSIS
    Host-side smoke test for Sunshine window-capture CLI workflow.
    Launches Notepad, waits for its window, resolves the HWND, and
    injects/verifies the Sunshine app entry -- all from one command.

.DESCRIPTION
    Designed to run on the Windows host (DEV-3) over SSH or local terminal.
    Outputs structured JSON so the Mac-side launcher can consume the result.

.PARAMETER Action
    list    - list running windows that Sunshine could capture
    launch  - launch Notepad and return its window info as JSON
    inject  - ensure Notepad (Window) entry exists in Sunshine apps.json
    status  - check if Notepad is running and report window info
    stop    - close Notepad and report

.PARAMETER AppsJsonPath
    Path to Sunshine apps.json. Defaults to the standard install location.

.EXAMPLE
    .\notepad-smoke.ps1 -Action launch
    .\notepad-smoke.ps1 -Action list
    .\notepad-smoke.ps1 -Action inject
    .\notepad-smoke.ps1 -Action status
    .\notepad-smoke.ps1 -Action stop
#>

param(
    [ValidateSet("list", "launch", "inject", "status", "stop")]
    [string]$Action = "launch",

    [string]$AppsJsonPath = "$env:ProgramFiles\Sunshine\config\apps.json"
)

$ErrorActionPreference = "Stop"

function Get-VisibleWindows {
    $results = @()
    $procs = Get-Process | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero -and $_.MainWindowTitle -ne "" }
    foreach ($p in $procs) {
        $results += @{
            hwnd  = "0x" + $p.MainWindowHandle.ToString("X")
            title = $p.MainWindowTitle
            exe   = $p.ProcessName + ".exe"
            pid   = $p.Id.ToString()
        }
    }
    return $results
}

function Find-NotepadWindow {
    $procs = Get-Process -Name "notepad" -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero }

    if ($procs) {
        $p = @($procs)[0]
        return @{
            hwnd  = "0x" + $p.MainWindowHandle.ToString("X")
            title = $p.MainWindowTitle
            exe   = $p.ProcessName + ".exe"
            pid   = $p.Id.ToString()
        }
    }

    $procs = Get-Process | Where-Object {
        $_.MainWindowHandle -ne [IntPtr]::Zero -and $_.MainWindowTitle -like "*Notepad*"
    }
    if ($procs) {
        $p = @($procs)[0]
        return @{
            hwnd  = "0x" + $p.MainWindowHandle.ToString("X")
            title = $p.MainWindowTitle
            exe   = $p.ProcessName + ".exe"
            pid   = $p.Id.ToString()
        }
    }

    return $null
}

function Write-JsonResult {
    param([hashtable]$Data)
    $Data | ConvertTo-Json -Compress
}

switch ($Action) {
    "list" {
        $windows = Get-VisibleWindows
        $output = @()
        foreach ($win in $windows) {
            $output += [PSCustomObject]@{
                hwnd  = $win["hwnd"]
                title = $win["title"]
                exe   = $win["exe"]
                pid   = $win["pid"]
            }
        }
        $output | ConvertTo-Json
    }

    "launch" {
        $existing = Find-NotepadWindow
        if ($existing) {
            Write-JsonResult @{
                status = "already_running"
                hwnd   = $existing["hwnd"]
                title  = $existing["title"]
                exe    = $existing["exe"]
                pid    = $existing["pid"]
            }
            return
        }

        # Launch in the interactive desktop session (SSH runs in session 0 which has no GUI).
        # Use a scheduled task with /IT flag to start in the logged-in user's session.
        $taskName = "SunshineSmoke_Notepad"
        $oldPref = $ErrorActionPreference
        $ErrorActionPreference = "SilentlyContinue"
        & schtasks /Delete /TN $taskName /F *>$null
        $ErrorActionPreference = $oldPref
        & schtasks /Create /TN $taskName /TR "notepad.exe" /SC ONCE /ST 00:00 /IT /F *>$null
        & schtasks /Run /TN $taskName *>$null
        Start-Sleep -Milliseconds 1000
        $ErrorActionPreference = "SilentlyContinue"
        & schtasks /Delete /TN $taskName /F *>$null
        $ErrorActionPreference = $oldPref

        $retries = 0
        $maxRetries = 30
        $win = $null

        while ($retries -lt $maxRetries) {
            Start-Sleep -Milliseconds 500
            $win = Find-NotepadWindow
            if ($win) { break }
            $retries++
            if ($retries % 5 -eq 0) {
                $allNotepad = Get-Process -Name "notepad" -ErrorAction SilentlyContinue
                $handleInfo = if ($allNotepad) {
                    ($allNotepad | ForEach-Object { "pid=$($_.Id) hwnd=$($_.MainWindowHandle) title='$($_.MainWindowTitle)'" }) -join "; "
                } else { "no notepad processes" }
                Write-Host "[debug] retry $retries/$maxRetries - $handleInfo" -ForegroundColor Yellow
            }
        }

        if ($win -eq $null) {
            $allNotepad = Get-Process -Name "notepad" -ErrorAction SilentlyContinue
            $debugInfo = if ($allNotepad) {
                ($allNotepad | ForEach-Object { "pid=$($_.Id) hwnd=$($_.MainWindowHandle)" }) -join "; "
            } else { "no notepad processes found" }
            Write-JsonResult @{
                status  = "error"
                message = "Notepad launched but window not found after $maxRetries retries. Debug: $debugInfo"
                pid     = $proc.Id.ToString()
            }
            exit 1
        }

        Write-JsonResult @{
            status = "launched"
            hwnd   = $win["hwnd"]
            title  = $win["title"]
            exe    = $win["exe"]
            pid    = $win["pid"]
        }
    }

    "inject" {
        $notepadEntry = @{
            name              = "Notepad (Window CLI)"
            cmd               = "notepad.exe"
            "capture-mode"    = "window"
            "window-match"    = "notepad.exe"
            "window-resolution" = "1280x720"
            "auto-detach"     = $true
            "wait-all"        = $true
            "exit-timeout"    = 5
        }

        if (-not (Test-Path $AppsJsonPath)) {
            $altPath = Join-Path $env:ProgramData "Sunshine\apps.json"
            if (Test-Path $altPath) {
                $AppsJsonPath = $altPath
            } else {
                Write-JsonResult @{
                    status = "error"
                    message = "apps.json not found at $AppsJsonPath or $altPath"
                }
                exit 1
            }
        }

        $appsJson = Get-Content $AppsJsonPath -Raw | ConvertFrom-Json

        $exists = $false
        foreach ($app in $appsJson.apps) {
            if ($app.name -eq $notepadEntry.name) {
                $exists = $true
                break
            }
        }

        if ($exists) {
            Write-JsonResult @{
                status   = "already_exists"
                name     = $notepadEntry.name
                path     = $AppsJsonPath
            }
            return
        }

        $newApp = [PSCustomObject]$notepadEntry
        $appsJson.apps += $newApp
        $appsJson | ConvertTo-Json -Depth 10 | Set-Content $AppsJsonPath -Encoding UTF8

        Write-JsonResult @{
            status   = "injected"
            name     = $notepadEntry.name
            path     = $AppsJsonPath
            message  = "Restart SunshineService or refresh apps for changes to take effect"
        }
    }

    "status" {
        $win = Find-NotepadWindow
        if ($win) {
            Write-JsonResult @{
                status = "running"
                hwnd   = $win["hwnd"]
                title  = $win["title"]
                exe    = $win["exe"]
                pid    = $win["pid"]
            }
        } else {
            Write-JsonResult @{
                status = "not_running"
            }
        }
    }

    "stop" {
        $procs = Get-Process -Name "notepad" -ErrorAction SilentlyContinue
        if ($procs) {
            $procs | Stop-Process -Force
            Write-JsonResult @{
                status  = "stopped"
                count   = $procs.Count
            }
        } else {
            Write-JsonResult @{
                status = "not_running"
            }
        }
    }
}

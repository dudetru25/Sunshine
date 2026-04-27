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
    Add-Type @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public class WinEnum {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] static extern int GetWindowTextLength(IntPtr hWnd);
    [DllImport("user32.dll")] static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    public static List<Dictionary<string, string>> GetWindows() {
        var result = new List<Dictionary<string, string>>();
        EnumWindows((hWnd, _) => {
            if (!IsWindowVisible(hWnd)) return true;
            int len = GetWindowTextLength(hWnd);
            if (len == 0) return true;

            var sb = new StringBuilder(len + 1);
            GetWindowText(hWnd, sb, sb.Capacity);
            string title = sb.ToString();

            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            string exeName = "";
            try {
                var proc = Process.GetProcessById((int)pid);
                exeName = proc.ProcessName + ".exe";
            } catch { }

            var dict = new Dictionary<string, string>();
            dict["hwnd"] = "0x" + hWnd.ToString("X");
            dict["title"] = title;
            dict["exe"] = exeName;
            dict["pid"] = pid.ToString();
            result.Add(dict);
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
"@
    return [WinEnum]::GetWindows()
}

function Find-NotepadWindow {
    $windows = Get-VisibleWindows
    foreach ($win in $windows) {
        if ($win["exe"] -eq "notepad.exe" -or $win["title"] -like "*Notepad*") {
            return $win
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

        $proc = Start-Process notepad.exe -PassThru
        $retries = 0
        $maxRetries = 20
        $win = $null

        while ($retries -lt $maxRetries) {
            Start-Sleep -Milliseconds 300
            $win = Find-NotepadWindow
            if ($win) { break }
            $retries++
        }

        if ($win -eq $null) {
            Write-JsonResult @{
                status = "error"
                message = "Notepad launched but window not found after $maxRetries retries"
                pid = $proc.Id.ToString()
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

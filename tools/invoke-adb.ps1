# Runs `adb` with a hard timeout, safely, from an agent or a script.
#
# The hazard: adb daemonizes its own server, and the server inherits the console
# the adb client was started from. If that console belongs to the caller (for
# example an editor's integrated terminal), the server holds it open forever, so the
# caller never sees EOF and the command appears to hang even after adb exits.
# Redirecting the client's output is not enough on its own, because the server
# inherits the console, not just the output handle.
#
# The workaround: run the client in a console of its own, hidden, with its output
# redirected to a file at the OS level. The server then inherits that private
# console instead of the caller's. On timeout the client is killed as a tree.
#
#   . ./tools/invoke-adb.ps1
#   $result = Invoke-Adb -Arguments @("devices")
#   $result.Output
#
# `-TimeoutSeconds` bounds each call, so a genuine hang is reported instead of
# blocking the caller. A command that needs to move a large file should be given a
# correspondingly larger timeout.
function Invoke-Adb
{
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int]$TimeoutSeconds = 60,
        [string]$Adb = "adb"
    )

    $stamp = [Guid]::NewGuid().ToString("N")
    $outFile = Join-Path $env:TEMP "invoke-adb-$stamp.out"
    $cmdFile = Join-Path $env:TEMP "invoke-adb-$stamp.cmd"

    $quoted = $Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $line = '"' + $Adb + '" ' + ($quoted -join ' ') + ' > "' + $outFile + '" 2>&1'
    [System.IO.File]::WriteAllText($cmdFile, "@echo off`r`n$line`r`n")

    # A private console is the whole point, so there is deliberately no
    # `-NoNewWindow` here.
    $process = Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", $cmdFile) -PassThru -WindowStyle Hidden
    $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut)
    {
        & taskkill /PID $process.Id /T /F 2>&1 | Out-Null
        $process.WaitForExit(5000) | Out-Null
    }

    $output = if (Test-Path $outFile) { Get-Content -Raw $outFile } else { "" }
    Remove-Item -Force $outFile, $cmdFile -ErrorAction SilentlyContinue

    [pscustomobject]@{
        ExitCode = if ($timedOut) { $null } else { $process.ExitCode }
        TimedOut = $timedOut
        Output   = $output
    }
}

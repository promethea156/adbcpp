# Measures push and pull throughput against a real device.
#
# For each size, a file of random bytes is pushed to the device, pulled back, and the
# two are compared by SHA-256, so a length or ordering error anywhere in the transfer
# would be caught. Each result is printed as a markdown table row, which is what the
# "Measured transfer performance" section of docs/06-sync-protocol.md records.
#
# Stop the adb server before running this, because adb holds the device's USB interface
# while it runs (blocker 5). Use tools/invoke-adb.ps1 to run adb without hanging the
# terminal:
#
#   . ./tools/invoke-adb.ps1
#   Invoke-Adb -Arguments @("kill-server")
#   ./tools/bench-transfer.ps1
#
#   # a single large file, or a custom size list
#   ./tools/bench-transfer.ps1 -Sizes 1073741824
#   ./tools/bench-transfer.ps1 -Sizes 1048576,16777216
[CmdletBinding()]
param(
    [string]$Example = "build/examples/Release/adbcpp_usb_example.exe",
    [string]$Remote = "/data/local/tmp/adbcpp_bench_test.bin",
    [long[]]$Sizes = @(67108864, 134217728, 268435456, 1073741824)
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Example))
{
    throw "the example was not found: $Example (build it with -DADBCPP_BUILD_EXAMPLES=ON)"
}
$Example = (Resolve-Path -LiteralPath $Example).Path

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) "adbcpp_bench"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$local = Join-Path $scratch "src.bin"
$back = Join-Path $scratch "back.bin"

function New-RandomFile
{
    param([string]$Path, [long]$Size)

    # Written in blocks so that a very large file does not need a matching allocation.
    $stream = [System.IO.File]::Create($Path)
    try
    {
        $written = 0
        while ($written -lt $Size)
        {
            $block = New-Object byte[] ([Math]::Min(67108864, $Size - $written))
            [System.Security.Cryptography.RandomNumberGenerator]::Fill($block)
            $stream.Write($block, 0, $block.Length)
            $written += $block.Length
        }
    }
    finally
    {
        $stream.Dispose()
    }
}

function Format-Size
{
    param([long]$Size)

    if ($Size -ge 1GB) { return "$([Math]::Round($Size / 1GB, 0)) GiB" }
    return "$([Math]::Round($Size / 1MB, 0)) MiB"
}

"| size | push | push rate | pull | pull rate | verified |"
"| ---- | ---- | --------- | ---- | --------- | -------- |"

try
{
    foreach ($size in $Sizes)
    {
        New-RandomFile -Path $local -Size $size
        $sourceHash = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash

        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        $pushOutput = & $Example --push $local $Remote 2>&1
        $pushExit = $LASTEXITCODE
        $pushSeconds = $stopwatch.Elapsed.TotalSeconds
        if ($pushOutput -match "no USB device")
        {
            throw "no matching USB device is attached; attach one and try again"
        }

        Remove-Item -Force $back -ErrorAction SilentlyContinue
        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        $pullOutput = & $Example --pull $Remote $back 2>&1
        $pullExit = $LASTEXITCODE
        $pullSeconds = $stopwatch.Elapsed.TotalSeconds
        if ($pullOutput -match "no USB device")
        {
            throw "no matching USB device is attached; attach one and try again"
        }

        $backHash = if (Test-Path -LiteralPath $back) { (Get-FileHash -LiteralPath $back -Algorithm SHA256).Hash } else { "missing" }
        $verified = if ($sourceHash -eq $backHash -and $pushExit -eq 0 -and $pullExit -eq 0) { "SHA-256" } else { "FAILED" }

        $mib = $size / 1MB
        $pushRate = [Math]::Round($mib / [Math]::Max($pushSeconds, 0.001), 1)
        $pullRate = [Math]::Round($mib / [Math]::Max($pullSeconds, 0.001), 1)
        "| $(Format-Size $size) | $([Math]::Round($pushSeconds, 1)) s | $pushRate MB/s | $([Math]::Round($pullSeconds, 1)) s | $pullRate MB/s | $verified |"
    }
}
finally
{
    & $Example "rm -f $Remote" 2>&1 | Out-Null
    Remove-Item -Force $local, $back -ErrorAction SilentlyContinue
}

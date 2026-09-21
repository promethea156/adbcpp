# Compares `push` and `pull` throughput with and without the v2 compressed forms.
#
# The v1 `RECV`/`SEND` forms are what `pull`/`push` used before compression
# existed, and `SyncCompression::None` still sends exactly them, so `none` is the
# "before" control and each other mode is an "after". For each size and payload the
# script pushes the file and pulls it back, verifies the two by SHA-256, and prints
# one markdown row per mode, with the speedup against `none`.
#
# Two payloads are used because the gain is real only when the data compresses: a
# text payload shrinks a lot, while random bytes cannot shrink at all, so the
# compressed form only adds CPU and frame overhead there.
#
# Stop the adb server before running this, because adb holds the device's USB
# interface while it runs (blocker 5). Use tools/invoke-adb.ps1 to run adb without
# hanging the terminal:
#
#   . ./tools/invoke-adb.ps1
#   Invoke-Adb -Arguments @("kill-server")
#   ./tools/bench-compression.ps1
#   ./tools/bench-compression.ps1 -Modes none,auto,zstd -Sizes 67108864
#   ./tools/bench-compression.ps1 -Pattern random
[CmdletBinding()]
param(
    [string]$Example = "build/examples/Release/adbcpp_usb_example.exe",
    [string]$Remote = "/data/local/tmp/adbcpp_bench_test.bin",
    [long[]]$Sizes = @(67108864, 268435456),
    [ValidateSet("text", "random")][string]$Pattern = "text",
    [string[]]$Modes = @("none", "auto"),
    [string]$Device = "",
    [int]$TransferTimeoutMs = 0
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Example))
{
    throw "the example was not found: $Example (build it with -DADBCPP_BUILD_EXAMPLES=ON)"
}
$Example = (Resolve-Path -LiteralPath $Example).Path

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) "adbcpp_bench_compression"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$local = Join-Path $scratch "src.bin"
$back = Join-Path $scratch "back.bin"

# The extra arguments the example gets for every call.
$common = @()
if ($Device -ne "")
{
    $common += @("--device", $Device)
}
if ($TransferTimeoutMs -gt 0)
{
    $common += @("--timeout", "$TransferTimeoutMs")
}

function New-Payload
{
    param([string]$Path, [long]$Size, [string]$Kind)

    # Written in blocks so that a very large file does not need a matching
    # allocation.
    $stream = [System.IO.File]::Create($Path)
    try
    {
        $blockSize = 1048576
        if ($Kind -eq "random")
        {
            $written = 0
            while ($written -lt $Size)
            {
                $length = [Math]::Min($blockSize, $Size - $written)
                $block = New-Object byte[] $length
                [System.Security.Cryptography.RandomNumberGenerator]::Fill($block)
                $stream.Write($block, 0, $block.Length)
                $written += $block.Length
            }
        }
        else
        {
            # A repeated line is what a text file looks like to a compressor:
            # the next block is nearly the one before it.
            $line = [System.Text.Encoding]::ASCII.GetBytes(
                "the quick brown fox jumps over the lazy dog 0123456789 adbcpp compression benchmark`n")
            $block = New-Object byte[] $blockSize
            for ($i = 0; $i -lt $block.Length; $i += $line.Length)
            {
                [Array]::Copy($line, 0, $block, $i, [Math]::Min($line.Length, $block.Length - $i))
            }
            $written = 0
            while ($written -lt $Size)
            {
                $length = [Math]::Min($block.Length, $Size - $written)
                $stream.Write($block, 0, $length)
                $written += $length
            }
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

# Runs one `--push` and one `--pull` in `mode`, returns the seconds and whether
# the SHA-256 matched.
function Measure-Transfer
{
    param([string]$Mode)

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $pushOutput = & $Example @common --compression $Mode --push $local $Remote 2>&1
    $pushExit = $LASTEXITCODE
    $pushSeconds = $stopwatch.Elapsed.TotalSeconds
    if ($pushOutput -match "no USB device")
    {
        throw "no matching USB device is attached; attach one and try again"
    }
    if ($pushExit -ne 0)
    {
        throw "push with $Mode failed: $pushOutput"
    }

    Remove-Item -Force $back -ErrorAction SilentlyContinue
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $pullOutput = & $Example @common --compression $Mode --pull $Remote $back 2>&1
    $pullExit = $LASTEXITCODE
    $pullSeconds = $stopwatch.Elapsed.TotalSeconds
    if ($pullOutput -match "no USB device")
    {
        throw "no matching USB device is attached; attach one and try again"
    }
    if ($pullExit -ne 0)
    {
        throw "pull with $Mode failed: $pullOutput"
    }

    $backHash = if (Test-Path -LiteralPath $back)
    {
        (Get-FileHash -LiteralPath $back -Algorithm SHA256).Hash
    }
    else
    {
        "missing"
    }

    [pscustomobject]@{
        PushSeconds = $pushSeconds
        PullSeconds = $pullSeconds
        Verified    = ($sourceHash -eq $backHash)
    }
}

"| size | payload | mode | push | push rate | pull | pull rate | vs none (push/pull) | verified |"
"| ---- | ------- | ---- | ---- | --------- | ---- | --------- | ------------------ | -------- |"

try
{
    foreach ($size in $Sizes)
    {
        New-Payload -Path $local -Size $size -Kind $Pattern
        $sourceHash = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash

        $results = [ordered]@{}
        foreach ($mode in $Modes)
        {
            $results[$mode] = Measure-Transfer -Mode $mode
        }

        # The speedup is against `none`, which is the v1 form, when it was run.
        $baseline = $null
        if ($results.Contains("none"))
        {
            $baseline = $results["none"]
        }

        $mib = $size / 1MB
        foreach ($mode in $Modes)
        {
            $result = $results[$mode]
            $pushRate = [Math]::Round($mib / [Math]::Max($result.PushSeconds, 0.001), 1)
            $pullRate = [Math]::Round($mib / [Math]::Max($result.PullSeconds, 0.001), 1)
            $speedup = "-"
            if ($null -ne $baseline)
            {
                $pushFactor = [Math]::Round($baseline.PushSeconds / [Math]::Max($result.PushSeconds, 0.001), 2)
                $pullFactor = [Math]::Round($baseline.PullSeconds / [Math]::Max($result.PullSeconds, 0.001), 2)
                $speedup = "${pushFactor}x/${pullFactor}x"
            }
            $verified = if ($result.Verified) { "SHA-256" } else { "FAILED" }
            "| $(Format-Size $size) | $Pattern | $mode | $([Math]::Round($result.PushSeconds, 1)) s | $pushRate MB/s | $([Math]::Round($result.PullSeconds, 1)) s | $pullRate MB/s | $speedup | $verified |"
        }
    }
}
finally
{
    & $Example @common "rm -f $Remote" 2>&1 | Out-Null
    Remove-Item -Force $local, $back -ErrorAction SilentlyContinue
}

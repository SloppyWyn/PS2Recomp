# Contention-proof EE work measurement, off vs on (alternating): PS2X_REALTIME_CLOCK=0 makes the
# guest clock follow charged cycles only, which lean_codegen does not change, so both builds make
# the same guest progress per charged cycle; PS2X_FASTFORWARD=max keeps the EE thread CPU-bound.
# The measure is the EE thread's CPU time (not wall time) at fixed guest vsync marks: the sampler
# polls the runner's stdout tail every 0.5 s and records every new [run:tick] line (vsync, eeMs)
# with the process CPU and the busiest thread's CPU at that moment; the run stops at -StopVsync.
#   powershell -File local\lean\throughput3.ps1 -Rounds 3 -StopVsync 6000 [-Seconds 600]
# Samples: local\runs\lean-cyc-<mode>-<n>.samples.csv; table: python local\lean\throughput3_report.py
param([int]$Rounds = 3, [int]$Seconds = 600, [string]$Tag = "cyc", [int]$StopVsync = 6000,
      [string[]]$Modes = @("off", "on"))
$root = "C:\Users\Wyn\Desktop\dev\SRWZ-native"
. "$root\tools\launch.ps1"
. "$root\tools\runstop.ps1"
$runs = "$root\local\runs"
$exes = @{
    off = "$root\local\lean\build-off\ps2xRuntime\Release\ps2EntryRunner.exe"
    on  = "$root\local\lean\build-on\ps2xRuntime\Release\ps2EntryRunner.exe"
}
$envBase = @{ PS2X_MUTE = "1"; PS2X_HEADLESS = "1"; PS2X_NO_AUDIO = "1"; PS2X_FASTFORWARD = "max"; PS2X_REALTIME_CLOCK = "0" }
for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($mode in $Modes) {
        $name = "lean-$Tag-$mode-$r"
        $out = Join-Path $runs "$name.out.txt"
        $err = Join-Path $runs "$name.err.txt"
        $csv = Join-Path $runs "$name.samples.csv"
        $stop = New-RunnerStopFile (Join-Path $runs $name)
        $env = $envBase.Clone(); $env.PS2X_STOP_FILE = $stop
        "hostS,vsync,eeMs,totalCpuS,maxThreadCpuS" | Set-Content $csv
        $p = Start-Quiet $exes[$mode] -ArgumentList @("$root\local\game\SLPS_258.87", "$root\disc\srwz.iso") `
            -WorkingDirectory "$root\local\game" -StdOut $out -StdErr $err -Env $env -Window Hidden
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $last = -1
        while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Seconds) {
            Start-Sleep -Milliseconds 500
            $tick = (Get-Content $out -Tail 40 -ErrorAction SilentlyContinue | Select-String -Pattern '\[run:tick\].*vsync=\d+' | Select-Object -Last 1).Line
            if (-not $tick) { continue }
            if ($tick -notmatch 'eeMs=(\d+) vsync=(\d+)') { continue }
            $eeMs = [int64]$matches[1]; $vsync = [int]$matches[2]
            if ($vsync -eq $last) { continue }
            $last = $vsync
            try {
                $p.Refresh()
                $total = $p.TotalProcessorTime.TotalSeconds
                $maxThread = ($p.Threads | ForEach-Object { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Maximum).Maximum
            } catch { break }
            "{0:F2},{1},{2},{3:F3},{4:F3}" -f $sw.Elapsed.TotalSeconds, $vsync, $eeMs, $total, $maxThread | Add-Content $csv
            if ($vsync -ge $StopVsync) { break }
        }
        $how = if ($p.HasExited) { "exited by itself, code $($p.ExitCode)" } else { Stop-Runner $p $stop }
        "$name vsync=$last hostS=$([int]$sw.Elapsed.TotalSeconds) $how"
    }
}

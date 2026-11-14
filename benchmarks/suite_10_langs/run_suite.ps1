param(
    [int]$Runs = 5,
    [int]$PythonRuns = 3,
    [switch]$SkipPython,
    [string]$OutJson = "results_raw.json"
)

$ErrorActionPreference = "Stop"
$SuiteDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $SuiteDir "..\..")
$VortexBin = Join-Path $SuiteDir "bench_vortex_cli.exe"
$clangBuild = Get-Command clang -ErrorAction SilentlyContinue
if ($clangBuild) {
    Push-Location $RepoRoot
    & clang -std=c99 -Os -Wall -Wextra -flto -ffunction-sections -fdata-sections src/arena.c src/lexer_parser.c src/ir_opt.c src/jit_x64.c src/jit_arm64.c src/debug_info.c src/pkg.c src/lsp.c src/runtime.c src/main.c -o $VortexBin -luser32 -lgdi32 "-Wl,--gc-sections" -s
    Pop-Location
} elseif (Test-Path (Join-Path $RepoRoot "vortex.exe")) {
    $VortexBin = Join-Path $RepoRoot "vortex.exe"
}

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0 }
    $sorted = $Values | Sort-Object
    $mid = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2 -eq 1) { return $sorted[$mid] }
    return ($sorted[$mid - 1] + $sorted[$mid]) / 2.0
}

function Invoke-MeasuredProcess {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList,
        [string]$WorkDir
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = ($ArgumentList | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '
    $psi.WorkingDirectory = $WorkDir
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$proc.Start()

    $peakWs = 0L
    while (-not $proc.HasExited) {
        try {
            $proc.Refresh()
            if ($proc.PeakWorkingSet64 -gt $peakWs) { $peakWs = $proc.PeakWorkingSet64 }
        } catch {}
        Start-Sleep -Milliseconds 10
    }
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    $sw.Stop()
    try {
        if ($proc.PeakWorkingSet64 -gt $peakWs) { $peakWs = $proc.PeakWorkingSet64 }
    } catch {}
    $exitCode = $proc.ExitCode
    $proc.Dispose()

    if ($exitCode -ne 0) {
        throw "Process $FilePath exited with $exitCode`: $stderr`n$stdout"
    }

    $parsed = @{}
    foreach ($line in ($stdout -split "`r?`n")) {
        $parts = $line.Trim() -split '\s+'
        if ($parts.Count -eq 2) {
            $parsed[$parts[0]] = $parts[1]
        }
    }
    return [pscustomobject]@{
        WallMs   = $sw.Elapsed.TotalMilliseconds
        PeakMb   = [math]::Round($peakWs / (1024.0 * 1024.0), 2)
        Metrics  = $parsed
        RawOut   = $stdout
    }
}

$Results = [System.Collections.ArrayList]::new()

function Add-BenchmarkResult {
    param(
        [string]$Language,
        [string]$Toolchain,
        [string]$Status,
        [double]$CompileMs,
        [double]$BinaryKb,
        [int]$NumRuns,
        [System.Collections.ArrayList]$RunData
    )
    $t1 = Get-Median ($RunData | ForEach-Object { [double]$_.Metrics["TASK1_US"] / 1000.0 })
    $t2 = Get-Median ($RunData | ForEach-Object { [double]$_.Metrics["TASK2_US"] / 1000.0 })
    $t3 = Get-Median ($RunData | ForEach-Object { [double]$_.Metrics["TASK3_US"] / 1000.0 })
    $t4 = Get-Median ($RunData | ForEach-Object { [double]$_.Metrics["TASK4_US"] / 1000.0 })
    $t5 = Get-Median ($RunData | ForEach-Object { [double]$_.Metrics["TASK5_US"] / 1000.0 })
    $tot = Get-Median ($RunData | ForEach-Object { [double]$_.Metrics["TOTAL_US"] / 1000.0 })
    $wall = Get-Median ($RunData | ForEach-Object { [double]$_.WallMs })
    $ram = ($RunData | ForEach-Object { [double]$_.PeakMb } | Measure-Object -Maximum).Maximum
    $sample = $RunData[0].Metrics

    $aotCompileMs = [math]::Round($CompileMs, 2)
    $runtimeOverheadMs = [math]::Round([math]::Max(0.1, $wall - $tot), 2)

    $obj = [ordered]@{
        Language          = $Language
        Toolchain         = $Toolchain
        Status            = $Status
        Runs              = $NumRuns
        Task1Ms           = [math]::Round($t1, 3)
        Task2Ms           = [math]::Round($t2, 3)
        Task3Ms           = [math]::Round($t3, 3)
        Task4Ms           = [math]::Round($t4, 3)
        Task5Ms           = [math]::Round($t5, 3)
        TotalMs           = [math]::Round($tot, 3)
        WallMs            = [math]::Round($wall, 2)
        CompileAotMs      = $aotCompileMs
        StartupRuntimeMs  = $runtimeOverheadMs
        BinaryKb          = [math]::Round($BinaryKb, 2)
        PeakRamMb         = [math]::Round($ram, 2)
        Task1Pi           = $sample["TASK1_PI"]
        Task2Chk          = $sample["TASK2_CHK"]
        Task3Chk          = $sample["TASK3_CHK"]
        Task4Chk          = $sample["TASK4_CHK"]
        Task5Chk          = $sample["TASK5_CHK"]
    }
    [void]$Results.Add($obj)
    Write-Host ("[{0}] T1={1}ms T2={2}ms T3={3}ms T4={4}ms T5={5}ms | Total={6}ms | CompileAOT={7}ms | Startup/JIT={8}ms | Size={9}KB | RAM={10}MB" -f `
        $Language, $obj.Task1Ms, $obj.Task2Ms, $obj.Task3Ms, $obj.Task4Ms, $obj.Task5Ms, $obj.TotalMs, $obj.CompileAotMs, $obj.StartupRuntimeMs, $obj.BinaryKb, $obj.PeakRamMb)
    Write-Host ("    Checksums: PI={0} CHK2={1} CHK3={2} CHK4={3} CHK5={4}" -f `
        $obj.Task1Pi, $obj.Task2Chk, $obj.Task3Chk, $obj.Task4Chk, $obj.Task5Chk)
}

Write-Host "========================================================================"
Write-Host "  VORTEX v1.0.0 - 10-LANGUAGE BENCHMARK SUITE RUNNER ($Runs runs median)"
Write-Host "========================================================================"

# 1. Vortex v1.0.0
if (Test-Path $VortexBin) {
    $vxSizeKb = (Get-Item $VortexBin).Length / 1024.0
    $runsVx = [System.Collections.ArrayList]::new()
    for ($i = 0; $i -lt $Runs; $i++) {
        [void]$runsVx.Add((Invoke-MeasuredProcess -FilePath $VortexBin -ArgumentList @("--release", "bench_vortex.vx") -WorkDir $SuiteDir))
    }
    Add-BenchmarkResult -Language "Vortex v1.0.0" -Toolchain "JIT x86-64 AVX2 (--release)" -Status "Mesure localement" -CompileMs 0 -BinaryKb $vxSizeKb -NumRuns $Runs -RunData $runsVx
}

# 2. C (Clang -O3 -march=native)
$clangCmd = Get-Command clang -ErrorAction SilentlyContinue
if ($clangCmd) {
    $cExe = Join-Path $SuiteDir "bench_c.exe"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & clang -O3 -march=native (Join-Path $SuiteDir "bench_c.c") -o $cExe
    $sw.Stop()
    if ($LASTEXITCODE -eq 0) {
        $cSizeKb = (Get-Item $cExe).Length / 1024.0
        $runsC = [System.Collections.ArrayList]::new()
        for ($i = 0; $i -lt $Runs; $i++) {
            [void]$runsC.Add((Invoke-MeasuredProcess -FilePath $cExe -ArgumentList @() -WorkDir $SuiteDir))
        }
        $cVer = (cmd /c "clang --version 2>&1" | Select-Object -First 1).Trim()
        Add-BenchmarkResult -Language "C (-O3)" -Toolchain "Clang -O3 -march=native ($cVer)" -Status "Mesure localement" -CompileMs $sw.Elapsed.TotalMilliseconds -BinaryKb $cSizeKb -NumRuns $Runs -RunData $runsC
    }

    $cFastExe = Join-Path $SuiteDir "bench_c_fast.exe"
    $swFast = [System.Diagnostics.Stopwatch]::StartNew()
    & clang -O3 -march=native -ffast-math (Join-Path $SuiteDir "bench_c.c") -o $cFastExe
    $swFast.Stop()
    if ($LASTEXITCODE -eq 0) {
        $cfSizeKb = (Get-Item $cFastExe).Length / 1024.0
        $runsCFast = [System.Collections.ArrayList]::new()
        for ($i = 0; $i -lt $Runs; $i++) {
            [void]$runsCFast.Add((Invoke-MeasuredProcess -FilePath $cFastExe -ArgumentList @() -WorkDir $SuiteDir))
        }
        $cVer2 = (cmd /c "clang --version 2>&1" | Select-Object -First 1).Trim()
        Add-BenchmarkResult -Language "C (-O3 -ffast-math)" -Toolchain "Clang -O3 -march=native -ffast-math ($cVer2)" -Status "Mesure localement" -CompileMs $swFast.Elapsed.TotalMilliseconds -BinaryKb $cfSizeKb -NumRuns $Runs -RunData $runsCFast
    }
}

# 3. C++ (Clang++ -O3 -march=native & -ffast-math)
$clangppCmd = Get-Command clang++ -ErrorAction SilentlyContinue
if ($clangppCmd) {
    $cppExe = Join-Path $SuiteDir "bench_cpp.exe"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & clang++ -std=c++20 -O3 -march=native (Join-Path $SuiteDir "bench_cpp.cpp") -o $cppExe
    $sw.Stop()
    if ($LASTEXITCODE -eq 0) {
        $cppSizeKb = (Get-Item $cppExe).Length / 1024.0
        $runsCpp = [System.Collections.ArrayList]::new()
        for ($i = 0; $i -lt $Runs; $i++) {
            [void]$runsCpp.Add((Invoke-MeasuredProcess -FilePath $cppExe -ArgumentList @() -WorkDir $SuiteDir))
        }
        $cppVer = (cmd /c "clang++ --version 2>&1" | Select-Object -First 1).Trim()
        Add-BenchmarkResult -Language "C++ (-O3)" -Toolchain "Clang++ -O3 -march=native ($cppVer)" -Status "Mesure localement" -CompileMs $sw.Elapsed.TotalMilliseconds -BinaryKb $cppSizeKb -NumRuns $Runs -RunData $runsCpp
    }

    $cppFastExe = Join-Path $SuiteDir "bench_cpp_fast.exe"
    $swCppFast = [System.Diagnostics.Stopwatch]::StartNew()
    & clang++ -std=c++20 -O3 -march=native -ffast-math (Join-Path $SuiteDir "bench_cpp.cpp") -o $cppFastExe
    $swCppFast.Stop()
    if ($LASTEXITCODE -eq 0) {
        $cppfSizeKb = (Get-Item $cppFastExe).Length / 1024.0
        $runsCppFast = [System.Collections.ArrayList]::new()
        for ($i = 0; $i -lt $Runs; $i++) {
            [void]$runsCppFast.Add((Invoke-MeasuredProcess -FilePath $cppFastExe -ArgumentList @() -WorkDir $SuiteDir))
        }
        $cppVer2 = (cmd /c "clang++ --version 2>&1" | Select-Object -First 1).Trim()
        Add-BenchmarkResult -Language "C++ (-O3 -ffast-math)" -Toolchain "Clang++ -O3 -march=native -ffast-math ($cppVer2)" -Status "Mesure localement" -CompileMs $swCppFast.Elapsed.TotalMilliseconds -BinaryKb $cppfSizeKb -NumRuns $Runs -RunData $runsCppFast
    }
}

# 4. Rust (rustc -C opt-level=3 -C target-cpu=native)
$rustcCmd = Get-Command rustc -ErrorAction SilentlyContinue
if ($rustcCmd) {
    $rsExe = Join-Path $SuiteDir "bench_rust.exe"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & rustc -C opt-level=3 -C target-cpu=native (Join-Path $SuiteDir "bench_rust.rs") -o $rsExe
    $sw.Stop()
    if ($LASTEXITCODE -eq 0) {
        $rsSizeKb = (Get-Item $rsExe).Length / 1024.0
        $runsRs = [System.Collections.ArrayList]::new()
        for ($i = 0; $i -lt $Runs; $i++) {
            [void]$runsRs.Add((Invoke-MeasuredProcess -FilePath $rsExe -ArgumentList @() -WorkDir $SuiteDir))
        }
        $rsVer = (cmd /c "rustc --version 2>&1" | Select-Object -First 1).Trim()
        Add-BenchmarkResult -Language "Rust" -Toolchain "$rsVer (-C opt-level=3 -C target-cpu=native)" -Status "Mesure localement" -CompileMs $sw.Elapsed.TotalMilliseconds -BinaryKb $rsSizeKb -NumRuns $Runs -RunData $runsRs
    }
}

# 5. C# (.NET Framework 64-bit /optimize+)
$cscPath = "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (Test-Path $cscPath) {
    $csExe = Join-Path $SuiteDir "BenchCSharp.exe"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $cscPath /nologo /optimize+ /platform:x64 /out:$csExe (Join-Path $SuiteDir "BenchCSharp.cs") | Out-Null
    $sw.Stop()
    if ($LASTEXITCODE -eq 0) {
        $csSizeKb = (Get-Item $csExe).Length / 1024.0
        $runsCs = [System.Collections.ArrayList]::new()
        for ($i = 0; $i -lt $Runs; $i++) {
            [void]$runsCs.Add((Invoke-MeasuredProcess -FilePath $csExe -ArgumentList @() -WorkDir $SuiteDir))
        }
        Add-BenchmarkResult -Language "C# (.NET)" -Toolchain "csc v4.8 /optimize+ /platform:x64 (RyuJIT)" -Status "Mesure localement" -CompileMs $sw.Elapsed.TotalMilliseconds -BinaryKb $csSizeKb -NumRuns $Runs -RunData $runsCs
    }
}

# 6. Java (OpenJDK HotSpot)
$javacCmd = Get-Command javac -ErrorAction SilentlyContinue
$javaCmd = Get-Command java -ErrorAction SilentlyContinue
if ($javacCmd -and $javaCmd) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & javac -d $SuiteDir (Join-Path $SuiteDir "BenchJava.java")
    $sw.Stop()
    if ($LASTEXITCODE -eq 0) {
        $classFiles = Get-ChildItem -Path $SuiteDir -Filter "BenchJava*.class"
        $javaSizeKb = (($classFiles | Measure-Object -Property Length -Sum).Sum) / 1024.0
        $runsJava = [System.Collections.ArrayList]::new()
        for ($i = 0; $i -lt $Runs; $i++) {
            [void]$runsJava.Add((Invoke-MeasuredProcess -FilePath "java" -ArgumentList @("-Xmx2g", "-cp", ".", "BenchJava") -WorkDir $SuiteDir))
        }
        $javaVer = (cmd /c "java -version 2>&1" | Select-Object -First 1).Trim()
        Add-BenchmarkResult -Language "Java" -Toolchain "OpenJDK HotSpot ($javaVer)" -Status "Mesure localement" -CompileMs $sw.Elapsed.TotalMilliseconds -BinaryKb $javaSizeKb -NumRuns $Runs -RunData $runsJava
    }
}

# 7. JavaScript (Node.js V8 JIT)
$nodeCmd = Get-Command node -ErrorAction SilentlyContinue
if ($nodeCmd) {
    $nodeSizeKb = (Get-Item $nodeCmd.Source).Length / 1024.0
    $runsJs = [System.Collections.ArrayList]::new()
    for ($i = 0; $i -lt $Runs; $i++) {
        [void]$runsJs.Add((Invoke-MeasuredProcess -FilePath "node" -ArgumentList @("bench_js.js") -WorkDir $SuiteDir))
    }
    $nodeVer = (cmd /c "node -v 2>&1" | Select-Object -First 1).Trim()
    Add-BenchmarkResult -Language "JavaScript (Node.js)" -Toolchain "Node.js $nodeVer (V8 JIT)" -Status "Mesure localement" -CompileMs 0 -BinaryKb $nodeSizeKb -NumRuns $Runs -RunData $runsJs
}

# 8. Python 3 (CPython)
$pyCmd = Get-Command python -ErrorAction SilentlyContinue
if ($pyCmd -and -not $SkipPython) {
    $pyExe = $pyCmd.Source
    $pyDir = Split-Path -Parent $pyExe
    $pyDll = Get-ChildItem -Path $pyDir -Filter "python3*.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
    $pySizeKb = ((Get-Item $pyExe).Length + $(if ($pyDll) { $pyDll.Length } else { 0 })) / 1024.0
    $runsPy = [System.Collections.ArrayList]::new()
    for ($i = 0; $i -lt $PythonRuns; $i++) {
        Write-Host "[Python 3] Running exact unscaled suite run $($i + 1)/$PythonRuns ..."
        [void]$runsPy.Add((Invoke-MeasuredProcess -FilePath "python" -ArgumentList @("bench_python.py") -WorkDir $SuiteDir))
    }
    $pyVer = (cmd /c "python --version 2>&1" | Select-Object -First 1).Trim()
    Add-BenchmarkResult -Language "Python 3" -Toolchain "$pyVer (CPython)" -Status "Mesure localement" -CompileMs 0 -BinaryKb $pySizeKb -NumRuns $PythonRuns -RunData $runsPy
}

$jsonPath = Join-Path $SuiteDir $OutJson
$Results | ConvertTo-Json -Depth 4 | Set-Content -Path $jsonPath -Encoding UTF8
Write-Host "Saved raw measurements to $jsonPath"

# Cleanup all temporary compiled artifacts
Remove-Item -Path (Join-Path $SuiteDir "*.exe"), (Join-Path $SuiteDir "*.class"), (Join-Path $SuiteDir "*.pdb"), (Join-Path $SuiteDir "*.obj"), (Join-Path $SuiteDir "*.exp"), (Join-Path $SuiteDir "*.lib") -ErrorAction SilentlyContinue
Write-Host "Cleaned up compiled artifacts in $SuiteDir"

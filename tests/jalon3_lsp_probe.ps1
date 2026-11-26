param(
    [Parameter(Mandatory=$true)][string]$VortexBin,
    [Parameter(Mandatory=$true)][string]$RootDir
)

$ErrorActionPreference = "Stop"

# 1. Validate VS Code extension JSON manifests
foreach ($rel in @(
    "editors\vscode\package.json",
    "editors\vscode\language-configuration.json",
    "editors\vscode\syntaxes\vortex.tmLanguage.json"
)) {
    $full = Join-Path $RootDir $rel
    if (-not (Test-Path $full)) { throw "Missing $full" }
    $null = Get-Content $full -Raw | ConvertFrom-Json
}

$node = Get-Command node -ErrorAction SilentlyContinue
if ($node) {
    $extJs = Join-Path $RootDir "editors\vscode\src\extension.js"
    & $node.Source --check $extJs
    if ($LASTEXITCODE -ne 0) { throw "extension.js syntax check failed" }
}

# 2. Live stdio JSON-RPC session with vortex lsp
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $VortexBin
$psi.Arguments = "lsp"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)
$sin = $proc.StandardInput.BaseStream
$sout = $proc.StandardOutput.BaseStream
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Send-Lsp($obj) {
    $json = $obj | ConvertTo-Json -Depth 12 -Compress
    $body = $utf8.GetBytes($json)
    $hdr = $utf8.GetBytes("Content-Length: $($body.Length)`r`n`r`n")
    $sin.Write($hdr, 0, $hdr.Length)
    $sin.Write($body, 0, $body.Length)
    $sin.Flush()
}

function Read-LineAscii($stream) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ($true) {
        $b = $stream.ReadByte()
        if ($b -lt 0) { return $null }
        if ($b -eq 10) { break }
        if ($b -ne 13) { $bytes.Add([byte]$b) }
    }
    return $utf8.GetString($bytes.ToArray())
}

function Recv-Lsp {
    $cl = 0
    while ($true) {
        $line = Read-LineAscii $sout
        if ($null -eq $line) { throw "Unexpected EOF reading LSP header" }
        if ($line -eq "") { break }
        if ($line -match "(?i)^Content-Length:\s*(\d+)") {
            $cl = [int]$Matches[1]
        }
    }
    if ($cl -le 0) { throw "Invalid Content-Length: $cl" }
    $buf = New-Object byte[] $cl
    $off = 0
    while ($off -lt $cl) {
        $n = $sout.Read($buf, $off, $cl - $off)
        if ($n -le 0) { throw "Unexpected EOF reading LSP body" }
        $off += $n
    }
    return ($utf8.GetString($buf) | ConvertFrom-Json)
}

$stage = "start"
try {
    # 1. initialize
    $stage = "initialize"
    Send-Lsp @{
        jsonrpc = "2.0"; id = 1; method = "initialize"
        params = @{ processId = $PID; rootUri = $null; capabilities = @{} }
    }
    $r1 = Recv-Lsp
    if ($r1.result.serverInfo.name -ne "vortex-lsp") { throw "Bad serverInfo" }
    if (-not $r1.result.capabilities.definitionProvider) { throw "No definitionProvider" }
    if (-not $r1.result.capabilities.hoverProvider) { throw "No hoverProvider" }
    if (-not $r1.result.capabilities.completionProvider) { throw "No completionProvider" }

    Send-Lsp @{ jsonrpc = "2.0"; method = "initialized"; params = @{} }

    $uri = "file:///C:/temp/lsp_probe.vx"
    $src = "struct Vec2 { x: f32, y: f32 }`nfn add_x(v: Vec2, dx: f32) -> f32 { return v.x + dx; }`nlet p = Vec2 { x: 1.0, y: 2.0 };`nlet s = add_x(p, 3.0);`n"

    # 2. didOpen clean -> empty diagnostics
    $stage = "didOpen"
    Send-Lsp @{
        jsonrpc = "2.0"; method = "textDocument/didOpen"
        params = @{ textDocument = @{ uri = $uri; languageId = "vortex"; version = 1; text = $src } }
    }
    $d1 = Recv-Lsp
    if ($d1.method -ne "textDocument/publishDiagnostics") { throw "Expected publishDiagnostics" }
    if ($d1.params.diagnostics.Count -ne 0) { throw "Expected 0 diagnostics on clean file" }

    # 3. definition on add_x (line 3, col 9)
    $stage = "definition"
    Send-Lsp @{
        jsonrpc = "2.0"; id = 2; method = "textDocument/definition"
        params = @{ textDocument = @{ uri = $uri }; position = @{ line = 3; character = 9 } }
    }
    $r2 = Recv-Lsp
    if ($r2.result.range.start.line -ne 1) { throw "Definition line mismatch: $($r2.result.range.start.line)" }

    # 4. hover on add_x
    $stage = "hover"
    Send-Lsp @{
        jsonrpc = "2.0"; id = 3; method = "textDocument/hover"
        params = @{ textDocument = @{ uri = $uri }; position = @{ line = 3; character = 9 } }
    }
    $r3 = Recv-Lsp
    if ($r3.result.contents.value -notmatch "add_x") { throw "Hover missing fn signature" }

    # 5. completion after 'p.' (add dot test line)
    $stage = "completion"
    $src2 = $src + "let z = p."
    Send-Lsp @{
        jsonrpc = "2.0"; method = "textDocument/didChange"
        params = @{
            textDocument = @{ uri = $uri; version = 2 }
            contentChanges = @(@{ text = $src2 })
        }
    }
    $null = Recv-Lsp
    Send-Lsp @{
        jsonrpc = "2.0"; id = 4; method = "textDocument/completion"
        params = @{ textDocument = @{ uri = $uri }; position = @{ line = 4; character = 10 } }
    }
    $r4 = Recv-Lsp
    $labels = @($r4.result.items | ForEach-Object { $_.label })
    if (($labels -notcontains "x") -or ($labels -notcontains "y")) {
        throw "Dot completion missing Vec2 fields: $($labels -join ',')"
    }

    # 6. didChange introducing unsafe violation -> non-empty diagnostic
    $stage = "didChange_bad"
    $badSrc = "let k = 1;`nlet bad = 1 as *i64;`n"
    Send-Lsp @{
        jsonrpc = "2.0"; method = "textDocument/didChange"
        params = @{
            textDocument = @{ uri = $uri; version = 3 }
            contentChanges = @(@{ text = $badSrc })
        }
    }
    $d2 = Recv-Lsp
    if ($d2.params.diagnostics.Count -lt 1) { throw "Expected safety error diagnostic" }
    if ($d2.params.diagnostics[0].range.start.line -ne 1) { throw "Expected error on line 1, got $($d2.params.diagnostics[0].range.start.line)" }

    # 7. shutdown + exit
    $stage = "shutdown"
    Send-Lsp @{ jsonrpc = "2.0"; id = 5; method = "shutdown"; params = $null }
    $r5 = Recv-Lsp
    if ($r5.id -ne 5) { throw "Shutdown response mismatch" }

    Send-Lsp @{ jsonrpc = "2.0"; method = "exit"; params = $null }
    $sin.Close()
    if (-not $proc.WaitForExit(5000)) {
        $proc.Kill()
        throw "vortex lsp did not exit cleanly"
    }
    if ($proc.ExitCode -ne 0) { throw "vortex lsp exited with $($proc.ExitCode)" }
    Write-Host "LSP_PROBE_ALL_OK"
} catch {
    Write-Host "LSP_STAGE=$stage"
    Start-Sleep -Milliseconds 100
    if ($proc.HasExited) {
        $errOut = $proc.StandardError.ReadToEnd()
        Write-Host "LSP_EXIT_CODE=$($proc.ExitCode)"
        if ($errOut) { Write-Host "LSP_STDERR=$errOut" }
    }
    throw
} finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}

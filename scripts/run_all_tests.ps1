# 先 aerovistaSyncTests，再 vsgEngineTests。串行，避免两边同时占用 loopback 端口。
# 其余参数原样传给两个 Catch2 可执行文件，例如 -s、或一个 tag 过滤器。
# -OutFile 把两段 reporter 输出收成一个文件（各自 -o 再拼接；直接传同一个 -o 会被后者覆盖）。

param(
    [string]$OutFile,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CatchArgs
)

$ErrorActionPreference = "Stop"
if (-not $CatchArgs) {
    $CatchArgs = @()
}
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$build = Join-Path $root "out\build\clang-Ninja-Debug"
$exes = @(
    (Join-Path $build "thirdparty\sync\tests\aerovistaSyncTests.exe"),
    (Join-Path $build "engine\Tests\vsgEngineTests.exe")
)

$logPath = $null
if ($OutFile) {
    $logPath = if ([System.IO.Path]::IsPathRooted($OutFile)) { $OutFile } else { Join-Path $root $OutFile }
    if (Test-Path -LiteralPath $logPath) {
        Remove-Item -LiteralPath $logPath
    }
}

foreach ($exe in $exes) {
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Error "missing test executable: $exe"
    }
    $name = Split-Path -Leaf $exe
    Write-Host "=== $name ==="
    if ($logPath) {
        [System.IO.File]::AppendAllText($logPath, "=== $name ===`r`n", [System.Text.Encoding]::ASCII)
        $part = Join-Path ([System.IO.Path]::GetTempPath()) ("ave_" + [System.IO.Path]::GetFileNameWithoutExtension($name) + ".txt")
        if (Test-Path -LiteralPath $part) {
            Remove-Item -LiteralPath $part
        }
        & $exe @CatchArgs "-o" $part
        $code = $LASTEXITCODE
        if (Test-Path -LiteralPath $part) {
            $bytes = [System.IO.File]::ReadAllBytes($part)
            $stream = [System.IO.File]::Open($logPath, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
            try {
                $stream.Write($bytes, 0, $bytes.Length)
            }
            finally {
                $stream.Close()
            }
            Remove-Item -LiteralPath $part
        }
        if ($code -ne 0) {
            exit $code
        }
    }
    else {
        & $exe @CatchArgs
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
}

if ($logPath) {
    Write-Host "log -> $logPath"
}
exit 0

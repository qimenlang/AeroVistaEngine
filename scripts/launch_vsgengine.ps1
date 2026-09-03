param(
    [Parameter(Mandatory = $true)][string]$Engine,
    [Parameter(Mandatory = $true)][string]$Config,
    [Parameter(Mandatory = $true)][string]$OutLog,
    [Parameter(Mandatory = $true)][string]$ErrLog
)

# 隐藏启动一个控制台程序（vsgEngine），stdout/stderr 重定向到文件。
# -WindowStyle Hidden 隐藏目标进程的 console 窗口。
$arg = @('-c', ('"' + $Config + '"'))
$p = Start-Process -FilePath $Engine `
    -ArgumentList $arg `
    -WindowStyle Hidden `
    -RedirectStandardOutput $OutLog `
    -RedirectStandardError $ErrLog `
    -PassThru

Write-Host "started pid=$($p.Id): $Engine -c $Config"
Write-Host "  stdout -> $OutLog"
Write-Host "  stderr -> $ErrLog"

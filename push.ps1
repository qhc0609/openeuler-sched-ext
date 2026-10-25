<#
.SYNOPSIS
    一键把本项目发布到 GitHub。

.DESCRIPTION
    做四件事：初始化本地仓库 -> 提交 -> 在 GitHub 创建仓库 -> 推送。
    重复执行是安全的：仓库已存在时只做提交和推送。

.PARAMETER RepoName
    GitHub 仓库名，默认 openeuler-sched-ext

.PARAMETER Owner
    仓库归属（用户名或组织）。留空则用当前 gh 登录账号。

.PARAMETER Private
    创建为私有仓库。默认公开。

.EXAMPLE
    .\push.ps1

.EXAMPLE
    .\push.ps1 -RepoName my-scx-lab -Owner myorg -Private
#>
[CmdletBinding()]
param(
    [string]$RepoName = 'openeuler-sched-ext',
    [string]$Owner = '',
    [switch]$Private,
    [string]$Description = 'Enable sched_ext on openEuler 24.03 LTS (Linux 6.6 baseline)'
)

$ErrorActionPreference = 'Stop'

function Say  ($m) { Write-Host "[push] $m" -ForegroundColor Cyan }
function Good ($m) { Write-Host "[ ok ] $m" -ForegroundColor Green }
function Bad  ($m) { Write-Host "[fail] $m" -ForegroundColor Red }

# 调用原生命令并把 stderr 也吃掉，避免 PowerShell 5.1 把 stderr 当成终止错误
function Invoke-Native {
    param(
        [string]$Exe,
        [string[]]$ArgList
    )
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @ArgList 2>&1
        $code = $LASTEXITCODE
    } catch {
        $out = @($_.Exception.Message)
        $code = 1
    } finally {
        $ErrorActionPreference = $prev
    }
    return [pscustomobject]@{ Output = $out; Code = $code }
}

# ---------------------------------------------------------------- 前置检查
Say '检查 git / gh'
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Bad '找不到 git。先装 Git for Windows: https://git-scm.com/download/win'
    exit 1
}
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Bad '找不到 gh（GitHub CLI）。'
    Write-Host '  下载: https://github.com/cli/cli/releases  （Server 版没有 winget，用 .msi）'
    Write-Host '  装完重开终端，再执行本脚本。'
    exit 1
}
Good ("git " + (git --version))

Say '检查 gh 登录状态'
$r = Invoke-Native 'gh' @('auth', 'status')
if ($r.Code -ne 0) {
    Bad 'gh 尚未登录。先执行一次（只需一次）：'
    Write-Host '  gh auth login' -ForegroundColor Yellow
    Write-Host '  选 GitHub.com -> HTTPS -> Yes -> Login with a web browser'
    exit 1
}
Good 'gh 已登录'

$r = Invoke-Native 'gh' @('api', 'user', '--jq', '.login')
$login = ''
if ($r.Code -eq 0 -and $r.Output) { $login = ($r.Output | Select-Object -First 1).ToString().Trim() }
if ($login) { Good "当前账号: $login" }

# ---------------------------------------------------------------- 仓库初始化
Set-Location $PSScriptRoot

if (-not (Test-Path (Join-Path $PSScriptRoot '.git'))) {
    Say '初始化本地仓库'
    $null = Invoke-Native 'git' @('init', '-b', 'main')
    Good 'git init -b main'
} else {
    Say '本地仓库已存在'
}

# 内核项目必须关掉 CRLF 转换
$null = Invoke-Native 'git' @('config', 'core.autocrlf', 'false')
$null = Invoke-Native 'git' @('config', 'core.eol', 'lf')

# 补全身份（没配过就用 GitHub 的 noreply 地址兜底，避免提交失败）
$uname = (& git config user.name)
if ([string]::IsNullOrWhiteSpace($uname)) {
    if (-not $login) { $login = 'user' }
    $null = Invoke-Native 'git' @('config', 'user.name', $login)
    Say "已设置 user.name = $login"
}
$umail = (& git config user.email)
if ([string]::IsNullOrWhiteSpace($umail)) {
    if (-not $login) { $login = 'user' }
    $umail = "$login@users.noreply.github.com"
    $null = Invoke-Native 'git' @('config', 'user.email', $umail)
    Say "已设置 user.email = $umail"
}

# ---------------------------------------------------------------- 提交
$null = Invoke-Native 'git' @('branch', '-M', 'main')
$null = Invoke-Native 'git' @('add', '-A')

# Windows 上 git 不记录执行位，手动把脚本标成 +x，
# 否则在 Linux/CI 里 ./scripts/xxx.sh 会 permission denied
foreach ($s in @('scripts/00-preflight.sh','scripts/10-build-kernel.sh','scripts/20-verify.sh')) {
    if (Test-Path (Join-Path $PSScriptRoot $s)) {
        $null = Invoke-Native 'git' @('update-index', '--add', '--chmod=+x', $s)
    }
}

$pending = (& git status --porcelain)
if ([string]::IsNullOrWhiteSpace($pending)) {
    Say '没有需要提交的改动'
} else {
    $msg = 'chore: bootstrap openEuler 24.03 sched_ext baseline toolchain'
    $null = Invoke-Native 'git' @('commit', '-m', $msg)
    Good ("已提交: " + $msg)
}

# ---------------------------------------------------------------- 远端
if ([string]::IsNullOrWhiteSpace($Owner)) { $Owner = $login }
if ([string]::IsNullOrWhiteSpace($Owner)) {
    Bad '拿不到 GitHub 账号名，请用 -Owner 指定'
    exit 1
}
$full = "$Owner/$RepoName"
Say "目标仓库: $full"

$r = Invoke-Native 'gh' @('repo', 'view', $full, '--json', 'name')
$exists = ($r.Code -eq 0)

if ($exists) {
    Say '仓库已存在，配置 remote 并推送'
    $remotes = (& git remote)
    if ($remotes -contains 'origin') {
        $null = Invoke-Native 'git' @('remote', 'set-url', 'origin', "https://github.com/$full.git")
    } else {
        $null = Invoke-Native 'git' @('remote', 'add', 'origin', "https://github.com/$full.git")
    }
    $r = Invoke-Native 'git' @('push', '-u', 'origin', 'main')
    $r.Output | ForEach-Object { Write-Host "  $_" }
    if ($r.Code -ne 0) { Bad '推送失败'; exit 1 }
} else {
    Say '在 GitHub 创建仓库并推送'
    $vis = '--public'
    if ($Private) { $vis = '--private' }
    $r = Invoke-Native 'gh' @(
        'repo', 'create', $full, $vis,
        '--description', $Description,
        '--source', '.', '--remote', 'origin', '--push'
    )
    $r.Output | ForEach-Object { Write-Host "  $_" }
    if ($r.Code -ne 0) {
        Bad 'gh repo create 失败'
        Write-Host ''
        Write-Host '常见原因：' -ForegroundColor Yellow
        Write-Host '  1) token 缺 workflow 权限  -> gh auth refresh -h github.com -s workflow'
        Write-Host '  2) 仓库名已被占用           -> .\push.ps1 -RepoName 换个名字'
        Write-Host '  3) 网络问题                 -> 重跑一次'
        exit 1
    }
}

# ---------------------------------------------------------------- 收尾
$url = "https://github.com/$full"
Good "发布完成: $url"
Write-Host ''
Write-Host '确认方式：浏览器打开上面这个地址，能看到 README 就成功了。'
Write-Host ''
try { Start-Process $url } catch { }

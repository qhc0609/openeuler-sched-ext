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
& gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
    Bad 'gh 尚未登录。先执行一次（只需一次）：'
    Write-Host '  gh auth login' -ForegroundColor Yellow
    Write-Host '  选 GitHub.com -> HTTPS -> 浏览器登录'
    exit 1
}
Good 'gh 已登录'

# ---------------------------------------------------------------- 仓库初始化
Set-Location $PSScriptRoot

if (-not (Test-Path (Join-Path $PSScriptRoot '.git'))) {
    Say '初始化本地仓库'
    git init -b main | Out-Null
    Good 'git init -b main'
} else {
    Say '本地仓库已存在'
}

# 内核项目必须关掉 CRLF 转换
git config core.autocrlf false
git config core.eol lf

# 补全身份（没配过就用 GitHub 的 noreply 地址兜底，避免提交失败）
$uname = git config user.name
if ([string]::IsNullOrWhiteSpace($uname)) {
    $login = (& gh api user --jq .login 2>$null)
    if ([string]::IsNullOrWhiteSpace($login)) { $login = 'user' }
    git config user.name $login
    Say "已设置 user.name = $login"
}
$umail = git config user.email
if ([string]::IsNullOrWhiteSpace($umail)) {
    $login = (& gh api user --jq .login 2>$null)
    if ([string]::IsNullOrWhiteSpace($login)) { $login = 'user' }
    $umail = "$login@users.noreply.github.com"
    git config user.email $umail
    Say "已设置 user.email = $umail"
}

# ---------------------------------------------------------------- 提交
git branch -M main 2>$null | Out-Null
git add -A

# Windows 上 git 不记录执行位，手动把脚本标成 +x，
# 否则在 Linux/CI 里 ./scripts/xxx.sh 会 permission denied
foreach ($s in @('scripts/00-preflight.sh','scripts/10-build-kernel.sh','scripts/20-verify.sh')) {
    if (Test-Path (Join-Path $PSScriptRoot $s)) {
        git update-index --add --chmod=+x $s 2>$null
    }
}

$pending = git status --porcelain
if ([string]::IsNullOrWhiteSpace($pending)) {
    Say '没有需要提交的改动'
} else {
    $msg = 'chore: bootstrap openEuler 24.03 sched_ext baseline toolchain'
    git commit -m $msg | Out-Null
    Good ("已提交: " + $msg)
    Write-Host ('  改动文件数: ' + ($pending -split "`n").Count)
}

# ---------------------------------------------------------------- 远端
if ([string]::IsNullOrWhiteSpace($Owner)) {
    $Owner = (& gh api user --jq .login 2>$null)
}
if ([string]::IsNullOrWhiteSpace($Owner)) {
    Bad '拿不到 GitHub 账号名，请用 -Owner 指定'
    exit 1
}
$full = "$Owner/$RepoName"
Say "目标仓库: $full"

& gh repo view $full --json name *> $null
$exists = ($LASTEXITCODE -eq 0)

if ($exists) {
    Say '仓库已存在，配置 remote 并推送'
    $remotes = git remote
    if ($remotes -contains 'origin') {
        git remote set-url origin "https://github.com/$full.git"
    } else {
        git remote add origin "https://github.com/$full.git"
    }
    git push -u origin main
} else {
    Say '在 GitHub 创建仓库并推送'
    $vis = '--public'
    if ($Private) { $vis = '--private' }
    & gh repo create $full $vis --description $Description --source . --remote origin --push
    if ($LASTEXITCODE -ne 0) {
        Bad 'gh repo create 失败，检查网络或权限'
        exit 1
    }
}

# ---------------------------------------------------------------- 收尾
$url = "https://github.com/$full"
Good "发布完成: $url"
Write-Host ''
Write-Host '下一步：' -ForegroundColor Yellow
Write-Host '  1) 在虚拟机里执行 ./scripts/00-preflight.sh'
Write-Host '  2) 用 vm-ssh.ps1 从 Windows 直接驱动虚拟机，实现真正的"一键"'
Write-Host ''
try { Start-Process $url } catch { }

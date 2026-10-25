<#
.SYNOPSIS
    一键把本项目发布到 GitHub，带自动重试与推送结果核对。

.DESCRIPTION
    做五件事：初始化本地仓库 -> 提交 -> 在 GitHub 创建仓库 -> 推送 -> 核对远端。
    重复执行是安全的：仓库已存在时只做提交和推送。

    推送采用「直连 / 代理」两条路轮流重试。原因：在某些受管终端里，
    直连 github.com:443 会被按进程拦截，而环境变量里的代理又会间歇性
    返回 502。单次失败不代表网络坏了，重试几乎总能成功。

.PARAMETER RepoName
    GitHub 仓库名，默认 openeuler-sched-ext

.PARAMETER Owner
    仓库归属（用户名或组织）。留空则用当前 gh 登录账号。

.PARAMETER Private
    创建为私有仓库。默认公开。

.PARAMETER MaxTry
    推送最大尝试次数，默认 6。

.PARAMETER Watch
    挂机重试的分钟数。设成 30 就一直试 30 分钟，成功即退出。
    适用于 github.com 时通时不通的情况（浏览器能开、git 却推不上去）。

.EXAMPLE
    .\push.ps1

.EXAMPLE
    .\push.ps1 -Watch 30

.EXAMPLE
    .\push.ps1 -RepoName my-scx-lab -Owner myorg -Private
#>
[CmdletBinding()]
param(
    [string]$RepoName = 'openeuler-sched-ext',
    [string]$Owner = '',
    [switch]$Private,
    [string]$Description = 'Enable sched_ext on openEuler 24.03 LTS (Linux 6.6 baseline)',
    [int]$MaxTry = 6,
    [int]$Watch = 0
)

$ErrorActionPreference = 'Stop'

function Say  ($m) { Write-Host "[push] $m" -ForegroundColor Cyan }
function Good ($m) { Write-Host "[ ok ] $m" -ForegroundColor Green }
function Bad  ($m) { Write-Host "[fail] $m" -ForegroundColor Red }
function Warn ($m) { Write-Host "[warn] $m" -ForegroundColor Yellow }

# 调用原生命令并把 stderr 也吃掉，避免 PowerShell 5.1 把 stderr 当成终止错误
# ClearEnv: 调用期间要清空的代理变量名（Windows 环境变量名大小写不敏感，写一次即可）
function Invoke-Native {
    param(
        [string]$Exe,
        [string[]]$ArgList,
        [string[]]$ClearEnv = $null
    )
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'

    # 备份并按需清空代理相关环境变量，调用结束后原样恢复
    $names = @('http_proxy','https_proxy','all_proxy','no_proxy')
    $saved = @{}
    foreach ($n in $names) { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
    if ($ClearEnv) {
        foreach ($k in $ClearEnv) { [Environment]::SetEnvironmentVariable($k, $null) }
        # 有些受管环境会强制注入代理，SetEnvironmentVariable(null) 删不掉。
        # 实测过：删完读回来值还在。这时"直连"是假的，必须承认。
        if ([Environment]::GetEnvironmentVariable($ClearEnv[0])) {
            $script:DirectBlocked = $true
        }
    }
    try {
        $out = & $Exe @ArgList 2>&1
        $code = $LASTEXITCODE
    } catch {
        $out = @($_.Exception.Message)
        $code = 1
    } finally {
        foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
        $ErrorActionPreference = $prev
    }
    return [pscustomobject]@{ Output = $out; Code = $code }
}

# 直连模式：清空代理变量
$DirectEnv = @('http_proxy','https_proxy','all_proxy')
# 代理模式：保持环境原样，传 $null 即不清空
$ProxyEnv = $null

function Test-ProxyAvailable {
    foreach ($n in @('https_proxy','HTTPS_PROXY')) {
        $v = [Environment]::GetEnvironmentVariable($n)
        if ($v) { return $true }
    }
    return $false
}

<#
  直接测 TCP 443 是否连得上，5 秒超时。
  git push 走的是 github.com，gh 走的是 api.github.com —— 两者经常一个通一个不通，
  分开测才能定位到底是谁的问题。
#>
function Test-Endpoint {
    param([string]$HostName, [int]$Port = 443, [int]$TimeoutMs = 5000)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect($HostName, $Port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs, $false)) { return $false }
        $client.EndConnect($iar)
        return $true
    } catch {
        return $false
    } finally {
        try { $client.Close() } catch { }
    }
}

<#
  推送失败时给出可执行的判断，而不是一句"网络问题"。
#>
function Show-NetworkVerdict {
    Say '定位断点（分别探测两个域名）'
    $gitHost = Test-Endpoint 'github.com' 443 6000
    $apiHost = Test-Endpoint 'api.github.com' 443 6000

    $line1 = "github.com:443      " + $(if ($gitHost) { '可达' } else { '不可达' })
    $line2 = "api.github.com:443  " + $(if ($apiHost) { '可达' } else { '不可达' })
    Write-Host "  $line1" -ForegroundColor $(if ($gitHost) { 'Green' } else { 'Red' })
    Write-Host "  $line2" -ForegroundColor $(if ($apiHost) { 'Green' } else { 'Red' })

    if (-not $gitHost -and $apiHost) {
        Warn 'github.com 被阻断，但 api.github.com 正常。'
        Write-Host '  git push 必须走 github.com，所以这不是凭据或权限问题，只能等它恢复。' -ForegroundColor Yellow
        Write-Host '  浏览器能打开页面不代表 git 能推 —— 浏览器会重试、会走不同连接。' -ForegroundColor Yellow
        Write-Host '  建议：.\push.ps1 -Watch 30   挂着自动重试 30 分钟' -ForegroundColor Yellow
        return 'github-blocked'
    }
    if (-not $gitHost -and -not $apiHost) {
        Warn '两个域名都不通，整机出网有问题（断网 / 代理挂了 / 需要开 VPN）。'
        return 'offline'
    }
    return 'reachable'
}

<#
  带重试的推送。
  轮流尝试「直连」和「走环境代理」，任一成功即返回。
  成功后再用 ls-remote 核对远端分支是否已指向本地 HEAD。
#>
function Push-Robust {
    param(
        [string]$Remote = 'origin',
        [string]$Branch = 'main',
        [string[]]$ExtraArgs = @(),
        [int]$Tries = 0
    )
    if ($Tries -le 0) { $Tries = $MaxTry }

    $haveProxy = Test-ProxyAvailable
    if ($haveProxy) {
        $shown = [Environment]::GetEnvironmentVariable('https_proxy')
        Say "检测到环境代理: $shown"
    } else {
        Say '未检测到代理，仅尝试直连'
    }

    # 代理变量删不掉的环境，直连根本发不出去，别浪费 21 秒一次的超时
    $canDirect = -not $script:DirectBlocked
    if ($haveProxy -and -not $canDirect) {
        Warn '该环境的代理变量无法清除（被强制注入），只能走代理'
    }

    $lastMsg = ''
    for ($i = 1; $i -le $Tries; $i++) {
        # 奇数轮直连，偶数轮走代理；直连不可用时全部走代理
        $useProxy = $haveProxy -and ((-not $canDirect) -or ($i % 2 -eq 0))
        $label = if ($useProxy) { '走代理' } else { '直连' }
        $envs  = if ($useProxy) { $ProxyEnv } else { $DirectEnv }

        Say ("推送尝试 $i/$Tries（$label）")
        $pushArgs = @('push', $Remote, $Branch) + $ExtraArgs
        $r = Invoke-Native 'git' $pushArgs -ClearEnv $envs

        if ($r.Code -eq 0) {
            Good "推送成功（第 $i 次尝试，$label）"
            return $true
        }

        $lines = @($r.Output | Where-Object { "$_".Trim() })
        if ($lines.Count -gt 0) { $lastMsg = ("$($lines[-1])").Trim() }
        Warn "失败: $lastMsg"

        # 已经是 up-to-date 也算成功
        if ($lastMsg -match 'up-to-date|Everything up-to-date|已是最新') {
            Good '远端已是最新，无需推送'
            return $true
        }
        if ($i -lt $Tries) {
            $wait = [Math]::Min(3 * $i, 15)
            Say "等待 $wait 秒后重试..."
            Start-Sleep -Seconds $wait
        }
    }
    Bad "推送连续失败 $Tries 次"
    return $false
}

<#
  按 -Watch 决定是"试一次"还是"挂着一直试"。
#>
function Invoke-PushPhase {
    param(
        [string]$Remote = 'origin',
        [string]$Branch = 'main',
        [string[]]$ExtraArgs = @()
    )

    if ($Watch -le 0) {
        if (Push-Robust -Remote $Remote -Branch $Branch -ExtraArgs $ExtraArgs) { return $true }
        $null = Show-NetworkVerdict
        return $false
    }

    $deadline = (Get-Date).AddMinutes($Watch)
    $round = 1
    while ((Get-Date) -lt $deadline) {
        $left = [int][Math]::Ceiling(($deadline - (Get-Date)).TotalMinutes)
        Say "挂机模式第 $round 轮，剩余约 $left 分钟"
        if (Push-Robust -Remote $Remote -Branch $Branch -ExtraArgs $ExtraArgs -Tries 2) { return $true }
        $verdict = Show-NetworkVerdict
        if ($verdict -eq 'offline') {
            Warn '整机不通，挂机重试无意义，先检查网络'
            return $false
        }
        Say '30 秒后进入下一轮（Ctrl+C 可中断，已提交的内容不会丢）'
        Start-Sleep -Seconds 30
        $round++
    }
    Bad "挂机 $Watch 分钟仍未成功"
    return $false
}

<#
  核对远端分支是否真的指向本地 HEAD。
  这是唯一能证明「推上去了」的判据 —— git push 返回 0 也可能只是 no-op。
#>
function Confirm-Push {
    param([string]$Remote = 'origin', [string]$Branch = 'main')

    $local = (& git rev-parse HEAD 2>$null)
    if (-not $local) { Warn '拿不到本地 HEAD，跳过核对'; return $false }
    $local = $local.Trim()

    Say '核对远端与本地是否一致'
    for ($i = 1; $i -le 4; $i++) {
        $useProxy = (Test-ProxyAvailable) -and ($script:DirectBlocked -or ($i % 2 -eq 0))
        $envs = if ($useProxy) { $ProxyEnv } else { $DirectEnv }
        $r = Invoke-Native 'git' @('ls-remote', $Remote, "refs/heads/$Branch") -ClearEnv $envs
        if ($r.Code -eq 0) {
            $txt = ("$($r.Output)" -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -First 1)
            $remoteSha = ''
            if ($txt) { $remoteSha = ($txt -split '\s+')[0].Trim() }
            if ($remoteSha -eq $local) {
                Good "远端 HEAD = 本地 HEAD = $($local.Substring(0,7))"
                return $true
            }
            Warn "不一致：远端 $($remoteSha.Substring(0,[Math]::Min(7,$remoteSha.Length))) / 本地 $($local.Substring(0,7))"
            return $false
        }
        if ($i -lt 4) { Start-Sleep -Seconds (2 * $i) }
    }
    Warn '无法连接远端完成核对（推送本身可能已成功）'
    return $false
}

# ---------------------------------------------------------------- 前置检查
# git / gh 装了但不在 PATH 里是最常见的失败原因（安装器勾选了"仅当前用户"
# 或者装完没重开终端）。这里主动去常见位置找，找到就把目录塞进 PATH。
function Add-ToolToPath {
    param([string]$Name, [string[]]$Candidates)
    if (Get-Command $Name -ErrorAction SilentlyContinue) { return $true }
    foreach ($c in $Candidates) {
        if (Test-Path $c) {
            $dir = Split-Path -Parent $c
            $env:PATH = "$dir;$env:PATH"
            Say "从 $c 找到 $Name（已临时加入 PATH）"
            return $true
        }
    }
    return $false
}

Say '检查 git / gh'
$gitOk = Add-ToolToPath 'git' @(
    'C:\Program Files\Git\cmd\git.exe',
    'C:\Program Files\Git\bin\git.exe',
    'C:\Program Files (x86)\Git\cmd\git.exe',
    "$env:LOCALAPPDATA\Programs\Git\cmd\git.exe"
)
if (-not $gitOk) {
    Bad '找不到 git。先装 Git for Windows: https://git-scm.com/download/win'
    exit 1
}
$ghOk = Add-ToolToPath 'gh' @(
    'C:\Program Files\GitHub CLI\gh.exe',
    'C:\Program Files (x86)\GitHub CLI\gh.exe',
    "$env:LOCALAPPDATA\GitHubCLI\gh.exe"
)
if (-not $ghOk) {
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
    if (-not (Invoke-PushPhase -Remote 'origin' -Branch 'main')) { exit 1 }
} else {
    Say '在 GitHub 创建仓库（先不推送）'
    $vis = '--public'
    if ($Private) { $vis = '--private' }
    $r = Invoke-Native 'gh' @(
        'repo', 'create', $full, $vis,
        '--description', $Description,
        '--source', '.', '--remote', 'origin'
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
    # 仓库建好后再用带重试的推送，而不是让 gh 一次定生死
    if (-not (Invoke-PushPhase -Remote 'origin' -Branch 'main' -ExtraArgs @('-u'))) { exit 1 }
}

# ---------------------------------------------------------------- 收尾
$verified = Confirm-Push -Remote 'origin' -Branch 'main'

$url = "https://github.com/$full"
Write-Host ''
Good "发布完成: $url"
if (-not $verified) {
    Warn '远端核对未通过。推送命令返回成功但未能确认，请打开上面的地址看一眼。'
}
Write-Host ''
Write-Host '确认方式：浏览器打开上面这个地址，能看到 README 就成功了。'
Write-Host ''
try { Start-Process $url } catch { }

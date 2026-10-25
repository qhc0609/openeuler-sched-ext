<#
.SYNOPSIS
    从 Windows 直接驱动 VMware 里的 openEuler 虚拟机 —— 这是"一键"的关键桥。

.DESCRIPTION
    WorkBuddy（或你的终端）跑在 Windows 宿主上，内核编译必须在虚拟机里做。
    本脚本用 SSH 把两端连起来，于是宿主侧的一条命令就能等价于虚拟机里的一条命令。

    前置（在虚拟机里执行一次）：
        sudo dnf install -y openssh-server
        sudo systemctl enable --now sshd
        hostname -I            # 记下 IP
        ip -4 addr show | grep inet

    然后在 Windows 侧建 .vm.env（参照 .vm.env.example），之后就能：
        .\vm-ssh.ps1 -Command "uname -r"
        .\vm-ssh.ps1 -Upload
        .\vm-ssh.ps1 -Command "cd ~/openeuler-sched-ext && ./scripts/00-preflight.sh"

.PARAMETER Command
    要在虚拟机里执行的命令。含空格请用引号包住。

.PARAMETER Upload
    把整个项目目录同步到虚拟机 ~/openeuler-sched-ext。

.PARAMETER Shell
    上传后顺带在虚拟机里起一个交互式 shell。

.EXAMPLE
    .\vm-ssh.ps1 -Command "uname -r && grep -c SCHED_CLASS_EXT /boot/config-`$(uname -r)"

.EXAMPLE
    .\vm-ssh.ps1 -Upload
#>
[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run', Position = 0)]
    [string]$Command = '',

    [Parameter(ParameterSetName = 'Upload')]
    [switch]$Upload,

    [Parameter(ParameterSetName = 'Upload')]
    [switch]$Shell,

    [string]$EnvFile = '.vm.env'
)

$ErrorActionPreference = 'Stop'

function Say  ($m) { Write-Host "[vm] $m" -ForegroundColor Cyan }
function Good ($m) { Write-Host "[ok] $m" -ForegroundColor Green }
function Bad  ($m) { Write-Host "[--] $m" -ForegroundColor Red }

# ---------------------------------------------------------------- 读配置
$envPath = if ([System.IO.Path]::IsPathRooted($EnvFile)) { $EnvFile }
           else { Join-Path $PSScriptRoot $EnvFile }

if (-not (Test-Path $envPath)) {
    Bad "找不到 $envPath"
    Write-Host ''
    Write-Host '先创建它（复制 .vm.env.example 改名即可）：' -ForegroundColor Yellow
    Write-Host '  Copy-Item .vm.env.example .vm.env'
    Write-Host '  notepad .vm.env'
    Write-Host ''
    Write-Host '内容形如：'
    Write-Host '  VM_HOST=192.168.1.50'
    Write-Host '  VM_USER=root'
    Write-Host '  VM_PORT=22'
    Write-Host '  VM_KEY=C:\Users\Administrator\.ssh\id_ed25519'
    exit 1
}

$cfg = @{}
Get-Content $envPath | ForEach-Object {
    $line = $_.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { return }
    $i = $line.IndexOf('=')
    if ($i -lt 1) { return }
    $cfg[$line.Substring(0, $i).Trim()] = $line.Substring($i + 1).Trim()
}

if (-not $cfg['VM_HOST']) { Bad '.vm.env 里缺少 VM_HOST'; exit 1 }
if (-not $cfg['VM_USER']) { $cfg['VM_USER'] = 'root' }
if (-not $cfg['VM_PORT']) { $cfg['VM_PORT'] = '22' }

$target = "$($cfg['VM_USER'])@$($cfg['VM_HOST'])"
$port   = $cfg['VM_PORT']

if (-not (Get-Command ssh -ErrorAction SilentlyContinue)) {
    Bad '没有 ssh 客户端。Windows Server 2022 自带，检查：Add-WindowsCapability -Online -Name OpenSSH.Client~~~~0.0.1.0'
    exit 1
}

# 公共 ssh 选项
$sshOpts = @(
    '-o', 'StrictHostKeyChecking=accept-new',
    '-o', 'ConnectTimeout=10',
    '-p', $port
)
if ($cfg['VM_KEY']) { $sshOpts = @('-i', $cfg['VM_KEY']) + $sshOpts }

# ---------------------------------------------------------------- 连通性
Say "测试连接 $target : $port"
& ssh @sshOpts $target 'echo __OK__' 2>&1 | Out-String | ForEach-Object {
    if ($_ -notmatch '__OK__') {
        Bad '连不上。检查：'
        Write-Host '  1) 虚拟机里 sshd 在跑:  systemctl status sshd'
        Write-Host '  2) 防火墙放行:          firewall-cmd --add-service=ssh --permanent && firewall-cmd --reload'
        Write-Host '  3) IP 是否正确:         hostname -I'
        Write-Host '  4) VMware 网络模式:     NAT 需端口转发，桥接则直接用虚拟机 IP'
        Write-Host ''
        Write-Host $_ -ForegroundColor DarkGray
        exit 1
    }
}
Good '连接正常'

# ---------------------------------------------------------------- 上传
if ($Upload) {
    Say '同步项目到虚拟机 ~/openeuler-sched-ext'
    $remote = "$($cfg['VM_USER'])@$($cfg['VM_HOST'])"
    $scpOpts = @('-o', 'StrictHostKeyChecking=accept-new', '-P', $port, '-r')
    if ($cfg['VM_KEY']) { $scpOpts = @('-i', $cfg['VM_KEY']) + $scpOpts }

    # 先建目录，再整目录拷过去（tar 管道比 scp -r 快，且保留权限）
    & ssh @sshOpts $target 'mkdir -p ~/openeuler-sched-ext'
    if ($LASTEXITCODE -ne 0) { Bad '创建远端目录失败'; exit 1 }

    Push-Location $PSScriptRoot
    try {
        tar -czf - --exclude=.git --exclude=target --exclude=build . |
            & ssh @sshOpts $target 'tar -xzf - -C ~/openeuler-sched-ext && chmod +x ~/openeuler-sched-ext/scripts/*.sh'
    } finally {
        Pop-Location
    }
    if ($LASTEXITCODE -ne 0) { Bad '上传失败'; exit 1 }
    Good '已同步到 ~/openeuler-sched-ext'

    if ($Shell) {
        Say '进入交互式 shell（exit 退出）'
        & ssh @sshOpts -t $target 'cd ~/openeuler-sched-ext && exec bash -l'
    }
    exit 0
}

# ---------------------------------------------------------------- 执行命令
if ([string]::IsNullOrWhiteSpace($Command)) {
    Write-Host '用法：'
    Write-Host '  .\vm-ssh.ps1 -Command "uname -r"'
    Write-Host '  .\vm-ssh.ps1 -Upload'
    Write-Host '  .\vm-ssh.ps1 -Upload -Shell'
    exit 2
}

Say "执行: $Command"
& ssh @sshOpts -t $target $Command
exit $LASTEXITCODE

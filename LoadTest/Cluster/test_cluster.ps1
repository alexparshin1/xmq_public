# Native Windows front-end for the test cluster.
#
# The cluster nodes are Linux processes; what runs here is only the command surface. The actual
# orchestration - directories, certificates, configurations, launchers - stays in test_cluster.sh on
# a Linux control host, reached with the OpenSSH client that ships with Windows. No WSL, no Git
# Bash, no extra PowerShell module.
#
#   $env:CLUSTER_CONTROL_HOST = "theater"     # required; a host with ~/cluster/test_cluster.sh
#   .\test_cluster.ps1 status
#   .\test_cluster.ps1 start node1
#   test_cluster.cmd stop node1
#
# CLUSTER_REMOTE_SCRIPT overrides ~/cluster/test_cluster.sh on the control host.

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Arguments
)

$ErrorActionPreference = "Stop"

$controlHost = $env:CLUSTER_CONTROL_HOST
if ([string]::IsNullOrWhiteSpace($controlHost)) {
    Write-Error "CLUSTER_CONTROL_HOST is not set. Point it at the Linux host that has ~/cluster/test_cluster.sh, for example: `$env:CLUSTER_CONTROL_HOST = 'theater'"
    exit 2
}

$remoteScript = $env:CLUSTER_REMOTE_SCRIPT
if ([string]::IsNullOrWhiteSpace($remoteScript)) {
    $remoteScript = "~/cluster/test_cluster.sh"
}

$controlUser = $env:CLUSTER_CONTROL_USER
if ([string]::IsNullOrWhiteSpace($controlUser)) {
    $controlUser = "alexeyp"
}
$destination = if ($controlHost.Contains("@")) { $controlHost } else { "$controlUser@$controlHost" }

function Quote-Posix([string] $value) {
    # POSIX single-quote: close, escaped quote, reopen.
    return "'" + $value.Replace("'", "'\''") + "'"
}

$ssh = Get-Command ssh.exe -ErrorAction SilentlyContinue
if ($null -eq $ssh) {
    Write-Error "ssh.exe was not found. Enable the Windows OpenSSH Client optional feature; WSL and Git Bash are not needed."
    exit 2
}

$argumentLine = ($Arguments | ForEach-Object { Quote-Posix $_ }) -join " "
$remoteCommand = "bash -lc " + (Quote-Posix ($remoteScript + " " + $argumentLine))

& $ssh.Source -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 $destination $remoteCommand
exit $LASTEXITCODE

# Downloads from Microsoft only when the user runs Install Windows Runtime.cmd.
# No unattended installation, automatic elevation, or machine policy changes.
param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$runtimeLock = Join-Path $PSScriptRoot 'vc-redist-x64.json'
try {
    $runtime = Get-Content -LiteralPath $runtimeLock -Raw | ConvertFrom-Json
    $uri = [Uri]$runtime.url
    if ($runtime.id -ne 'vc-redist-x64' -or $runtime.mode -ne 'download' -or
        $uri.Scheme -ne 'https' -or $uri.DnsSafeHost -ne 'download.visualstudio.microsoft.com' -or
        $runtime.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'The packaged Microsoft runtime download information is invalid.'
    }
    if ($CheckOnly) {
        Write-Host "Verified download information for $($runtime.name) $($runtime.version)."
        exit 0
    }
    Write-Host "Microsoft Visual C++ x64 runtime $($runtime.version)"
    Write-Host 'Downloading the official installer from Microsoft and checking its signature.'
    Write-Host 'The installer will open normally so you can review its terms and choose Install or Cancel.'
    $runtimeDir = Join-Path ([IO.Path]::GetTempPath()) ('RidgeRacerMarathon-Runtime-' + $runtime.sha256)
    [IO.Directory]::CreateDirectory($runtimeDir) | Out-Null
    $installer = Join-Path $runtimeDir 'VC_redist.x64.exe'
    if (!(Test-Path -LiteralPath $installer) -or
        (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $runtime.sha256) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $runtime.url -OutFile $installer -UseBasicParsing
    }
    if ((Get-Item -LiteralPath $installer).Length -ne $runtime.size -or
        (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $runtime.sha256) {
        Remove-Item -LiteralPath $installer -Force
        throw 'The downloaded installer failed its SHA256 check. Nothing was run.'
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $installer
    if ($signature.Status -ne 'Valid' -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw 'The installer did not have a valid Microsoft signature. Nothing was run.'
    }
    Write-Host 'Hash and Microsoft signature verified. Opening the installer...'
    $process = Start-Process -FilePath $installer -PassThru -Wait
    if ($process.ExitCode -eq 0) {
        Write-Host 'Runtime setup completed. You can now launch Ridge Racer Marathon.'
    } elseif ($process.ExitCode -eq 3010) {
        Write-Host 'Runtime setup completed. Windows reports that a restart is needed.'
    } else {
        Write-Host "The installer exited with code $($process.ExitCode). No unattended retry will run."
    }
} catch {
    Write-Host "Runtime setup stopped: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'Official Microsoft download page: https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/'
    exit 1
}

# Copyright (c) 2026 The ConnectCoin Core developers. MIT license.
# Download pinned build tools into an isolated directory, never install a product.
[CmdletBinding()]
param([string]$ToolsParent = (Join-Path $PSScriptRoot '..\..\build\msi-tools'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-VerifiedFile([string]$Url, [string]$Path, [string]$Sha256) {
    Invoke-WebRequest -Uri $Url -OutFile $Path
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Downloaded tool checksum mismatch: $Path"
    }
}

$parentDirectory = [IO.Path]::GetFullPath($ToolsParent)
$toolDirectory = Join-Path $parentDirectory ('wix-5.0.2-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $toolDirectory -Force | Out-Null
$cliMsi = Join-Path $toolDirectory 'wix-cli-x64.msi'
$uiArchive = Join-Path $toolDirectory 'wixtoolset.ui.wixext.5.0.2.zip'
Write-Host 'Downloading pinned WiX 5.0.2 CLI and UI extension (build tools only)...'
Get-VerifiedFile 'https://github.com/wixtoolset/wix/releases/download/v5.0.2/wix-cli-x64.msi' $cliMsi '097383B2773BDD3D76A6A2CD3F4DE6357BAE59172E1B6FA5AD87CE062074E8D0'
Get-VerifiedFile 'https://api.nuget.org/v3-flatcontainer/wixtoolset.ui.wixext/5.0.2/wixtoolset.ui.wixext.5.0.2.nupkg' $uiArchive '5EF2C707614B9F70B6BBADD2D4ABCB4124EFEE215E9B16BFBC80113079A604C7'
$cliDirectory = Join-Path $toolDirectory 'cli'
$log = Join-Path $toolDirectory 'extract.log'
$process = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList @(
    '/a', ('"' + $cliMsi + '"'), '/qn', ('TARGETDIR="' + $cliDirectory + '"'),
    '/L*v', ('"' + $log + '"')
) -PassThru -Wait -WindowStyle Hidden
if ($process.ExitCode -ne 0) {
    throw "WiX administrative extraction failed ($($process.ExitCode)); see $log"
}
$uiDirectory = Join-Path $toolDirectory 'ui'
Expand-Archive -LiteralPath $uiArchive -DestinationPath $uiDirectory
$wix = Join-Path $cliDirectory 'PFiles64\WiX Toolset v5.0\bin\wix.exe'
$ui = Join-Path $uiDirectory 'wixext5\WixToolset.UI.wixext.dll'
foreach ($binary in @($wix, $ui)) {
    $signature = Get-AuthenticodeSignature -LiteralPath $binary
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'WiX Toolset') {
        throw "Expected a valid WiX Toolset signature on $binary"
    }
}
$version = & $wix --version
if ($LASTEXITCODE -ne 0 -or $version -notlike '5.0.2+*') {
    throw 'Unexpected WiX CLI version or missing build-tool runtime.'
}
Write-Host "WiX ready in $toolDirectory; no application was installed."
[pscustomobject]@{ Wix = $wix; UiExtension = $ui; Directory = $toolDirectory }

<#
.SYNOPSIS
Builds Windows Terminal with GPU-accelerated smooth scrolling and produces both
distributions: a signed MSIX for a normal install, and a self-contained portable folder
(plus zip) that runs without installing anything.

.DESCRIPTION
This wraps the pieces the repository already has:
  * MSBuild builds CascadiaPackage, which lays out the MSIX payload.
  * A self-signed certificate is created once (CurrentUser\My) and used to sign the MSIX
    so that it can be side-loaded. The .cer is exported next to the package; installing
    it into LocalMachine\TrustedPeople is the one step that needs elevation.
  * build\scripts\New-UnpackagedTerminalDistribution.ps1 turns the same layout into the
    portable distribution, with the .portable marker so settings stay in the folder.

.PARAMETER Platform
x64 (default) or arm64.

.PARAMETER Configuration
Release (default) or Debug.

.PARAMETER SkipBuild
Package what is already built instead of building again.

.PARAMETER OutputDirectory
Where the artifacts are written. Defaults to <repo>\dist.
#>
[CmdletBinding()]
Param(
    [ValidateSet('x64', 'arm64', 'x86')]
    [string]$Platform = 'x64',

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [switch]$SkipBuild,

    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'dist' }

$certSubject = 'CN=WTF Terminal (Smooth Scrolling) Development'
$certFriendlyName = 'WTF Terminal smooth scrolling - development signing'

function Get-MSBuildPath {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Is Visual Studio (or the Build Tools) installed?" }
    $installPath = & $vswhere -products * -requires Microsoft.Component.MSBuild -property installationPath -latest
    if (-not $installPath) { throw 'No Visual Studio installation with MSBuild was found.' }
    $msbuild = Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path $msbuild)) { throw "MSBuild.exe not found at $msbuild" }
    return $msbuild
}

function Get-WindowsSdkTool([string]$Name) {
    $kitsRoot = (Get-ItemProperty 'HKLM:\Software\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10).KitsRoot10
    $candidates = Get-ChildItem (Join-Path $kitsRoot 'bin') -Directory |
        Where-Object { $_.Name -match '^10\.' } |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$Name" } |
        Where-Object { Test-Path $_ }
    if (-not $candidates) { throw "$Name was not found in any Windows SDK under $kitsRoot." }
    return $candidates[0]
}

# ---------------------------------------------------------------------------- build
$msbuild = Get-MSBuildPath
$env:MSBUILDENABLESLNXSUPPORT = '1'

if (-not $SkipBuild) {
    Write-Host "==> Building CascadiaPackage ($Configuration|$Platform)" -ForegroundColor Cyan
    & $msbuild (Join-Path $repoRoot 'src\cascadia\CascadiaPackage\CascadiaPackage.wapproj') `
        "-p:Configuration=$Configuration" `
        "-p:Platform=$Platform" `
        "-p:SolutionDir=$repoRoot\\" `
        '-p:SolutionName=OpenConsole' `
        '-p:AppxBundle=Never' `
        '-p:AppxPackageSigningEnabled=false' `
        '-p:UapAppxPackageBuildMode=SideloadOnly' `
        '-m' -nologo -v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
}

$layoutRoot = Join-Path $repoRoot "src\cascadia\CascadiaPackage\bin\$Platform\$Configuration"
if (-not (Test-Path (Join-Path $layoutRoot 'AppxManifest.xml'))) {
    throw "No MSIX layout at $layoutRoot. Build first (drop -SkipBuild)."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$manifest = [xml](Get-Content (Join-Path $layoutRoot 'AppxManifest.xml'))
$version = $manifest.Package.Identity.Version
$baseName = "WtfTerminal_${version}_$Platform"

# ------------------------------------------------------------------------ signing key
Write-Host '==> Preparing the development signing certificate' -ForegroundColor Cyan
$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $certSubject -and $_.NotAfter -gt (Get-Date) } |
    Select-Object -First 1

if (-not $cert) {
    $cert = New-SelfSignedCertificate `
        -Type Custom `
        -Subject $certSubject `
        -FriendlyName $certFriendlyName `
        -KeyUsage DigitalSignature `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -NotAfter (Get-Date).AddYears(3)
    Write-Host "    created $($cert.Thumbprint)"
} else {
    Write-Host "    reusing $($cert.Thumbprint)"
}

$cerPath = Join-Path $OutputDirectory "$baseName.cer"
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null

# The manifest publisher has to match the signing certificate or signtool refuses. The
# repo ships a Microsoft publisher, so patch the layout copy before packing.
$layoutManifest = Join-Path $layoutRoot 'AppxManifest.xml'
$manifestXml = [xml](Get-Content $layoutManifest)
if ($manifestXml.Package.Identity.Publisher -ne $cert.Subject) {
    Write-Host "    rewriting manifest publisher to $($cert.Subject)"
    $manifestXml.Package.Identity.Publisher = $cert.Subject
    $manifestXml.Save($layoutManifest)
}

# --------------------------------------------------------------------------- the MSIX
Write-Host '==> Packing the MSIX' -ForegroundColor Cyan
$makeappx = Get-WindowsSdkTool 'MakeAppx.exe'
$signtool = Get-WindowsSdkTool 'SignTool.exe'
$msixPath = Join-Path $OutputDirectory "$baseName.msix"

& $makeappx pack /o /d $layoutRoot /p $msixPath | Write-Verbose
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed with exit code $LASTEXITCODE." }

& $signtool sign /fd SHA256 /a /sha1 $cert.Thumbprint $msixPath | Write-Verbose
if ($LASTEXITCODE -ne 0) { throw "SignTool failed with exit code $LASTEXITCODE." }
Write-Host "    $msixPath"

# ---------------------------------------------------------------------- portable build
Write-Host '==> Building the portable distribution' -ForegroundColor Cyan
$xamlAppx = Join-Path $repoRoot "packages\Microsoft.UI.Xaml.2.8.4\tools\AppX\$Platform\Release\Microsoft.UI.Xaml.2.8.appx"
if (-not (Test-Path $xamlAppx)) {
    throw "The Microsoft.UI.Xaml AppX is missing at $xamlAppx. Restore NuGet packages first."
}

& (Join-Path $PSScriptRoot 'New-UnpackagedTerminalDistribution.ps1') `
    -TerminalLayout $layoutRoot `
    -XamlAppX $xamlAppx `
    -Destination $OutputDirectory `
    -MakeAppxPath $makeappx `
    -PortableMode

$portableZip = Get-ChildItem $OutputDirectory -Filter '*.zip' | Sort-Object LastWriteTime -Descending | Select-Object -First 1

# ------------------------------------------------------------------------- install aid
$installScript = Join-Path $OutputDirectory 'Install-WtfTerminal.ps1'
@"
# Installs the smooth-scrolling Windows Terminal build produced by
# build\scripts\New-SmoothScrollingRelease.ps1.
#
# Run this from an elevated PowerShell: the signing certificate has to go into the
# machine-wide TrustedPeople store before Windows will accept a side-loaded MSIX.
`$ErrorActionPreference = 'Stop'
`$here = Split-Path -Parent `$MyInvocation.MyCommand.Path

Import-Certificate -FilePath (Join-Path `$here '$baseName.cer') -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
Add-AppxPackage -Path (Join-Path `$here '$baseName.msix')

Write-Host 'Installed. Look for "Terminal (Smooth Scrolling)" in the Start menu.'
"@ | Set-Content -Path $installScript -Encoding UTF8

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host "  MSIX      : $msixPath"
Write-Host "  Certificate: $cerPath"
if ($portableZip) { Write-Host "  Portable  : $($portableZip.FullName)" }
Write-Host "  Installer : $installScript (run elevated)"

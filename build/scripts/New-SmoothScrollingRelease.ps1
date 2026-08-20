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
    $candidates = @(Get-ChildItem (Join-Path $kitsRoot 'bin') -Directory |
        Where-Object { $_.Name -match '^10\.' } |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$Name" } |
        Where-Object { Test-Path $_ })
    if ($candidates.Count -eq 0) { throw "$Name was not found in any Windows SDK under $kitsRoot." }
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

# --------------------------------------------------------------------------- the MSIX
Write-Host '==> Packing the MSIX' -ForegroundColor Cyan
$makeappx = Get-WindowsSdkTool 'MakeAppx.exe'
$signtool = Get-WindowsSdkTool 'SignTool.exe'
$msixPath = Join-Path $OutputDirectory "$baseName.msix"

# Pack from a copy: the manifest publisher has to be rewritten to match the signing
# certificate (signtool refuses otherwise), and the layout carries build leftovers that
# have no business in a shipped package. Neither should touch the build output.
$packRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('wtfpack_' + [guid]::NewGuid().ToString('N'))
Copy-Item -Path $layoutRoot -Destination $packRoot -Recurse
try {
    Get-ChildItem $packRoot -Recurse -File -Include '*.pdb', '*.lib', '*.exp', '*.ipdb', '*.iobj', '*.ilk' |
        Remove-Item -Force

    $packManifest = Join-Path $packRoot 'AppxManifest.xml'
    $manifestXml = [xml](Get-Content $packManifest)
    if ($manifestXml.Package.Identity.Publisher -ne $cert.Subject) {
        Write-Host "    rewriting manifest publisher to $($cert.Subject)"
        $manifestXml.Package.Identity.Publisher = $cert.Subject
        $manifestXml.Save($packManifest)
    }

    & $makeappx pack /o /d $packRoot /p $msixPath | Write-Verbose
    if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed with exit code $LASTEXITCODE." }
}
finally {
    Remove-Item $packRoot -Recurse -Force -ErrorAction SilentlyContinue
}

& $signtool sign /fd SHA256 /a /sha1 $cert.Thumbprint $msixPath | Write-Verbose
if ($LASTEXITCODE -ne 0) { throw "SignTool failed with exit code $LASTEXITCODE." }
Write-Host "    $msixPath  ($([int]((Get-Item $msixPath).Length / 1MB)) MB)"

# ---------------------------------------------------------------------- portable build
Write-Host '==> Building the portable distribution' -ForegroundColor Cyan
$xamlAppx = Join-Path $repoRoot "packages\Microsoft.UI.Xaml.2.8.4\tools\AppX\$Platform\Release\Microsoft.UI.Xaml.2.8.appx"
if (-not (Test-Path $xamlAppx)) {
    throw "The Microsoft.UI.Xaml AppX is missing at $xamlAppx. Restore NuGet packages first."
}

# New-UnpackagedTerminalDistribution.ps1 reads $Verbose, which strict mode refuses to
# resolve when it is not set. Strict mode flows into the callee, so lift it for the call.
Set-StrictMode -Off
# In -TerminalLayout mode the script leaves the finished tree in a temp directory and
# returns it; it only zips when combining two AppX files. So take it from there.
$portableSource = & (Join-Path $PSScriptRoot 'New-UnpackagedTerminalDistribution.ps1') `
    -TerminalLayout $layoutRoot `
    -XamlAppX $xamlAppx `
    -Destination $OutputDirectory `
    -MakeAppxPath $makeappx `
    -PortableMode
Set-StrictMode -Version Latest

if (-not $portableSource) { throw 'The portable distribution was not produced.' }

$portableDir = Join-Path $OutputDirectory "${baseName}_portable"
if (Test-Path $portableDir) { Remove-Item $portableDir -Recurse -Force }
New-Item -ItemType Directory -Path $portableDir | Out-Null
Copy-Item -Path (Join-Path $portableSource.FullName '*') -Destination $portableDir -Recurse

# Debug symbols dwarf the actual program (the PDBs are ~750 MB); they have no place in
# a distribution. They stay in bind\Release for anyone who needs to debug a crash.
Get-ChildItem $portableDir -Recurse -File -Include '*.pdb', '*.ilk', '*.exp', '*.lib', '*.ipdb', '*.iobj' |
    Remove-Item -Force

# The temp tree lives one level above what the script hands back.
Remove-Item (Split-Path -Parent $portableSource.FullName) -Recurse -Force -ErrorAction SilentlyContinue

$portableZipPath = Join-Path $OutputDirectory "${baseName}_portable.zip"
if (Test-Path $portableZipPath) { Remove-Item $portableZipPath -Force }
Compress-Archive -Path (Join-Path $portableDir '*') -DestinationPath $portableZipPath
$portableZip = Get-Item $portableZipPath
Write-Host "    $portableZipPath  ($([int]($portableZip.Length / 1MB)) MB)"

# ------------------------------------------------------------------------- install aid
$thumbprint = $cert.Thumbprint
$packageName = $manifest.Package.Identity.Name

$installScript = Join-Path $OutputDirectory 'Install-WtfTerminal.ps1'
@"
<#
.SYNOPSIS
    Installs the smooth-scrolling Windows Terminal build.

.DESCRIPTION
    The package is signed with a self-signed certificate, so the certificate has to be
    trusted machine-wide before Windows will accept the MSIX. Both steps need
    elevation; the script elevates itself.

.PARAMETER Uninstall
    Removes the package and the certificate again.
#>
[CmdletBinding()]
param([switch]`$Uninstall)

`$ErrorActionPreference = 'Stop'

`$here        = Split-Path -Parent `$MyInvocation.MyCommand.Path
`$msix        = Join-Path `$here '$baseName.msix'
`$cer         = Join-Path `$here '$baseName.cer'
`$dependency  = Join-Path `$here 'Microsoft.UI.Xaml.2.8.appx'
`$packageName = '$packageName'
`$thumbprint  = '$thumbprint'

function Test-Admin {
    `$id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal(`$id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    Write-Host 'Elevation is required; relaunching...' -ForegroundColor Yellow
    `$argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "```"`$PSCommandPath```"")
    if (`$Uninstall) { `$argList += '-Uninstall' }
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList `$argList
    return
}

if (`$Uninstall) {
    Get-AppxPackage -Name `$packageName | ForEach-Object {
        Write-Host "Removing `$(`$_.PackageFullName)..." -ForegroundColor Cyan
        Remove-AppxPackage -Package `$_.PackageFullName
    }
    Get-ChildItem Cert:\LocalMachine\TrustedPeople |
        Where-Object Thumbprint -eq `$thumbprint |
        ForEach-Object {
            Write-Host 'Removing the signing certificate...' -ForegroundColor Cyan
            Remove-Item `$_.PSPath -Force
        }
    Write-Host 'Done.' -ForegroundColor Green
    return
}

foreach (`$f in @(`$msix, `$cer)) {
    if (-not (Test-Path `$f)) { throw "Missing file: `$f" }
}

if (-not (Get-ChildItem Cert:\LocalMachine\TrustedPeople |
          Where-Object Thumbprint -eq `$thumbprint)) {
    Write-Host 'Trusting the signing certificate...' -ForegroundColor Cyan
    Import-Certificate -FilePath `$cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
} else {
    Write-Host 'The signing certificate is already trusted.' -ForegroundColor DarkGray
}

`$addArgs = @{ Path = `$msix }
if ((Test-Path `$dependency) -and -not (Get-AppxPackage -Name 'Microsoft.UI.Xaml.2.8')) {
    `$addArgs['DependencyPath'] = `$dependency
}

Write-Host 'Installing...' -ForegroundColor Cyan
Add-AppxPackage @addArgs

`$installed = Get-AppxPackage -Name `$packageName
if (-not `$installed) { throw 'The installation did not take.' }

Write-Host "Installed: `$(`$installed.PackageFullName)" -ForegroundColor Green
Write-Host 'Start it from the Start menu, or with: wtd' -ForegroundColor Green
"@ | Set-Content -Path $installScript -Encoding UTF8

# The WinUI 2.8 dependency, so the package installs on a machine without it.
Copy-Item $xamlAppx (Join-Path $OutputDirectory 'Microsoft.UI.Xaml.2.8.appx') -Force

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host "  MSIX      : $msixPath"
Write-Host "  Certificate: $cerPath"
if ($portableZip) { Write-Host "  Portable  : $($portableZip.FullName)" }
Write-Host "  Installer : $installScript (run elevated)"

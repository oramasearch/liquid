$ErrorActionPreference = "Stop"

$LiquidBinary = (Resolve-Path $args[0]).Path
$ProjectionFixture = (Resolve-Path $args[1]).Path
$InstallerProgram = (Resolve-Path $args[2]).Path
$TestRoot = Join-Path $env:RUNNER_TEMP "liquid-windows-installer"
$Certificate = $null
$Package = $null

try {
    Remove-Item -LiteralPath $TestRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path (Join-Path $TestRoot "root\bin") -Force | Out-Null
    Copy-Item -LiteralPath $InstallerProgram `
        -Destination (Join-Path $TestRoot "root\bin\liquid-installer-fixture.exe")

    $SdkBin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" `
        -Directory | Sort-Object Name -Descending | ForEach-Object {
            Join-Path $_.FullName "x64"
        } | Where-Object {
            (Test-Path (Join-Path $_ "makeappx.exe")) -and `
            (Test-Path (Join-Path $_ "signtool.exe"))
        } | Select-Object -First 1
    if (-not $SdkBin) {
        throw "Windows SDK makeappx and signtool were not found"
    }
    $env:PATH = "$SdkBin;$env:PATH"

    & $ProjectionFixture msix `
        (Join-Path $TestRoot "root") `
        (Join-Path $TestRoot "output") `
        (Join-Path $TestRoot "repository") | Tee-Object -FilePath (Join-Path $TestRoot "project.txt")
    if ($LASTEXITCODE -ne 0) {
        throw "MSIX projection failed"
    }
    $ArtifactLine = Get-Content (Join-Path $TestRoot "project.txt") | `
        Where-Object { $_.StartsWith("artifact=") }
    $Artifact = $ArtifactLine.Substring("artifact=".Length)

    $Certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject "CN=Liquid Installer Test" `
        -CertStoreLocation "Cert:\CurrentUser\My"
    $CertificatePath = Join-Path $TestRoot "certificate.cer"
    Export-Certificate -Cert $Certificate -FilePath $CertificatePath | Out-Null
    Import-Certificate -FilePath $CertificatePath -CertStoreLocation "Cert:\CurrentUser\Root" | Out-Null
    $PfxPath = Join-Path $TestRoot "identity.pfx"
    $Password = ConvertTo-SecureString "liquid-test" -AsPlainText -Force
    Export-PfxCertificate -Cert $Certificate -FilePath $PfxPath -Password $Password | Out-Null
    $env:LIQUID_WINDOWS_PFX_PASSWORD = "liquid-test"

    Push-Location (Join-Path $TestRoot "repository")
    try {
        & $LiquidBinary sign windows $Artifact --identity "file:$PfxPath" |
            Tee-Object -FilePath (Join-Path $TestRoot "sign.txt")
        if ($LASTEXITCODE -ne 0) {
            throw "MSIX signing failed"
        }
    }
    finally {
        Pop-Location
    }
    $SignedLine = Get-Content (Join-Path $TestRoot "sign.txt") | `
        Where-Object { $_.StartsWith("artifact: ") }
    $SignedArtifact = ($SignedLine -split " ")[1]
    $Signature = Get-AuthenticodeSignature -LiteralPath $SignedArtifact
    if ($Signature.Status -ne "Valid") {
        throw "Authenticode status is $($Signature.Status)"
    }
    Add-AppxPackage -Path $SignedArtifact
    $Package = Get-AppxPackage -Name "OramaSearch.LiquidInstallerTest"
    if (-not $Package) {
        throw "installed MSIX was not registered"
    }
    $InstalledProgram = Join-Path $Package.InstallLocation "bin\liquid-installer-fixture.exe"
    $ProgramOutput = & $InstalledProgram
    if ($ProgramOutput -ne "liquid installer fixture") {
        throw "installed program returned unexpected output"
    }
    & $LiquidBinary sign verify $SignedArtifact
    if ($LASTEXITCODE -ne 0) {
        throw "platform signature verification failed"
    }
}
finally {
    if ($Package) {
        Remove-AppxPackage -Package $Package.PackageFullName -ErrorAction SilentlyContinue
    }
    if ($Certificate) {
        Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($Certificate.Thumbprint)" `
            -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath "Cert:\CurrentUser\Root\$($Certificate.Thumbprint)" `
            -Force -ErrorAction SilentlyContinue
    }
    Remove-Item Env:\LIQUID_WINDOWS_PFX_PASSWORD -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $TestRoot -Recurse -Force -ErrorAction SilentlyContinue
}

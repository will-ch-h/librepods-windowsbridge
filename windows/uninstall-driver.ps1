# Removes the AAP L2CAP driver and the test-signing certificate that the
# installer added. Run by installer.iss's [UninstallRun]; safe to run by hand.
#
# Leaving the signing cert in LocalMachine\Root after uninstall would keep a
# third-party code-signing root trusted on the machine forever, so removing it
# is the important half of this script.
$ErrorActionPreference = 'Continue'

# 1. Remove the driver package. pnputil needs the *published* name (oemNN.inf),
#    not the original filename, so look it up from the driver store listing.
$enum = (& pnputil.exe /enum-drivers) -join "`n"
foreach ($block in ($enum -split "(?m)^\s*$")) {
    if ($block -match 'BthEchoSampleCli\.inf' -and $block -match 'Published Name:\s*(oem\d+\.inf)') {
        $oem = $Matches[1]
        Write-Host "Removing driver package $oem"
        & pnputil.exe /delete-driver $oem /uninstall /force
    }
}

# 2. Drop the signing cert from both stores the installer put it in.
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq 'CN=L2CAP Bridge' } |
        ForEach-Object {
            Write-Host "Removing cert $($_.Thumbprint) from LocalMachine\$store"
            Remove-Item $_.PSPath -Force -ErrorAction SilentlyContinue
        }
}
